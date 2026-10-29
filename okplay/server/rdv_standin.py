"""Local stand-in for server/okplay.php (same API) so okplay can be tested offline.

    py -3 server/rdv_standin.py [--port 8787] [--key KEY]

POST form fields, plain-text line replies:
  a=announce room peer tok cands enum name players maxp flags -> "ok <ip>" + "knock <peer> <cands>"*
  a=list                                                      -> "ok <ip>" + "room <room> <peer> <age> <cands> <enum> <name>"*
  a=knock room peer cands                                     -> "ok <ip>"
  a=close room tok                                            -> "ok"
  a=report kind ver body   (crash|conn|check)                 -> "ok <ref>"   (0.8.3; kept in memory, and in
                                                                --reports FILE as JSON lines)
  a=reports                (stand-in only, for tests)         -> the reports so far as JSON
okplay.php's usage stats (okstats.php) are not mirrored: nothing in the DLL reads them.
"""
import argparse
import json
import os
import re
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TTL = 15          # seconds a room survives without an announce
KNOCK_TTL = 20    # seconds a knock waits for the host to pick it up
MAX_ROOMS = 200
MAX_ROOMS_PER_IP = 4   # as okplay.php: one address can't fill the list
MAX_KNOCKS = 16        # joiners waiting per room; a repeat from the same joiner replaces its earlier one
TEST_APP = "0e7c4b6193a24f0b8d356a1f2c9b7e58"   # Connection Check's test games (okplay.php TEST_APP)
RE_APP = re.compile(r"[0-9a-f]{32}")
RE_GAMEVER = re.compile(r"[0-9A-Za-z .,_-]{0,24}")
def state_of(r):   # as okplay.php: the host's word, else the session flags (hosts before 0.8.1)
    return r.get("state") or ("playing" if int(r.get("flags", 0)) & 0x21 else "lobby")


KNOWN_APPS = {   # as okplay.php: names for rooms from hosts older than 0.8.1
    "5de93f3ffc904ee1ae5a63dafa055950": "Age of Empires II: The Conquerors",
    "fb69a260503111d3a2d4006097ba6550": "Age of Empires II",
}
# the field formats okplay.php accepts (fullmatch: a trailing newline is not part of a value)
RE_ROOM, RE_PEER, RE_TOK = re.compile(r"[0-9a-f]{8}"), re.compile(r"[0-9a-f]{16}"), re.compile(r"[0-9a-f]{8,32}")

rooms = {}
reports = []
REPORTS_FILE = ""
REPORT_MAX = {"crash": 16384, "conn": 8192, "check": 16384}   # as okplay.php
MAX_REPORTS_PER_DAY = 40
lock = threading.Lock()
# the relay (okplay.php handle_relay): room -> peer -> {"tok", "q": [[from, b64]], "n"}; its own lock,
# so a waiting rpoll never holds up the rest
relay = {}
relay_cv = threading.Condition()
RELAY_MAX_FRAME, RELAY_MAX_BATCH, RELAY_MAX_QUEUE, RELAY_MAX_BYTES, RELAY_MAX_WAIT = 8192, 64, 512, 524288, 2000
RE_B64U = re.compile(r"[A-Za-z0-9_-]*")   # base64url, no padding (what the mailboxes hold)
KEY = "okp_7Qm2xV9rLs4Hd8Wn"
WWW = ""
FAKE_IP = ""   # --fake-client-ip: pretend clients come from this public address (tests)
# the newest drop-in, as okplay.php's LATEST_VERSION / LATEST_NOTE (--latest / --latest-note for tests)
LATEST_VERSION = "0.7.0"
LATEST_NOTE = "Adds Windows 98 support and the combined connection list."


def latest_line():
    note = urllib.parse.quote(LATEST_NOTE, safe="") if LATEST_NOTE else "-"
    return f"latest {LATEST_VERSION} {note}\n"


def prune(now):
    for rid in [r for r, v in rooms.items() if now - v["updated"] > TTL]:
        del rooms[rid]


WEB_WIRE, SIG_MAX_DATA, SIG_MAX_BOX, SIG_MAX_JOIN, SIG_JOINER_TTL, SIG_MSG_TTL = 100, 16384, 64, 32, 60, 60


