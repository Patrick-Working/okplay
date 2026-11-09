#!/usr/bin/env python3
"""tcp_run.py - runs tcpsim scenarios on this PC, two processes each (host, then joiners), and prints a table.

    python tcp_run.py [--mode real|shim|both] [--only lockstep,peek,...] [--frames 300] [--players 2]

real: tcpsim.exe alone in a temporary folder - Windows' own TCP over loopback (the reference).
shim: tcpsim.exe + okplay wsock's wsock32.dll + an okplay.ini in a temporary folder, OKP_WSOCK_MODE=lan,
      OKP_WSOCK_GAME=nfs3: every stream socket is virtual and travels over okplay's reliable lane (LAN Play,
      each process its own okplay peer). The joiner types 127.0.0.1; in LAN Play the shim takes the one room
      on the LAN for that (a test stand-in for the join list, which comes later).
Both should give the same RESULT lines. Uses ../bin/tcpsim.exe and ../bin/wsock32.dll (build.sh builds both).
No game is started; nothing makes sound.
"""
import argparse, os, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.normpath(os.path.join(HERE, "..", "bin"))
SCENARIOS = ["lockstep", "lockstep3", "refuse", "peek", "halfclose", "rstunread", "window", "selftest", "probe"]


def folder(mode, extra_ini=""):
    d = tempfile.mkdtemp(prefix=f"tcpsim_{mode}_")
    shutil.copy(os.path.join(BIN, "tcpsim.exe"), d)
    if mode == "shim":
        shutil.copy(os.path.join(BIN, "wsock32.dll"), d)
        with open(os.path.join(d, "okplay.ini"), "w") as f:
            f.write("[okplay]\nloglevel=3\nuse_password=off\nexit_watch=0\n" + extra_ini)
    return d


EXTRA_ENV = {}


def run(d, mode, args_host, args_joins, timeout=120, join_delay=1.5):
    env = dict(os.environ)
    env.update(EXTRA_ENV)
    if mode == "shim":
        env.update(OKP_WSOCK_MODE="lan", OKP_WSOCK_GAME="nfs3", OKP_TEST_HOST_PASSWORD="<none>")
    else:
        for k in ("OKP_WSOCK_MODE", "OKP_WSOCK_GAME"):
            env.pop(k, None)
    exe = os.path.join(d, "tcpsim.exe")
    procs = []
    if args_host is not None:
        procs.append(("host", subprocess.Popen([exe] + args_host, cwd=d, env=env, stdout=subprocess.PIPE,
                                               stderr=subprocess.STDOUT, text=True)))
        time.sleep(join_delay)
    for i, a in enumerate(args_joins):
        procs.append((f"join{i + 1}", subprocess.Popen([exe] + a, cwd=d, env=env, stdout=subprocess.PIPE,
                                                       stderr=subprocess.STDOUT, text=True)))
    out = {}
    for name, p in procs:
        try:
            o, _ = p.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            o, _ = p.communicate()
            o += "\nRESULT fail (killed: timeout)\n"
        out[name] = o
    return out


def scenario(name, mode, a):
    port = "9803" if mode == "real" else "9803"
    if name == "selftest":
        return run(folder(mode), mode, None, [["selftest"]])
    if name == "probe":   # through okplay: probes alone must not reach the Online/LAN box (no mode decision logged)
        d = folder(mode)
        out = run(d, mode, None, [["probe"]])
        logs = os.path.join(d, "logs")
        text = "".join(open(os.path.join(logs, f), errors="replace").read() for f in os.listdir(logs)) if os.path.isdir(logs) else ""
        if mode == "shim" and not text:
            out["join1"] += "\nRESULT fail no okplay log: the shim did not load\n"
        if "LAN Play (okplay.ini" in text or "for a Winsock game" in text:
            out["join1"] += "\nRESULT fail the probes made okplay decide the mode (a box would have come)\n"
        return out
    if name.startswith("lockstep"):
        n = 3 if name == "lockstep3" else a.players
        host = ["host", "--port", port, "--players", str(n), "--frames", str(a.frames), "--hz", "30"]
        return run(folder(mode), mode, host, [["join", "127.0.0.1:" + port, "--frames", str(a.frames)]] * (n - 1))
    host = ["host", "--port", port, "--edge", name, "--secs", "40"]
    return run(folder(mode), mode, host, [["join", "127.0.0.1:" + port, "--edge", name]])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="both", choices=["real", "shim", "both"])
    ap.add_argument("--only", default="")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--env", action="append", default=[], help="K=V for the tcpsim processes (e.g. OKP_DROP_PCT=10)")
    ap.add_argument("-v", action="store_true", help="print every process's output")
    a = ap.parse_args()
    for kv in a.env:
        k, v = kv.split("=", 1)
        EXTRA_ENV[k] = v
    names = [s for s in (a.only.split(",") if a.only else SCENARIOS) if s]
    modes = ["real", "shim"] if a.mode == "both" else [a.mode]
    rows = []
    for n in names:
        for m in modes:
            out = scenario(n, m, a)
            res = {}
            for who, o in out.items():
                lines = o.splitlines()
                r = [l for l in lines if l.startswith("RESULT")]
                st = [l for l in lines if l.startswith("STAT")]
                ev = [l for l in lines if l.startswith("EVENT")]
                res[who] = (r[-1] if r else "RESULT (none)", st, ev)
                if a.v:
                    print(f"--- {n} {m} {who}\n{o}")
            ok = all(v[0] == "RESULT ok" for v in res.values())
            rows.append((n, m, ok, res))
            print(f"{n:10} {m:5} {'ok' if ok else 'FAIL'}", flush=True)
            for who, (r, st, ev) in res.items():
                print(f"    {who}: {r}")
                for l in st:
                    print(f"        {l}")
                for l in ev:
                    if any(k in l for k in ("connected after", "refused", "gave up", "WOULDBLOCK", "resumed", "partial",
                                            "after the joiner", "then EOF", "select", "FIONREAD", "UDP", "failure")):
                        print(f"        {l}")
    bad = [r for r in rows if not r[2]]
    print(f"\n{len(rows) - len(bad)}/{len(rows)} ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
