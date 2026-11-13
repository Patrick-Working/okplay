#!/usr/bin/env python3
"""boxgame_check.py - judges one boxgame run from its two logs (docs/BOX_METHOD.md 5.2), version 1.

    python boxgame_check.py BOXGAME_LOG [OKPLAY_LOG]

BOXGAME_LOG is boxgame.exe's own log (okplay/wsock/tests/boxgame.c), OKPLAY_LOG the okplay log of the same run
(logs/okplay-*.log next to the wsock32.dll; may be left out, the okplay rules then say SKIP).
Prints one line per rule: PASS / FAIL / WARN / SKIP with the reason, then "RESULT pass" or "RESULT fail".
Exit 0 = no FAIL, 1 = a FAIL, 2 = the logs could not be read.

Judged here (from log lines that exist today):
  front     rule 1  the stand-in saw no deactivation, no lost keyboard, no minimise; the front after the network step and at
                    the end is the game
  stall     rule 2  no "stall:" line in the okplay log
  usable    rule 2  each box answered its messages within 0.5 s of coming up (boxgame's watcher)
  one box   rule 3  never two box windows of the program visible at once
  clock     rule 4  the stand-in's join clock did not run out
  keys      rules 6, 7  no key reached the game while a box was open; keys typed (--keys) landed in the box's field
  put back  rule 8  pointer (showing, trap) and front the same before and after the network step
  screen          the game's DirectDraw object was wrapped by okplay (ddshow), the surface was never lost, frames kept
                    coming, and the display mode was put back
Not judged (no log line for it yet): see MISSING below, printed with each run.
"""
import os
import re
import sys

TS = r"(\d\d):(\d\d):(\d\d)\.(\d\d\d)"


def ms_of(line):
    m = re.match(TS, line)
    if not m:
        return None
    h, mi, s, ms = (int(x) for x in m.groups())
    return ((h * 60 + mi) * 60 + s) * 1000 + ms


def read(path):
    with open(path, "r", encoding="latin-1", errors="replace") as f:
        return [l.rstrip("\r\n") for l in f]


