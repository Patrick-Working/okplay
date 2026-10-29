#!/usr/bin/env python3
"""okagent MCP server: gives Claude tools to drive machines running okagent.exe (via vmctl.py serve).

Register once (Claude Code):   claude mcp add okagent -- python <path>\\okagent\\mcp_server.py
The bridge must be running:    python <path>\\okagent\\vmctl.py serve

Speaks MCP over stdio (JSON-RPC 2.0, one message per line) with no packages beyond the standard
library (+ Pillow, optional, to send screenshots as PNG and scale them down).
"""
import base64, io, json, os, socket, sys

CONTROL = int(os.environ.get("OKAGENT_CONTROL", "7791"))
MAX_WIDTH = int(os.environ.get("OKAGENT_SHOT_MAX_WIDTH", "1280"))


def bridge(cmd=None, data=b"", agent=None, op=None, timeout=120):
    try:
        c = socket.create_connection(("127.0.0.1", CONTROL), timeout=timeout + 10)
    except OSError:
        return {"ok": False, "header": "err no bridge running - start it: python vmctl.py serve"}
    req = {"op": op, "agent": agent, "cmd": cmd, "data": base64.b64encode(data).decode(), "timeout": timeout}
    c.sendall((json.dumps(req) + "\n").encode())
    rep = json.loads(c.makefile("rb").readline())
    c.close()
    return rep


def text(s, error=False):
    return {"content": [{"type": "text", "text": s}], "isError": error}


AGENT = {"agent": {"type": "string", "description": "agent (computer) name; needed only when several are connected"}}


def tool(name, desc, props=None, required=()):
    p = dict(AGENT)
    p.update(props or {})
    return {"name": name, "description": desc,
            "inputSchema": {"type": "object", "properties": p, "required": list(required)}}


I = {"type": "integer"}
S = {"type": "string"}
TOOLS = [
    tool("vm_agents", "List the machines whose okagent is connected."),
    tool("vm_info", "Windows version, screen size and colour depth of a machine."),
    tool("vm_screenshot", "Screenshot of the whole screen, a window (number or part of its title) or a rectangle.",
         {"window": S, "x": I, "y": I, "w": I, "h": I}),
    tool("vm_click", "Move the mouse to screen x,y and click (left, right or double).",
         {"x": I, "y": I, "button": {"type": "string", "enum": ["left", "right", "double"]},
          "hold_ms": {"type": "integer", "description": "how long the button stays down (default 80)"}}, ("x", "y")),
    tool("vm_move", "Move the mouse to screen x,y.", {"x": I, "y": I}, ("x", "y")),
    tool("vm_drag", "Drag with the left button from x1,y1 to x2,y2.", {"x1": I, "y1": I, "x2": I, "y2": I},
         ("x1", "y1", "x2", "y2")),
    tool("vm_key", "Press a key or combination, e.g. enter, esc, alt+f4, ctrl+s, f5, a.",
         {"keys": S, "mode": {"type": "string", "enum": ["press", "down", "up"]}}, ("keys",)),
    tool("vm_type", "Type text with the keyboard into whatever has focus.", {"text": S}, ("text",)),
    tool("vm_windows", "List the top-level windows (hwnd pid x,y,w,h class title).", {"all": {"type": "boolean"}}),
    tool("vm_window", "Bring a window to the front, close it, or get its rectangles (window and client area).",
         {"action": {"type": "string", "enum": ["front", "close", "rect"]}, "window": S}, ("action", "window")),
    tool("vm_run", "Start a program (its folder becomes the working folder). Returns the pid.",
         {"command": S}, ("command",)),
    tool("vm_ps", "List running processes (pid exe)."),
    tool("vm_kill", "End a process by pid.", {"pid": I}, ("pid",)),
    tool("vm_put", "Copy a file from this PC to the machine.", {"local": S, "remote": S}, ("local", "remote")),
    tool("vm_get", "Copy a file from the machine to this PC.", {"remote": S, "local": S}, ("remote", "local")),
    tool("vm_ls", "List files matching a pattern on the machine, e.g. C:\\GAMES\\*.*", {"pattern": S}, ("pattern",)),
]


