"""Every host/joiner router-type pair through natsim + dptest -> a connectivity matrix.

    python tests/nat_matrix.py [--bin bin_matrix] [--types open,full,port]
    DPTEST_SP=auto DPTEST_DPLAYX=<drop-in dplayx.dll> python tests/nat_matrix.py    # the drop-in

Needs okplay built with the OKP_TEST_NAT hook (any build since 0.2) and dptest.exe + that build as
dpwsockx.dll in the --bin folder (a separate folder so a running soak/test in bin\ is untouched).
Each pair gets a fresh rendezvous stand-in and a fresh router simulator.

Theory (no relay): only port-restricted<->symmetric and symmetric<->symmetric cannot connect directly.
"""
import argparse
import itertools
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TYPES = ["open", "full", "restricted", "port", "symmetric"]
EXPECT_FAIL = {("port", "symmetric"), ("symmetric", "port"), ("symmetric", "symmetric")}


def ip_for(role, kind):
    if kind == "open":
        return "203.0.113.101" if role == "host" else "203.0.113.102"
    return "10.0.1.2" if role == "host" else "10.0.2.2"


def run_pair(bin_dir, h, j, idx):
    rdv_port, sim_port = 8800 + idx % 50, 47700 + idx % 50
    py = sys.executable
    rdv = subprocess.Popen([py, os.path.join(ROOT, "server", "rdv_standin.py"), "--port", str(rdv_port)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    hip, jip = ip_for("host", h), ip_for("join", j)
    sim = subprocess.Popen([py, os.path.join(ROOT, "tests", "natsim.py"), "--port", str(sim_port), "--quiet",
                            "--secs", "24", "--peer", f"{hip}={h}", "--peer", f"{jip}={j}"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.0)
    env = dict(os.environ, OKP_TEST_AS="online", OKP_SERVER=f"http://127.0.0.1:{rdv_port}/okplay",
               OKP_TEST_NAT=f"127.0.0.1:{sim_port}", OKP_STUN="198.51.100.1:3478", OKP_FAMILY="4", OKP_LOGLEVEL="2")
    exe = os.path.join(bin_dir, "dptest.exe")
    sp = os.environ.get("DPTEST_SP", "ipx")  # like run_local.sh: auto + DPTEST_DPLAYX=<dll> for the drop-in
    host = subprocess.Popen([exe, "--sp", sp, "host", "--name", h, "--secs", "16", "--expect", "2"],
                            env=dict(env, OKP_TEST_PRIVATE=hip), cwd=bin_dir, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    time.sleep(3)
    join = subprocess.Popen([exe, "--sp", sp, "join", "--name", j, "--secs", "10"],
                            env=dict(env, OKP_TEST_PRIVATE=jip), cwd=bin_dir, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    jout = join.communicate(timeout=90)[0]
    hout = host.communicate(timeout=90)[0]
    sout = sim.communicate(timeout=40)[0]
    rdv.kill()
    ok = "RESULT ok" in jout and "RESULT ok" in hout
    m = re.search(r"joined .* in (\d+)ms", jout)
    summary = next((l for l in sout.splitlines() if l.startswith("SUMMARY")), "")
    drops = re.findall(r"(drop[^=,]*)=(\d+)", summary)
    return ok, (m.group(1) + " ms") if m else "-", drops


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "bin_matrix"))
    ap.add_argument("--types", default=",".join(TYPES))
    a = ap.parse_args()
    types = a.types.split(",")
    results = {}
    for idx, (h, j) in enumerate(itertools.product(types, types)):
        ok, t, drops = run_pair(a.bin, h, j, idx)
        results[(h, j)] = ok
        exp = (h, j) not in EXPECT_FAIL
        verdict = "as expected" if ok == exp else ("UNEXPECTED FAIL" if exp else "unexpectedly connected")
        why = "; ".join(f"{k.strip()} x{v}" for k, v in drops[:2]) if not ok else ""
        print(f"host {h:10s} joiner {j:10s} -> {'CONNECTED' if ok else 'no path  '} join {t:>7s}   {verdict}  {why}",
              flush=True)
        time.sleep(1)

    print("\n" + "host \\ joiner".ljust(14) + "".join(t[:10].ljust(12) for t in types))
    for h in types:
        print(h.ljust(14) + "".join(("yes" if results[(h, j)] else "NO").ljust(12) for j in types))
    bad = [(h, j) for (h, j), ok in results.items() if ok != ((h, j) not in EXPECT_FAIL)]
    print(f"\n{sum(results.values())}/{len(results)} pairs connected; "
          f"{'matches theory' if not bad else 'DIFFERS from theory: ' + str(bad)}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
