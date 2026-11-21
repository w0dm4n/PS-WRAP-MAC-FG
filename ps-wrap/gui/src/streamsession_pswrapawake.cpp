// PS-WRAP: Stay Awake — ต่อเครื่องไว้แต่ไม่มีใครกดจอยนาน PS จะเข้าโหมดพักเองตามการตั้งค่าประหยัดพลังงาน (เช่น 15 นาที)
// เปิดไว้ (Settings › General › While Streaming) → ไม่มี input จริงจากใครเลย (จอย/คีย์บอร์ด/ทัชแพด/เพื่อนใน Link) ครบ 10 นาที
// → ดันสติ๊กซ้ายเบาๆ แวบเดียว (สลับซ้าย/ขวาทุกครั้ง) ให้เครื่องรู้ว่ามีคนอยู่ · มีคนกดอยู่ = ไม่ยุ่งเลย
// แยกไฟล์จาก streamsession.cpp เพื่อลด conflict ตอน merge upstream (ดู docs/04-upstream-sync.md)
// ทดสอบ: env PSWRAP_STAYAWAKE_TEST_S=<วินาที> ย่อเวลารอ (เช่น 30)
#include "streamsession.h"
#include "settings.h"

#include <chiaki/time.h>

#include <QTimer>
#include <QHash>
#include <QRegularExpression>

#include <cstdlib>

namespace {
constexpr qint64 kIdleMs = 10 * 60 * 1000;   // ไม่มีใครกดนานเท่านี้ → ขยับ
constexpr int kCheckMs = 15 * 1000;
constexpr int kPulseMs = 150;                // ค้างสติ๊กไว้นานเท่านี้ (ให้ feedback sender ส่งทันแน่)
constexpr int16_t kNudge = 3000;             // ~9% ของสุดสติ๊ก — ในเดดโซนของเกมส่วนใหญ่ แต่เครื่องเห็นว่าค่าเปลี่ยน
constexpr int kStickNoise = 1500;            // สติ๊กขยับน้อยกว่านี้ = drift ไม่นับเป็นคนกด

qint64 idleLimitMs()
{
	static const qint64 v = [] {
		const char *e = std::getenv("PSWRAP_STAYAWAKE_TEST_S");
		const long s = e ? std::strtol(e, nullptr, 10) : 0;
		return s > 0 ? qint64(s) * 1000 : kIdleMs;
	}();
	return v;
}

qint64 nowMs() { return qint64(chiaki_time_now_monotonic_ms()); }

bool stickMoved(int16_t a, int16_t b) { return std::abs(int(a) - int(b)) > kStickNoise; }
}

void StreamSession::PsWrapStayAwakeInit(Settings *settings)
{
	pswrap_settings = settings;
	chiaki_controller_state_set_idle(&awake_state);
	chiaki_controller_state_set_idle(&pswrap_last_input);
	pswrap_last_input_ms = nowMs();
	pswrap_awake_timer = new QTimer(this);
	pswrap_awake_timer->setInterval(idleLimitMs() < kIdleMs ? 1000 : kCheckMs);
	connect(pswrap_awake_timer, &QTimer::timeout, this, &StreamSession::PsWrapStayAwakeTick);
	pswrap_awake_timer->start();
}

// เรียกจาก SendFeedbackState ด้วย state รวมของ input จริง (ก่อนรวม awake_state) — gyro/accel ไม่นับ (จอยวางบนโต๊ะก็สั่นเอง)
void StreamSession::PsWrapNoteInput(const ChiakiControllerState &s)
{
	const ChiakiControllerState &p = pswrap_last_input;
	bool changed = s.buttons != p.buttons || s.l2_state != p.l2_state || s.r2_state != p.r2_state
		|| stickMoved(s.left_x, p.left_x) || stickMoved(s.left_y, p.left_y)
		|| stickMoved(s.right_x, p.right_x) || stickMoved(s.right_y, p.right_y);
	for (size_t i = 0; !changed && i < CHIAKI_CONTROLLER_TOUCHES_MAX; i++)
		changed = s.touches[i].id != p.touches[i].id || s.touches[i].x != p.touches[i].x || s.touches[i].y != p.touches[i].y;
	if (!changed)
		return;
	pswrap_last_input = s;
	pswrap_last_input_ms = nowMs();
}

