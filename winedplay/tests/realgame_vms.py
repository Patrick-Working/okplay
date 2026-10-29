#!/usr/bin/env python3
"""AoE2 games between machines running okagent (Windows 98 VMs in 86Box, old PCs): one hosts, the
others join, over Online Play through the local room-server stand-in (okplay/server/rdv_standin.py on
port 8787; VMs reach it as 10.0.2.2). The drop-in's test lobby (OKP_TEST_LOBBY) takes every copy into
the room with no menus; Ready / Start are real clicks inside the machines (okagent). Progress is read
from the stand-in's public room list (players, state).

    python realgame_vms.py --host AGENT --join AGENT[,AGENT...] [--play 120] [--out DIR]
                           [--fake-build AGENT[,...] [--build-answer join|cancel]] [--sp online|lan]
                           [--blackout AGENT:AT:SECS] [--kill AGENT:AT] [--continue-at AT] [--expect-players N]

--fake-build: those machines pretend to run another build of the game (OKP_TEST_EXEBUILD), so joining
shows the real "different version" box inside the machine; --build-answer presses Join anyway (Enter)
or Cancel (Esc) in it. With cancel the machine must stay out of the room and its game keep running.

--blackout AGENT:AT:SECS: that machine's network goes dead both ways AT seconds into the match, for
SECS seconds (OKP_BLACKOUT_FILE: okplay checks for a file the script creates and deletes - the VMs'
clocks drift, so no wall-clock window). --kill AGENT:AT ends that machine's game AT seconds in.
--continue-at AT: AT seconds in, vote "Continue Game" in AoE2's Multiplayer Status box (it pauses the
match when a player drops) on every machine still playing, except a blacked-out one.
--expect-players N: the room must list N players at the end (Online Play).
--look-every N: captures and the room every N seconds (long runs).

--sp lan: LAN Play instead (rooms found by broadcast, no room server). Joins are read from the
joiners' own session logs ("lobby Connect: join ... -> 0x00000000").

Each machine needs the drop-in in its game folder with server=http://10.0.2.2:8787/okplay, and the
same AoE2 build. Captures and the machines' newest session logs go to the output folder; at the end
each machine's peer links (address, reliable lane give-ups and unacked messages) are summarised.
"""
import argparse, json, os, re, subprocess, sys, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
VMCTL = os.path.join(HERE, "..", "..", "okagent", "vmctl.py")
GAME = r"C:\Program Files\Microsoft Games\Age of Empires II\age2_x1"
ROOMS = "http://127.0.0.1:8787/okplay?a=list&fmt=json"
FAKE_BUILD = "0123456789abcdef"
TEST_VARS = ("OKP_TEST_LOBBY", "OKP_TEST_LOBBY_SP", "OKP_TEST_LOBBY_PLAYER", "OKP_TEST_LOBBY_MAX",
             "OKP_TEST_HOST_PASSWORD", "OKP_TEST_BUILD_ANSWER", "OKP_TEST_EXEBUILD", "OKP_BLACKOUT_FILE")
BLACKOUT_FLAG = r"C:\OKAGENT\BLACKOUT.FLG"
T0 = time.time()


def say(msg):
    print(f"{time.time() - T0:6.1f}s {msg}", flush=True)


def vm(agent, *args, timeout=120):
    r = subprocess.run([sys.executable, VMCTL, "--agent", agent, *args], capture_output=True, text=True,
                       timeout=timeout)
    return (r.stdout + r.stderr).strip()


def room(name):
    try:
        data = json.load(urllib.request.urlopen(ROOMS, timeout=5))
    except OSError:
        return None
    return next((r for r in data.get("rooms", []) if r.get("name") == name), None)


def wait_for(what, cond, secs):
    t = time.time()
    while time.time() - t < secs:
        v = cond()
        if v:
            return v
        time.sleep(2)
    raise RuntimeError(f"timed out waiting for {what}")


