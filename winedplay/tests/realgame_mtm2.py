#!/usr/bin/env python3
"""Monster Truck Madness 2 multiplayer between Windows 98 machines running okagent, on the drop-in's
LAN Play (or Online Play), through the game's own menus - MTM2 lists the connections itself, so no
test lobby: real clicks on Driver Check-in > Multiplayer > Connect > Host / Join.

    python realgame_mtm2.py --host AGENT --join AGENT [--sp lan|online] [--race 60]

Each machine needs the drop-in (dplayx.dll + okplay.ini) in its MTM2 folder. The race starts with
Ready on both; the joiner then drives forward (Up arrow) so the captures show the two in sync.
Captures and both session logs go to the output folder. The screen is 640x480 (MTM2's front end).
"""
import argparse, os, sys, time

from realgame_vms import vm, say

HERE = os.path.dirname(os.path.abspath(__file__))
MTM2 = r"C:\Program Files\Microsoft Games\Monster Truck Madness 2"
# front-end positions (640x480)
CHECKIN, MULTI, CONNECT, HOST, JOIN, READY = (550, 445), (430, 448), (474, 219), (253, 214), (402, 214), (565, 447)
HOST_SP = {"online": (155, 222), "lan": (155, 236)}     # the protocol list in "Host a Network Game"
HOST_OK = (478, 347)
JOIN_SP = {"online": (128, 149), "lan": (128, 163)}     # "Join a Network Game": protocol, first session, Ok
JOIN_SESSION, JOIN_OK = (140, 260), (525, 379)


def click(agent, xy, wait):
    r = vm(agent, "click", str(xy[0]), str(xy[1]))
    time.sleep(wait)
    return r


def game_pid(agent):
    for line in vm(agent, "ps").splitlines():
        f = line.split("\t")
        if len(f) >= 2 and f[-1].lower().endswith("monster.exe"):
            return f[0]
    return None


def newest_log(agent, dest):
    listing = vm(agent, "ls", MTM2 + "\\netplay-logs\\*")
    files = sorted(l.split("\t")[2] for l in listing.splitlines() if l.startswith("file\t") and l.endswith(".log"))
    if files:
        vm(agent, "get", MTM2 + "\\netplay-logs\\" + files[-1], dest)
        return files[-1]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--join", required=True)
    ap.add_argument("--sp", default="lan", choices=("lan", "online"))
    ap.add_argument("--race", type=int, default=60)
    a = ap.parse_args()
    out = os.path.join(HERE, "..", "test-logs", time.strftime("realgame-mtm2-%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    fail, started = [], []
    try:
        for m, role in ((a.host, "host"), (a.join, "join")):
            vm(m, "setenv", "OKP_TEST_HOST_PASSWORD=<none>")   # (the drop-in's host password box)
            say(f"{m}: MTM2 ({vm(m, 'run', chr(34) + MTM2 + chr(92) + 'Monster.EXE' + chr(34))})")
            started.append(m)
            time.sleep(40)
            click(m, CHECKIN, 6)
            click(m, MULTI, 5)
            click(m, CONNECT, 4)
            if role == "host":
                click(m, HOST, 4)
                click(m, HOST_SP[a.sp], 1)
                click(m, HOST_OK, 8)
            else:
                click(m, JOIN, 5)
                click(m, JOIN_SP[a.sp], 8)
                vm(m, "shot", os.path.join(out, "join-sessions.png"))
                click(m, JOIN_SESSION, 1)
                click(m, JOIN_OK, 12)
            vm(m, "shot", os.path.join(out, f"{role}-room.png"))
            say(f"{m}: {role} done")
        click(a.join, READY, 3)
        click(a.host, READY, 45)
        for m in (a.host, a.join):
            if not game_pid(m):
                raise RuntimeError(f"{m}: the game is gone")
            vm(m, "shot", os.path.join(out, f"{'host' if m == a.host else 'join'}-race-start.png"))
        time.sleep(15)   # (past the start countdown, or the keys are lost)
        say("race on; the joiner drives")
        for _ in range(25):
            vm(a.join, "key", "up")
        time.sleep(max(0, a.race - 30))
        for m in (a.host, a.join):
            if not game_pid(m):
                raise RuntimeError(f"{m}: the game is gone")
            vm(m, "shot", os.path.join(out, f"{'host' if m == a.host else 'join'}-race-end.png"))
        say("captured: compare the joiner's position on both screens (host-race-end / join-race-end)")
    except Exception as e:
        fail.append(str(e))
    finally:
        for m in started:
            pid = game_pid(m)
            if pid:
                vm(m, "kill", pid)
            vm(m, "setenv", "OKP_TEST_HOST_PASSWORD=")
        time.sleep(2)
        for m in started:
            fn = newest_log(m, os.path.join(out, f"{'host' if m == a.host else 'join'}-okplay.log"))
            say(f"{m}: log {fn}")
        print(f"logs and captures in {os.path.abspath(out)}")
    for f in fail:
        print(f"  - {f}")
    print("RESULT FAIL" if fail else "RESULT PASS (check the captures)")
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
