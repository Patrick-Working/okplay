#!/usr/bin/env python3
"""rr_bench.py - automated Road Rash races between two Windows 98 VMs, for measuring okplay wsock.

Drives the 1996 CD version on two okagent machines (see okagent/vmctl.py): installs the shim and an
okplay.ini (LAN Play, no password boxes), starts both games, hosts on one and joins from the other through
the game's own menus, then runs one race per test setting. The test knobs (test_delay_ms, test_jitter_ms,
test_blackout_ms, test_blackout_every_ms, test_loss_pct) are rewritten in okplay.ini between races; the shim
re-reads them every 2 s. Each machine's log gets a "race from ..." line every 10 s (stamp gaps per sender);
those lines are collected per race and printed as a table.

    python rr_bench.py --host WIN98B-... --join WIN98C-... [--dll wsock32.dll] [--secs 50] [--only NAME]

Progress is checked through the shim's own log lines, not screenshots: "hosting room", "found room",
"joining room" and "the game closed its room" (the host closes its room when a race starts). Menu clicks
are positions on the game's 640x480 screen. The game is always left through its own windows (never
killed): repeated force-kills on Windows 98 stopped its 33 ms timer until a reboot.
"""
import argparse, os, re, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
VMCTL = os.path.normpath(os.path.join(HERE, "..", "..", "..", "okagent", "vmctl.py"))
GAME_DIR = r"C:\ElectronicArts\RoadRash"
EXE = GAME_DIR + r"\ROADRASH.EXE"

BASE_INI = {
    "server": "https://oldkid.net/aoe2net/okplay.php",
    "loglevel": "3",
    "wsock_mode": "lan",
    "use_password": "off",
    "wsock_trace": "0",
    "rr_novideo": "1",       # lab build: no videos (the race-end ones took the game off screen)
    "brand": "Oldkid.net Online Play",
    "online_name": "Oldkid.net Online Play",
    "lan_name": "Oldkid.net LAN Play",
}

# name, knobs (applied on both machines: each one delays what it receives)
SETTINGS = [
    ("lan", {}),
    ("d40", {"test_delay_ms": 40}),
    ("d40j20", {"test_delay_ms": 40, "test_jitter_ms": 20}),
    ("d80j40l1", {"test_delay_ms": 80, "test_jitter_ms": 40, "test_loss_pct": 1}),
    ("d100l3", {"test_delay_ms": 100, "test_loss_pct": 3}),
    ("d150l5", {"test_delay_ms": 150, "test_loss_pct": 5}),
    ("bo300", {"test_blackout_ms": 300, "test_blackout_every_ms": 5000}),
    ("bo500", {"test_blackout_ms": 500, "test_blackout_every_ms": 5000}),
    ("bo1000", {"test_blackout_ms": 1000, "test_blackout_every_ms": 6000}),
    ("bo2000", {"test_blackout_ms": 2000, "test_blackout_every_ms": 8000}),
]


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


class GameLost(Exception):
    """The game is not on screen (minimized or gone): never click or type then - it would land on the
    Windows desktop (a click at a menu position once selected a desktop icon)."""


