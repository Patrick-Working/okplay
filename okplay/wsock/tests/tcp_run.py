#!/usr/bin/env python3
"""tcp_run.py - runs tcpsim scenarios on this PC, each as separate processes (host, then joiners), and prints a table.

    python tcp_run.py [--mode real|shim|both] [--only a,b,...] [--env K=V ...] [-v] [--list]

real: tcpsim.exe alone in a temporary folder - Windows' own TCP (the reference).
shim: tcpsim.exe + okplay wsock's wsock32.dll + an okplay.ini in a temporary folder, OKP_WSOCK_GAME=nfs3, LAN Play
      (Online Play with a local room-server stand-in, server/rdv_standin.py, for the *_online scenarios): every stream
      socket is virtual and travels over okplay's reliable lane, each process its own okplay peer.
Every scenario says what each side must print (RESULT ok, or e.g. a refusal); a few are shim-only (passwords, builds,
okplay's own join rules). Boxes never show: the test answers (OKP_TEST_*) are always set, and cancel by default.
Uses ../bin/tcpsim.exe and ../bin/wsock32.dll (build.sh builds both). No game is started; nothing makes sound.
"""
import argparse, os, re, shutil, socket, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.normpath(os.path.join(HERE, "..", "bin"))
STANDIN = os.path.normpath(os.path.join(HERE, "..", "..", "server", "rdv_standin.py"))
EXTRA_ENV = {}
OK = r"^RESULT ok"
ANY = "."


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))   # (no packet is sent: this only picks the interface)
        return s.getsockname()[0]
    finally:
        s.close()


def H(*a, players=2, frames=None, port=9803):
    return ["host", "--port", str(port), "--players", str(players)] + (["--frames", str(frames)] if frames else []) + list(a)


def J(addr="{lo}:9803", *a, menu=True):
    # the joiner's menu listener: the game's port through okplay (ports are per process), any port on Windows' own TCP
    # (two processes on one PC can't both listen on 9803)
    return ["join", addr] + (["--menu-port", "{menuport}"] if menu else []) + list(a)


# name: host args, joiner args (one list per joiner), what each side must print per mode, extra settings and checks
SCENARIOS = {
    "lockstep":   dict(host=H(), joins=[J()]),
    "lockstep3":  dict(host=H(players=3), joins=[J(), J()]),
    "refuse":     dict(host=["host", "--port", "9803", "--edge", "refuse", "--secs", "20"], joins=[J("{lo}:9803", "--edge", "refuse")]),
    "peek":       dict(host=["host", "--edge", "peek", "--secs", "40"], joins=[J("{lo}:9803", "--edge", "peek")]),
    "halfclose":  dict(host=["host", "--edge", "halfclose", "--secs", "40"], joins=[J("{lo}:9803", "--edge", "halfclose")]),
    "rstunread":  dict(host=["host", "--edge", "rstunread", "--secs", "40"], joins=[J("{lo}:9803", "--edge", "rstunread")]),
    "window":     dict(host=["host", "--edge", "window", "--secs", "40"], joins=[J("{lo}:9803", "--edge", "window")]),
    "selftest":   dict(host=None, joins=[["selftest"]]),
    "probe":      dict(host=None, joins=[["probe"]], check="probe"),
    # C.6: how a joiner finds the host
    "standin":    dict(host=H(), joins=[J("{standin}:9803")], ini={"wsock_join": ""}),
    "byip":       dict(host=H(), joins=[J("{lanip}:9803")], ini={"wsock_join": ""}),
    "byip_online": dict(host=H(), joins=[J("{lanip}:9803")], ini={"wsock_join": ""}, online=True),
    "pick":       dict(host=H(), joins=[J()], ini={"wsock_join": ""}, env_join={"OKP_TEST_JOIN_PICK": "first"}),
    "byname":     dict(host=H(), joins=[J("okplay:9803")], expect_real={"join1": r"^RESULT fail address", "host": ANY}, kill_host=True),
    "adopt":      dict(host=H(), joins=[J()], env_join={"OKP_TEST_SYN_DELAY_MS": "3000"}, check="adopt"),
    "accepthold": dict(host=H("--claim-delay", "3000", players=3), joins=[J(), J()], check="accepthold"),
    "origports":  dict(host=H(), joins=[J("{lo}:1030", "--orig", menu=False)], modes=("shim",)),
    "menuwait":   dict(host=H(), joins=[J("{lo}:9803", "--menu-ms", "4000")], check="menuwait"),
    # C.7: passwords and builds (okplay only)
    "pw_ok":      dict(host=H(), joins=[J()], env_host={"OKP_TEST_HOST_PASSWORD": "secret"},
                       env_join={"OKP_TEST_JOIN_PASSWORD": "secret"}, modes=("shim",)),
    "pw_wrong":   dict(host=H(), joins=[J()], env_host={"OKP_TEST_HOST_PASSWORD": "secret"},
                       env_join={"OKP_TEST_JOIN_PASSWORD": "guess"}, modes=("shim",), kill_host=True,
                       expect_shim={"join1": r"^RESULT fail refused", "host": ANY}, check="pw_wrong"),
    "build_same": dict(host=H(), joins=[J()], env_host={"OKP_TEST_EXEBUILD": "240a083a78ac3b83"},
                       env_join={"OKP_TEST_EXEBUILD": "397dee58eb88822c"}, modes=("shim",)),
    "build_diff_cancel": dict(host=H(), joins=[J()], env_host={"OKP_TEST_EXEBUILD": "240a083a78ac3b83"},
                              env_join={"OKP_TEST_EXEBUILD": "53aa0dfcf4387888"}, modes=("shim",), kill_host=True,
                              expect_shim={"join1": r"^RESULT fail refused", "host": ANY}),
    "build_diff_join": dict(host=H(), joins=[J()], env_host={"OKP_TEST_EXEBUILD": "240a083a78ac3b83"},
                            env_join={"OKP_TEST_EXEBUILD": "53aa0dfcf4387888", "OKP_TEST_BUILD_ANSWER": "join"}, modes=("shim",)),
    # C.8: the host process ends mid-race without closing anything
    "diesoft":    dict(host=H("--die-at", "150", "--die", "soft", frames=600), joins=[J("{lo}:9803", "--expect-loss", "1500")],
                       expect={"host": ANY}),
    "diehard":    dict(host=H("--die-at", "150", "--die", "hard", frames=600),
                       joins=[J("{lo}:9803", "--expect-loss", "18000", "--secs", "60")], expect={"host": ANY}),
}