def newest_log(agent, dest=None):
    """The machine's newest okplay session log: (text, file name)."""
    listing = vm(agent, "ls", GAME + "\\netplay-logs\\okplay-*.log")
    files = sorted(l.split("\t")[2] for l in listing.splitlines() if l.startswith("file\t"))
    if not files:
        return "", None
    dest = dest or os.path.join(os.environ.get("TEMP", "."), f"okp-{agent}.log")
    vm(agent, "get", GAME + "\\netplay-logs\\" + files[-1], dest)
    try:
        return open(dest, encoding="latin-1").read(), files[-1]
    except OSError:
        return "", files[-1]


def join_result(agent):
    """The joiner's lobby Connect result: None (not yet), else (joined?, hresult)."""
    m = re.findall(r"lobby Connect: join \S+ -> 0x([0-9a-f]{8})", newest_log(agent)[0])
    return (m[-1] == "00000000", m[-1]) if m else None


def link_summary(text):
    """Per peer: {peer: (path, gave_up, unacked)} - from its last stats line, gave-ups counted from
    the log (a peer that is removed stops appearing in the stats; its unacked is then "gone")."""
    peers, last_round = {}, None
    for m in re.finditer(r"^(\S+) \[\d+\] I stats peer (\w+)\b(.*)", text, re.M):
        path = re.search(r"path ([0-9.]+:\d+)", m.group(3))
        rel = re.search(r"unacked (\d+) gave-up (\d+)", m.group(3))
        if path and rel:
            peers[m.group(2)] = [path.group(1), int(rel.group(2)), int(rel.group(1)), m.group(1)]
            last_round = m.group(1)[:8]   # (stats lines of one round share the second)
    for p in peers.values():
        if p[3][:8] != last_round:
            p[2] = "gone"
    for peer in peers:
        peers[peer][1] = max(peers[peer][1], len(re.findall(rf"peer {peer}: gave up on reliable", text)))
    return {k: tuple(v[:3]) for k, v in peers.items()}


def start_game(agent, role, session, player, maxp, sp, fake=False, blackout=False):
    vm(agent, "del", BLACKOUT_FLAG)
    for e in (f"OKP_BLACKOUT_FILE={BLACKOUT_FLAG}" if blackout else "OKP_BLACKOUT_FILE=",
              f"OKP_TEST_LOBBY={role}:{session}", f"OKP_TEST_LOBBY_SP={sp}", f"OKP_TEST_LOBBY_PLAYER={player}",
              f"OKP_TEST_LOBBY_MAX={maxp}", "OKP_TEST_HOST_PASSWORD=<none>",
              # a fake build gets the real box (answered below), everyone else the automatic answer
              f"OKP_TEST_EXEBUILD={FAKE_BUILD}" if fake else "OKP_TEST_EXEBUILD=",
              "OKP_TEST_BUILD_ANSWER=" if fake else "OKP_TEST_BUILD_ANSWER=join"):
        vm(agent, "setenv", e)
    r = vm(agent, "run", f'"{GAME}\\age2_x1.exe" Lobby')
    if not r.startswith("ok pid "):
        raise RuntimeError(f"{agent}: could not start AoE2: {r}")
    return r.split()[2]


def game_dialogs(agent, pid):
    """Dialog boxes (#32770) of the game process: (hwnd, title). The box is owned by the game's
    window, so it is only in the full list."""
    out = []
    for line in vm(agent, "windows", "all").splitlines():
        f = line.split("\t")
        if len(f) >= 5 and f[1] == pid and f[3] == "#32770":
            out.append((f[0], f[4]))
    return out