class VM:
    def __init__(self, agent):
        self.a = agent

    def ctl(self, *args, check=False):
        r = subprocess.run([sys.executable, VMCTL, "--agent", self.a] + [str(x) for x in args],
                           capture_output=True, text=True, timeout=120)
        if check and r.returncode:
            raise RuntimeError(f"{self.a} {args}: {r.stdout}{r.stderr}")
        return r.stdout

    def guard(self):
        ws = self.windows()
        game = [w for w in ws if w["cls"] == "RashRoot"]
        if not game or game[0]["rect"].startswith("3000,"):
            raise GameLost(f"{self.a}: the game is {'minimized' if game else 'not running'}")

    def click(self, x, y, wait=5.0):
        self.guard()
        self.ctl("click", x, y)
        time.sleep(wait)

    def key(self, k, hold=0.25, wait=0.8):
        self.guard()
        self.ctl("key", k, "down")
        time.sleep(hold)
        self.ctl("key", k, "up")
        time.sleep(wait)

    def windows(self):
        out = []
        for line in self.ctl("windows", "all").splitlines():
            f = line.split("\t")
            if len(f) >= 5:
                out.append({"h": f[0], "pid": f[1], "rect": f[2], "cls": f[3], "title": f[4]})
        return out

    def dialogs(self):
        return [(w["h"], w["title"]) for w in self.windows() if w["cls"] == "#32770"]

    def game_pid(self):
        for w in self.windows():
            if w["cls"] == "RashRoot":
                return w["pid"]
        return None

    def minimized(self):
        return any(w["cls"] == "RashRoot" and w["rect"].startswith("3000,") for w in self.windows())

    def close_dialogs(self, game_only=False):
        """Windows' own boxes (logon, ...) are closed only while the game is not running: closing one
        takes the focus, and Road Rash shows only black after losing it."""
        pid = self.game_pid() if game_only else None
        for w in self.windows():
            if w["cls"] == "#32770" and (not game_only or w["pid"] == pid):
                self.ctl("close", w["h"])
                time.sleep(1)

    def running(self):
        return "ROADRASH" in self.ctl("ps").upper()

    def script(self, text):
        """Lab build: keys played from inside the game (rr_macro.txt). Used in races: keys sent from
        outside (the agent's SendInput) at a race's end knocked the host's game off the screen."""
        tmp = os.path.join(tempfile.gettempdir(), f"rr_macro_{self.a}.txt")
        with open(tmp, "w") as f:
            f.write(text)
        self.put(tmp, GAME_DIR + r"\rr_macro.txt")

    def put(self, local, remote):
        self.ctl("put", local, remote, check=True)

    def newest_log(self):
        names = [l.split("\t")[2].strip() for l in self.ctl("ls", GAME_DIR + r"\logs\*").splitlines()
                 if l.startswith("file\t")]
        if not names:
            return ""
        tmp = os.path.join(tempfile.gettempdir(), f"rr_bench_{self.a}.log")
        self.ctl("get", GAME_DIR + "\\logs\\" + sorted(names)[-1], tmp)
        try:
            return open(tmp, encoding="latin-1").read()
        except OSError:
            return ""

    def wait_log(self, pattern, secs, after=0):
        t0 = time.time()
        while time.time() - t0 < secs:
            text = self.newest_log()
            if len(re.findall(pattern, text)) > after:
                return True
            time.sleep(2)
        return False

    def write_ini(self, knobs):
        d = dict(BASE_INI)
        d.update({k: str(v) for k, v in knobs.items()})
        for k in ("test_delay_ms", "test_jitter_ms", "test_blackout_ms", "test_blackout_every_ms", "test_loss_pct"):
            d.setdefault(k, "0")
        tmp = os.path.join(tempfile.gettempdir(), f"rr_bench_{self.a}.ini")
        with open(tmp, "w", newline="\r\n") as f:
            f.write("[okplay]\n" + "".join(f"{k}={v}\n" for k, v in d.items()))
        self.put(tmp, GAME_DIR + r"\okplay.ini")


def start_game(vm):
    for _ in range(3):
        for _ in range(3):        # Windows' boxes first (the logon box comes a while after boot)
            vm.close_dialogs()
            time.sleep(2)
        vm.ctl("run", EXE)
        time.sleep(14)
        try:
            vm.close_dialogs(game_only=True)   # the game's own DirectSound warning
            vm.click(322, 281, 8)     # past the first screen
            vm.key("esc", wait=5)
            vm.key("esc", wait=5)     # past the intro and credits: main menu
            vm.guard()
            return True
        except GameLost:
            log(f"{vm.a}: the game lost the focus (black screen) - closing and starting it again")
        close_game(vm)
    return False