def screenshot_content(bmp):
    try:
        from PIL import Image
        im = Image.open(io.BytesIO(bmp))
        scaled = im.width > MAX_WIDTH
        if scaled:
            im = im.resize((MAX_WIDTH, im.height * MAX_WIDTH // im.width))
        out = io.BytesIO()
        im.save(out, "PNG")
        return {"type": "image", "data": base64.b64encode(out.getvalue()).decode(), "mimeType": "image/png"}, (im.width if scaled else None)
    except ImportError:
        return {"type": "image", "data": base64.b64encode(bmp).decode(), "mimeType": "image/bmp"}, None


def call_tool(name, a):
    ag = a.get("agent")
    if name == "vm_agents":
        rep = bridge(op="agents")
        return text("\n".join(f"{x['name']}  {x['addr']}  {x['info']}" for x in rep.get("agents", [])) or "no agents")
    if name == "vm_screenshot":
        if a.get("window"):
            cmd = f"shot {a['window']}"
        elif "w" in a:
            cmd = f"shot {a.get('x', 0)} {a.get('y', 0)} {a['w']} {a.get('h', 0)}"
        else:
            cmd = "shot"
        rep = bridge(cmd, agent=ag)
        if not rep["ok"]:
            return text(rep["header"], True)
        img, w = screenshot_content(base64.b64decode(rep["data"]))
        note = rep["header"] + (f" (sent scaled to {w} px wide)" if w else "")
        return {"content": [img, {"type": "text", "text": note}], "isError": False}
    if name == "vm_click":
        b = a.get("button", "left")
        verb = {"left": "click", "right": "rclick", "double": "dclick"}[b]
        cmd = f"{verb} {a['x']} {a['y']}" + (f" {a['hold_ms']}" if a.get("hold_ms") else "")
    elif name == "vm_move":
        cmd = f"move {a['x']} {a['y']}"
    elif name == "vm_drag":
        cmd = f"drag {a['x1']} {a['y1']} {a['x2']} {a['y2']}"
    elif name == "vm_key":
        cmd = f"key {a['keys']}" + (f" {a['mode']}" if a.get("mode") and a["mode"] != "press" else "")
    elif name == "vm_type":
        cmd = f"type {a['text']}"
    elif name == "vm_windows":
        cmd = "windows all" if a.get("all") else "windows"
    elif name == "vm_window":
        cmd = f"{a['action']} {a['window']}"
    elif name == "vm_run":
        cmd = f"run {a['command']}"
    elif name == "vm_ps":
        cmd = "ps"
    elif name == "vm_kill":
        cmd = f"kill {a['pid']}"
    elif name == "vm_put":
        try:
            data = open(a["local"], "rb").read()
        except OSError as e:
            return text(f"err {e}", True)
        rep = bridge(f"put {a['remote']}", data, agent=ag)
        return text(rep["header"], not rep["ok"])
    elif name == "vm_get":
        rep = bridge(f"get {a['remote']}", agent=ag)
        if rep["ok"]:
            open(a["local"], "wb").write(base64.b64decode(rep["data"]))
        return text(rep["header"] + (f" -> {a['local']}" if rep["ok"] else ""), not rep["ok"])
    elif name == "vm_ls":
        cmd = f"ls {a['pattern']}"
    elif name == "vm_info":
        cmd = "info"
    else:
        return text(f"unknown tool {name}", True)
    rep = bridge(cmd, agent=ag)
    body = base64.b64decode(rep.get("data", "")).decode("latin-1")
    return text(rep["header"] + ("\n" + body if body else ""), not rep["ok"])


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        mid, method = msg.get("id"), msg.get("method")
        if mid is None:          # a notification (e.g. notifications/initialized): nothing to answer
            continue
        if method == "initialize":
            result = {"protocolVersion": msg.get("params", {}).get("protocolVersion", "2024-11-05"),
                      "capabilities": {"tools": {}},
                      "serverInfo": {"name": "okagent", "version": "0.1"}}
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "tools/call":
            p = msg.get("params", {})
            try:
                result = call_tool(p.get("name"), p.get("arguments") or {})
            except Exception as e:
                result = text(f"err {type(e).__name__}: {e}", True)
        elif method == "ping":
            result = {}
        else:
            sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": mid,
                                         "error": {"code": -32601, "message": f"no method {method}"}}) + "\n")
            sys.stdout.flush()
            continue
        sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": mid, "result": result}) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
