// PS-WRAP-MAC-FG: keyboard + mouse mode (window side) and the local control server.
//
// KBM mode: on by default for each stream (setting pswrap/kbm_default), Cmd+K (Ctrl+K off macOS) toggles it. The pointer is captured, keys and mouse go to
// StreamSession::PsWrapKbm* (streamsession_pswrapkbm.cpp). It turns itself off when the window loses
// focus, a menu grabs input or the stream ends, so the cursor is never left captured.
//
// Control server: off unless PSWRAP_CONTROL=1 or the setting pswrap/control_server=true. It listens on a
// Unix socket, <AppData>/control.sock, readable by this user only. One JSON object per line, one reply per line:
//   {"op":"status"}                          -> {"ok":true,"session":true,"video":true,"kbm":false}
//   {"op":"set","buttons":["CROSS","L1"],"lx":0,"ly":-1,"rx":0.5,"ry":0,"l2":0,"r2":1}
//        full pad state (missing fields = neutral). Sticks -1..1 (ly/ry < 0 = up), triggers 0..1. Held until changed.
//   {"op":"release"}                         -> neutral pad
//   {"op":"shot"}                            -> {"ok":true,"path":"/…/Screenshot.png"} (the decoded game frame)
//   {"op":"kbm","on":true}                   -> toggle keyboard + mouse mode
// The pad state is released when the client that set it disconnects. tools/pswrap-mcp wraps this as an MCP server.
#include "qmlmainwindow.h"
#include "qmlbackend.h"
#include "streamsession.h"
#include "pswrapkbmconfig.h"

#include <QCursor>
#include <QTimer>
#include <cmath>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QSettings>
#include <QStandardPaths>
#include <memory>

#ifdef Q_OS_MACOS
#include "macMouseCapture.h"
#endif

Q_DECLARE_LOGGING_CATEGORY(chiakiGui)

namespace {

uint32_t controlButtons(const QJsonValue &v)
{
	static const QHash<QString, uint32_t> names = {
		{"CROSS", CHIAKI_CONTROLLER_BUTTON_CROSS}, {"X", CHIAKI_CONTROLLER_BUTTON_CROSS},
		{"CIRCLE", CHIAKI_CONTROLLER_BUTTON_MOON}, {"MOON", CHIAKI_CONTROLLER_BUTTON_MOON},
		{"SQUARE", CHIAKI_CONTROLLER_BUTTON_BOX}, {"BOX", CHIAKI_CONTROLLER_BUTTON_BOX},
		{"TRIANGLE", CHIAKI_CONTROLLER_BUTTON_PYRAMID}, {"PYRAMID", CHIAKI_CONTROLLER_BUTTON_PYRAMID},
		{"L1", CHIAKI_CONTROLLER_BUTTON_L1}, {"R1", CHIAKI_CONTROLLER_BUTTON_R1},
		{"L3", CHIAKI_CONTROLLER_BUTTON_L3}, {"R3", CHIAKI_CONTROLLER_BUTTON_R3},
		{"OPTIONS", CHIAKI_CONTROLLER_BUTTON_OPTIONS}, {"SHARE", CHIAKI_CONTROLLER_BUTTON_SHARE},
		{"CREATE", CHIAKI_CONTROLLER_BUTTON_SHARE}, {"TOUCHPAD", CHIAKI_CONTROLLER_BUTTON_TOUCHPAD},
		{"PS", CHIAKI_CONTROLLER_BUTTON_PS},
		{"UP", CHIAKI_CONTROLLER_BUTTON_DPAD_UP}, {"DOWN", CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN},
		{"LEFT", CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT}, {"RIGHT", CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT}};
	QStringList list;
	if (v.isArray())
		for (const QJsonValue &e : v.toArray())
			list << e.toString();
	else
		list = v.toString().split('|', Qt::SkipEmptyParts);
	uint32_t b = 0;
	for (const QString &n : std::as_const(list))
		b |= names.value(n.trimmed().toUpper(), 0);
	return b;
}

int16_t axis(const QJsonObject &o, const char *key)
{
	return static_cast<int16_t>(std::lround(std::clamp(o.value(QLatin1String(key)).toDouble(0), -1.0, 1.0) * 0x7fff));
}

uint8_t trigger(const QJsonObject &o, const char *key)
{
	return static_cast<uint8_t>(std::lround(std::clamp(o.value(QLatin1String(key)).toDouble(0), 0.0, 1.0) * 255));
}

void reply(QLocalSocket *sock, const QJsonObject &o)
{
	if (sock->state() != QLocalSocket::ConnectedState)
		return;
	sock->write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
	sock->flush();
}

QJsonObject error(const QString &msg)
{
	return {{"ok", false}, {"error", msg}};
}

} // namespace