def close_game(vm, disconnect=False):
    """Through its window (WM_CLOSE), never killed. Closed while still connected (lobby, race) the game's
    process does not exit (seen on Windows 98 and 10; under investigation), so disconnect first."""
    vm.close_dialogs(game_only=True)
    if disconnect:
        try:
            vm.click(385, 466, 5)             # lobby: Disconnect
        except GameLost:
            pass
    vm.ctl("close", "Road Rash")
    for _ in range(20):
        if not vm.running():
            return True
        time.sleep(2)
    return False


def to_main_menu(vm):
    for _ in range(4):
        vm.close_dialogs(game_only=True)
        vm.key("esc", wait=2)


def host(vm):
    for attempt in range(3):
        n = len(re.findall("hosting room", vm.newest_log()))
        vm.click(396, 433)                    # Mano-a-Mano
        vm.click(40, 416)                     # Host
        vm.click(366, 139)                    # Level 1
        vm.click(366, 139)                    # Disable cars
        vm.click(366, 173)                    # TCP/IP
        if any("Warning" in t for _, t in vm.dialogs()):   # landed on Modem: back out and retry
            vm.close_dialogs(game_only=True)
            to_main_menu(vm)
            continue
        vm.click(385, 175 if AI else 139)     # Limited A.I. bikers / Humans only
        vm.key("enter", wait=4)               # rider
        if vm.wait_log("hosting room", 20, after=n):
            return True
        log(f"{vm.a}: not hosting yet (attempt {attempt + 1}), back to the menu")
        to_main_menu(vm)
    return False


def join(vm):
    for attempt in range(3):
        n = len(re.findall("joining room", vm.newest_log()))
        vm.click(396, 433)                    # Mano-a-Mano
        vm.click(40, 440)                     # Join
        vm.click(366, 173, 6)                 # TCP/IP
        if any("Warning" in t for _, t in vm.dialogs()):
            vm.close_dialogs(game_only=True)
            to_main_menu(vm)
            continue
        if not any("Pick from" in t for _, t in vm.dialogs()):
            log(f"{vm.a}: no game list (attempt {attempt + 1})")
            to_main_menu(vm)
            continue
        vm.wait_log("found room", 15)
        time.sleep(3)
        vm.click(77, 175, 1)                  # the first game in the list
        vm.click(178, 332, 6)                 # OK
        if vm.wait_log("joining room", 15, after=n):
            vm.key("enter", wait=4)           # rider
            return True
        to_main_menu(vm)
    return False


def start_race(h, j):
    """Started when the lab build logs "race drawing started" (remote riders drawn again)."""
    time.sleep(6)                             # the lobby takes a moment after the last race
    n = len(re.findall("race drawing started", h.newest_log()))
    for vm in (h, j):
        vm.guard()
        vm.ctl("move", 75, 467)               # the lobby ignores a click without a hover first
    time.sleep(1.5)
    for vm in (h, j):
        vm.click(75, 467, 3)                  # Bulletin Board
    for vm in (h, j):
        vm.click(58, 433, 1)                  # The City
    return h.wait_log("race drawing started", 30, after=n)


def ride(vms, secs):
    time.sleep(4)                             # countdown
    for vm in vms:
        vm.guard()
        vm.script(f"HOLD UP {secs * 1000}\n")
    t0 = time.time()
    while time.time() - t0 < secs:
        time.sleep(5)
        for vm in vms:
            vm.guard()


def cancel_go_home(vm):
    """Escape in the lobby opens the Restroom; a second one asks "Go Home?" - cancel (WM_CLOSE) and leave
    the Restroom through Done."""
    for h, title in vm.dialogs():
        if "Go Home" in title:
            vm.ctl("close", h)
            time.sleep(2)
            vm.key("esc", wait=2)


