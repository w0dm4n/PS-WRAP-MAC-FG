// PS-WRAP-MAC-FG: keyboard + mouse mode, plus the pad state driven by the local control server.
//
// Keyboard + mouse ("KBM") mode turns the keyboard and mouse into a DualSense the way PC games do it:
//   WASD = left stick (opposite keys cancel, diagonals normalised), mouse movement = right stick
//   (the window captures the pointer and feeds deltas into PsWrapKbmMouseDelta), keys / mouse buttons = pad buttons.
// It is separate from chiaki's old keyboard mapping (Settings "keymap/…"), which stays as it is when KBM is off.
// Bindings and tuning come from PsWrapKbmConfig (pswrapkbmconfig.cpp, <AppData>/kbm.json), edited in the
// "Keyboard & mouse" dialog. Special actions: WALK (hold = left stick at walk_scale), DRIVE (toggle: forward /
// back keys also pull R2 / L2 and stop moving the left stick, for driving with WASD).
#include "streamsession.h"
#include "pswrapkbmconfig.h"

#include <QLoggingCategory>
#include <cmath>

Q_DECLARE_LOGGING_CATEGORY(chiakiGui)

namespace {

constexpr int kKbmMouseTag = 0x40000000;   // kbm_down key for a mouse button (never a Qt::Key value)
constexpr int kKbmTickMs = 8;

bool toKbmAction(const QString &id, StreamSession::KbmAction *out)
{
	using K = StreamSession::KbmAction;
	static const QHash<QString, QPair<K::Kind, uint32_t>> m = {
		{"CROSS", {K::Button, CHIAKI_CONTROLLER_BUTTON_CROSS}}, {"CIRCLE", {K::Button, CHIAKI_CONTROLLER_BUTTON_MOON}},
		{"SQUARE", {K::Button, CHIAKI_CONTROLLER_BUTTON_BOX}}, {"TRIANGLE", {K::Button, CHIAKI_CONTROLLER_BUTTON_PYRAMID}},
		{"L1", {K::Button, CHIAKI_CONTROLLER_BUTTON_L1}}, {"R1", {K::Button, CHIAKI_CONTROLLER_BUTTON_R1}},
		{"L3", {K::Button, CHIAKI_CONTROLLER_BUTTON_L3}}, {"R3", {K::Button, CHIAKI_CONTROLLER_BUTTON_R3}},
		{"OPTIONS", {K::Button, CHIAKI_CONTROLLER_BUTTON_OPTIONS}}, {"SHARE", {K::Button, CHIAKI_CONTROLLER_BUTTON_SHARE}},
		{"TOUCHPAD", {K::Button, CHIAKI_CONTROLLER_BUTTON_TOUCHPAD}}, {"PS", {K::Button, CHIAKI_CONTROLLER_BUTTON_PS}},
		{"UP", {K::Button, CHIAKI_CONTROLLER_BUTTON_DPAD_UP}}, {"DOWN", {K::Button, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN}},
		{"LEFT", {K::Button, CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT}}, {"RIGHT", {K::Button, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT}},
		{"L2", {K::L2, 0}}, {"R2", {K::R2, 0}},
		{"LS_UP", {K::LsUp, 0}}, {"LS_DOWN", {K::LsDown, 0}}, {"LS_LEFT", {K::LsLeft, 0}}, {"LS_RIGHT", {K::LsRight, 0}},
		{"RS_UP", {K::RsUp, 0}}, {"RS_DOWN", {K::RsDown, 0}}, {"RS_LEFT", {K::RsLeft, 0}}, {"RS_RIGHT", {K::RsRight, 0}},
		{"WALK", {K::Walk, 0}}, {"DRIVE", {K::Drive, 0}},
	};
	auto it = m.constFind(id);
	if (it == m.constEnd())
		return false;
	out->kind = it->first;
	out->button = it->second;
	return true;
}

int16_t toAxis(double v)
{
	return static_cast<int16_t>(std::lround(std::clamp(v, -1.0, 1.0) * 0x7fff));
}

} // namespace

