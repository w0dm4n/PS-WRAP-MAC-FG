// PS-WRAP-MAC-FG: keyboard + mouse mode bindings (see pswrapkbmconfig.h)
#include "pswrapkbmconfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

Q_DECLARE_LOGGING_CATEGORY(chiakiGui)

namespace {

// PC-style layout for console action games (GTA-like): WASD + mouse, Shift = sprint (L3), Space = jump (✕), F = interact.
const char *const kDefaults[][2] = {
	{"W", "LS_UP"}, {"S", "LS_DOWN"}, {"A", "LS_LEFT"}, {"D", "LS_RIGHT"},
	{"Alt", "WALK"},
	{"Shift", "L3"},          // sprint: hold the left stick in while moving
	{"Space", "CROSS"},       // jump
	{"X", "SQUARE"},
	{"F", "TRIANGLE"},
	{"R", "CIRCLE"},
	{"mouse:right", "L2"},
	{"mouse:left", "R2"},
	{"Q", "L1"},
	{"E", "R1"},
	{"mouse:middle", "R3"}, {"V", "R3"},
	{"mouse:back", "L1"}, {"mouse:forward", "R1"},
	{"G", "DRIVE"},
	{"I", "RS_UP"}, {"K", "RS_DOWN"}, {"J", "RS_LEFT"}, {"L", "RS_RIGHT"},
	{"Up", "UP"}, {"Down", "DOWN"}, {"Left", "LEFT"}, {"Right", "RIGHT"},
	{"1", "UP"}, {"2", "RIGHT"}, {"3", "DOWN"}, {"4", "LEFT"},
	{"Tab", "TOUCHPAD"}, {"M", "TOUCHPAD"},
	{"Esc", "OPTIONS"}, {"P", "OPTIONS"},
	{"Return", "CROSS"}, {"Backspace", "CIRCLE"},
	{"F12", "SHARE"},
	{"Home", "PS"},
};

const struct { const char *name; Qt::MouseButton button; const char *label; } kMouse[] = {
	{"left", Qt::LeftButton, "Left click"}, {"right", Qt::RightButton, "Right click"},
	{"middle", Qt::MiddleButton, "Middle click"}, {"back", Qt::BackButton, "Mouse back"},
	{"forward", Qt::ForwardButton, "Mouse forward"},
};

// Modifier keys don't round-trip through QKeySequence. On macOS Qt::Key_Control is ⌘ and Qt::Key_Meta is ⌃.
const QHash<QString, int> &specialKeys()
{
	static const QHash<QString, int> m = {
		{"shift", Qt::Key_Shift}, {"alt", Qt::Key_Alt}, {"option", Qt::Key_Alt},
#ifdef Q_OS_MACOS
		{"ctrl", Qt::Key_Meta}, {"control", Qt::Key_Meta}, {"cmd", Qt::Key_Control}, {"command", Qt::Key_Control},
#else
		{"ctrl", Qt::Key_Control}, {"control", Qt::Key_Control}, {"meta", Qt::Key_Meta},
#endif
		{"space", Qt::Key_Space}, {"tab", Qt::Key_Tab}, {"esc", Qt::Key_Escape}, {"escape", Qt::Key_Escape},
		{"return", Qt::Key_Return}, {"enter", Qt::Key_Enter}, {"backspace", Qt::Key_Backspace},
		{"capslock", Qt::Key_CapsLock},
	};
	return m;
}

} // namespace