void QmlMainWindow::pswrapKbmSet(bool on)
{
	if (pswrap_kbm_want != on) {
		pswrap_kbm_want = on;
		emit kbmChanged();
	}
	pswrapKbmApply();
}

// ---------------------------------------------------------------- bindings for the dialog

namespace {
template <typename F>
void editKbmConfig(QmlMainWindow *w, StreamSession *session, F &&change)
{
	PsWrapKbmConfig c = PsWrapKbmConfig::load();
	change(c);
	if (!c.save())
		qCWarning(chiakiGui) << "[kbm] can't save" << PsWrapKbmConfig::path();
	if (session)
		session->PsWrapKbmReload();   // live: the next key press already uses the new table
	emit w->kbmChanged();
}
}

double QmlMainWindow::kbmSensitivity() const { return PsWrapKbmConfig::load().sensitivity; }
bool QmlMainWindow::kbmInvertY() const { return PsWrapKbmConfig::load().invert_y; }

void QmlMainWindow::setKbmSensitivity(double v)
{
	editKbmConfig(this, session, [v](PsWrapKbmConfig &c) { c.sensitivity = std::clamp(v, 0.005, 0.3); });
}

void QmlMainWindow::setKbmInvertY(bool on)
{
	editKbmConfig(this, session, [on](PsWrapKbmConfig &c) { c.invert_y = on; });
}

QVariantList QmlMainWindow::kbmRows() const
{
	const PsWrapKbmConfig c = PsWrapKbmConfig::load();
	QVariantList rows;
	for (const PsWrapKbmActionInfo &a : PsWrapKbmConfig::actions()) {
		QVariantList inputs;
		for (const QString &in : c.inputsFor(a.id))
			inputs << QVariantMap{{"id", in}, {"label", PsWrapKbmConfig::displayName(in)}};
		rows << QVariantMap{{"action", a.id}, {"group", a.group}, {"pad", a.pad}, {"use", a.use}, {"inputs", inputs}};
	}
	return rows;
}

QString QmlMainWindow::kbmInputForKey(int key) const { return PsWrapKbmConfig::nameFromKey(key); }
QString QmlMainWindow::kbmInputForMouse(int button) const { return PsWrapKbmConfig::inputFromMouse(Qt::MouseButton(button)); }
QString QmlMainWindow::kbmInputLabel(const QString &input) const { return PsWrapKbmConfig::displayName(input); }

QString QmlMainWindow::kbmActionOf(const QString &input) const
{
	for (const auto &b : PsWrapKbmConfig::load().bindings)
		if (b.first.compare(input, Qt::CaseInsensitive) == 0)
			return b.second;
	return {};
}

void QmlMainWindow::kbmBind(const QString &action, const QString &input)
{
	if (input.isEmpty() || !PsWrapKbmConfig::isAction(action))
		return;
	editKbmConfig(this, session, [&](PsWrapKbmConfig &c) { c.bind(input, action); });
}

void QmlMainWindow::kbmUnbind(const QString &input)
{
	editKbmConfig(this, session, [&](PsWrapKbmConfig &c) { c.unbind(input); });
}

void QmlMainWindow::kbmReset()
{
	editKbmConfig(this, session, [](PsWrapKbmConfig &c) {
		const PsWrapKbmConfig d = PsWrapKbmConfig::defaults();
		c.bindings = d.bindings;
		c.sensitivity = d.sensitivity;
		c.invert_y = d.invert_y;
	});
}

