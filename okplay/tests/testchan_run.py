#!/usr/bin/env python3
"""testchan_run.py - Connection Check's test game (okplay/src/testchan.c) on this PC: netcheck.exe --cli host / join
in separate processes, each in its own game-like folder, against a local room-server stand-in (server/rdv_standin.py on
port 8791; never 8787 or 8790: other tests use those). Prints a table; exit 1 if a scenario failed.

    python testchan_run.py [--only a,b,...] [--list] [-v]

Folders (FIXED paths under tests/out/testchan/, files overwritten in place: each program path asks Windows Firewall at
most once, ever):
    W1, W2   netcheck.exe + okplay wsock's wsock32.dll + okplay.ini            (a Winsock game's folder)
    D1       netcheck.exe + the drop-in's dplayx.dll + okplay.ini              (a DirectPlay game's folder)
    B        netcheck.exe + dplayx.dll + wsock32.dll + okplay.ini              (both: the channel goes through dplayx.dll)
    R85      the released 0.8.5 netcheck.exe + dplayx.dll (deliverables/, when on disk)   (an old Connection Check)
    WOLD     netcheck.exe + the released 0.8.4 wsock32.dll (no test channel: no socket is ever opened there)
Uses ../bin/netcheck.exe, ../wsock/bin/wsock32.dll, ../../winedplay/bin/dropin/dplayx.dll (build them first).
STUN is off (OKP_STUN=""): every path is a local one. No game is started; nothing makes sound.
"""
import argparse, os, re, shutil, subprocess, sys, threading, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
NETCHECK = os.path.join(ROOT, "okplay", "bin", "netcheck.exe")
WSOCK = os.path.join(ROOT, "okplay", "wsock", "bin", "wsock32.dll")
DROPIN = os.path.join(ROOT, "winedplay", "bin", "dropin", "dplayx.dll")
REL85 = os.path.join(ROOT, "deliverables", "Oldkid.net Online Play v0.8.5")
REL84 = os.path.join(ROOT, "deliverables", "Oldkid.net Online Play v0.8.4")
STANDIN = os.path.join(ROOT, "okplay", "server", "rdv_standin.py")
OUT = os.path.join(HERE, "out", "testchan")
PORT = 8791
SERVER = f"http://127.0.0.1:{PORT}/okplay.php"

FOLDERS = {
    "W1": [(NETCHECK, "netcheck.exe"), (WSOCK, "wsock32.dll")],
    "W2": [(NETCHECK, "netcheck.exe"), (WSOCK, "wsock32.dll")],
    "D1": [(NETCHECK, "netcheck.exe"), (DROPIN, "dplayx.dll")],
    "B": [(NETCHECK, "netcheck.exe"), (DROPIN, "dplayx.dll"), (WSOCK, "wsock32.dll")],
    "R85": [(os.path.join(REL85, "netcheck.exe"), "netcheck.exe"), (os.path.join(REL85, "dplayx.dll"), "dplayx.dll")],
    "WOLD": [(NETCHECK, "netcheck.exe"), (os.path.join(REL84, "wsock32.dll"), "wsock32.dll")],
}

JOINED = r'\[ OK \] joined "Connection test - {host}'
PING = r"{peer}: ping \d+ ms"
XFER = r"\[ OK \] guaranteed messages \(64 bytes\) -> .*: 50 of 50 arrived"
BIG = r"\[ OK \] large guaranteed messages \(3000 bytes\) -> .*: 10 of 10 arrived"
METHOD = r"connection method: direct on the local network, IPv[46] \(\d player"


def J(folder, name, expect, args=(), secs=None, start_after=None, rc=0, unexpect=()):
    return dict(folder=folder, name=name, expect=expect, args=list(args), secs=secs, start_after=start_after, rc=rc,
                unexpect=list(unexpect))


def joiner_ok(host, *peers):   # what a joiner of the channel must print
    return [JOINED.format(host=host), XFER, BIG, METHOD, r"RESULT ok"] + [PING.format(peer=p) for p in peers]


def host_ok(*joiners):
    return [r"\[ OK \] your test game is listed on the room server", METHOD, r"RESULT ok"] + \
           [r"\[ OK \] {} joined".format(j) for j in joiners] + [PING.format(peer=j) for j in joiners]