def folder(mode, ini):
    d = tempfile.mkdtemp(prefix=f"tcpsim_{mode}_")
    shutil.copy(os.path.join(BIN, "tcpsim.exe"), d)
    if mode == "shim":
        shutil.copy(os.path.join(BIN, "wsock32.dll"), d)
        settings = {"loglevel": "3", "use_password": "off", "exit_watch": "0", "wsock_join": "auto"}
        settings.update(ini)
        with open(os.path.join(d, "okplay.ini"), "w") as f:
            f.write("[okplay]\n" + "".join(f"{k}={v}\n" for k, v in settings.items() if v != ""))
    return d


class Proc:
    def __init__(self, name, args, cwd, env):
        self.name, self.lines = name, []
        self.p = subprocess.Popen(args, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.t = threading.Thread(target=self._read, daemon=True)
        self.t.start()

    def _read(self):
        for line in self.p.stdout:
            self.lines.append(line.rstrip("\n"))

    def wait_line(self, pat, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            for l in list(self.lines):
                m = re.search(pat, l)
                if m:
                    return m
            if self.p.poll() is not None and not self.t.is_alive():
                return None
            time.sleep(0.05)
        return None

    def finish(self, timeout):
        try:
            self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            self.lines.append("RESULT (killed: still running)")
        self.t.join(2)
        return "\n".join(self.lines)


def run_scenario(sc, mode):
    d = folder(mode, sc.get("ini", {}))
    env = {k: v for k, v in os.environ.items() if not k.startswith("OKP_")}
    env.update(EXTRA_ENV)
    if mode == "shim":
        env.update(OKP_WSOCK_MODE="lan", OKP_WSOCK_GAME="nfs3")
        if sc.get("online"):
            env.update(OKP_WSOCK_MODE="online", OKP_SERVER="http://127.0.0.1:8787/okplay.php", OKP_STUN="")
    env_host = dict(env, OKP_TEST_HOST_PASSWORD="<none>")
    env_join = dict(env, OKP_TEST_JOIN_PICK="cancel", OKP_TEST_BUILD_ANSWER="cancel", OKP_TEST_JOIN_PASSWORD="<cancel>")
    if mode == "shim":
        env_host.update(sc.get("env_host", {}))
        env_join.update(sc.get("env_join", {}))
    exe = os.path.join(d, "tcpsim.exe")
    subst = {"lo": "127.0.0.1", "lanip": lan_ip(), "menuport": "9803" if mode == "shim" else "0"}
    host = None
    joins = []
    if sc["host"] is not None:
        host = Proc("host", [exe] + sc["host"], d, env_host)
        if any("{standin}" in x for j in sc["joins"] for x in j):
            m = host.wait_line(r"EVENT my address (\S+)", 15)
            subst["standin"] = m.group(1) if m else "0.0.0.0"
        time.sleep(1.5)
    for i, ja in enumerate(sc["joins"]):
        joins.append(Proc(f"join{i + 1}", [exe] + [x.format(**subst) for x in ja], d, env_join))
    out = {p.name: p.finish(150) for p in joins}
    out["_pids"] = {p.name: p.p.pid for p in joins}
    if host:
        if sc.get("kill_host"):
            time.sleep(1)
            if host.p.poll() is None:
                host.p.kill()
        out["host"] = host.finish(150)
    out["_dir"] = d
    return out


def logs_of(d):
    logs = os.path.join(d, "logs")
    if not os.path.isdir(logs):
        return ""
    return "".join(open(os.path.join(logs, f), errors="replace").read() for f in sorted(os.listdir(logs)))


def check(sc, mode, out):
    exp = {r: OK for r in out if not r.startswith("_")}
    exp.update(sc.get("expect", {}))
    exp.update(sc.get(f"expect_{mode}", {}))
    why = []
    for role, pat in exp.items():
        r = [l for l in out.get(role, "").splitlines() if l.startswith("RESULT")]
        line = r[-1] if r else "RESULT (none)"
        if pat != ANY and not re.search(pat, line):
            why.append(f"{role}: {line}")
    kind = sc.get("check")
    m = re.search(r"Host Game: listening again on port \d+ after (\d+) ms", out.get("host", ""))
    if m and int(m.group(1)) > 500:
        why.append(f"Host Game's re-listen took {m.group(1)} ms")
    if mode == "shim" and sc["host"] is not None:
        text = logs_of(out["_dir"])
        for r, pid in out["_pids"].items():
            if re.search(r"\[\s*%d\] .*(hosting room|this PC hosts)" % pid, text):
                why.append(f"{r} hosted a room")
        if sc["host"][0] == "host" and "this PC hosts" not in text:
            why.append("the host never hosted")
    if kind == "adopt" and mode == "shim":
        m = re.search(r"\((\d+) attempts\)", out.get("join1", ""))
        n = re.search(r"accepted (\d+) connection", out.get("host", ""))
        if not m or int(m.group(1)) < 2 or not n or n.group(1) != "1":
            why.append(f"adopt: joiner attempts {m.group(1) if m else '?'}, host accepted {n.group(1) if n else '?'} (want >= 2 and 1)")
    if kind == "accepthold":
        for r in ("join1", "join2"):
            m = re.search(r"connected after (\d+) ms \((\d+) attempts\)", out.get(r, ""))
            if not m or int(m.group(2)) != 1 or int(m.group(1)) >= 1000:
                why.append(f"accepthold: {r} {m.group(0) if m else 'did not connect'}")
    if kind == "probe" and mode == "shim":
        text = logs_of(out["_dir"])
        if not text:
            why.append("no okplay log: the shim did not load")
        if "LAN Play (okplay.ini" in text or "for a Winsock game" in text:
            why.append("the probes made okplay decide the mode (a box would have come)")
    if kind == "pw_wrong" and mode == "shim" and "turned away (wrong password)" not in logs_of(out["_dir"]):
        why.append("the host's log does not say it turned the joiner away")
    return why


SHOW = ("connected after", "refused", "gave up", "WOULDBLOCK", "resumed", "partial", "after the joiner", "then EOF",
        "select", "FIONREAD", "UDP", "failure", "accepted", "connection lost", "my address", "ends now", "Host Game", "at the menu")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="both", choices=["real", "shim", "both"])
    ap.add_argument("--only", default="")
    ap.add_argument("--env", action="append", default=[], help="K=V for every tcpsim process (e.g. OKP_DROP_PCT=10)")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-v", action="store_true", help="print every process's output")
    a = ap.parse_args()
    if a.list:
        print("\n".join(SCENARIOS))
        return 0
    for kv in a.env:
        k, v = kv.split("=", 1)
        EXTRA_ENV[k] = v
    names = [s for s in (a.only.split(",") if a.only else SCENARIOS) if s]
    modes = ["real", "shim"] if a.mode == "both" else [a.mode]
    standin = None
    if "shim" in modes and any(SCENARIOS[n].get("online") for n in names):
        standin = subprocess.Popen([sys.executable, STANDIN, "--port", "8787"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.5)
    rows = []
    try:
        for n in names:
            sc = SCENARIOS[n]
            for m in modes:
                if m not in sc.get("modes", ("real", "shim")):
                    continue
                out = run_scenario(sc, m)
                why = check(sc, m, out)
                rows.append((n, m, not why))
                print(f"{n:18} {m:5} {'ok' if not why else 'FAIL: ' + '; '.join(why)}", flush=True)
                for who, o in out.items():
                    if who.startswith("_"):
                        continue
                    if a.v:
                        print(f"--- {n} {m} {who}\n{o}")
                        continue
                    lines = o.splitlines()
                    r = [l for l in lines if l.startswith("RESULT")]
                    print(f"    {who}: {r[-1] if r else 'RESULT (none)'}")
                    for l in lines:
                        if l.startswith("STAT") or (l.startswith("EVENT") and any(k in l for k in SHOW)):
                            print(f"        {l}")
    finally:
        if standin:
            standin.kill()
    bad = [r for r in rows if not r[2]]
    print(f"\n{len(rows) - len(bad)}/{len(rows)} ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