void StreamSession::PsWrapKbmReload()
{
	const PsWrapKbmConfig c = PsWrapKbmConfig::load();
	kbm_keys.clear();
	kbm_mouse.clear();
	for (const auto &b : c.bindings) {
		KbmAction a;
		if (!toKbmAction(b.second, &a))
			continue;
		const Qt::MouseButton mb = PsWrapKbmConfig::mouseFromInput(b.first);
		if (mb != Qt::NoButton)
			kbm_mouse.insert(int(mb), a);
		else if (const int key = PsWrapKbmConfig::keyFromName(b.first))
			kbm_keys.insert(key, a);
	}
	kbm_sens = c.sensitivity;
	kbm_deadzone = c.deadzone;
	kbm_curve = c.curve;
	kbm_walk = c.walk_scale;
	kbm_invert_y = c.invert_y;
	qCInfo(chiakiGui) << "[kbm]" << kbm_keys.size() << "keys," << kbm_mouse.size() << "mouse buttons, sens" << kbm_sens
		<< "deadzone" << kbm_deadzone;
}

void StreamSession::PsWrapKbmSetActive(bool on)
{
	if (on == kbm_active)
		return;
	if (on)
		PsWrapKbmReload();   // pick up kbm.json edits each time the mode is turned on
	kbm_active = on;
	PsWrapKbmReleaseAll();
	if (!kbm_timer) {
		kbm_timer = new QTimer(this);
		kbm_timer->setTimerType(Qt::PreciseTimer);
		kbm_timer->setInterval(kKbmTickMs);
		connect(kbm_timer, &QTimer::timeout, this, &StreamSession::PsWrapKbmTick);
	}
	if (on)
		kbm_timer->start();
	else
		kbm_timer->stop();
	qCInfo(chiakiGui) << "[kbm] mode" << (on ? "on" : "off");
}

void StreamSession::PsWrapKbmReleaseAll()
{
	kbm_down.clear();
	kbm_acc_dx = kbm_acc_dy = kbm_rx = kbm_ry = 0;
	chiaki_controller_state_set_idle(&kbm_state);
	SendFeedbackState();
}

bool StreamSession::PsWrapKbmKey(QKeyEvent *event)
{
	if (!kbm_active)
		return false;
	auto it = kbm_keys.constFind(event->key());
	if (it == kbm_keys.constEnd())
		return true;   // unbound keys do nothing in KBM mode (the old keymap must not fire as well)
	if (event->isAutoRepeat())
		return true;
	const bool press = event->type() == QEvent::KeyPress;
	if (it->kind == KbmAction::Drive) {
		if (press) {
			kbm_drive = !kbm_drive;
			qCInfo(chiakiGui) << "[kbm] drive mode" << (kbm_drive ? "on" : "off");
			PsWrapKbmRecompute();
		}
		return true;
	}
	if (press)
		kbm_down.insert(event->key(), it.value());
	else
		kbm_down.remove(event->key());
	PsWrapKbmRecompute();
	return true;
}

void StreamSession::PsWrapKbmMouseButton(Qt::MouseButton button, bool pressed)
{
	if (!kbm_active)
		return;
	auto it = kbm_mouse.constFind(button);
	if (it == kbm_mouse.constEnd())
		return;
	if (pressed)
		kbm_down.insert(kKbmMouseTag | int(button), it.value());
	else
		kbm_down.remove(kKbmMouseTag | int(button));
	PsWrapKbmRecompute();
}

void StreamSession::PsWrapKbmMouseDelta(double dx, double dy)
{
	if (!kbm_active)
		return;
	kbm_acc_dx += dx;
	kbm_acc_dy += dy;
}