def leave_race(h, j):
    """Both players quit with Escape - in a race the game reads it from its window messages, so it is posted
    there from inside the game. The postcard ("Busted!") and the race results after it go on with a click;
    a click in the middle of the lobby's picture does nothing, so a spare one is harmless. Back in the lobby
    the host lists its room again ("hosting room"), the signal to go on."""
    n = len(re.findall("hosting room", h.newest_log()))
    for vm in (j, h):                        # both quit (the host alone only leaves the race itself)
        vm.guard()
        vm.script("POST ESC 500\n")
        time.sleep(3)
    t0 = time.time()
    while True:                              # a black loading screen can hold a machine back for a while
        time.sleep(3)
        for vm in (h, j):
            # a host left alone watching the race ("You are the master") needs another Escape; the lab
            # posts this one only while a race runs, so it never reaches the lobby (the Restroom)
            vm.script("RACEPOST ESC 300\n")
            vm.click(320, 240, 1)
        if time.time() - t0 > 8 and h.wait_log("hosting room", 2, after=n):
            break
        if time.time() - t0 > 90:
            for vm in (h, j):
                log(f"{vm.a}: not back in the lobby: {snap(vm, 'leave')}")
            break
    for _ in range(3):                       # the joiner may be a screen behind
        time.sleep(3)
        for vm in (h, j):
            vm.click(320, 240, 1)
    for vm in (h, j):
        cancel_go_home(vm)


def apply_knobs(vms, knobs):
    """Writes the test knobs and waits for each game's log to confirm them ("TEST KNOBS now: ...")."""
    if not any(int(v) for x, v in knobs.items() if x.startswith("test_")):
        for vm in vms:                       # lab switches only: the shim reads them with the same 2 s reload
            vm.write_ini(knobs)
        time.sleep(3)
        return True
    k = {x: knobs.get(x, 0) for x in ("test_delay_ms", "test_jitter_ms", "test_loss_pct", "test_blackout_ms",
                                       "test_blackout_every_ms")}
    line = re.escape(f"TEST KNOBS now: delay {k['test_delay_ms']} ms + 0..{k['test_jitter_ms']} ms jitter, "
                     f"{k['test_loss_pct']}% loss, blackout {k['test_blackout_ms']} ms every "
                     f"{k['test_blackout_every_ms']} ms")
    ok = True
    for vm in vms:
        n = len(re.findall(line, vm.newest_log()))
        for _ in range(2):
            vm.write_ini(knobs)
            if vm.wait_log(line, 8, after=n):
                break
        else:
            log(f"{vm.a}: the test knobs were not taken up")
            ok = False
    return ok


def snap(vm, tag):
    path = os.path.join(tempfile.gettempdir(), f"rr_bench_{tag}_{vm.a}.png")
    vm.ctl("shot", path)
    return path


RACE_RE = re.compile(r"^(\d\d:\d\d:\d\d)\.\d+ .*race from (\w+), last 10 s: (\d+) packets, (\d+) stamp steps; gap median (\d+), "
                     r"p99 (\d+)\+?, max (\d+) steps, (\d+) over 20; stamps run (\d+)\.(\d) steps/s; longest wait (\d+) ms", re.M)


def secs(hms):
    h, m, s = (int(x) for x in hms.split(":"))
    return h * 3600 + m * 60 + s


def steady(text, regex, settle=12):
    """Rows of the race that just ran, on that machine's own clock (the VMs' clocks differ): from its
    last "race drawing started" line plus `settle` seconds, so race starts and the time between races
    are not counted."""
    starts = re.findall(r"^(\d\d:\d\d:\d\d)\.\d+ .*race drawing started", text, re.M)
    if not starts:
        return []
    t0 = secs(starts[-1]) + settle
    return [m for m in regex.finditer(text) if secs(m.group(1)) >= t0]


RACE_RE = re.compile(r"^(\d\d:\d\d:\d\d)\.\d+ .*race from (\w+), last 10 s: (\d+) packets, (\d+) stamp steps; gap median (\d+), "
                     r"p99 (\d+)\+?, max (\d+) steps, (\d+) over 20; stamps run (\d+)\.(\d) steps/s; longest wait (\d+) ms", re.M)