def answer_build_box(agent, pid, answer, out, name):
    """Wait for the "different version" box in the game, capture it, press Enter or Esc."""
    t = time.time()
    while time.time() - t < 90:
        boxes = game_dialogs(agent, pid)
        if boxes:
            time.sleep(2)   # let it draw
            vm(agent, "shot", os.path.join(out, f"{name}-buildbox.png"))
            say(f"{agent}: build box up ({boxes[0][1] or 'no title'})")
            r = vm(agent, "key", "enter" if answer == "join" else "escape")
            say(f"{agent}: answered {answer} ({r})")
            time.sleep(3)
            if game_dialogs(agent, pid):
                say(f"{agent}: the box is still there after the key")
                return False
            return True
        time.sleep(2)
    say(f"{agent}: no build box seen")
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--join", required=True)
    ap.add_argument("--play", type=int, default=120)
    ap.add_argument("--out", default="")
    ap.add_argument("--fake-build", default="")
    ap.add_argument("--build-answer", default="join", choices=("join", "cancel"))
    ap.add_argument("--sp", default="online", choices=("online", "lan"))
    ap.add_argument("--blackout", default="")
    ap.add_argument("--kill", default="")
    ap.add_argument("--continue-at", type=int, default=0)
    ap.add_argument("--look-every", type=int, default=0)
    ap.add_argument("--expect-players", type=int, default=0)
    a = ap.parse_args()
    bo = a.blackout.split(":") if a.blackout else None
    kl = a.kill.split(":") if a.kill else None
    # the match timeline: (seconds in, what, machine)
    events = [(30, "look", None), (a.play, "look", None)]
    if a.look_every:   # long runs: a look (captures, room) every N seconds
        events += [(t, "look", None) for t in range(a.look_every, a.play, a.look_every)]
    if bo:
        events += [(int(bo[1]), "blackout on", bo[0]), (int(bo[1]) + int(bo[2]), "blackout off", bo[0]),
                   (int(bo[1]) + int(bo[2]) - 2, "look", None)]
    if kl:
        events += [(int(kl[1]), "kill", kl[0])]
    if a.continue_at:
        events += [(a.continue_at - 1, "look", None), (a.continue_at, "continue", None)]
    events.sort(key=lambda e: (e[0], e[1] != "look"))
    lan = a.sp == "lan"
    fakes = set(f for f in a.fake_build.split(",") if f)
    joiners = [j for j in a.join.split(",") if j]
    names = {a.host: "Host98"}
    names.update({j: f"Join98{chr(ord('A') + i)}" for i, j in enumerate(joiners)})
    session = f"vms {int(time.time()) % 10000}"
    out = a.out or os.path.join(HERE, "..", "test-logs", time.strftime("realgame-vms-%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    pids, fail, joined = {}, [], []
    print(f"session: {session}   host {a.host}   join {', '.join(joiners)}   {a.sp}")
    try:
        pids[a.host] = start_game(a.host, "host", session, names[a.host], 1 + len(joiners), a.sp,
                                  blackout=bool(bo and bo[0] == a.host))
        if lan:
            time.sleep(20)   # LAN rooms are not on the server: nothing to watch
            say(f"{a.host} hosts (LAN)")
        else:
            r = wait_for("the host's room on the server", lambda: room(session), 120)
            say(f"{a.host} hosts (room listed, state {r.get('state')})")
        for j in joiners:
            pids[j] = start_game(j, "join", session, names[j], 1 + len(joiners), a.sp, j in fakes,
                                 blackout=bool(bo and bo[0] == j))
            if j in fakes:
                if not answer_build_box(j, pids[j], a.build_answer, out, names[j]):
                    fail.append(f"{j}: the build box never appeared")
                    continue
                if a.build_answer == "cancel":
                    time.sleep(20)
                    alive = "age2_x1" in vm(j, "ps").lower()
                    vm(j, "shot", os.path.join(out, f"{names[j]}-after-cancel.png"))
                    res = join_result(j)
                    players = None if lan else (room(session) or {}).get("players")
                    say(f"{j}: after Cancel - join result {res and res[1]}, room {players} players, "
                        f"game {'running' if alive else 'gone'}")
                    if (res and res[0]) or (players is not None and players != 1 + len(joined)):
                        fail.append(f"{j}: in the room after Cancel")
                    if not alive:
                        fail.append(f"{j}: the game closed after Cancel")
                    continue
            joined.append(j)
            n = 1 + len(joined)
            if lan:
                res = wait_for(f"{j}'s join result", lambda: join_result(j), 180)
                if not res[0]:
                    raise RuntimeError(f"{j}: the join failed (0x{res[1]})")
            else:
                wait_for(f"{j} in the room", lambda: (room(session) or {}).get("players", 0) >= n, 180)
            say(f"{j} joined ({n} players)")
        if not joined:
            a.play = 0
        time.sleep(15)
        for ag in pids:
            vm(ag, "shot", os.path.join(out, f"{names[ag]}-room.png"))
        if a.play > 0:
            for j in joined:
                say(f"{j}: I'm Ready! ({vm(j, 'click', '51', '489', '150')})")
                time.sleep(2)
            vm(a.host, "click", "751", "471", "150")    # Record Game off (one folder per machine, but keep runs light)
            time.sleep(1)
            say(f"{a.host}: I'm Ready! ({vm(a.host, 'click', '51', '489', '150')})")
            time.sleep(3)
            say(f"{a.host}: Start Game ({vm(a.host, 'click', '118', '560', '400')})")
            time.sleep(6)
            vm(a.host, "click", "118", "560", "400")
            started = time.time()
            playing = [a.host] + joined
            for at, what, ag in events:
                time.sleep(max(0, at - (time.time() - started)))
                if what == "blackout on":
                    say(f"at {at} s: {ag}: network dead ({vm(ag, 'put', os.devnull, BLACKOUT_FLAG)})")
                    continue
                if what == "blackout off":
                    say(f"at {at} s: {ag}: network back ({vm(ag, 'del', BLACKOUT_FLAG)})")
                    continue
                if what == "continue":
                    for m in playing:
                        if not (bo and m == bo[0]):
                            say(f"at {at} s: {m}: Continue Game ({vm(m, 'click', '444', '369', '150')})")
                    continue
                if what == "kill":
                    say(f"at {at} s: {ag}: game ended ({vm(ag, 'kill', pids[ag])})")
                    playing.remove(ag)
                    continue
                for m in playing:
                    if "age2_x1" not in vm(m, "ps").lower():
                        raise RuntimeError(f"{m}: the game is gone")
                    vm(m, "shot", os.path.join(out, f"{names[m]}-play-{at}s.png"))
                if lan:
                    say(f"at {at} s: all {len(playing)} games running")
                else:
                    r = room(session) or {}
                    say(f"at {at} s: room {r.get('players')} players, state {r.get('state')}")
            r = {} if lan else room(session) or {}
            if not lan and r.get("state") != "playing":
                fail.append("the room never showed as playing on the server")
            if a.expect_players and not lan and r.get("players") != a.expect_players:
                fail.append(f"the room lists {r.get('players')} players at the end, expected {a.expect_players}")
    except Exception as e:
        fail.append(str(e))
    finally:
        for ag, pid in pids.items():
            vm(ag, "kill", pid)
            vm(ag, "del", BLACKOUT_FLAG)
        time.sleep(2)
        for ag in pids:
            text, fn = newest_log(ag)
            if fn:
                with open(os.path.join(out, f"{names[ag]}-{fn}"), "w", encoding="latin-1") as f:
                    f.write(text)
            links = link_summary(text)
            say(f"{ag}: " + ("; ".join(f"{p} gave-up {g} unacked {u}" for p, g, u in links.values())
                             or "no peer links"))
            for e in TEST_VARS:
                vm(ag, "setenv", e + "=")
        print(f"logs and captures in {os.path.abspath(out)}")
    for f in fail:
        print(f"  - {f}")
    print("RESULT FAIL" if fail else "RESULT PASS")
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