void StreamSession::PsWrapStayAwakeTick()
{
	if (!pswrap_settings || !pswrap_settings->GetStayAwake())
		return;
	const qint64 now = nowMs();
	const qint64 idle = now - pswrap_last_input_ms;
	if (idle < idleLimitMs())
		return;
	pswrap_last_input_ms = now;   // ครั้งถัดไปอีก 10 นาที (ตัวขยับเองไม่ผ่าน PsWrapNoteInput)
	awake_state.left_x = int16_t(kNudge * pswrap_awake_dir);
	pswrap_awake_dir = -pswrap_awake_dir;
	CHIAKI_LOGI(log.GetChiakiLog(), "PSWRAP stay awake: nobody pressed anything for %lld s, nudging the left stick", (long long)(idle / 1000));
	SendFeedbackState();
	QTimer::singleShot(kPulseMs, this, [this] {
		chiaki_controller_state_set_idle(&awake_state);
		SendFeedbackState();
	});
}

// ---------------------------------------------------------------- PS-WRAP-MAC-FG test autopilot
// Enabled only with the env var PSWRAP_AUTOPILOT=/path/script.txt (never by settings). Script lines:
//   <t_ms> <buttons> <lx> <ly> <rx> <ry> <l2> <r2> [# note]     state held from t_ms until the next line
//   buttons: '-' or names joined by '|': CROSS MOON(CIRCLE) BOX(SQUARE) PYRAMID(TRIANGLE) L1 R1 L3 R3 OPTIONS SHARE TOUCHPAD PS UP DOWN LEFT RIGHT
//   sticks -32767..32767 (ly < 0 = up), triggers 0..255 · "loop <t_ms>" at the end repeats from t_ms
// The clock starts when the session is created; every applied line is logged with the "[autopilot]" prefix.
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QLoggingCategory>
#include <QTextStream>
Q_DECLARE_LOGGING_CATEGORY(chiakiGui)

static uint32_t pswrapAutoButtons(const QString &spec)
{
	if (spec == QLatin1String("-"))
		return 0;
	static const QHash<QString, uint32_t> names = {
		{"CROSS", CHIAKI_CONTROLLER_BUTTON_CROSS}, {"MOON", CHIAKI_CONTROLLER_BUTTON_MOON}, {"CIRCLE", CHIAKI_CONTROLLER_BUTTON_MOON},
		{"BOX", CHIAKI_CONTROLLER_BUTTON_BOX}, {"SQUARE", CHIAKI_CONTROLLER_BUTTON_BOX}, {"PYRAMID", CHIAKI_CONTROLLER_BUTTON_PYRAMID},
		{"TRIANGLE", CHIAKI_CONTROLLER_BUTTON_PYRAMID}, {"L1", CHIAKI_CONTROLLER_BUTTON_L1}, {"R1", CHIAKI_CONTROLLER_BUTTON_R1},
		{"L3", CHIAKI_CONTROLLER_BUTTON_L3}, {"R3", CHIAKI_CONTROLLER_BUTTON_R3}, {"OPTIONS", CHIAKI_CONTROLLER_BUTTON_OPTIONS},
		{"SHARE", CHIAKI_CONTROLLER_BUTTON_SHARE}, {"TOUCHPAD", CHIAKI_CONTROLLER_BUTTON_TOUCHPAD}, {"PS", CHIAKI_CONTROLLER_BUTTON_PS},
		{"UP", CHIAKI_CONTROLLER_BUTTON_DPAD_UP}, {"DOWN", CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN},
		{"LEFT", CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT}, {"RIGHT", CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT}};
	uint32_t b = 0;
	for (const QString &n : spec.split('|', Qt::SkipEmptyParts))
		b |= names.value(n.trimmed().toUpper(), 0);
	return b;
}

static bool pswrapAutoLoad(const QString &path, QVector<StreamSession::PsWrapAutoStep> &steps, qint64 &loop_ms);

void StreamSession::PsWrapAutopilotInit()
{
	chiaki_controller_state_set_idle(&autopilot_state);
	const QString path = qEnvironmentVariable("PSWRAP_AUTOPILOT");
	if (path.isEmpty())
		return;
	pswrap_auto_path = path;
	pswrap_auto_mtime = QFileInfo(path).lastModified();
	pswrapAutoLoad(path, pswrap_auto_steps, pswrap_auto_loop_ms);
	qCInfo(chiakiGui) << "[autopilot] loaded" << pswrap_auto_steps.size() << "steps from" << path << "loop" << pswrap_auto_loop_ms;
	pswrap_auto_start_ms = nowMs();
	pswrap_auto_timer = new QTimer(this);
	pswrap_auto_timer->setTimerType(Qt::PreciseTimer);
	pswrap_auto_timer->setInterval(16);
	connect(pswrap_auto_timer, &QTimer::timeout, this, &StreamSession::PsWrapAutopilotTick);
	pswrap_auto_timer->start();
}