ROUTE_DP = r"the test game runs through dplayx.dll"
SCENARIOS = {
    # Winsock folder <-> Winsock folder
    "ww": dict(host=J("W1", "Hw", host_ok("Jw")), joins=[J("W2", "Jw", joiner_ok("Hw", "Hw"))]),
    # the cross-edition cases: DirectPlay folder <-> Winsock folder
    "dw": dict(host=J("D1", "Hd", host_ok("Jw")), joins=[J("W1", "Jw", joiner_ok("Hd", "Hd"))]),
    "wd": dict(host=J("W1", "Hw", host_ok("Jd")), joins=[J("D1", "Jd", joiner_ok("Hw", "Hw"))]),
    # a folder with both DLLs, hosting and joining
    "bw": dict(host=J("B", "Hb", host_ok("Jw")), joins=[J("W1", "Jw", joiner_ok("Hb", "Hb"))]),
    "db": dict(host=J("D1", "Hd", host_ok("Jb")), joins=[J("B", "Jb", joiner_ok("Hd", "Hd"))]),
    # three players, all three kinds of folder: everyone pings everyone (the joiners reach each other directly)
    "mix3": dict(host=J("B", "Hb", host_ok("Jw", "Jd"), secs=50),
                 joins=[J("W1", "Jw", joiner_ok("Hb", "Hb", "Jd"), secs=40),
                        J("D1", "Jd", joiner_ok("Hb", "Hb", "Jw"), secs=40, start_after=r"\[ OK \] joined")]),
    # LAN Play (no room server: discovery on UDP 47632), Winsock host, DirectPlay joiner
    "lan_wd": dict(host=J("W1", "Hw", [r"\[ OK \] Jd joined", METHOD, r"RESULT ok"], args=["--lan"]),
                   joins=[J("D1", "Jd", joiner_ok("Hw", "Hw"), args=["--lan"])]),
    # a released Connection Check (0.8.5, DirectPlay messages) hosts: a new DirectPlay-folder check lists it as an
    # older one and joins it through DirectPlay; a Winsock-folder check lists it as DirectPlay only and says so
    "old_host": dict(host=J("R85", "Hr", [r"\[ OK \] Jd joined", r"RESULT ok"], secs=50), needs=REL85,
                     joins=[J("D1", "Jd", [r'found "Connection test - Hr \(older Connection Check\)', r"through DirectPlay",
                                           JOINED.format(host="Hr"), XFER, r"RESULT ok"]),
                            J("W2", "Jw", [r'found "Connection test - Hr \(DirectPlay only\)',
                                           r"test games from older Connection Check versions can only be joined from a "
                                           r"DirectPlay game's folder \(1 listed now\)", r"no test game found"], rc=1, secs=10)]),
    # ... and a released Connection Check never sees (so never tries) a new test game
    "old_join": dict(host=J("W1", "Hw", [r"RESULT ok"], secs=40), needs=REL85,
                     joins=[J("R85", "Jr", [r"no test game found in 30 seconds"], rc=1, unexpect=[r'found "Connection test'])]),
    # the old DirectPlay-message test itself, hosted by the new build (--dp), joined by a new DirectPlay folder
    "dp_path": dict(host=J("B", "Hp", [r"\[ OK \] Jd joined", METHOD, r"RESULT ok"], args=["--dp"]),
                    joins=[J("D1", "Jd", [r"\(older Connection Check\)", JOINED.format(host="Hp"), XFER, METHOD, r"RESULT ok"])]),
    # a Winsock folder with an older wsock32.dll (0.8.4): the test game is not there, said plainly
    "old_wsock": dict(host=J("WOLD", "Ho", [r"needs a newer wsock32.dll", r"RESULT skipped"], rc=3), joins=[], needs=REL84),
}


def prepare():
    for name, files in FOLDERS.items():
        d = os.path.join(OUT, name)
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):   # (only the files listed stay: a stale DLL would change the folder's kind)
            p = os.path.join(d, f)
            if os.path.isfile(p) and f.lower() not in [t.lower() for _, t in files] + ["okplay.ini"]:
                os.remove(p)
        for src, to in files:
            if os.path.isfile(src):
                shutil.copyfile(src, os.path.join(d, to))
        with open(os.path.join(d, "okplay.ini"), "w") as f:
            f.write(f"[okplay]\nserver={SERVER}\nstun=\nupdate_check=off\nconnection_reports=no\n")