def signaling(a, f, now):   # (lock held) -> (code, JSON text); same rules as okplay.php
    def err(code, msg):
        return code, json.dumps({"ok": False, "error": msg})
    rid, peer = f.get("room", ""), f.get("peer", "")
    r = rooms.get(rid)
    if not RE_PEER.fullmatch(peer) or not r or int(r.get("wire", 0)) != WEB_WIRE:
        return err(404, "no such room")
    sig = r.setdefault("sig", {"j": {}, "box": {}})
    for jp in [jp for jp, j in sig["j"].items() if now - j["t"] > SIG_JOINER_TTL]:
        del sig["j"][jp]
        sig["box"].pop(jp, None)
    for to in sig["box"]:
        sig["box"][to] = [m for m in sig["box"][to] if now - m["t"] <= SIG_MSG_TTL]
    if a == "sjoin":
        if peer == r["peer"]:
            return err(400, "the host cannot join")
        if peer not in sig["j"] and len(sig["j"]) >= SIG_MAX_JOIN:
            return err(429, "too many joiners connecting, try again")
        jt = sig["j"][peer]["tok"] if peer in sig["j"] else os.urandom(8).hex()
        sig["j"][peer] = {"tok": jt, "t": now}
        return 200, json.dumps({"ok": True, "tok": jt})
    is_host = peer == r["peer"]
    tok = f.get("tok", "")
    known = (r["tok"] == tok) if is_host else (peer in sig["j"] and sig["j"][peer]["tok"] == tok)
    if not known:
        return err(403, "not a member of this room")
    if a == "signal":
        to, data = f.get("to", ""), f.get("data", "")
        if not ((to in sig["j"]) if is_host else (to == r["peer"])):
            return err(403, "no such recipient")
        if not data or len(data) > SIG_MAX_DATA:
            return err(413, "empty or too big")
        if len(sig["box"].get(to, [])) >= SIG_MAX_BOX:
            return err(429, "recipient mailbox full")
        sig["box"].setdefault(to, []).append({"from": peer, "data": data, "t": now})
        return 200, json.dumps({"ok": True})
    msgs = [{"from": m["from"], "data": m["data"]} for m in sig["box"].pop(peer, [])]
    if not is_host:
        sig["j"][peer]["t"] = now
    return 200, json.dumps({"ok": True, "msgs": msgs})


def relay_action(a, f):   # (no lock held) -> (code, JSON text); same rules as okplay.php
    room, peer, ptok = f.get("room", ""), f.get("peer", ""), f.get("ptok", "")
    if not (RE_ROOM.fullmatch(room) and RE_PEER.fullmatch(peer) and re.fullmatch(r"[0-9a-f]{16,32}", ptok)):
        return 400, json.dumps({"ok": False, "error": "missing room/peer/ptok"})

    def me(boxes):   # the caller's own mailbox, or None for someone else's key
        b = boxes.setdefault(peer, {"tok": ptok, "q": [], "n": 0})
        if not b["tok"]:
            b["tok"] = ptok
        return b if b["tok"] == ptok else None

    with relay_cv:
        boxes = relay.setdefault(room, {})
        mine = me(boxes)
        if mine is None:
            return 403, json.dumps({"ok": False, "error": "not your mailbox"})
        if a == "rsend":
            if f.get("f"):   # okweb 1.1.1+: "to.data*to.data" (base64url)
                frames = [one.split(".", 1) for one in f["f"].split("*") if "." in one]
            else:            # okweb 1.1.0: JSON [[to, base64]]
                try:
                    old = json.loads(f.get("frames", "[]"))
                except ValueError:
                    old = None
                if not isinstance(old, list):
                    return 400, json.dumps({"ok": False, "error": "bad frames"})
                frames = [[fr[0], fr[1].replace("+", "-").replace("/", "_").rstrip("=")] for fr in old
                          if isinstance(fr, list) and len(fr) == 2 and all(isinstance(x, str) for x in fr)]
            if len(frames) > RELAY_MAX_BATCH:
                return 400, json.dumps({"ok": False, "error": "bad frames"})
            dropped = 0
            for to, data in frames:
                if not RE_PEER.fullmatch(to) or to == peer or len(data) > RELAY_MAX_FRAME or not RE_B64U.fullmatch(data):
                    continue
                box = boxes.setdefault(to, {"tok": "", "q": [], "n": 0})
                if len(box["q"]) >= RELAY_MAX_QUEUE or box["n"] + len(data) > RELAY_MAX_BYTES:
                    dropped += 1
                    continue
                box["q"].append([peer, data])
                box["n"] += len(data)
            relay_cv.notify_all()
            return 200, json.dumps({"ok": True, "dropped": dropped})
        try:
            wait = max(0, min(RELAY_MAX_WAIT, int(f.get("wait", "0") or 0))) / 1000
        except ValueError:
            wait = 0
        until = time.time() + wait
        while not mine["q"] and time.time() < until:
            relay_cv.wait(until - time.time())
        q, mine["q"], mine["n"] = mine["q"], [], 0
        if f.get("c") == "1":
            return 200, json.dumps({"ok": True, "f": "*".join(fr + "." + d for fr, d in q)})
        return 200, json.dumps({"ok": True, "frames": [[fr, d.replace("-", "+").replace("_", "/") + "=" * (-len(d) % 4)] for fr, d in q]})