AI = False
HIST_RE = re.compile(r"^(\d\d:\d\d:\d\d)\.\d+ .*lab history, last 10 s: (\d+) updates, (\d+) mix-ups", re.M)
LAB_RE = re.compile(r"^(\d\d:\d\d:\d\d)\.\d+ .*lab draw rider (\d+), last 10 s: (\d+) draws; dt median (\d+), max (\d+), "
                    r"over 20 (\d+); movement avg (\d+) max (\d+); jumps (\d+) \(change > \d+\); change median 2\^(\d+) "
                    r"p99 2\^(\d+) max (\d+)", re.M)


OFF_RE = re.compile(r"^(\d\d:\d\d:\d\d)\.\d+ .*lab quality rider .*clock offset (-?\d+\.\d) steps", re.M)
QUAL_RE = re.compile(r"^(\d\d:\d\d:\d\d)\.\d+ .*lab quality rider (\d+), last 10 s: smooth (\d+) steps median 2\^(\d+) "
                     r"p99 2\^(\d+) max (\d+); accuracy (\d+) checks median 2\^(\d+) p99 2\^(\d+) max (\d+)", re.M)


def summarize(name, texts):
    rows = [r for t in texts for r in steady(t, RACE_RE)]
    lab = [r for t in texts for r in steady(t, LAB_RE) if int(r.group(3)) >= 50]
    hist = [r for t in texts for r in steady(t, HIST_RE)]
    if not rows:
        return f"{name:22s}  no steady race windows"
    out = (f"{name:22s}  win {len(rows):2d}  gap p99 {max(int(r.group(6)) for r in rows):3d} max {max(int(r.group(7)) for r in rows):4d}"
           f" over20 {sum(int(r.group(8)) for r in rows):3d}  wait max {max(int(r.group(11)) for r in rows):5d} ms")
    if lab:
        draws = sum(int(r.group(3)) for r in lab)
        out += (f" | drawn: dt max {max(int(r.group(5)) for r in lab):4d} over20 {sum(int(r.group(6)) for r in lab):4d}/{draws}"
                f"  change p99 2^{max(int(r.group(11)) for r in lab):2d} max {max(int(r.group(12)) for r in lab):7d}"
                f"  jumps {sum(int(r.group(9)) for r in lab):4d}")
    qual = [r for t in texts for r in steady(t, QUAL_RE) if int(r.group(3)) >= 50]
    if qual:
        sm = sorted(int(r.group(5)) for r in qual)
        acc = [r for r in qual if int(r.group(7)) >= 20]
        out += f" | smooth p99 2^{sm[len(sm) // 2]:2d} (worst 2^{sm[-1]:2d})"
        if acc:
            am = sorted(int(r.group(8)) for r in acc)
            ap = sorted(int(r.group(9)) for r in acc)
            out += f", error median 2^{am[len(am) // 2]:2d} p99 2^{ap[len(ap) // 2]:2d} (worst 2^{ap[-1]:2d})"
    if hist:
        out += f" | history mix-ups {sum(int(r.group(3)) for r in hist)}/{sum(int(r.group(2)) for r in hist)}"
    offs = []                                # the lab's clock-offset estimate, each side (host first)
    for t in texts:
        o = sorted(float(m.group(2)) for m in steady(t, OFF_RE))
        offs.append(f"{o[len(o) // 2]:+.1f}" if o else "?")
    if any(o != "?" for o in offs):
        out += f" | clock offset {' / '.join(offs)} steps"
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--join", required=True)
    ap.add_argument("--dll", default=os.path.join(HERE, "..", "bin", "lab", "wsock32.dll"),
                    help="the shim to test; the default lab build (build.sh with OKW_LAB_SRC) has the in-game key "
                         "scripts this bench uses in races")
    ap.add_argument("--secs", type=int, default=50)
    ap.add_argument("--only", default="")
    ap.add_argument("--trace", action="store_true", help="wsock_trace=1 (first race packets dumped)")
    ap.add_argument("--set", default="", help="extra okplay.ini keys for every race, e.g. rr_clamp=1")
    ap.add_argument("--ai", action="store_true", help="host picks Limited A.I. bikers")
    ap.add_argument("--repeat", type=int, default=1, help="run every setting this many times")
    ap.add_argument("--variants", default="", help="name:k=v,k=v;name:... - every setting is raced once per "
                    "variant, one after the other in the same session (fairer than a session per variant)")
    a = ap.parse_args()
    global AI
    AI = a.ai
    if a.trace:
        BASE_INI["wsock_trace"] = "1"
    for kv in filter(None, a.set.split(",")):
        k, v = kv.split("=", 1)
        BASE_INI[k.strip()] = v.strip()
    h, j = VM(a.host), VM(a.join)
    settings = [s for s in SETTINGS if not a.only or s[0] in a.only.split(",")]
    if a.variants:
        vs = []
        for v in filter(None, a.variants.split(";")):
            vn, _, kv = v.partition(":")
            vs.append((vn, dict(x.split("=", 1) for x in filter(None, kv.split(",")))))
        settings = [(f"{n}/{vn}", {**k, **vk}) for n, k in settings for vn, vk in vs]
    if a.repeat > 1:
        settings = [(f"{n}#{i + 1}", k) for i in range(a.repeat) for n, k in settings]

    for vm in (h, j):
        if vm.running():
            sys.exit(f"{vm.a}: Road Rash is already running - close it first")
        vm.put(os.path.abspath(a.dll), GAME_DIR + r"\WSOCK32.DLL")
        vm.write_ini(settings[0][1])
    def session():
        log("starting both games")
        for vm in (h, j):
            if not start_game(vm):
                raise GameLost(f"{vm.a}: the game would not start cleanly")
        if not host(h):
            raise GameLost("host never got to hosting")
        log("host is hosting")
        if not join(j):
            raise GameLost("joiner never joined")
        log("joined")

    restarts = 0
    session()
    results = []
    for name, knobs in settings:
        try:
            for vm in (h, j):                  # the knobs act only while racing: in the lobby they
                vm.write_ini({})                # lost the race-start handshake (dropout settings)
            time.sleep(3)
            if not start_race(h, j):
                log(f"{name}: race did not start - trying once more (screens: {snap(h, name)} {snap(j, name)})")
                for vm in (h, j):
                    cancel_go_home(vm)
                if not start_race(h, j):
                    results.append(f"{name:22s}  race did not start")
                    continue
            if not apply_knobs((h, j), knobs):
                results.append(f"{name:22s}  test knobs not taken up - no result")
                ride((h, j), a.secs)
                for vm in (h, j):
                    vm.write_ini({})
                leave_race(h, j)
                continue
            log(f"{name}: racing {a.secs} s with {knobs or 'no test knobs'}")
            ride((h, j), a.secs)
            for vm in (h, j):
                vm.write_ini({})
            time.sleep(11)                         # one more 10 s report
            results.append(summarize(name, (h.newest_log(), j.newest_log())))
            log(results[-1])
            leave_race(h, j)
        except GameLost as e:
            log(f"{name}: {e} (screens: {snap(h, name + '_lost')} {snap(j, name + '_lost')}) - restarting both games")
            if not any(r.startswith(name) for r in results):
                results.append(f"{name:22s}  game lost: {e}")
            restarts += 1
            for vm in (h, j):
                if not close_game(vm):
                    sys.exit(f"{vm.a}: the game does not close - restart that VM, then run again")
            if restarts > 3:
                break
            session()

    for vm in (h, j):
        vm.write_ini({})
    for vm in (j, h):
        if not close_game(vm, disconnect=True):
            log(f"{vm.a}: the game did not exit - restart that VM before the next run")
    print("\n" + "\n".join(results))


if __name__ == "__main__":
    main()
