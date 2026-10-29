#!/usr/bin/env python3
"""vmctl - the host side of okagent (okagent.c): control old Windows machines for test automation.

    python vmctl.py serve [--bind 127.0.0.1] [--port 7790] [--control 7791]
    python vmctl.py agents
    python vmctl.py [--agent NAME] info | ping
    python vmctl.py [--agent NAME] shot OUT.png [x y w h | WINDOW]
    python vmctl.py [--agent NAME] click X Y [HOLD_MS] | rclick X Y | dclick X Y | move X Y | drag X1 Y1 X2 Y2
    python vmctl.py [--agent NAME] key ctrl+s [down|up] | type TEXT...
    python vmctl.py [--agent NAME] windows [all] | front WINDOW | close WINDOW | rect WINDOW
    python vmctl.py [--agent NAME] run COMMANDLINE... | ps | kill PID
    python vmctl.py [--agent NAME] put LOCAL REMOTE | get REMOTE LOCAL | ls PATTERN | del PATH | mkdir PATH
    python vmctl.py [--agent NAME] raw COMMAND...

`serve` holds the agents' connections (they dial in: a VM in 86Box reaches this PC's loopback as
10.0.2.2) and takes requests from the other subcommands on 127.0.0.1:7791. WINDOW is a window
number from `windows` or part of its title. Agents must present the token in token.txt next to this
file (made on the first `serve`; give it to the agent as okagent.ini token= or its 2nd argument).
Bind to a LAN address only to reach real machines on your network; it is never needed for a VM.
"""
import argparse, base64, io, json, os, secrets, socket, struct, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
TOKEN_FILE = os.path.join(HERE, "token.txt")


def token():
    if not os.path.exists(TOKEN_FILE):
        with open(TOKEN_FILE, "w") as f:
            f.write(secrets.token_hex(12))
    with open(TOKEN_FILE) as f:
        return f.read().strip()


# ---------------------------------------------------------------- framing (same as okagent.c)
def recv_exact(s, n):
    b = bytearray()
    while len(b) < n:
        k = s.recv(n - len(b))
        if not k:
            raise ConnectionError("closed")
        b += k
    return bytes(b)


def send_frame(s, header, data=b""):
    body = header.encode("latin-1") + b"\n" + data
    s.sendall(struct.pack("<I", len(body)) + body)


def recv_frame(s):
    n = struct.unpack("<I", recv_exact(s, 4))[0]
    body = recv_exact(s, n)
    nl = body.find(b"\n")
    if nl < 0:
        return body.decode("latin-1"), b""
    return body[:nl].decode("latin-1").rstrip("\r"), body[nl + 1:]


# ---------------------------------------------------------------- serve
class Agent:
    def __init__(self, sock, hello, addr):
        self.sock, self.addr, self.lock = sock, addr, threading.Lock()
        fields = dict(f.split("=", 1) for f in hello.split()[2:] if "=" in f)
        self.name = fields.get("name", addr[0])
        self.info = " ".join(hello.split()[2:])   # (without the token)

    def request(self, cmd, data=b"", timeout=120):
        with self.lock:
            self.sock.settimeout(timeout)
            send_frame(self.sock, cmd, data)
            return recv_frame(self.sock)


agents = {}
agents_lock = threading.Lock()


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def accept_agents(srv, tok):
    while True:
        s, addr = srv.accept()
        try:
            s.settimeout(10)
            hello, _ = recv_frame(s)
            parts = hello.split()
            if len(parts) < 2 or parts[0] != "hello" or not secrets.compare_digest(parts[1], tok):
                log(f"refused {addr[0]}: wrong token or not an agent")
                s.close()
                continue
            a = Agent(s, hello, addr)
            with agents_lock:
                old = agents.get(a.name)
                agents[a.name] = a
            if old:
                old.sock.close()
            log(f"agent {a.name} connected from {addr[0]}: {hello.split(' ', 2)[2] if len(parts) > 2 else ''}")
        except Exception as e:
            log(f"agent from {addr[0]} failed: {e}")
            s.close()


def pick(name):
    with agents_lock:
        if name:
            return agents.get(name)
        return next(iter(agents.values()), None) if len(agents) == 1 else None