def handle(f, ip):
    a = f.get("a", "")
    public = a == "list" and f.get("fmt") == "json"   # web page view: no key, no addresses
    if not public and f.get("k") != KEY:
        return 403, "error bad key\n"
    if a in ("rsend", "rpoll"):
        return relay_action(a, f)
    now = time.time()
    with lock:
        prune(now)
        if a == "report":
            kind = f.get("kind", "")
            if kind not in REPORT_MAX:
                return 400, "error unknown report kind\n"
            body = re.sub(r"[\x00-\x08\x0b-\x1f\x7f]", "", f.get("body", "")[:REPORT_MAX[kind]])
            if not body.strip():
                return 400, "error empty report\n"
            if sum(1 for r in reports if r["ip"] == ip) >= MAX_REPORTS_PER_DAY:
                return 429, "error too many reports today\n"
            ver = f.get("ver", "") if re.fullmatch(r"[0-9.]{1,16}", f.get("ver", "")) else ""
            ref = time.strftime("%m%d", time.gmtime(now)) + "-" + os.urandom(3).hex()   # as okplay.php
            rec = {"t": int(now), "ref": ref, "kind": kind, "ver": ver, "body": body, "ip": ip}
            reports.append(rec)
            if REPORTS_FILE:
                with open(REPORTS_FILE, "a", encoding="utf-8") as out:
                    out.write(json.dumps(rec) + "\n")
            print(f"report {kind} v{ver} {ref}: {body.splitlines()[0][:120]}", flush=True)
            return 200, f"ok {ref}\n"
        if a == "reports":
            return 200, json.dumps(reports)
        if a in ("sjoin", "signal", "poll"):   # browser signaling, as okplay.php (docs/WEB_NETPLAY.md 2)
            return signaling(a, f, now)
        if a == "announce":
            rid, peer, tok = f.get("room", ""), f.get("peer", ""), f.get("tok", "")
            if not (RE_ROOM.fullmatch(rid) and RE_PEER.fullmatch(peer) and RE_TOK.fullmatch(tok)):
                return 400, "error missing room/peer/tok\n"
            r = rooms.get(rid)
            if r and r["tok"] != tok:
                return 403, "error room owned by another host\n"
            if not r:
                if len(rooms) >= MAX_ROOMS:
                    return 503, "error full\n"
                if sum(1 for v in rooms.values() if v.get("ip") == ip) >= MAX_ROOMS_PER_IP:
                    return 429, "error too many games from this address\n"
                r = rooms[rid] = {"tok": tok, "knocks": [], "created": now}
            r.update(peer=f.get("peer", ""), cands=f.get("cands", "") or "-", enum=f.get("enum", ""),
                     name=f.get("name", "") or "-", players=f.get("players", "0"),
                     maxp=f.get("maxp", "0"), flags=f.get("flags", "0"), ver=f.get("ver", ""),
                     updated=now, ip=ip, app=f.get("app", "") if RE_APP.fullmatch(f.get("app", "")) else "",
                     game=re.sub(r"[\x00-\x1f\x7f]", "", f.get("game", ""))[:64],
                     gamever=f.get("gamever", "") if RE_GAMEVER.fullmatch(f.get("gamever", "")) else "",
                     state=f.get("state", "") if f.get("state", "") in ("lobby", "playing", "between") else "",
                     build=f.get("build", "") if re.fullmatch(r"[0-9a-f]{16}", f.get("build", "")) else "",
                     exe=re.sub(r"[\x00-\x1f\x7f\s]", "", f.get("exe", ""))[:48])
            wire = int(f.get("wire", "0") or 0)
            if wire == 0 and r["ver"].startswith("0.1"):
                wire = 1   # 0.1 hosts predate the field
            r["wire"] = wire
            knocks = [k for k in r["knocks"] if now - k[2] <= KNOCK_TTL]
            r["knocks"] = []
            return 200, f"ok {ip}\n" + latest_line() + "".join(f"knock {p} {c}\n" for p, c, _ in knocks)
        if public:
            pub = [{"name": r["name"], "players": int(r["players"]), "maxp": int(r["maxp"]),
                    "started": state_of(r) == "playing", "state": state_of(r),
                    "age": int(now - r.get("created", now)),
                    "pw": bool(int(r["flags"]) & 0x400),   # DPSESSION_PASSWORDREQUIRED
                    "ver": r.get("ver", ""), "wire": r.get("wire", 0),
                    "game": r.get("game") or KNOWN_APPS.get(r.get("app", ""), ""),
                    "gamever": r.get("gamever", ""), "build": r.get("build", ""),
                    "exe": r.get("exe", "")} for r in rooms.values()
                   if r.get("app") != TEST_APP]   # test games stay off the page
            tests = sum(1 for r in rooms.values() if r.get("app") == TEST_APP)
            return 200, json.dumps({"rooms": pub, "tests": tests, "now": int(now), "latest": LATEST_VERSION})
        if a == "list":
            lines = [f"ok {ip}\n", latest_line()]
            want = f.get("app", "") if RE_APP.fullmatch(f.get("app", "")) else ""
            only_tests = f.get("test") == "1" or want == TEST_APP
            for rid, r in rooms.items():
                app = r.get("app", "")
                if (app != TEST_APP) if only_tests else (app == TEST_APP):
                    continue
                if want and not only_tests and app and app != want:
                    continue   # another game's room
                name = urllib.parse.quote(r["name"], safe="")
                build = r.get("build") or "-"   # (as okplay.php: clients before 0.8.1 stop reading at wire)
                exe = urllib.parse.quote(r.get("exe") or "-", safe="")
                lines.append(f"room {rid} {r['peer']} {int(now - r['updated'])} {r['cands']} {r['enum']} {name} "
                             f"{r.get('wire', 0)} {build} {exe}\n")
            return 200, "".join(lines)
        if a == "knock":
            r = rooms.get(f.get("room", ""))
            peer = f.get("peer", "")
            if not r or not RE_PEER.fullmatch(peer):
                return 404, "error no such room\n"
            pending = [k for k in r["knocks"] if now - k[2] <= KNOCK_TTL and k[0] != peer]
            if len(pending) >= MAX_KNOCKS:
                return 429, "error too many joiners waiting, knock again\n"
            r["knocks"] = pending + [(peer, f.get("cands", "") or "-", now)]
            return 200, f"ok {ip}\n" + latest_line()
        if a == "close":
            r = rooms.get(f.get("room", ""))
            if r and r["tok"] == f.get("tok"):
                del rooms[f["room"]]
            return 200, "ok\n"
    return 400, "error unknown action\n"


