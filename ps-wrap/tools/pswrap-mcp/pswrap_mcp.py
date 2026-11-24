#!/usr/bin/env python3
"""PS-WRAP-MAC-FG MCP server: drive the PS5 game streamed by PS-WRAP from an MCP client (Claude Code, ...).

It talks to the app's local control server (qmlmainwindow_pswrapcontrol.cpp), which is off unless the app
runs with PSWRAP_CONTROL=1 or `defaults write com.ps-wrap-fg.PS-WRAP pswrap.control_server -bool true`.

No dependencies: MCP over stdio (JSON-RPC 2.0, one message per line), Python 3.9+.

Register with Claude Code:
    claude mcp add pswrap -- python3 /path/to/pswrap_mcp.py

Pad conventions: sticks -1..1, y < 0 = up / forward; triggers 0..1.
Button names: CROSS CIRCLE SQUARE TRIANGLE L1 R1 L3 R3 OPTIONS SHARE TOUCHPAD PS UP DOWN LEFT RIGHT,
plus "L2"/"R2" (full pull) wherever a button list is accepted.
"""
import base64
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

SOCK_PATH = os.environ.get(
    "PSWRAP_CONTROL_SOCK",
    os.path.expanduser("~/Library/Application Support/PS-WRAP-FG/PS-WRAP/control.sock"))
MAX_HOLD_S = 30.0
SERVER_INFO = {"name": "pswrap", "version": "0.1.0"}

# High-level actions. Layout of console action games with vehicles (GTA-style); edit or extend
# with PSWRAP_ACTIONS=/path/actions.json ({"name": {"buttons": [...], "ms": 150, "desc": "..."}}).
ACTIONS = {
    "enter_vehicle": {"buttons": ["TRIANGLE"], "ms": 150, "desc": "Enter / exit the nearest vehicle (Triangle)"},
    "exit_vehicle": {"buttons": ["TRIANGLE"], "ms": 150, "desc": "Get out of the vehicle (Triangle)"},
    "interact": {"buttons": ["TRIANGLE"], "ms": 150, "desc": "Interact / pick up (Triangle)"},
    "jump": {"buttons": ["SQUARE"], "ms": 150, "desc": "Jump / climb (Square)"},
    "melee": {"buttons": ["CIRCLE"], "ms": 150, "desc": "Melee attack (Circle)"},
    "reload": {"buttons": ["CIRCLE"], "ms": 150, "desc": "Reload (Circle)"},
    "crouch": {"buttons": ["L3"], "ms": 150, "desc": "Stealth / crouch toggle (L3)"},
    "horn": {"buttons": ["L3"], "ms": 600, "desc": "Horn while driving (L3)"},
    "look_behind": {"buttons": ["R3"], "ms": 800, "desc": "Look behind (R3)"},
    "cover": {"buttons": ["R1"], "ms": 150, "desc": "Take cover (R1)"},
    "handbrake": {"buttons": ["R1"], "ms": 600, "desc": "Handbrake (R1)"},
    "weapon_wheel": {"buttons": ["L1"], "ms": 800, "desc": "Open the weapon wheel briefly (L1)"},
    "shoot": {"buttons": ["R2"], "ms": 300, "desc": "Fire (R2)"},
    "aim": {"buttons": ["L2"], "ms": 1000, "desc": "Aim (L2) for a second"},
    "phone": {"buttons": ["UP"], "ms": 150, "desc": "Phone (D-pad up)"},
    "map": {"buttons": ["TOUCHPAD"], "ms": 150, "desc": "Map / change camera (Touchpad)"},
    "camera_view": {"buttons": ["TOUCHPAD"], "ms": 150, "desc": "Cycle camera view (Touchpad)"},
    "pause": {"buttons": ["OPTIONS"], "ms": 150, "desc": "Pause menu (Options)"},
    "confirm": {"buttons": ["CROSS"], "ms": 150, "desc": "Confirm in menus (Cross)"},
    "back": {"buttons": ["CIRCLE"], "ms": 150, "desc": "Back in menus (Circle)"},
    "ps_home": {"buttons": ["PS"], "ms": 150, "desc": "PS button (console home)"},
}

DIRECTIONS = {
    "forward": (0, -1), "back": (0, 1), "backward": (0, 1), "left": (-1, 0), "right": (1, 0),
    "forward_left": (-0.7071, -0.7071), "forward_right": (0.7071, -0.7071),
    "back_left": (-0.7071, 0.7071), "back_right": (0.7071, 0.7071),
}


