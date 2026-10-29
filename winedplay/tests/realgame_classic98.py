#!/usr/bin/env python3
"""Our DirectPlay against Microsoft's, on the wire, in a real AoE2 match between Windows 98 machines
(okagent): classic TCP/IP (Microsoft's TCP/IP provider, dpwsockx) on both ends.

  * Microsoft's end: the drop-in's dplayx.dll is taken out of that machine's game folder for the run,
    and okplay/bin/dplaunch.exe (a minimal DirectPlay lobby program) starts AoE2 into the session
    through IDirectPlayLobby::RunApplication - pure Microsoft DirectPlay.
  * Our end(s): the drop-in with OKP_MODE=oldkid (no switch to Windows' DirectPlay) and
    show_windows_connections=1 in okplay.ini, and the test lobby on TCP/IP (OKP_TEST_LOBBY_SP=tcpip):
    our DirectPlay drives Microsoft's TCP/IP provider (see --provider).

    python realgame_classic98.py --ms AGENT --ours AGENT[,AGENT...] [--ms-hosts] [--play 120]
                                 [--provider ours|ms]

--provider: the TCP/IP service provider under our DirectPlay - ms (default: Microsoft's dpwsockx, the
system's) or ours (Wine's dpwsockx from winedplay/build_dpwsockx.sh, put in the game folder for the run).
NOTE: on Windows 98 "ours" is not picked up - our DirectPlay loads the provider by the registry's full
path (C:\WINDOWS\SYSTEM\dpwsockx.dll); it stays for when that changes. On Windows 98 our joiner is
NOT refused at CreatePlayer by Microsoft's provider (it is on Windows 10 - DPLAY_CONFORMANCE.md S4).

--ms-hosts: Microsoft's end hosts (default: the first of ours hosts and Microsoft's end joins).
Addresses are the machines' local-switch addresses (192.168.77.x, --addr AGENT=IP to change).
Everything is put back afterwards (dplayx.dll, okplay.ini, environment).
"""
import argparse, os, re, sys, time

import realgame_vms as rv
from realgame_vms import vm, say, wait_for, GAME, TEST_VARS

HERE = os.path.dirname(os.path.abspath(__file__))
DPLAUNCH = os.path.join(HERE, "..", "..", "okplay", "bin", "dplaunch.exe")
DROPIN = os.path.join(HERE, "..", "bin", "dropin", "dplayx.dll")
OUR_SP = os.path.join(HERE, "..", "bin", "dpwsockx", "dpwsockx.dll")
ADDR = {"WIN98A-B5731E": "192.168.77.1", "WIN98B-B5731F": "192.168.77.2", "WIN98C-B57320": "192.168.77.3"}
REMOTE_DPL = r"C:\OKAGENT\DPLAUNCH.EXE"
REMOTE_OUT = r"C:\OKAGENT\DPL%d.TXT" % (int(time.time()) % 100000)   # (fresh each run: a killed run's console host can keep the old one locked)


def game_pid(agent):
    for line in vm(agent, "ps").splitlines():
        f = line.split("\t")
        if len(f) >= 2 and "age2_x1" in f[-1].lower():
            return f[0]
    return None