# what okplay does not log yet, but the checks of 5.2 need (reported, not invented here)
MISSING = [
    "dds_guard_end: guard off, pointer count given back on the game window's thread, SetForegroundWindow result",
    "okbox_cursor_end: pointer count and trap put back (okbox_cursor_begin/end log nothing)",
    "dds_show_gdi: a line when no DirectDraw object was found (the silent fallback, BOX_METHOD 6.5)",
    "na_key_proc: keys handed to the box (count) - 'keys never reached the game' is judged from boxgame's side only",
]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    try:
        bg = read(sys.argv[1])
    except OSError as e:
        print(f"cannot read {sys.argv[1]}: {e}")
        return 2
    ok_lines = None
    if len(sys.argv) > 2 and sys.argv[2]:
        try:
            ok_lines = read(sys.argv[2])
        except OSError as e:
            print(f"cannot read {sys.argv[2]}: {e}")
            return 2

    results = []

    def say(verdict, rule, why):
        results.append(verdict)
        print(f"{verdict:4}  {rule:9} {why}")

    summary = {}
    for l in bg:
        m = re.search(r"SUMMARY (.*)$", l)
        if m:
            summary = dict(kv.split("=", 1) for kv in m.group(1).split() if "=" in kv)
    if not summary:
        say("FAIL", "run", "boxgame wrote no SUMMARY line (it did not end by itself)")
    elif summary.get("result") != "ended":
        say("FAIL", "run", f"boxgame ended by {summary.get('result')} (the window thread was held, or no screen)")
    else:
        say("PASS", "run", f"ended by itself; {summary.get('frames')} frames; network step {summary.get('net')}")

    windowed = any("windowed" in l for l in bg[:1])
    # ---- front (rule 1)
    upto = next((i for i, l in enumerate(bg) if " SUMMARY " in l), len(bg))   # (after it: boxgame's own window going)
    since = next((i for i, l in enumerate(bg) if " game: window " in l), 0)   # (before it: the window being made, the mode set)
    bad = [l for l in bg[since:upto] if " msg: " in l and (" - deactivated" in l or " - lost the keyboard" in l or "MINIMISED" in l)]
    minim = [l for l in bg[since:upto] if "MINIMISED" in l]
    fronts = {}
    for l in bg:
        m = re.search(r"front: (before the network step|after the network step|at the end): (\S+)", l)
        if m:
            fronts[m.group(1)] = m.group(2)
    why = []
    if bad:
        why.append(f"{len(bad)} deactivation/focus/minimise message(s), first: {bad[0][13:]}")
    if minim:
        why.append("minimised")
    for k in ("after the network step", "at the end"):
        if k in fronts and fronts[k] != "us":
            why.append(f"front {k}: {fronts[k]}")
    if why:
        say("FAIL", "front", "; ".join(why) + (" (windowed run: a click elsewhere counts too)" if windowed else ""))
    else:
        say("PASS", "front", "no deactivation, no minimise; front after the step and at the end: " +
            ", ".join(f"{fronts.get(k, '?')}" for k in ("after the network step", "at the end")))

    # ---- stall (rule 2)
    if ok_lines is None:
        say("SKIP", "stall", "no okplay log")
    else:
        st = [l for l in ok_lines if "stall:" in l]
        say("FAIL" if st else "PASS", "stall", f"{len(st)} stall line(s)" + (f", first: {st[0]}" if st else ""))

    # ---- box usable (rule 2)
    ups = [l for l in bg if "box: up " in l]
    usable = [re.search(r"box: usable (\d+) ms after it came up \((\d+) ms after the network call began", l) for l in bg]
    usable = [(int(m.group(1)), int(m.group(2))) for m in usable if m]
    slow = [l for l in bg if "box: its thread took" in l]
    if not ups:
        say("SKIP", "usable", "no box came up in this run")
    elif len(usable) < len(ups):
        say("FAIL", "usable", f"{len(ups)} box(es) up, only {len(usable)} answered")
    else:
        worst = max(u[0] for u in usable)
        worst_call = max(u[1] for u in usable)
        v = "PASS" if worst < 500 and not slow else "FAIL"
        say(v, "usable", f"answered {worst} ms after coming up at worst (< 500); {worst_call} ms after the network call began"
            + (f"; later slow answers: {len(slow)}" if slow else ""))
        if worst_call >= 500:
            say("WARN", "usable", f"a box came {worst_call} ms after the game's network call began (the game waited that long)")
    for l in ok_lines or []:
        m = re.search(r"box: the (room list|waiting box) is up after (\d+) ms", l)
        if m and int(m.group(2)) >= 500:
            say("FAIL", "usable", f"okplay: {m.group(1)} up after {m.group(2)} ms")

    # ---- one box (rule 3)
    two = [l for l in bg if "FAIL two boxes open at once" in l]
    say("FAIL" if two else "PASS", "one box", f"at most {summary.get('boxes_max', '?')} box window(s) at once, "
        f"{summary.get('boxes_seen', '?')} box(es) in all")

    # ---- clock (rule 4)
    cl = [l for l in bg if "FAIL clock ran out" in l]
    c = summary.get("clock", "?")
    if c == "off":
        say("SKIP", "clock", "no --clock given")
    else:
        say("FAIL" if cl else "PASS", "clock", cl[0][13:] if cl else f"the join clock held ({c})")

    # ---- keys (rules 6, 7)
    kin = [l for l in bg if "KEY WHILE A BOX IS OPEN" in l]
    sent = [l for l in bg if "keys: typing" in l]
    notsent = [l for l in bg if "keys: not sent" in l]
    if kin:
        say("FAIL", "keys", f"{len(kin)} key message(s) reached the game while a box was open, first: {kin[0][13:]}")
    elif sent:
        say("PASS", "keys", "keys were typed while a box was up; none reached the game")
    else:
        say("PASS" if summary.get("keys_in_box", "0") == "0" else "FAIL", "keys", "no key reached the game while a box was open"
            + ("" if not notsent else " (synthetic keys not sent: " + notsent[0].split("keys: not sent - ")[-1] + ")"))
    m = re.search(r'keys: typing "([^"]*)"', "\n".join(sent))
    if m:
        typed = re.sub(r"\{[^}]*\}", "", m.group(1).split("{tab}")[0])
        fields = [re.search(r'box: field now "([^"]*)"', l) for l in bg]
        fields = [f.group(1) for f in fields if f]
        if typed:
            say("PASS" if typed in fields else "FAIL", "keys", f"typed \"{typed}\": the field held {fields[-3:] if fields else 'nothing'}")

    # ---- put back (rule 8)
    ptr = {}
    for l in bg:
        m = re.search(r"pointer: (before the network step|after the network step|at start|at the end): showing=(\S+) at \S+ clip=(\S+)", l)
        if m:
            ptr[m.group(1)] = (m.group(2), m.group(3))
    b, a = ptr.get("before the network step"), ptr.get("after the network step")
    if not b or not a:
        say("SKIP", "put back", "the network step did not finish (no pointer line after it)")
    else:
        why = []
        if b[0] != a[0]:
            why.append(f"pointer showing {b[0]} -> {a[0]}")
        if b[1] != a[1]:
            why.append(f"pointer trap {b[1]} -> {a[1]}")
        fb, fa = fronts.get("before the network step"), fronts.get("after the network step")
        if fb != fa:
            why.append(f"front {fb} -> {fa}")
        say("FAIL" if why else "PASS", "put back", "; ".join(why) if why else f"pointer (showing {a[0]}, trap {a[1]}) and front ({fa}) as before")
    if ok_lines is not None:
        hooks = [l for l in ok_lines if "box: hooks not set" in l]
        if hooks:
            say("FAIL", "put back", f"okplay: {hooks[0]}")
        op = [l for l in ok_lines if "box: opens (" in l]
        cl = [l for l in ok_lines if "box: closed (" in l and "hooks removed" in l]
        if not op and not cl:
            say("SKIP", "hooks", "this okplay does not log boxes opening and closing (older build)")
        elif len(op) != len(cl):
            say("FAIL", "hooks", f"{len(op)} box(es) opened, {len(cl)} closed with their hooks removed")
        else:
            way = re.search(r"thread \w+, (.*)\)", op[0])
            say("PASS", "hooks", f"{len(op)} box(es) opened and closed, hooks removed each time; the first: {way.group(1) if way else '?'}")

    # ---- screen
    wr = [l for l in bg if "ddraw: object" in l]
    if not wr:
        say("FAIL", "screen", "no DirectDraw object made")
    else:
        w = "wrapped" in wr[0] and "not wrapped" not in wr[0]
        prof = None
        for l in ok_lines or []:
            m = re.search(r"wsock: game profile (\S+)", l)
            if m:
                prof = m.group(1)
        if w:
            say("PASS", "screen", "okplay's ddshow wrapped the game's DirectDraw object")
        elif prof == "roadrash":
            say("WARN", "screen", "DirectDraw object not wrapped: okplay's screen code is only on for TCP games (profile roadrash)")
        else:
            say("FAIL", "screen", "DirectDraw object not wrapped by okplay (" + wr[0].split(" - ")[-1] + ")")
    lost = int(summary.get("surface_lost", "0") or 0)
    if lost:
        say("FAIL", "screen", f"the game's surface was lost {lost} time(s) (it lost the screen)")
    fib = int(summary.get("frames_in_box", "0") or 0)
    held = sum(int(m.group(1)) for l in ok_lines or [] for m in [re.search(r"ddshow: held (\d+) page flips", l)] if m)
    if held and not windowed:
        say("PASS", "screen", f"okplay held {held} page flip(s) of the game while a box was open (the box is not drawn over)")
    elif fib and not windowed:
        say("WARN", "screen", f"the game flipped {fib} frame(s) while a box was open (BOX_METHOD 6.9: the box may be covered;"
            " only a screen capture can tell)")
    if ok_lines is not None:
        fl = [l for l in ok_lines if "FlipToGDISurface" in l]
        if ups and not fl and not windowed:
            say("FAIL", "screen", "a box came up but okplay never switched to the GDI page (no FlipToGDISurface line)")
    if not any("display mode put back" in l for l in bg):
        say("FAIL", "screen", "the display mode was not put back")
    say("SKIP", "visible", "needs a screen capture while the box is open (not in the logs)")

    print("MISSING (okplay log lines the checks above would need):")
    for m in MISSING:
        print("  - " + m)
    failed = "FAIL" in results
    print("RESULT " + ("fail" if failed else "pass"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