def log(*a):
    print("[pswrap-mcp]", *a, file=sys.stderr, flush=True)


class Pad:
    """One persistent connection: the app releases the pad when it closes, so keep it open."""

    def __init__(self):
        self.sock = None
        self.buf = b""
        self.lock = threading.Lock()
        self.latched = {}   # state kept between tool calls (hold with seconds=0)

    def _connect(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(20)
        try:
            s.connect(SOCK_PATH)
        except OSError as e:
            s.close()
            raise RuntimeError(
                "Can't reach PS-WRAP's control socket (%s): %s. Start PS-WRAP with the control server on "
                "(PSWRAP_CONTROL=1, or `defaults write com.ps-wrap-fg.PS-WRAP pswrap.control_server -bool true` "
                "then relaunch) and start a stream." % (SOCK_PATH, e))
        self.sock, self.buf = s, b""

    def call(self, msg):
        with self.lock:
            for attempt in (0, 1):
                if self.sock is None:
                    self._connect()
                try:
                    self.sock.sendall((json.dumps(msg) + "\n").encode())
                    while b"\n" not in self.buf:
                        chunk = self.sock.recv(65536)
                        if not chunk:
                            raise OSError("connection closed")
                        self.buf += chunk
                    line, self.buf = self.buf.split(b"\n", 1)
                    return json.loads(line)
                except OSError:
                    self.sock = None
                    if attempt:
                        raise

    def set(self, state):
        r = self.call(dict(state, op="set"))
        if not r.get("ok"):
            raise RuntimeError(r.get("error", "set failed"))

    def restore(self):
        if self.latched:
            self.set(self.latched)
        else:
            r = self.call({"op": "release"})
            if not r.get("ok"):
                raise RuntimeError(r.get("error", "release failed"))


PAD = Pad()


def build_state(buttons=None, lx=0.0, ly=0.0, rx=0.0, ry=0.0, l2=0.0, r2=0.0):
    btns = []
    for b in buttons or []:
        b = str(b).upper()
        if b == "L2":
            l2 = max(l2, 1.0)
        elif b == "R2":
            r2 = max(r2, 1.0)
        else:
            btns.append(b)
    return {"buttons": btns, "lx": lx, "ly": ly, "rx": rx, "ry": ry, "l2": l2, "r2": r2}


def merge(base, extra):
    """Pad state = latched state plus this call's input (buttons OR'ed, axes/triggers overridden when set)."""
    out = dict(base) if base else build_state()
    out["buttons"] = sorted(set(out.get("buttons", [])) | set(extra.get("buttons", [])))
    for k in ("lx", "ly", "rx", "ry", "l2", "r2"):
        if extra.get(k):
            out[k] = extra[k]
    return out


def timed(state, seconds):
    seconds = max(0.0, min(float(seconds), MAX_HOLD_S))
    PAD.set(merge(PAD.latched, state))
    time.sleep(seconds)
    PAD.restore()


def clamp(v, lo=-1.0, hi=1.0):
    return max(lo, min(hi, float(v)))


# ---------------------------------------------------------------- tools

def t_status(a):
    return json.dumps(PAD.call({"op": "status"}))


def t_screenshot(a):
    r = PAD.call({"op": "shot"})
    if not r.get("ok"):
        raise RuntimeError(r.get("error", "screenshot failed"))
    path = r["path"]
    width = int(a.get("max_width", 1280))
    fd, out = tempfile.mkstemp(suffix=".jpg")
    os.close(fd)
    try:
        subprocess.run(["sips", "-Z", str(width), "-s", "format", "jpeg", "-s", "formatOptions", "70", path, "--out", out],
                       check=True, capture_output=True)
        data = open(out, "rb").read()
    finally:
        os.unlink(out)
    return [{"type": "image", "data": base64.b64encode(data).decode(), "mimeType": "image/jpeg"},
            {"type": "text", "text": "Frame saved at %s" % path}]


def t_press(a):
    buttons = a.get("buttons") or []
    if isinstance(buttons, str):
        buttons = [buttons]
    times = max(1, min(int(a.get("times", 1)), 20))
    ms = int(a.get("ms", 120))
    for i in range(times):
        timed(build_state(buttons), ms / 1000.0)
        if i + 1 < times:
            time.sleep(0.12)
    return "pressed %s x%d" % ("+".join(buttons), times)


def t_hold(a):
    state = build_state(a.get("buttons"), clamp(a.get("lx", 0)), clamp(a.get("ly", 0)), clamp(a.get("rx", 0)),
                        clamp(a.get("ry", 0)), clamp(a.get("l2", 0), 0, 1), clamp(a.get("r2", 0), 0, 1))
    seconds = float(a.get("seconds", 1.0))
    if seconds <= 0:
        PAD.latched = merge(PAD.latched, state)
        PAD.set(PAD.latched)
        return "latched until release: %s" % json.dumps(PAD.latched)
    timed(state, seconds)
    return "held %.2fs" % seconds


def t_move(a):
    d = a.get("direction", "forward")
    if d not in DIRECTIONS:
        raise ValueError("direction must be one of %s" % ", ".join(DIRECTIONS))
    x, y = DIRECTIONS[d]
    speed = clamp(a.get("speed", 1.0), 0, 1)
    buttons = ["CROSS"] if a.get("sprint") else []
    seconds = float(a.get("seconds", 1.0))
    if a.get("sprint"):
        # GTA-style sprint = tap Cross repeatedly while moving
        end = time.time() + min(seconds, MAX_HOLD_S)
        tap = False
        while time.time() < end:
            tap = not tap
            PAD.set(merge(PAD.latched, build_state(["CROSS"] if tap else [], lx=x * speed, ly=y * speed)))
            time.sleep(0.09)
        PAD.restore()
    else:
        timed(build_state(buttons, lx=x * speed, ly=y * speed), seconds)
    return "moved %s for %.2fs" % (d, seconds)


def t_look(a):
    x, y = clamp(a.get("x", 0)), clamp(a.get("y", 0))
    timed(build_state(rx=x, ry=y), float(a.get("seconds", 0.5)))
    return "camera stick (%.2f, %.2f)" % (x, y)


def t_drive(a):
    throttle = clamp(a.get("throttle", 1.0))   # >0 gas (R2), <0 brake / reverse (L2)
    steer = clamp(a.get("steer", 0.0))
    buttons = ["R1"] if a.get("handbrake") else []
    timed(build_state(buttons, lx=steer, r2=max(throttle, 0), l2=max(-throttle, 0)), float(a.get("seconds", 1.0)))
    return "drove throttle=%.2f steer=%.2f for %ss" % (throttle, steer, a.get("seconds", 1.0))


def t_action(a):
    name = a.get("name", "")
    if name not in ACTIONS:
        raise ValueError("unknown action %r; known: %s" % (name, ", ".join(sorted(ACTIONS))))
    act = ACTIONS[name]
    timed(build_state(act["buttons"]), act.get("ms", 150) / 1000.0)
    return "%s: %s" % (name, act["desc"])


def t_sequence(a):
    steps = a.get("steps") or []
    for st in steps[:200]:
        state = build_state(st.get("buttons"), clamp(st.get("lx", 0)), clamp(st.get("ly", 0)), clamp(st.get("rx", 0)),
                            clamp(st.get("ry", 0)), clamp(st.get("l2", 0), 0, 1), clamp(st.get("r2", 0), 0, 1))
        PAD.set(merge(PAD.latched, state))
        time.sleep(max(0, min(int(st.get("ms", 100)), 30000)) / 1000.0)
    PAD.restore()
    return "ran %d steps" % len(steps)


def t_release(a):
    PAD.latched = {}
    PAD.restore()
    return "pad released"


def t_kbm(a):
    msg = {"op": "kbm"}
    if "on" in a:
        msg["on"] = bool(a["on"])
    return json.dumps(PAD.call(msg))


BUTTONS_SCHEMA = {"type": "array", "items": {"type": "string"},
                  "description": "CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 L3 R3 OPTIONS SHARE TOUCHPAD PS UP DOWN LEFT RIGHT"}
AXIS = {"type": "number", "minimum": -1, "maximum": 1}
TRIG = {"type": "number", "minimum": 0, "maximum": 1}

TOOLS = [
    ("status", t_status, "Stream status: session running, video, keyboard+mouse mode, window size.", {}),
    ("screenshot", t_screenshot, "Capture the current game frame and return it as an image. Use it to see what's on screen before and after acting.",
     {"max_width": {"type": "integer", "description": "Resize to this width (default 1280)"}}),
    ("press", t_press, "Tap pad buttons together (e.g. ['TRIANGLE'] to enter a car, ['CROSS'] to confirm).",
     {"buttons": BUTTONS_SCHEMA, "ms": {"type": "integer", "description": "Press length, default 120"},
      "times": {"type": "integer", "description": "Repeat count, default 1"}}),
    ("move", t_move, "Walk/run the character with the left stick.",
     {"direction": {"type": "string", "enum": list(DIRECTIONS)}, "seconds": {"type": "number"},
      "speed": {"type": "number", "minimum": 0, "maximum": 1}, "sprint": {"type": "boolean", "description": "Tap Cross while moving (GTA sprint)"}}),
    ("look", t_look, "Turn the camera with the right stick. x>0 right, y<0 up. ~0.5s at x=1 is a quarter to half turn in most games.",
     {"x": AXIS, "y": AXIS, "seconds": {"type": "number"}}),
    ("drive", t_drive, "Drive a vehicle: throttle >0 = R2 gas, <0 = L2 brake/reverse; steer = left stick X.",
     {"throttle": AXIS, "steer": AXIS, "seconds": {"type": "number"}, "handbrake": {"type": "boolean"}}),
    ("action", t_action, "Named game action (GTA-style layout): " + ", ".join(sorted(ACTIONS)) + ".",
     {"name": {"type": "string", "enum": sorted(ACTIONS)}}),
    ("hold", t_hold, "Hold an exact pad state for `seconds` (0 = keep it held across calls until `release`).",
     {"buttons": BUTTONS_SCHEMA, "lx": AXIS, "ly": AXIS, "rx": AXIS, "ry": AXIS, "l2": TRIG, "r2": TRIG,
      "seconds": {"type": "number"}}),
    ("sequence", t_sequence, "Run a timed list of pad states, e.g. a combo. Each step: {buttons, lx, ly, rx, ry, l2, r2, ms}.",
     {"steps": {"type": "array", "items": {"type": "object"}}}),
    ("release", t_release, "Release everything (neutral pad), including latched holds.", {}),
    ("keyboard_mouse_mode", t_kbm, "Turn PS-WRAP's keyboard + mouse mode on/off (same as Cmd+K in the window).",
     {"on": {"type": "boolean"}}),
]
TOOL_FNS = {name: fn for name, fn, _, _ in TOOLS}


def tool_list():
    return [{"name": n, "description": d, "inputSchema": {"type": "object", "properties": p}} for n, _, d, p in TOOLS]


def handle(req):
    method = req.get("method")
    if method == "initialize":
        return {"protocolVersion": req.get("params", {}).get("protocolVersion", "2025-06-18"),
                "capabilities": {"tools": {}}, "serverInfo": SERVER_INFO,
                "instructions": "Controls the PS5 game streamed in PS-WRAP. Take a screenshot to see the game, act with "
                                "move/look/press/action/drive, then screenshot again to check the result."}
    if method == "tools/list":
        return {"tools": tool_list()}
    if method == "tools/call":
        p = req.get("params", {})
        fn = TOOL_FNS.get(p.get("name"))
        if not fn:
            return {"content": [{"type": "text", "text": "unknown tool %s" % p.get("name")}], "isError": True}
        try:
            out = fn(p.get("arguments") or {})
            content = out if isinstance(out, list) else [{"type": "text", "text": str(out)}]
            return {"content": content}
        except Exception as e:   # report to the model instead of failing the RPC
            try:
                PAD.restore()
            except Exception:
                pass
            return {"content": [{"type": "text", "text": "error: %s" % e}], "isError": True}
    if method == "ping":
        return {}
    raise KeyError(method)


def main():
    extra = os.environ.get("PSWRAP_ACTIONS")
    if extra:
        ACTIONS.update(json.load(open(extra)))
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError:
            continue
        if "id" not in req:   # notification
            continue
        try:
            resp = {"jsonrpc": "2.0", "id": req["id"], "result": handle(req)}
        except KeyError as e:
            resp = {"jsonrpc": "2.0", "id": req["id"], "error": {"code": -32601, "message": "method not found: %s" % e}}
        sys.stdout.write(json.dumps(resp) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