// Rebuild everything but the mouse-driven right stick from the held keys.
void StreamSession::PsWrapKbmRecompute()
{
	uint32_t buttons = 0;
	bool l2 = false, r2 = false, walk = false;
	int lx = 0, ly = 0, rx = 0, ry = 0;
	for (const KbmAction &a : std::as_const(kbm_down)) {
		switch (a.kind) {
		case KbmAction::Button: buttons |= a.button; break;
		case KbmAction::L2: l2 = true; break;
		case KbmAction::R2: r2 = true; break;
		case KbmAction::LsUp: ly -= 1; if (kbm_drive) r2 = true; break;
		case KbmAction::LsDown: ly += 1; if (kbm_drive) l2 = true; break;
		case KbmAction::LsLeft: lx -= 1; break;
		case KbmAction::LsRight: lx += 1; break;
		case KbmAction::RsUp: ry -= 1; break;
		case KbmAction::RsDown: ry += 1; break;
		case KbmAction::RsLeft: rx -= 1; break;
		case KbmAction::RsRight: rx += 1; break;
		case KbmAction::Walk: walk = true; break;
		case KbmAction::Drive: break;
		}
	}
	lx = std::clamp(lx, -1, 1); ly = std::clamp(ly, -1, 1);
	rx = std::clamp(rx, -1, 1); ry = std::clamp(ry, -1, 1);
	double mag = walk ? kbm_walk : 1.0;
	if (kbm_drive)
		ly = 0;   // in drive mode W/S are gas/brake; the left stick only steers
	const double ln = (lx && ly) ? M_SQRT1_2 : 1.0;   // diagonal = full deflection, not 141 %
	kbm_state.buttons = buttons;
	kbm_state.l2_state = l2 ? 0xff : 0;
	kbm_state.r2_state = r2 ? 0xff : 0;
	kbm_state.left_x = toAxis(lx * ln * mag);
	kbm_state.left_y = toAxis(ly * ln * mag);
	kbm_keys_rx = rx;
	kbm_keys_ry = ry;
	PsWrapKbmTick();
}

void StreamSession::PsWrapKbmTick()
{
	// Mouse distance → rotation budget, counted in "ticks at full stick". A flick fills the budget and the
	// stick stays deflected until it is spent, so the camera turns in proportion to how far the mouse went.
	// (Mapping speed straight to the stick gave 1–2 tick spikes: games ramp the camera up while the stick is
	// held, so short spikes never reached real turning speed.) kbm_rx / kbm_ry hold the unspent budget.
	constexpr double kMaxBudget = 40.0;   // ≈ 0.32 s at full stick: the camera never runs on long after the mouse stops
	double px = kbm_rx + kbm_acc_dx * kbm_sens;
	double py = kbm_ry + kbm_acc_dy * kbm_sens * (kbm_invert_y ? -1.0 : 1.0);
	kbm_acc_dx = kbm_acc_dy = 0;
	double m = std::hypot(px, py);
	if (m > kMaxBudget) {
		px *= kMaxBudget / m;
		py *= kMaxBudget / m;
		m = kMaxBudget;
	}
	double ox = 0, oy = 0;
	if (m > 0.02) {
		const double raw = std::min(1.0, m);   // spend at most one full-stick tick now
		// skip the game's inner deadzone so small moves still turn the camera
		const double out = kbm_deadzone + (1.0 - kbm_deadzone) * std::pow(raw, kbm_curve);
		ox = px / m * out;
		oy = py / m * out;
		px -= px / m * raw;
		py -= py / m * raw;
	} else {
		px = py = 0;
	}
	kbm_rx = px;
	kbm_ry = py;
	if (kbm_keys_rx || kbm_keys_ry) {   // IJKL camera keys win over the mouse
		const double n = (kbm_keys_rx && kbm_keys_ry) ? M_SQRT1_2 : 1.0;
		ox = kbm_keys_rx * n;
		oy = kbm_keys_ry * n;
	}
	kbm_state.right_x = toAxis(ox);
	kbm_state.right_y = toAxis(oy);
	SendFeedbackState();   // steady stream while the mode is on
}

// ---------------------------------------------------------------- external control state

void StreamSession::PsWrapSetControlState(const ChiakiControllerState &state)
{
	control_state = state;
	if (!control_timer) {
		control_timer = new QTimer(this);
		control_timer->setTimerType(Qt::PreciseTimer);
		control_timer->setInterval(16);
		connect(control_timer, &QTimer::timeout, this, [this]() { SendFeedbackState(); });
	}
	ChiakiControllerState idle;
	chiaki_controller_state_set_idle(&idle);
	const bool is_idle = chiaki_controller_state_equals(&idle, &control_state);
	if (is_idle)
		control_timer->stop();
	else if (!control_timer->isActive())
		control_timer->start();   // the console expects a steady input stream while something is held
	SendFeedbackState();
}