const QList<PsWrapKbmActionInfo> &PsWrapKbmConfig::actions()
{
	static const QList<PsWrapKbmActionInfo> a = {
		{"LS_UP", "Move", "Left stick ↑", "Move forward"},
		{"LS_DOWN", "Move", "Left stick ↓", "Move back"},
		{"LS_LEFT", "Move", "Left stick ←", "Move left"},
		{"LS_RIGHT", "Move", "Left stick →", "Move right"},
		{"WALK", "Move", "Left stick ½", "Walk slowly (hold)"},
		{"CROSS", "Actions", "✕", "Jump · confirm"},
		{"SQUARE", "Actions", "□", "Pick up · put down"},
		{"TRIANGLE", "Actions", "△", "Interact · sit · enter / exit vehicle"},
		{"CIRCLE", "Actions", "○", "Melee · reload · back"},
		{"L3", "Actions", "L3", "Sprint (hold while moving) · horn"},
		{"L2", "Combat & driving", "L2", "Aim · brake / reverse"},
		{"R2", "Combat & driving", "R2", "Shoot · accelerate"},
		{"L1", "Combat & driving", "L1", "Weapon wheel"},
		{"R1", "Combat & driving", "R1", "Cover · handbrake"},
		{"DRIVE", "Combat & driving", "Drive mode", "Toggle: forward / back keys become R2 / L2"},
		{"R3", "Camera", "R3", "Look behind"},
		{"RS_UP", "Camera", "Right stick ↑", "Look up (the mouse also turns the camera)"},
		{"RS_DOWN", "Camera", "Right stick ↓", "Look down"},
		{"RS_LEFT", "Camera", "Right stick ←", "Look left"},
		{"RS_RIGHT", "Camera", "Right stick →", "Look right"},
		{"UP", "D-pad & system", "D-pad ↑", "Phone"},
		{"DOWN", "D-pad & system", "D-pad ↓", ""},
		{"LEFT", "D-pad & system", "D-pad ←", ""},
		{"RIGHT", "D-pad & system", "D-pad →", ""},
		{"TOUCHPAD", "D-pad & system", "Touchpad", "Map · camera view"},
		{"OPTIONS", "D-pad & system", "Options", "Pause menu"},
		{"SHARE", "D-pad & system", "Create", "Capture menu"},
		{"PS", "D-pad & system", "PS", "Console home"},
	};
	return a;
}

bool PsWrapKbmConfig::isAction(const QString &id)
{
	for (const auto &a : actions())
		if (a.id == id)
			return true;
	return false;
}

int PsWrapKbmConfig::keyFromName(const QString &name)
{
	if (name.startsWith(QLatin1String("mouse:")))
		return 0;
	auto it = specialKeys().constFind(name.trimmed().toLower());
	if (it != specialKeys().constEnd())
		return it.value();
	const QKeySequence seq = QKeySequence::fromString(name.trimmed(), QKeySequence::PortableText);
	return seq.isEmpty() ? 0 : seq[0].key();
}

QString PsWrapKbmConfig::nameFromKey(int key)
{
	switch (key) {
	case Qt::Key_Shift: return QStringLiteral("Shift");
	case Qt::Key_Alt: return QStringLiteral("Alt");
#ifdef Q_OS_MACOS
	case Qt::Key_Meta: return QStringLiteral("Ctrl");
	case Qt::Key_Control: return QStringLiteral("Cmd");
#else
	case Qt::Key_Control: return QStringLiteral("Ctrl");
	case Qt::Key_Meta: return QStringLiteral("Meta");
#endif
	case Qt::Key_Space: return QStringLiteral("Space");
	case Qt::Key_Tab: return QStringLiteral("Tab");
	case Qt::Key_Escape: return QStringLiteral("Esc");
	case Qt::Key_Return: return QStringLiteral("Return");
	case Qt::Key_Enter: return QStringLiteral("Enter");
	case Qt::Key_Backspace: return QStringLiteral("Backspace");
	case Qt::Key_CapsLock: return QStringLiteral("CapsLock");
	case 0: case Qt::Key_unknown: return {};
	default: break;
	}
	const QString s = QKeySequence(key).toString(QKeySequence::PortableText);
	return keyFromName(s) == key ? s : QString();
}

Qt::MouseButton PsWrapKbmConfig::mouseFromInput(const QString &input)
{
	if (!input.startsWith(QLatin1String("mouse:")))
		return Qt::NoButton;
	const QString n = input.mid(6).toLower();
	for (const auto &m : kMouse)
		if (n == QLatin1String(m.name))
			return m.button;
	return Qt::NoButton;
}

QString PsWrapKbmConfig::inputFromMouse(Qt::MouseButton button)
{
	for (const auto &m : kMouse)
		if (m.button == button)
			return QStringLiteral("mouse:") + QLatin1String(m.name);
	return {};
}