void QmlMainWindow::pswrapKbmApply()
{
	// Only while the game is on screen and the window has the keyboard: otherwise key releases would be
	// lost (stuck buttons) and the pointer would stay captured outside the game.
	const bool on = pswrap_kbm_want && session && has_video && !grab_input && isActive();
	if (on == pswrap_kbm_on)
		return;
	pswrap_kbm_on = on;
	if (session)
		session->PsWrapKbmSetActive(on);
	pswrap_kbm_capture = on;
#ifdef Q_OS_MACOS
	if (!pswrap_kbm_poll) {
		// 250 Hz: read how far the parked cursor moved, feed it to the right stick, park it again
		pswrap_kbm_poll = new QTimer(this);
		pswrap_kbm_poll->setTimerType(Qt::PreciseTimer);
		pswrap_kbm_poll->setInterval(4);
		connect(pswrap_kbm_poll, &QTimer::timeout, this, [this]() {
			const QPoint c = mapToGlobal(QPoint(width() / 2, height() / 2));
			double dx, dy;
			if (pswrapMacMouseCapturePoll(c.x(), c.y(), &dx, &dy) && session && pswrap_kbm_on) {
				session->PsWrapKbmMouseDelta(dx, dy);
				pswrap_kbm_diag_n++;
				pswrap_kbm_diag_sum += std::abs(dx) + std::abs(dy);
			}
			if (++pswrap_kbm_diag_ticks >= 500) {   // every 2 s while moving: proof that the mouse reaches the stick
				if (pswrap_kbm_diag_n)
					qCInfo(chiakiGui) << "[kbm] mouse moves" << pswrap_kbm_diag_n << "total" << pswrap_kbm_diag_sum << "pt in 2 s";
				pswrap_kbm_diag_ticks = pswrap_kbm_diag_n = 0;
				pswrap_kbm_diag_sum = 0;
			}
		});
	}
	if (on) {
		const QPoint c = mapToGlobal(QPoint(width() / 2, height() / 2));
		pswrapMacMouseCaptureStart(c.x(), c.y());
		pswrap_kbm_poll->start();
	} else {
		pswrap_kbm_poll->stop();
		pswrapMacMouseCaptureStop();
	}
#else
	setCursor(on ? Qt::BlankCursor : Qt::ArrowCursor);
	if (on)
		QCursor::setPos(mapToGlobal(QPoint(width() / 2, height() / 2)));
#endif
	qCInfo(chiakiGui) << "[kbm] window mode" << (on ? "on" : "off") << "(wanted" << pswrap_kbm_want << ")";
	emit kbmChanged();
}

bool QmlMainWindow::pswrapKbmMouseEvent(QMouseEvent *event)
{
	if (!pswrap_kbm_on || !session)
		return false;
	switch (event->type()) {
	case QEvent::MouseMove: {
#ifndef Q_OS_MACOS
		// No raw deltas off macOS: measure from the window centre and put the pointer back there.
		const QPoint c(width() / 2, height() / 2);
		const QPointF d = event->position() - QPointF(c);
		if (!d.isNull()) {
			session->PsWrapKbmMouseDelta(d.x(), d.y());
			QCursor::setPos(mapToGlobal(c));
		}
#endif
		return true;
	}
	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonRelease:
		session->PsWrapKbmMouseButton(event->button(), event->type() == QEvent::MouseButtonPress);
		return true;
	default:
		return false;
	}
}

