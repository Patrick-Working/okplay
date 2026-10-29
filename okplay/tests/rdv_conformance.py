"""Conformance test for the okplay rendezvous API (okplay.php / rdv_standin.py).

    python tests/rdv_conformance.py [URL]
    default URL: http://127.0.0.1:8787/okplay

Exercises announce -> list -> knock -> announce(returns knock) -> ownership check
-> close -> list, exactly as okplay.dll uses them. Exit code 0 = conformant.
"""
import json
import os
import random
import sys
import urllib.parse
import urllib.request

URL = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8787/okplay"
KEY = os.environ.get("OKP_KEY", "okp_7Qm2xV9rLs4Hd8Wn")
fails = 0


def post(**f):
    data = urllib.parse.urlencode({"k": KEY, **f}).encode()
    req = urllib.request.Request(URL, data=data, headers={"User-Agent": "okplay-conformance"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def check(name, cond, detail=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else f"   -> {detail!r}"))
    if not cond:
        fails += 1


room = "%08x" % (random.getrandbits(32) | 1)
peer = "%016x" % random.getrandbits(64)
tok = "%016x" % random.getrandbits(64)
joiner = "%016x" % random.getrandbits(64)
enum_b64 = "cGxheQEADgBQAAAARAAAAA+/="          # has + / = which must survive form encoding
cands = "1.2.3.4:5000,[2001:db8::1]:5000"

s, t = post(a="announce", room=room, peer=peer, tok=tok, cands=cands, enum=enum_b64,
            name="Ann's Game #1", players=1, maxp=8, flags=68, ver="0.2.0", wire=2)
check("announce -> 200 ok", s == 200 and t.startswith("ok "), (s, t))

s, t = post(a="list")
latest = next((l for l in t.splitlines() if l.startswith("latest ")), "")
lp = latest.split(" ")
check("list: 'latest <version> <note>' line (update notice)",
      len(lp) == 3 and all(x.isdigit() for x in lp[1].split(".")) and " " not in lp[2] and lp[2] != "", latest)
line = next((l for l in t.splitlines() if l.startswith(f"room {room} ")), "")
parts = line.split(" ")
check("list shows the room", bool(line), t)
check("list: host peer", len(parts) >= 7 and parts[2] == peer, line)
check("list: candidates round-trip", len(parts) >= 7 and parts[4] == cands, line)
check("list: enum blob round-trip (+/= intact)", len(parts) >= 7 and parts[5] == enum_b64, line)
check("list: name url-encoded, no spaces",
      len(parts) >= 7 and urllib.parse.unquote(parts[6]) == "Ann's Game #1", line)

check("list: protocol version field (7th) = 2 (0.8.1+ servers add build and exe after it)",
      len(parts) in (8, 10) and parts[7] == "2", line)

old_room = "%08x" % (random.getrandbits(32) | 1)
post(a="announce", room=old_room, peer=peer, tok=tok, cands=cands, enum=enum_b64, name="old", players=1, maxp=8,
     flags=0, ver="0.1.0")
s, t = post(a="list")
old_line = next((l for l in t.splitlines() if l.startswith(f"room {old_room} ")), "")
check("a 0.1 host (no wire field) is listed as protocol 1",
      len(old_line.split(" ")) >= 8 and old_line.split(" ")[7] == "1", old_line)
post(a="close", room=old_room, tok=tok)

s, t = post(a="knock", room=room, peer=joiner, cands="5.6.7.8:6000")
check("knock -> 200 ok", s == 200 and t.startswith("ok "), (s, t))

s, t = post(a="announce", room=room, peer=peer, tok=tok, cands=cands, enum=enum_b64, name="x",
            players=2, maxp=8, flags=68, ver="9.9.9")
check("announce returns the knock", f"knock {joiner} 5.6.7.8:6000" in t, t)
s, t = post(a="announce", room=room, peer=peer, tok=tok, cands=cands, enum=enum_b64, name="x",
            players=2, maxp=8, flags=68, ver="9.9.9")
check("knock delivered only once", "knock" not in t, t)

s, t = post(a="announce", room=room, peer=peer, tok="%016x" % random.getrandbits(64), cands=cands,
            enum=enum_b64, name="hijack", players=1, maxp=8, flags=0)
check("other token cannot take over the room", s == 403, (s, t))

s, t = post(a="knock", room="%08x" % (random.getrandbits(32) | 1), peer=joiner, cands="-")
check("knock unknown room -> 404", s == 404, (s, t))

old_key, KEY = KEY, "wrong"
s, t = post(a="list")
KEY = old_key
check("bad key -> 403", s == 403, (s, t))

# the web page's public view: GET, no key, and never any addresses / tokens / peer ids
req = urllib.request.Request(URL + ("&" if "?" in URL else "?") + "a=list&fmt=json",
                             headers={"User-Agent": "okplay-conformance"})
try:
    with urllib.request.urlopen(req, timeout=10) as r:
        js_status, js = r.status, r.read().decode()
except urllib.error.HTTPError as e:
    js_status, js = e.code, e.read().decode()
check("public json list works without the key", js_status == 200 and '"rooms"' in js, (js_status, js[:200]))
try:
    mine = [x for x in json.loads(js)["rooms"] if x.get("name") == "x"]
except Exception:
    mine = []
check("public json shows the room (2/8 players, version)",
      any(x.get("players") == 2 and x.get("maxp") == 8 and x.get("ver") == "9.9.9" for x in mine), js[:300])
check("public json leaks no addresses, tokens or peer ids",
      "1.2.3.4" not in js and "2605:" not in js and tok not in js and peer not in js, js[:300])


def public_rooms():
    with urllib.request.urlopen(req, timeout=10) as r:
        return [x for x in json.loads(r.read().decode())["rooms"] if x.get("name") == "x"]


# the lock on the web page: "pw" follows DPSESSION_PASSWORDREQUIRED (0x400) in the host's flags
check("public json: an open game is not marked as a password game",
      bool(mine) and all(x.get("pw") is False for x in mine), mine)
post(a="announce", room=room, peer=peer, tok=tok, cands=cands, enum=enum_b64, name="x",
     players=2, maxp=8, flags=68 | 0x400, ver="9.9.9")
pw_rooms = public_rooms()
check("public json: a password game is marked (pw)", bool(pw_rooms) and all(x.get("pw") is True for x in pw_rooms),
      pw_rooms)

s, t = post(a="close", room=room, tok=tok)
check("close -> ok", s == 200 and t.startswith("ok"), (s, t))
s, t = post(a="list")
check("room gone after close", f"room {room} " not in t, t)

# --- the store next to the script holds tokens and addresses: it must not be fetchable (0.7.0's
# okplay_rooms.json was; 0.7.1 keeps it in okplay_rooms.php, which answers 404 and removes the old file)
base = URL.split("?")[0].rsplit("/", 1)[0]
for fname in ("okplay_rooms.json", "okplay_rooms.php"):
    try:
        with urllib.request.urlopen(urllib.request.Request(base + "/" + fname,
                                    headers={"User-Agent": "okplay-conformance"}), timeout=10) as r:
            st, body = r.status, r.read(120).decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        st, body = e.code, ""
    except Exception as e:
        st, body = 0, str(e)
    check(f"the room store is not readable over the web ({fname})", st != 200, (st, body))

# --- value formats end at the value: a trailing newline is not a token
s, t = post(a="announce", room=room, peer=peer, tok="deadbeef\n", cands=cands, enum=enum_b64, name="nl",
            players=1, maxp=8, flags=0)
check("a token with a trailing newline is refused", s == 400, (s, t))
post(a="close", room=room, tok="deadbeef\n")   # (an older server accepted it: tidy up)

# --- one client address can't fill the list: a few rooms, then 429
cap_rooms, got429, tries = [], False, 0
while tries < 6 and not got429:
    tries += 1
    rid = "%08x" % (random.getrandbits(32) | 1)
    s, t = post(a="announce", room=rid, peer=peer, tok=tok, cands=cands, enum=enum_b64, name="cap",
                players=1, maxp=8, flags=0, ver="9.9.9", wire=2)
    if s == 200:
        cap_rooms.append(rid)
    elif s == 429:
        got429 = True
    else:
        break
check("one address can't fill the list (429 after at most 4 rooms)", got429 and 1 <= len(cap_rooms) <= 4,
      (s, t, len(cap_rooms)))
for rid in cap_rooms:
    post(a="close", room=rid, tok=tok)

# --- knocks: a repeat from the same joiner replaces its earlier one; at most 16 wait per room
room2 = "%08x" % (random.getrandbits(32) | 1)
s, t = post(a="announce", room=room2, peer=peer, tok=tok, cands=cands, enum=enum_b64, name="knocks",
            players=1, maxp=8, flags=0, ver="9.9.9", wire=2)
check("announce (knock tests) -> 200", s == 200, (s, t))
post(a="knock", room=room2, peer=joiner, cands="5.6.7.8:6000")
post(a="knock", room=room2, peer=joiner, cands="5.6.7.8:6001")
s, t = post(a="announce", room=room2, peer=peer, tok=tok, cands=cands, enum=enum_b64, name="knocks",
            players=1, maxp=8, flags=0, ver="9.9.9", wire=2)
mine_knocks = [l for l in t.splitlines() if l.startswith(f"knock {joiner} ")]
check("a repeat knock from the same joiner replaces the earlier one",
      len(mine_knocks) == 1 and mine_knocks[0].endswith(" 5.6.7.8:6001"), t)
codes = [post(a="knock", room=room2, peer="%016x" % random.getrandbits(64), cands="-")[0] for _ in range(16)]
s, t = post(a="knock", room=room2, peer="%016x" % random.getrandbits(64), cands="-")
check("at most 16 joiners wait per room (the 17th gets 429)", codes == [200] * 16 and s == 429, (codes, s, t))
s, t = post(a="announce", room=room2, peer=peer, tok=tok, cands=cands, enum=enum_b64, name="knocks",
            players=1, maxp=8, flags=0, ver="9.9.9", wire=2)
check("the host still collects all 16", sum(1 for l in t.splitlines() if l.startswith("knock ")) == 16, t)
post(a="close", room=room2, tok=tok)

print(f"{'CONFORMANT' if fails == 0 else 'NOT CONFORMANT'}: {fails} failure(s) against {URL}")
sys.exit(1 if fails else 0)