class Proc:
    def __init__(self, spec, role):
        spec["secs"] = spec["secs"] or (50 if role == "host" else 34)   # (the host outlasts its joiners)
        self.spec, self.role, self.lines = spec, role, []
        exe = os.path.join(OUT, spec["folder"], "netcheck.exe")
        args = [exe, "--cli", role, "--name", spec["name"], "--secs", str(spec["secs"])] + spec["args"]
        env = {k: v for k, v in os.environ.items() if not k.startswith("OKP_")}
        env.update(OKP_SERVER=SERVER, OKP_STUN="")
        self.p = subprocess.Popen(args, cwd=os.path.join(OUT, spec["folder"]), env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, errors="replace")
        self.t = threading.Thread(target=self._read, daemon=True)
        self.t.start()

    def _read(self):
        for line in self.p.stdout:
            self.lines.append(re.sub(r"^\[\s*\d+\] ", "", line.rstrip("\n")))

    def wait_line(self, pat, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if any(re.search(pat, l) for l in list(self.lines)):
                return True
            if self.p.poll() is not None and not self.t.is_alive():
                return False
            time.sleep(0.1)
        return False

    def finish(self, timeout):
        try:
            self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            self.lines.append("(killed: still running)")
        self.t.join(2)
        return self.p.returncode

    def check(self):
        bad = [e for e in self.spec["expect"] if not any(re.search(e, l) for l in self.lines)]
        bad += ["NOT " + u for u in self.spec["unexpect"] if any(re.search(u, l) for l in self.lines)]
        if self.p.returncode != self.spec["rc"]:
            bad.append(f"exit {self.p.returncode}, wanted {self.spec['rc']}")
        return bad


def run(name, sc, verbose):
    if sc.get("needs") and not os.path.isdir(sc["needs"]):
        return "skip", [f"{sc['needs']} is not on disk"]
    host = Proc(sc["host"], "host")
    procs = [host]
    if sc["joins"] and not host.wait_line(r"\[ OK \] hosting", 20):
        host.finish(5)
        return "FAIL", ["the host never hosted"] + host.lines[-15:]
    time.sleep(2)   # (the room reaches the room server / the LAN)
    prev = None
    for j in sc["joins"]:
        if j["start_after"] and prev:
            prev.wait_line(j["start_after"], 30)
        prev = Proc(j, "join")
        procs.append(prev)
    for p in procs[1:]:
        p.finish(p.spec["secs"] + 60)
    host.finish(sc["host"]["secs"] + 60)
    problems = []
    for p in procs:
        for b in p.check():
            problems.append(f"{p.spec['folder']} {p.role} {p.spec['name']}: {b}")
    if verbose or problems:
        for p in procs:
            print(f"---- {name}: {p.spec['folder']} {p.role} {p.spec['name']} (exit {p.p.returncode})")
            print("\n".join(p.lines))
    return ("ok" if not problems else "FAIL"), problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()
    if a.list:
        print("\n".join(SCENARIOS))
        return 0
    for f in (NETCHECK, WSOCK, DROPIN):
        if not os.path.isfile(f):
            print(f"missing {f} - build first")
            return 2
    prepare()
    names = [n for n in SCENARIOS if not a.only or n in a.only.split(",")]
    sv = subprocess.Popen([sys.executable, STANDIN, "--port", str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{PORT}/okplay.php", data=b"k=x&a=list", timeout=1)
                break
            except Exception as e:
                if hasattr(e, "code"):
                    break
                time.sleep(0.1)
        results = []
        for n in names:
            t0 = time.time()
            st, why = run(n, SCENARIOS[n], a.v)
            results.append((n, st, why, time.time() - t0))
            print(f"{n:10} {st:5} {time.time() - t0:5.0f} s  {'; '.join(why[:3])}", flush=True)
    finally:
        sv.terminate()
        sv.wait()
    fails = sum(1 for _, st, _, _ in results if st == "FAIL")
    print(f"\n{len(results) - fails}/{len(results)} ok ({sum(1 for r in results if r[1] == 'skip')} skipped)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