def ours_log_count(agent, pattern):
    return len(re.findall(pattern, rv.newest_log(agent)[0]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ms", required=True)
    ap.add_argument("--ours", required=True)
    ap.add_argument("--ms-hosts", action="store_true")
    ap.add_argument("--play", type=int, default=120)
    ap.add_argument("--addr", action="append", default=[])
    ap.add_argument("--provider", default="ms", choices=("ms", "ours"))
    ap.add_argument("--debug", action="store_true", help="DPLAYX_DEBUG=+dplay on our machines")
    ap.add_argument("--room-looks", type=int, default=0, help="this many room captures of every machine, 10 s apart (pings)")
    ap.add_argument("--chat", default="", help="the host types this to everyone in the room (a broadcast you can see)")
    a = ap.parse_args()
    for x in a.addr:
        k, v = x.split("=", 1)
        ADDR[k] = v
    ours = [o for o in a.ours.split(",") if o]
    machines = [a.ms] + ours
    host = a.ms if a.ms_hosts else ours[0]
    joiners = [m for m in machines if m != host]
    names = {a.ms: "MS98"}
    names.update({o: f"Ours98{chr(ord('A') + i)}" for i, o in enumerate(ours)})
    session = f"classic {int(time.time()) % 10000}"
    out = os.path.join(HERE, "..", "test-logs", time.strftime("realgame-classic98-%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    scratch = os.path.join(out, "_setup")
    os.makedirs(scratch, exist_ok=True)
    fail, started, inis = [], [], {}
    print(f"session: {session}   host {names[host]} ({host})   join {', '.join(names[j] for j in joiners)}   "
          f"Microsoft's DirectPlay on {a.ms}; TCP/IP provider under ours: {a.provider}")
    try:
        # Microsoft's end: no drop-in, the lobby program in
        r = vm(a.ms, "del", GAME + "\\dplayx.dll")
        say(f"{a.ms}: drop-in out of the game folder ({r})")
        say(f"{a.ms}: {vm(a.ms, 'put', DPLAUNCH, REMOTE_DPL)}")
        # our end(s): oldkid mode, Windows' providers on our DirectPlay
        for o in ours:
            ini = os.path.join(scratch, f"{o}-okplay.ini")
            vm(o, "get", GAME + "\\okplay.ini", ini)
            text = open(ini, encoding="latin-1").read()
            inis[o] = ini
            patched = re.sub(r"(?m)^show_windows_connections=.*$", "show_windows_connections=1", text)
            p2 = os.path.join(scratch, f"{o}-okplay-test.ini")
            open(p2, "w", encoding="latin-1", newline="").write(patched)
            vm(o, "put", p2, GAME + "\\okplay.ini")
            if a.provider == "ours":
                say(f"{o}: our TCP/IP provider in the game folder ({vm(o, 'put', OUR_SP, GAME + chr(92) + 'dpwsockx.dll')})")
        n = len(machines)

        def start(m, role):
            if m == a.ms:
                vm(m, "del", REMOTE_OUT)
                cmd = (f'"C:\WINDOWS\COMMAND.COM" /c {REMOTE_DPL} host "{session}" {names[m]} {n} --stay 900 > {REMOTE_OUT}'
                       if role == "host" else
                       f'"C:\WINDOWS\COMMAND.COM" /c {REMOTE_DPL} join "{session}" {names[m]} {ADDR[host]} --stay 900 > {REMOTE_OUT}')
                r = vm(m, "run", cmd)
                say(f"{m}: dplaunch {role} ({r})")
            else:
                for e in ("OKP_MODE=oldkid", f"OKP_TEST_LOBBY={role}:{session}", "OKP_TEST_LOBBY_SP=tcpip",
                          f"OKP_TEST_LOBBY_ADDR={ADDR[host] if role == 'join' else ''}",
                          f"OKP_TEST_LOBBY_PLAYER={names[m]}", f"OKP_TEST_LOBBY_MAX={n}",
                          "OKP_TEST_HOST_PASSWORD=<none>", "OKP_TEST_BUILD_ANSWER=join",
                          "DPLAYX_DEBUG=+dplay" if a.debug else "DPLAYX_DEBUG="):
                    vm(m, "setenv", e)
                r = vm(m, "run", f'"{GAME}\\age2_x1.exe" Lobby')
                say(f"{m}: AoE2 ({role}, our DirectPlay) ({r})")
            started.append(m)
            wait_for(f"{m}'s game running", lambda: game_pid(m), 120)

        start(host, "host")
        time.sleep(25)
        for k, j in enumerate(joiners, 1):
            start(j, "join")
            if j in ours:
                res = wait_for(f"{j}'s join result", lambda: rv.join_result(j), 240)
                if not res[0]:
                    raise RuntimeError(f"{j}: the join failed (0x{res[1]})")
                say(f"{j}: joined Microsoft's/our host")
            else:
                # Microsoft's DirectPlay tells its lobby: DPLSYS_DPLAYCONNECTSUCCEEDED (3) once Open worked,
                # DPLSYS_DPLAYCONNECTFAILED (2) if not
                local = os.path.join(out, f"dplaunch-{names[j]}.txt")

                def connect_result():
                    vm(j, "get", REMOTE_OUT, local)
                    try:
                        t = open(local, encoding="latin-1").read()
                    except OSError:
                        return None
                    if "system message 0x3," in t:
                        return "ok"
                    if "system message 0x2," in t or "RESULT fail" in t:
                        return "failed"
                    return None
                res = wait_for(f"{j}'s connect result", connect_result, 240)
                if res != "ok":
                    raise RuntimeError(f"{j}: Microsoft's DirectPlay could not join (see dplaunch-{names[j]}.txt)")
                say(f"{j}: joined (its lobby got DPLSYS_DPLAYCONNECTSUCCEEDED)")
        time.sleep(15)
        if a.chat:   # the room's chat line, Enter: AoE2 sends it to everyone
            vm(host, "click", "260", "456")
            time.sleep(1)
            vm(host, "type", a.chat)
            vm(host, "key", "enter")
            say(f"{names[host]}: chat \"{a.chat}\" sent to everyone")
            time.sleep(5)
        for m in machines:
            vm(m, "shot", os.path.join(out, f"{names[m]}-room.png"))
        for k in range(a.room_looks):   # AoE2's ping column moves; a series shows the spread
            time.sleep(10)
            for m in machines:
                vm(m, "shot", os.path.join(out, f"{names[m]}-room-{k + 1}.png"))
        if a.play > 0:
            for j in joiners:
                say(f"{names[j]}: I'm Ready! ({vm(j, 'click', '51', '489', '150')})")
                time.sleep(2)
            vm(host, "click", "751", "471", "150")    # Record Game off
            time.sleep(1)
            say(f"{names[host]}: I'm Ready! ({vm(host, 'click', '51', '489', '150')})")
            time.sleep(3)
            say(f"{names[host]}: Start Game ({vm(host, 'click', '118', '560', '400')})")
            time.sleep(6)
            vm(host, "click", "118", "560", "400")
            t = time.time()
            for at in (30, a.play):
                time.sleep(max(0, at - (time.time() - t)))
                for m in machines:
                    if not game_pid(m):
                        raise RuntimeError(f"{names[m]}: the game is gone")
                    vm(m, "shot", os.path.join(out, f"{names[m]}-play-{at}s.png"))
                say(f"at {at} s: all {len(machines)} games running")
    except Exception as e:
        fail.append(str(e))
    finally:
        for m in started:
            pid = game_pid(m)
            if pid:
                vm(m, "kill", pid)
        for line in vm(a.ms, "ps").splitlines():
            # (dplaunch only: its console host, WINOA386.MOD, goes when it does - killing that one
            # left Windows 98 hung later; a fresh output file per run avoids the old lock instead)
            if "dplaunch" in line.lower():
                vm(a.ms, "kill", line.split("\t")[0])
        time.sleep(2)
        vm(a.ms, "get", REMOTE_OUT, os.path.join(out, "dplaunch-MS98.txt"))
        say(f"{a.ms}: drop-in back ({vm(a.ms, 'put', DROPIN, GAME + chr(92) + 'dplayx.dll')})")
        for o in ours:
            text, fn = rv.newest_log(o)
            if fn:
                with open(os.path.join(out, f"{names[o]}-{fn}"), "w", encoding="latin-1") as f:
                    f.write(text)
            if o in inis:
                vm(o, "put", inis[o], GAME + "\\okplay.ini")
            vm(o, "del", GAME + "\\dpwsockx.dll")
            # our provider's own Wine log (DPLAYX_DEBUG; dplayx-<pid>.log next to the game)
            for line in vm(o, "ls", GAME + "\\dplayx-*.log").splitlines():
                if line.startswith("file\t"):
                    fn = line.split("\t")[2]
                    vm(o, "get", GAME + "\\" + fn, os.path.join(out, f"{names[o]}-provider-{fn}"))
                    vm(o, "del", GAME + "\\" + fn)
            for e in TEST_VARS + ("OKP_MODE", "OKP_TEST_LOBBY_ADDR", "DPLAYX_DEBUG"):
                vm(o, "setenv", e + "=")
        print(f"logs and captures in {os.path.abspath(out)}")
    for f in fail:
        print(f"  - {f}")
    print("RESULT FAIL" if fail else "RESULT PASS")
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