QString PsWrapKbmConfig::displayName(const QString &input)
{
	for (const auto &m : kMouse)
		if (input == QStringLiteral("mouse:") + QLatin1String(m.name))
			return QStringLiteral("🖱 ") + QLatin1String(m.label);
	static const QHash<QString, QString> pretty = {
#ifdef Q_OS_MACOS
		{"Shift", "⇧ Shift"}, {"Alt", "⌥ Option"}, {"Ctrl", "⌃ Control"}, {"Cmd", "⌘ Cmd"},
#else
		{"Shift", "Shift"}, {"Alt", "Alt"}, {"Ctrl", "Ctrl"},
#endif
		{"Return", "↩ Return"}, {"Backspace", "⌫ Backspace"}, {"Tab", "⇥ Tab"}, {"Space", "Space"},
		{"Up", "↑"}, {"Down", "↓"}, {"Left", "←"}, {"Right", "→"}, {"Esc", "Esc"},
	};
	return pretty.value(input, input);
}

QStringList PsWrapKbmConfig::inputsFor(const QString &action) const
{
	QStringList out;
	for (const auto &b : bindings)
		if (b.second == action)
			out << b.first;
	return out;
}

void PsWrapKbmConfig::bind(const QString &input, const QString &action)
{
	unbind(input);
	bindings.append({input, action});
}

void PsWrapKbmConfig::unbind(const QString &input)
{
	bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [&](const QPair<QString, QString> &b) {
		return b.first.compare(input, Qt::CaseInsensitive) == 0;
	}), bindings.end());
}

PsWrapKbmConfig PsWrapKbmConfig::defaults()
{
	PsWrapKbmConfig c;
	for (const auto &d : kDefaults)
		c.bindings.append({QString::fromUtf8(d[0]), QString::fromUtf8(d[1])});
	return c;
}

QString PsWrapKbmConfig::path()
{
	return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("kbm.json"));
}

PsWrapKbmConfig PsWrapKbmConfig::load()
{
	PsWrapKbmConfig c = defaults();
	QFile f(path());
	if (!f.open(QIODevice::ReadOnly))
		return c;
	QJsonParseError err;
	const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &err).object();
	if (err.error != QJsonParseError::NoError) {
		qCWarning(chiakiGui) << "[kbm] can't parse" << f.fileName() << err.errorString() << "- using defaults";
		return c;
	}
	c.sensitivity = std::clamp(o.value("sensitivity").toDouble(c.sensitivity), 0.001, 1.0);
	c.deadzone = std::clamp(o.value("deadzone").toDouble(c.deadzone), 0.0, 0.9);
	c.curve = std::clamp(o.value("curve").toDouble(c.curve), 0.2, 3.0);
	c.walk_scale = std::clamp(o.value("walk_scale").toDouble(c.walk_scale), 0.1, 1.0);
	c.invert_y = o.value("invert_y").toBool(c.invert_y);

	auto accept = [&](const QString &input, const QJsonValue &v) {
		const bool is_mouse = input.startsWith(QLatin1String("mouse:"));
		if ((is_mouse && mouseFromInput(input) == Qt::NoButton) || (!is_mouse && !keyFromName(input))) {
			qCWarning(chiakiGui) << "[kbm] unknown input" << input;
			return;
		}
		const QString action = v.toString().trimmed().toUpper();
		if (action.isEmpty()) {
			c.unbind(input);
		} else if (isAction(action)) {
			c.bind(input, action);
		} else {
			qCWarning(chiakiGui) << "[kbm] unknown action" << action << "for" << input;
		}
	};
	if (o.contains("bindings")) {
		c.bindings.clear();
		const QJsonObject b = o.value("bindings").toObject();
		for (auto it = b.begin(); it != b.end(); ++it)
			accept(it.key(), it.value());
	} else {   // older format: overrides merged over the defaults
		const QJsonObject keys = o.value("keys").toObject();
		for (auto it = keys.begin(); it != keys.end(); ++it)
			accept(it.key(), it.value());
		const QJsonObject mouse = o.value("mouse").toObject();
		for (auto it = mouse.begin(); it != mouse.end(); ++it)
			accept(QStringLiteral("mouse:") + it.key().toLower(), it.value());
	}
	return c;
}

bool PsWrapKbmConfig::save() const
{
	QJsonObject b;
	for (const auto &p : bindings)
		b.insert(p.first, p.second);
	QJsonObject o{{"sensitivity", sensitivity}, {"deadzone", deadzone}, {"curve", curve},
		{"walk_scale", walk_scale}, {"invert_y", invert_y}, {"bindings", b}};
	QDir().mkpath(QFileInfo(path()).absolutePath());
	QSaveFile f(path());
	if (!f.open(QIODevice::WriteOnly))
		return false;
	f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
	return f.commit();
}
