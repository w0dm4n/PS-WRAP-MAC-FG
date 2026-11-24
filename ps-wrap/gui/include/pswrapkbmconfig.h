// PS-WRAP-MAC-FG: keyboard + mouse mode bindings, shared by the stream session (applies them) and the
// "Keyboard & mouse" dialog (shows / edits them). Stored in <AppData>/kbm.json:
//   { "sensitivity": 0.12, "deadzone": 0.25, "curve": 0.6, "walk_scale": 0.45, "invert_y": false,
//     "bindings": { "W": "LS_UP", "Space": "CROSS", "mouse:left": "R2", ... } }
// An input is a key name ("W", "Shift", "Space", "Esc", "F5") or "mouse:<left|right|middle|back|forward>".
// Each input drives one action; an action can have several inputs. Without "bindings" the defaults apply
// (older files: "keys" / "mouse" objects are merged over the defaults).
#ifndef PSWRAP_KBMCONFIG_H
#define PSWRAP_KBMCONFIG_H

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <Qt>

struct PsWrapKbmActionInfo
{
	QString id;        // "CROSS", "LS_UP", "WALK", ...
	QString group;     // section in the dialog
	QString pad;       // what it is on the pad ("✕", "Left stick ↑", ...)
	QString use;       // what it usually does in games
};

struct PsWrapKbmConfig
{
	double sensitivity = 0.12;   // stick fraction per point of mouse movement per 8 ms tick
	double deadzone = 0.25;      // start mouse-driven stick values above the game's inner deadzone
	double curve = 0.6;          // < 1: small mouse / trackpad moves already turn the camera fast
	double walk_scale = 0.45;
	bool invert_y = false;
	QList<QPair<QString, QString>> bindings;   // input -> action, in display order

	static PsWrapKbmConfig defaults();
	static PsWrapKbmConfig load();
	bool save() const;
	static QString path();

	QStringList inputsFor(const QString &action) const;
	void bind(const QString &input, const QString &action);   // the input leaves any other action
	void unbind(const QString &input);

	static const QList<PsWrapKbmActionInfo> &actions();
	static bool isAction(const QString &id);
	// key <-> input name
	static int keyFromName(const QString &name);              // 0 = unknown / not a key
	static QString nameFromKey(int key);                      // "" = can't be bound
	static Qt::MouseButton mouseFromInput(const QString &input);
	static QString inputFromMouse(Qt::MouseButton button);
	static QString displayName(const QString &input);         // "⇧ Shift", "Left click", ...
};

#endif