// re-read on every file change (clock restarts) → a test harness can drive the session step by step
static bool pswrapAutoLoad(const QString &path, QVector<StreamSession::PsWrapAutoStep> &steps, qint64 &loop_ms)
{
	steps.clear();
	loop_ms = -1;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		qCWarning(chiakiGui) << "[autopilot] can't open" << path;
		return false;
	}
	QTextStream ts(&f);
	while (!ts.atEnd()) {
		QString line = ts.readLine();
		QString note;
		const int hash = line.indexOf('#');
		if (hash >= 0) {
			note = line.mid(hash + 1).trimmed();
			line = line.left(hash);
		}
		const QStringList t = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
		if (t.isEmpty())
			continue;
		if (t[0] == QLatin1String("loop") && t.size() >= 2) {
			loop_ms = t[1].toLongLong();
			continue;
		}
		if (t.size() < 8)
			continue;
		StreamSession::PsWrapAutoStep st;
		st.t_ms = t[0].toLongLong();
		st.buttons = pswrapAutoButtons(t[1]);
		st.lx = int16_t(qBound(-32767, t[2].toInt(), 32767));
		st.ly = int16_t(qBound(-32767, t[3].toInt(), 32767));
		st.rx = int16_t(qBound(-32767, t[4].toInt(), 32767));
		st.ry = int16_t(qBound(-32767, t[5].toInt(), 32767));
		st.l2 = uint8_t(qBound(0, t[6].toInt(), 255));
		st.r2 = uint8_t(qBound(0, t[7].toInt(), 255));
		st.note = note;
		steps.append(st);
	}
	return !steps.isEmpty();
}

void StreamSession::PsWrapAutopilotTick()
{
	if (++pswrap_auto_ticks % 30 == 0) {
		const QDateTime m = QFileInfo(pswrap_auto_path).lastModified();
		if (m != pswrap_auto_mtime) {
			pswrap_auto_mtime = m;
			pswrapAutoLoad(pswrap_auto_path, pswrap_auto_steps, pswrap_auto_loop_ms);
			pswrap_auto_start_ms = nowMs();
			pswrap_auto_cur = -1;
			qCInfo(chiakiGui) << "[autopilot] reloaded" << pswrap_auto_steps.size() << "steps, loop" << pswrap_auto_loop_ms;
		}
	}
	if (pswrap_auto_steps.isEmpty()) {
		chiaki_controller_state_set_idle(&autopilot_state);
		return;
	}
	qint64 t = nowMs() - pswrap_auto_start_ms;
	const qint64 end = pswrap_auto_steps.last().t_ms;
	if (t > end + 1000 && pswrap_auto_loop_ms >= 0 && pswrap_auto_loop_ms < end) {
		pswrap_auto_start_ms = nowMs() - pswrap_auto_loop_ms;   // restart at the loop point
		t = pswrap_auto_loop_ms;
		pswrap_auto_cur = -1;
	}
	int idx = -1;
	for (int i = 0; i < pswrap_auto_steps.size() && pswrap_auto_steps[i].t_ms <= t; i++)
		idx = i;
	if (idx < 0)
		return;
	if (idx == pswrap_auto_steps.size() - 1 && t > end + 1000 && pswrap_auto_loop_ms < 0) {
		if (pswrap_auto_cur != -2) {
			chiaki_controller_state_set_idle(&autopilot_state);   // script finished (keep watching the file)
			SendFeedbackState();
			qCInfo(chiakiGui) << "[autopilot] done";
			pswrap_auto_cur = -2;
		}
		return;
	}
	if (idx != pswrap_auto_cur) {
		const PsWrapAutoStep &s = pswrap_auto_steps[idx];
		pswrap_auto_cur = idx;
		autopilot_state.buttons = s.buttons;
		autopilot_state.left_x = s.lx;
		autopilot_state.left_y = s.ly;
		autopilot_state.right_x = s.rx;
		autopilot_state.right_y = s.ry;
		autopilot_state.l2_state = s.l2;
		autopilot_state.r2_state = s.r2;
		qCInfo(chiakiGui).nospace() << "[autopilot] t=" << t << " step " << idx << " buttons=0x" << Qt::hex << s.buttons << Qt::dec
			<< " L=(" << s.lx << "," << s.ly << ") R=(" << s.rx << "," << s.ry << ") L2=" << s.l2 << " R2=" << s.r2 << " " << s.note;
	}
	SendFeedbackState();   // keep sending (the console expects a steady input stream)
}