def control_client(c):
    f = c.makefile("rwb")
    for line in f:
        try:
            req = json.loads(line)
            if req.get("op") == "agents":
                with agents_lock:
                    current = list(agents.values())
                alive = []
                for a in current:   # a quick ping drops agents that went away without a word
                    try:
                        a.request("ping", timeout=15)
                        alive.append(a)
                    except Exception:
                        with agents_lock:
                            if agents.get(a.name) is a:
                                del agents[a.name]
                        a.sock.close()
                rep = {"ok": True, "agents": [{"name": a.name, "addr": a.addr[0], "info": a.info} for a in alive]}
            else:
                a = pick(req.get("agent"))
                if not a:
                    with agents_lock:
                        names = list(agents)
                    rep = {"ok": False, "header": "err no agent" + (f" named {req.get('agent')}" if req.get("agent")
                           else "" if not names else f" chosen - connected: {', '.join(names)} (use --agent)")}
                else:
                    try:
                        hdr, data = a.request(req["cmd"], base64.b64decode(req.get("data", "")),
                                              req.get("timeout", 120))
                        rep = {"ok": hdr.startswith("ok"), "header": hdr, "data": base64.b64encode(data).decode()}
                    except Exception as e:
                        with agents_lock:
                            if agents.get(a.name) is a:
                                del agents[a.name]
                        a.sock.close()
                        rep = {"ok": False, "header": f"err agent {a.name} dropped: {e}"}
        except Exception as e:
            rep = {"ok": False, "header": f"err bad request: {e}"}
        f.write((json.dumps(rep) + "\n").encode())
        f.flush()
    c.close()


def serve(args):
    tok = token()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(8)
    ctl = socket.socket()
    ctl.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ctl.bind(("127.0.0.1", args.control))
    ctl.listen(8)
    log(f"waiting for agents on {args.bind}:{args.port} (token in {TOKEN_FILE}); requests on 127.0.0.1:{args.control}")
    threading.Thread(target=accept_agents, args=(srv, tok), daemon=True).start()
    while True:
        c, _ = ctl.accept()
        threading.Thread(target=control_client, args=(c,), daemon=True).start()


# ---------------------------------------------------------------- client side
def call(args, cmd=None, data=b"", op=None, timeout=120):
    try:
        c = socket.create_connection(("127.0.0.1", args.control), timeout=timeout + 10)
    except OSError:
        sys.exit("vmctl: no bridge running - start it with: python vmctl.py serve")
    req = {"op": op, "agent": args.agent, "cmd": cmd, "data": base64.b64encode(data).decode(), "timeout": timeout}
    c.sendall((json.dumps(req) + "\n").encode())
    rep = json.loads(c.makefile("rb").readline())
    c.close()
    return rep


def bmp_to_png(bmp, path):
    try:
        from PIL import Image
        Image.open(io.BytesIO(bmp)).save(path)
    except ImportError:   # no Pillow: keep the .bmp
        path = os.path.splitext(path)[0] + ".bmp"
        open(path, "wb").write(bmp)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--agent", help="which agent (its computer name), when more than one is connected")
    ap.add_argument("--control", type=int, default=7791)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7790)
    ap.add_argument("cmd")
    ap.add_argument("rest", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    c, r = args.cmd, args.rest

    if c == "serve":
        return serve(args)
    if c == "token":
        return print(token())
    if c == "agents":
        rep = call(args, op="agents")
        for a in rep["agents"]:
            print(f"{a['name']}\t{a['addr']}\t{a['info']}")
        return
    data, out = b"", None
    if c == "shot":
        if not r:
            sys.exit("usage: shot OUT.png [x y w h | WINDOW]")
        out, cmd = r[0], "shot " + " ".join(r[1:])
    elif c == "put":
        if len(r) != 2:
            sys.exit("usage: put LOCAL REMOTE")
        data, cmd = open(r[0], "rb").read(), "put " + r[1]
    elif c == "get":
        if len(r) != 2:
            sys.exit("usage: get REMOTE LOCAL")
        out, cmd = r[1], "get " + r[0]
    elif c == "raw":
        cmd = " ".join(r)
    else:
        cmd = " ".join([c] + r)
    rep = call(args, cmd.strip(), data)
    payload = base64.b64decode(rep.get("data", ""))
    if not rep["ok"]:
        print(rep["header"], file=sys.stderr)
        sys.exit(1)
    if c == "shot":
        print(rep["header"], "->", bmp_to_png(payload, out))
    elif c == "get":
        open(out, "wb").write(payload)
        print(rep["header"], "->", out)
    else:
        print(rep["header"])
        if payload:
            sys.stdout.write(payload.decode("latin-1"))


if __name__ == "__main__":
    main()
