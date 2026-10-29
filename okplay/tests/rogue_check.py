"""Decide whether a forged in-session message was refused or took effect, from the drop-in's own logs.

    python tests/rogue_check.py <netplay-logs dir> <marker file> <attack>

<attack> is deletehost | iamhost | sessiondesc. Only logs modified at/after the marker are read (this
run). Prints "refused=<0|1> took_effect=<0|1>" and exits 0.

"Took effect" is judged ONLY in the window between the forged message being sent and the real host
leaving the session, so the normal host migration that MIGRATEHOST sessions do at teardown is never
mistaken for the attack succeeding (that was the flaw in the first, migration-anywhere check).
  refused     = a victim logged the matching "ignored ..." line (the fix refused it)
  took_effect = a victim acted on it inside the window:
                  deletehost / iamhost  -> "is the new host" (a victim handed the role over early)
                  sessiondesc           -> "SESSIONDESCCHANGED applied" (a victim changed its session)
On the fixed build we expect refused=1 took_effect=0; on a -DDPLAYX_TEST_SKIP_AUTH build, the reverse.
"""
import glob
import os
import re
import sys

logdir, marker, attack = sys.argv[1], sys.argv[2], sys.argv[3]
since = os.path.getmtime(marker)

REFUSED = {
    "deletehost":  "DELETEPLAYER for .* ignored",
    "iamhost":     "IAMNAMESERVER .* ignored",
    "sessiondesc": "SESSIONDESCCHANGED for .* ignored",
}[attack]
EFFECT = {
    "deletehost":  "is the new host",
    "iamhost":     "is the new host",
    "sessiondesc": "SESSIONDESCCHANGED applied",
}[attack]

TS = re.compile(r"^(\d\d):(\d\d):(\d\d)\.(\d\d\d)")


def secs(line):
    m = TS.match(line)
    if not m:
        return None
    h, mi, s, ms = (int(x) for x in m.groups())
    return h * 3600 + mi * 60 + s + ms / 1000.0


fire = None          # when the forgery was sent
host_left = None     # when the original host first said it was leaving
refused = False
effects = []         # (time, line) of acceptance lines

for path in glob.glob(os.path.join(logdir, "okplay-*.log")):
    if os.path.getmtime(path) < since:
        continue
    for line in open(path, encoding="utf-8", errors="replace"):
        t = secs(line)
        if "TEST ROGUE" in line and attack in line and t is not None:
            fire = t if fire is None else min(fire, t)
        if "leaving the session" in line and t is not None:
            host_left = t if host_left is None else min(host_left, t)
        if re.search(REFUSED, line):
            refused = True
        if re.search(EFFECT, line) and t is not None:
            effects.append(t)

# the window: from the forgery until the first "leaving" (teardown). If we never saw the forgery,
# fall back to "any time" so a missing timestamp can't hide a real effect.
lo = fire if fire is not None else -1e9
hi = host_left if host_left is not None else 1e9
took_effect = any(lo <= t < hi for t in effects)

print("refused=%d took_effect=%d" % (int(refused), int(took_effect)))