class H(BaseHTTPRequestHandler):
    def do_GET(self):   # the PHP reads GET too; the web page uses GET for its JSON view
        u = urllib.parse.urlparse(self.path)
        if WWW and not (u.path.endswith("okplay.php") or u.path.endswith("/okplay")):
            return self._static(u.path)
        self._reply(*handle({k: v[0] for k, v in urllib.parse.parse_qs(u.query, keep_blank_values=True).items()},
                            FAKE_IP or self.client_address[0]))

    def _static(self, path):
        # --www: serve the site folder too, so index.html can be tried against a live room list
        name = os.path.basename(urllib.parse.unquote(path)) or "index.html"
        full = os.path.join(WWW, name)
        if not os.path.isfile(full) or name.startswith("okplay_rooms."):   # the store is never served
            return self._reply(404, "not found\n")
        data = open(full, "rb").read()
        ctype = "text/html; charset=utf-8" if name.endswith(".html") else "application/octet-stream"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode("utf-8", "replace")
        f = {k: v[0] for k, v in urllib.parse.parse_qs(body, keep_blank_values=True).items()}
        self._reply(*handle(f, FAKE_IP or self.client_address[0]))

    def _reply(self, code, text):
        data = text.encode()
        self.send_response(code)
        # (as okplay.php: JSON for the browser signaling actions, and CORS for browser games anywhere)
        self.send_header("Content-Type", "application/json" if text.startswith(("{", "[")) else "text/plain; charset=utf-8")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        print(time.strftime("%H:%M:%S"), self.client_address[0], fmt % args, flush=True)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--key", default=KEY)
    ap.add_argument("--www", default="", help="also serve this folder (index.html) like the oldkid.net site")
    ap.add_argument("--fake-client-ip", default="", help="report this as every client's public IP (tests)")
    ap.add_argument("--latest", default=LATEST_VERSION, help="the newest drop-in version to advertise")
    ap.add_argument("--latest-note", default=LATEST_NOTE)
    ap.add_argument("--reports", default="", help="also append reports (a=report) to this file as JSON lines")
    args = ap.parse_args()
    REPORTS_FILE = args.reports
    KEY = args.key
    WWW = args.www
    FAKE_IP = args.fake_client_ip
    LATEST_VERSION = args.latest
    LATEST_NOTE = args.latest_note
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), H)
    print(f"okplay rendezvous stand-in on http://127.0.0.1:{args.port}/okplay", flush=True)
    srv.serve_forever()