void QmlMainWindow::pswrapInitControl()
{
	// leave KBM mode whenever its preconditions go away
	// Each stream starts in keyboard + mouse mode unless pswrap/kbm_default=false; Cmd+K toggles it.
	connect(backend, &QmlBackend::sessionChanged, this, [this](StreamSession *s) {
		if (pswrap_kbm_on) {   // the old session (and its KBM state) is gone: just give the pointer back
			pswrap_kbm_on = pswrap_kbm_capture = false;
#ifdef Q_OS_MACOS
			if (pswrap_kbm_poll)
				pswrap_kbm_poll->stop();
			pswrapMacMouseCaptureStop();
#else
			setCursor(Qt::ArrowCursor);
#endif
		}
		pswrap_control_owner = nullptr;
		pswrap_kbm_want = s && QSettings().value(QStringLiteral("pswrap/kbm_default"), true).toBool();
		pswrapKbmApply();
	});
	connect(this, &QmlMainWindow::hasVideoChanged, this, &QmlMainWindow::pswrapKbmApply);
	connect(this, &QWindow::activeChanged, this, &QmlMainWindow::pswrapKbmApply);

	const bool enabled = qEnvironmentVariable("PSWRAP_CONTROL") == QLatin1String("1")
		|| QSettings().value(QStringLiteral("pswrap/control_server"), false).toBool();
	if (!enabled)
		return;
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	QDir().mkpath(dir);
	const QString path = QDir(dir).filePath(QStringLiteral("control.sock"));
	pswrap_control_server = new QLocalServer(this);
	pswrap_control_server->setSocketOptions(QLocalServer::UserAccessOption);
	QLocalServer::removeServer(path);   // stale socket from a crash
	if (!pswrap_control_server->listen(path)) {
		qCWarning(chiakiGui) << "[control] can't listen on" << path << pswrap_control_server->errorString();
		return;
	}
	qCInfo(chiakiGui) << "[control] listening on" << path;
	connect(pswrap_control_server, &QLocalServer::newConnection, this, [this]() {
		while (QLocalSocket *sock = pswrap_control_server->nextPendingConnection()) {
			connect(sock, &QLocalSocket::readyRead, this, [this, sock]() {
				while (sock->canReadLine())
					pswrapControlLine(sock, sock->readLine().trimmed());
			});
			connect(sock, &QLocalSocket::disconnected, this, [this, sock]() {
				if (pswrap_control_owner == sock) {   // never leave a button held by a client that went away
					pswrap_control_owner = nullptr;
					if (session) {
						ChiakiControllerState idle;
						chiaki_controller_state_set_idle(&idle);
						session->PsWrapSetControlState(idle);
					}
				}
				sock->deleteLater();
			});
		}
	});
}

void QmlMainWindow::pswrapControlLine(QLocalSocket *sock, const QByteArray &line)
{
	if (line.isEmpty())
		return;
	QJsonParseError err;
	const QJsonObject o = QJsonDocument::fromJson(line, &err).object();
	if (err.error != QJsonParseError::NoError) {
		reply(sock, error(QStringLiteral("bad json: ") + err.errorString()));
		return;
	}
	const QString op = o.value("op").toString();
	if (op == QLatin1String("status")) {
		reply(sock, {{"ok", true}, {"session", session != nullptr}, {"video", has_video}, {"kbm", pswrap_kbm_on}, {"kbm_wanted", pswrap_kbm_want},
			{"menu_open", grab_input > 0}, {"width", width()}, {"height", height()}});
		return;
	}
	if (op == QLatin1String("kbm")) {
		pswrapKbmSet(o.value("on").toBool(!pswrap_kbm_want));
		reply(sock, {{"ok", true}, {"kbm", pswrap_kbm_on}, {"wanted", pswrap_kbm_want}});
		return;
	}
	if (!session) {
		reply(sock, error(QStringLiteral("no stream session")));
		return;
	}
	if (op == QLatin1String("set") || op == QLatin1String("release")) {
		ChiakiControllerState st;
		chiaki_controller_state_set_idle(&st);
		if (op == QLatin1String("set")) {
			st.buttons = controlButtons(o.value("buttons"));
			st.left_x = axis(o, "lx");
			st.left_y = axis(o, "ly");
			st.right_x = axis(o, "rx");
			st.right_y = axis(o, "ry");
			st.l2_state = trigger(o, "l2");
			st.r2_state = trigger(o, "r2");
		}
		pswrap_control_owner = sock;
		session->PsWrapSetControlState(st);
		reply(sock, {{"ok", true}});
		return;
	}
	if (op == QLatin1String("shot")) {
		auto saved = std::make_shared<QMetaObject::Connection>();
		auto failed = std::make_shared<QMetaObject::Connection>();
		auto done = [saved, failed]() { QObject::disconnect(*saved); QObject::disconnect(*failed); };
		*saved = connect(this, &QmlMainWindow::screenshotSaved, sock, [sock, done](QString path) {
			done();
			reply(sock, {{"ok", true}, {"path", path}});
		});
		*failed = connect(this, &QmlMainWindow::screenshotFailed, sock, [sock, done](QString message) {
			done();
			reply(sock, error(message));
		});
		takeScreenshot();
		return;
	}
	reply(sock, error(QStringLiteral("unknown op: ") + op));
}
