#!/usr/bin/env python3
"""tcp_run.py - runs tcpsim scenarios on this PC, each as separate processes (host, then joiners), and prints a table.

    python tcp_run.py [--mode real|shim|both] [--only a,b,...] [--env K=V ...] [-v] [--list]

real: tcpsim.exe alone in a temporary folder - Windows' own TCP (the reference).
shim: tcpsim.exe + okplay wsock's wsock32.dll + an okplay.ini in a temporary folder, OKP_WSOCK_GAME=nfs3, LAN Play
      (Online Play with a local room-server stand-in, server/rdv_standin.py on port 8790, for the *_online scenarios): every stream
      socket is virtual and travels over okplay's reliable lane, each process its own okplay peer.
Every scenario says what each side must print (RESULT ok, or e.g. a refusal); a few are shim-only (passwords, builds,
okplay's own join rules). Boxes never show: the test answers (OKP_TEST_*) are always set, and cancel by default.
Uses ../bin/tcpsim.exe and ../bin/wsock32.dll (build.sh builds both). No game is started; nothing makes sound.
Known builds: a scenario with known="modern" (or --build modern|oem|cd for every scenario) runs as a build of the game
whose own state okplay reads (nfs3_fix.c): OKP_TEST_EXEBUILD names the build, TCPSIM_GAMESTATE makes tcpsim keep the
game's two values at that build's addresses. Without it the build is not known and okplay's listen-again rule decides.
TCP_RUN_PHP_SERVER=<path to an okplay.php> runs the *_online scenarios against that script under "php -S" (TCP_RUN_PHP
names the php program) instead of the stand-in - e.g. the okplay.php of a released pack, to see old servers still work.
"""
import argparse, os, re, shutil, socket, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.normpath(os.path.join(HERE, "..", "bin"))
STANDIN = os.path.normpath(os.path.join(HERE, "..", "..", "server", "rdv_standin.py"))
EXTRA_ENV = {}
DLLS = {"host": None, "join": None}
STANDIN_PORT = 8790   # our own room-server stand-in (never 8787: another stand-in may run there on purpose)
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


def S(*steps):   # tcpsim script: what a player does, step by step (tcpsim.c "scripts"); options may come among them
    return ["script", "--port", "9803", "--menu-port", "{menuport}"] + list(steps)


LO = "{lo}:9803"
# known builds: name -> (fingerprint for OKP_TEST_EXEBUILD, tcpsim's TCPSIM_GAMESTATE)
BUILDS = {"modern": ("240a083a78ac3b83", "modern"), "oem": ("b390be67925b2ad0", "modern"), "cd": ("53aa0dfcf438788d", "cd")}
KNOWN = {"all": None}   # --build: every scenario as that build
PW_ASKED = r"password: (none - an open game|this game needs a password|not asking)"   # one host password question
NOTICE_NO_ANSWER = "No answer from the host"   # (the notice's sentence, as okplay logs it)
FULL_REFUSED = r"refused \(the game is full: 2 of 2 players\)"   # the host's log when a room of 2 is full


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
    # the Join box's pre-filled "oldkid.net" (and "okplay"): okplay's room list, never a DNS lookup or a connect there
    "byname_oldkid": dict(host=H(), joins=[J("OldKid.net:9803")], ini={"wsock_join": ""}, env_join={"OKP_TEST_JOIN_PICK": "first"},
                          modes=("shim",), checks=[("log", "join1", '"OldKid.net" - the game gets 10.0.0.1'),
                                                   ("nolog", "join1", "doesn't resolve")]),
    # a known build: the Join box opens pre-filled with "oldkid.net" (nfs3_fix.c); Ok gives the room list
    "joinbox_prefill": dict(host=S("menu", "host", "lobby:1", "left:1"), joins=[S("menu", "wait:1500", "joinbox", "wait:500", "leave")],
                            ini={"wsock_join": ""}, env_join={"OKP_TEST_JOIN_PICK": "first"}, modes=("shim",), known="modern",
                            checks=[("log", "join1", "Join box is pre-filled"), ("log", "join1", '"oldkid.net" - the game gets 10.0.0.1')]),
    "joinbox_prefill_cd": dict(host=S("menu", "host", "lobby:1", "left:1"), joins=[S("menu", "wait:1500", "joinbox", "wait:500", "leave")],
                               ini={"wsock_join": ""}, env_join={"OKP_TEST_JOIN_PICK": "first"}, modes=("shim",), known="cd",
                               checks=[("log", "join1", "Join box is pre-filled"), ("log", "join1", '"oldkid.net" - the game gets 10.0.0.1')]),
    # okplay.ini nfs3_join_prefill=0: the box stays as the game left it (empty here)
    "joinbox_prefill_off": dict(host=S("menu", "host", "wait:6000"), joins=[S("menu", "wait:1500", "joinbox")],
                                ini={"wsock_join": "", "nfs3_join_prefill": "0"}, modes=("shim",), known="modern",
                                expect_shim={"join1": r"^RESULT fail step 3 \(joinbox\): the Join box is empty", "host": ANY},
                                checks=[("nolog", "join1", "pre-filled")]),
    # the game remembered an address from its last run: with okplay on, the box still opens with "oldkid.net"
    "joinbox_remembered": dict(host=S("menu", "host", "lobby:1", "left:1"), joins=[S("menu", "wait:1500", "joinbox", "wait:500", "leave")],
                               ini={"wsock_join": ""}, env_join={"OKP_TEST_JOIN_PICK": "first", "TCPSIM_JOINTEXT": "192.168.7.3"},
                               modes=("shim",), known="modern",
                               checks=[("log", "join1", "Join box is pre-filled"), ("log", "join1", '"oldkid.net" - the game gets 10.0.0.1')]),
    # the game remembered okplay's "oldkid.net" and now runs its own TCP/IP (wsock_mode=original): the text comes out,
    # so Ok never sends the game toward the real site
    "joinbox_original": dict(host=S("menu", "host", "wait:6000"), joins=[S("menu", "wait:1500", "joinbox")],
                             ini={"wsock_join": ""}, env_join={"OKP_WSOCK_MODE": "original", "TCPSIM_JOINTEXT": "oldkid.net"},
                             modes=("shim",), known="modern",
                             expect_shim={"join1": r"^RESULT fail step 3 \(joinbox\): the Join box is empty", "host": ANY},
                             checks=[("log", "join1", "taken out of the Join box"), ("nolog", "join1", "pre-filled")]),
    # ... and an address of the player's own stays as it is for the game's own TCP/IP
    "joinbox_original_keeps": dict(host=S("menu", "host", "wait:6000"), joins=[S("menu", "wait:1500")],
                                   ini={"wsock_join": ""}, env_join={"OKP_WSOCK_MODE": "original", "TCPSIM_JOINTEXT": "192.168.7.3"},
                                   modes=("shim",), known="modern", checks=[("nolog", "join1", "Join box")]),
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
                              env_join={"OKP_TEST_EXEBUILD": "53aa0dfcf438788d"}, modes=("shim",), kill_host=True,
                              expect_shim={"join1": r"^RESULT fail refused", "host": ANY}),
    "build_diff_join": dict(host=H(), joins=[J()], env_host={"OKP_TEST_EXEBUILD": "240a083a78ac3b83"},
                            env_join={"OKP_TEST_EXEBUILD": "53aa0dfcf438788d", "OKP_TEST_BUILD_ANSWER": "join"}, modes=("shim",)),
    # C.8: the host process ends mid-race without closing anything
    "diesoft":    dict(host=H("--die-at", "150", "--die", "soft", frames=600), joins=[J("{lo}:9803", "--expect-loss", "1500")],
                       expect={"host": ANY}),
    "diehard":    dict(host=H("--die-at", "150", "--die", "hard", frames=600),
                       joins=[J("{lo}:9803", "--expect-loss", "18000", "--secs", "60")], expect={"host": ANY}),
    # Menu paths, as steps a player takes in one run of the game (tcpsim script; okplay only). The game makes the same
    # socket calls for Host Game as for entering the TCP menu again with its listener still open.
    # 1. a joiner goes back to Connect Players and into the TCP menu again before joining
    "menu_again": dict(host=S("menu", "host", "lobby:1", "left:1"),
                       joins=[S("menu", "back", "menu", "wait:1500", "join:" + LO, "wait:500", "leave")], modes=("shim",)),
    # 5. the same with the room server: once the joiner is in, the list shows the host's room only
    "menu_again_online": dict(host=S("menu", "host", "lobby:1", "left:1"),
                              joins=[S("menu", "back", "menu", "wait:4000", "say:rooms_at_menu", "wait:1500", "join:" + LO,
                                       "say:rooms_joined", "wait:500", "leave")],
                              online=True, modes=("shim",), checks=[("rooms", "rooms_joined", 1)]),
    # (the same family: a PC that hosted, backed out and joins someone else)
    "host_then_join": dict(host=S("menu", "host", "lobby:1", "left:1"),
                           joins=[S("menu", "host", "wait:2000", "close", "back", "menu", "wait:1500", "join:" + LO,
                                    "wait:500", "leave")], modes=("shim",), may_host=("join1",)),
    # 2. wrong password, back to Connect Players, into the TCP menu again, the right password - one run
    "pw_retry":   dict(host=S("menu", "host", "lobby:1", "left:1"), env_host={"OKP_TEST_HOST_PASSWORD": "lap3"},
                       joins=[S("menu", "wait:1500", "joinfail:" + LO, "back", "menu", "wait:1500", "join:" + LO, "wait:500", "leave")],
                       env_join={"OKP_TEST_JOIN_PASSWORD": "guess|lap3"}, modes=("shim",),
                       checks=[("log", "host", "turned away \\(wrong password\\)")]),
    # 3. the host's PC freezes while it should check the password: the player is told, nothing more goes to that
    #    host, and joining again works once the host is back
    "pw_noanswer": dict(host=S("menu", "host", "lobby:1", "left:1"), env_host={"OKP_TEST_HOST_PASSWORD": "lap3"},
                        joins=[S("menu", "wait:2500", "say:freeze", "wait:1000", "joinfail:" + LO, "wait:10000", "join:" + LO, "wait:500", "leave")],
                        env_join={"OKP_TEST_JOIN_PASSWORD": "lap3|lap3"}, modes=("shim",),
                        freeze=[dict(role="host", on="EVENT say freeze", secs=22)],
                        checks=[("log", "join1", NOTICE_NO_ANSWER),
                                ("between", "join1", "no answer from the host", "goes to host", r"peer [0-9a-f]+: new")]),
    # 4. use_password=ask: entering the TCP menu again must not ask for a host password (a known build: the game's
    #    own state tells Host Game from a menu re-entry), nor list a room
    "pw_ask_menu": dict(host=S("menu", "host", "lobby:1", "left:1"),
                        joins=[S("menu", "back", "menu", "wait:1500", "join:" + LO, "wait:500", "leave")],
                        ini={"use_password": "ask"}, modes=("shim",), known="modern",
                        checks=[("nolog", "join1", PW_ASKED), ("nolog", "join1", "hosting room|this PC hosts"),
                                ("log", "join1", "game state: net 0, server 1 - ready, not hosting"),
                                ("log", "host", "game state: net 3, server 1 - Host Game: this PC hosts")]),
    "pw_ask_menu_cd": dict(host=S("menu", "host", "lobby:1", "left:1"),
                           joins=[S("menu", "back", "menu", "wait:1500", "join:" + LO, "wait:500", "leave")],
                           ini={"use_password": "ask"}, modes=("shim",), known="cd",
                           checks=[("nolog", "join1", PW_ASKED), ("nolog", "join1", "hosting room|this PC hosts"),
                                   ("log", "join1", "Need for Speed III 1998 CD")]),
    # 4b. the same on a build okplay doesn't know: the listen-again rule makes each re-entry a candidate host, but the
    #     password is asked once per okplay session there, not at every re-entry
    "pw_ask_menu_unknown": dict(host=S("menu", "host", "lobby:1", "left:1"),
                                joins=[S("menu", "back", "menu", "wait:1500", "back", "menu", "wait:1500", "join:" + LO,
                                         "wait:500", "leave")], modes=("shim",),
                                checks=[("count", "join1", PW_ASKED, 1), ("log", "join1", "build not known: the listen-again rule"),
                                        ("nolog", "join1", "the hosted game is over")]),
    # 4c. a known build: menu re-entries, then Host Game - one password question, one room, the password applies
    "known_host_after_menu": dict(host=S("menu", "back", "menu", "wait:1500", "back", "menu", "wait:500", "host", "lobby:1", "left:1"),
                                  joins=[S("menu", "wait:6000", "join:" + LO, "wait:500", "leave")], known="modern",
                                  env_host={"OKP_TEST_HOST_PASSWORD": "lap3"}, env_join={"OKP_TEST_JOIN_PASSWORD": "lap3"},
                                  modes=("shim",), checks=[("count", "host", PW_ASKED, 1), ("count", "host", "hosting room", 1),
                                                           ("log", "host", "let in \\(password matched\\)")]),
    # 5b. the same as a known build: the joiner's re-entry lists no room at all
    "menu_again_online_known": dict(host=S("menu", "host", "lobby:1", "left:1"),
                                    joins=[S("menu", "back", "menu", "wait:4000", "say:rooms_at_menu", "wait:1500", "join:" + LO,
                                             "say:rooms_joined", "wait:500", "leave")], known="modern",
                                    online=True, modes=("shim",), checks=[("rooms", "rooms_at_menu", 1), ("rooms", "rooms_joined", 1),
                                                                         ("nolog", "join1", "hosting room|comes off the list")]),
    "host_then_join_known": dict(host=S("menu", "host", "lobby:1", "left:1"),
                                 joins=[S("menu", "host", "wait:2000", "close", "back", "menu", "wait:1500", "join:" + LO,
                                          "wait:500", "leave")], modes=("shim",), may_host=("join1",), known="modern",
                                 checks=[("after", "join1", "the game closed its room", "goes to host")]),
    # 6. join, leave, then Host Game in the same run: a room is listed
    "join_leave_host": dict(host=S("menu", "host", "lobby:1", "left:1"),
                            joins=[S("menu", "join:" + LO, "wait:500", "leave", "back", "menu", "host", "wait:3000")],
                            modes=("shim",), may_host=("join1",),
                            checks=[("after", "join1", "goes to host", "hosting room")]),
    "join_leave_host_known": dict(host=S("menu", "host", "lobby:1", "left:1"),
                                  joins=[S("menu", "join:" + LO, "wait:500", "leave", "back", "menu", "host", "wait:3000")],
                                  modes=("shim",), may_host=("join1",), known="modern",
                                  checks=[("after", "join1", "goes to host", "hosting room"), ("count", "join1", PW_ASKED, 1)]),
    # 7. pick a room from the list, cancel at its password box, then Host Game
    "pick_cancel_host": dict(host=S("menu", "host", "wait:15000"), env_host={"OKP_TEST_HOST_PASSWORD": "lap3"},
                             joins=[S("menu", "wait:2000", "joinfail:" + LO, "back", "menu", "host", "wait:3000")],
                             ini={"wsock_join": ""}, env_join={"OKP_TEST_JOIN_PICK": "first"}, modes=("shim",),
                             may_host=("join1",), checks=[("after", "join1", "password cancelled", "hosting room")]),
    # 8. join host A from the list, leave, join host B from the list: the list shows again, A hears nothing more
    "join_a_then_b": dict(host=S("menu", "host", "lobby:1", "left:1", "quiet:12000"), host2=S("menu", "host", "lobby:1", "left:1"),
                          ini={"wsock_join": "", "name": "Alpha"}, host2_ini={"name": "Bravo"},
                          joins=[S("menu", "wait:2000", "env:OKP_TEST_JOIN_PICK=Alpha", "join:" + LO, "wait:500", "leave",
                                   "back", "menu", "wait:1500", "env:OKP_TEST_JOIN_PICK=Bravo", "join:" + LO, "wait:500", "leave")],
                          modes=("shim",), checks=[("after", "join1", "connection .* closed by the game", "picked from the list")]),
    # 9. the host leaves its lobby just as the joiner's password goes out (the room is still in the joiner's list): the
    #    joiner is told, nothing of the join stays, and its Host Game then hosts
    "host_closes_at_pw": dict(host=S("menu", "host", "wait:3000", "say:closing", "close", "wait:30000"),
                              env_host={"OKP_TEST_HOST_PASSWORD": "lap3"}, env_join={"OKP_TEST_JOIN_PASSWORD": "lap3"},
                              joins=[S("menu", "await:closing", "wait:300", "joinfail:" + LO, "back", "menu", "host", "wait:3000")],
                              modes=("shim",), may_host=("join1",),
                              checks=[("log", "join1", NOTICE_NO_ANSWER), ("after", "join1", "no answer from the host", "hosting room")]),
    # 9b. a join while the host's game is busy (its race runs): the connection comes up, no byte comes back and the
    #     joining game gives up - the player is told; a later join works
    "join_busy_host": dict(host=S("menu", "host", "busy:6000", "lobby:1", "left:1"),
                           joins=[S("menu", "wait:1500", "joinquiet:" + LO, "back", "menu", "wait:4000", "join:" + LO, "wait:500", "leave")],
                           modes=("shim",), checks=[("log", "join1", "The game may have started or closed")]),
    # 10. the host closes its room, leaves network play and hosts again with another password: the joiner is asked again
    "pw_rehost":  dict(host=S("menu", "host", "lobby:1", "left:1", "close", "quit", "env:OKP_TEST_HOST_PASSWORD=lap4",
                              "menu", "host", "say:rehosted", "lobby:2", "left:2"), env_host={"OKP_TEST_HOST_PASSWORD": "lap3"},
                       joins=[S("menu", "join:" + LO, "wait:500", "leave", "back", "await:rehosted", "wait:3000", "menu",
                                "wait:1500", "join:" + LO, "wait:500", "leave")],
                       env_join={"OKP_TEST_JOIN_PASSWORD": "lap3|lap4"}, modes=("shim",)),
    # 10b. a known build: the host leaves its lobby and hosts again in the same okplay session (no quit): the password
    #      is asked again, the room gets a new id, and the new password applies (the joiner's old one would not)
    "pw_rehost_known": dict(host=S("menu", "host", "lobby:1", "left:1", "close", "menu", "env:OKP_TEST_HOST_PASSWORD=lap4",
                                   "host", "say:rehosted", "lobby:2", "left:2"), env_host={"OKP_TEST_HOST_PASSWORD": "lap3"},
                            joins=[S("menu", "join:" + LO, "wait:500", "leave", "back", "await:rehosted", "wait:3000", "menu",
                                     "wait:1500", "join:" + LO, "wait:500", "leave")], known="modern",
                            env_join={"OKP_TEST_JOIN_PASSWORD": "lap3|lap4"}, modes=("shim",),
                            checks=[("count", "host", PW_ASKED, 2), ("distinct", "host", r"hosting room ([0-9a-f]{8})", 2),
                                    ("log", "host", "the hosted game is over"), ("nolog", "host", "okplay stops")]),
    # 10c. the same with the room server: after the new Host Game the list shows one room (the old one was closed with
    #      its own id before the new id was announced)
    "pw_rehost_known_online": dict(host=S("menu", "host", "lobby:1", "left:1", "close", "menu", "env:OKP_TEST_HOST_PASSWORD=lap4",
                                          "host", "wait:4000", "say:rooms_rehosted", "say:rehosted", "lobby:2", "left:2"),
                                   env_host={"OKP_TEST_HOST_PASSWORD": "lap3"},
                                   joins=[S("menu", "wait:4000", "join:" + LO, "wait:500", "leave", "back", "await:rehosted",
                                            "wait:1000", "menu", "wait:1500", "join:" + LO, "wait:500", "leave")], known="modern",
                                   env_join={"OKP_TEST_JOIN_PASSWORD": "lap3|lap4"}, modes=("shim",), online=True,
                                   checks=[("rooms", "rooms_rehosted", 1), ("count", "host", PW_ASKED, 2),
                                           ("distinct", "host", r"hosting room ([0-9a-f]{8})", 2), ("nolog", "host", "close room 00000000")]),
    # 10d. the host leaves its lobby (Disconnect) and stays there: the game keeps its listener open until the next menu
    #      entry, so the room has to come off the list on the game's own "not hosting"; hosting again lists a new room
    "lobby_left_online": dict(host=S("menu", "host", "lobby:1", "wait:1500", "say:rooms_hosting", "left:1", "disconnect", "wait:6000",
                                     "say:rooms_left", "wait:1000", "menu", "wait:2500", "say:rooms_menu", "wait:1000", "host", "wait:4000",
                                     "say:rooms_again", "wait:1000", "close", "wait:4000", "say:rooms_closed", "wait:1500"),
                              joins=[S("menu", "wait:4000", "join:" + LO, "wait:2500", "leave")], known="modern",
                              env_host={"OKP_TEST_CONN_REPORTS": "no"},   # (a new okplay session starts: no question box)
                              modes=("shim",), online=True,
                              checks=[("rooms", "rooms_hosting", 1), ("rooms", "rooms_left", 0), ("rooms", "rooms_menu", 0),
                                      ("rooms", "rooms_again", 1), ("rooms", "rooms_closed", 0),
                                      ("log", "host", "the game left its lobby"),
                                      ("distinct", "host", r"hosting room ([0-9a-f]{8})", 2), ("nolog", "host", "close room 00000000")]),
    # 10e. the same without the room server (LAN): nothing breaks, the next Host Game is a new game
    "lobby_left_lan": dict(host=S("menu", "host", "lobby:1", "left:1", "disconnect", "wait:4000", "menu", "wait:1000", "host",
                                  "lobby:2", "left:2"),
                           joins=[S("menu", "wait:2500", "join:" + LO, "wait:500", "leave", "back", "wait:9000", "menu", "wait:1500",
                                    "join:" + LO, "wait:500", "leave")], known="modern", modes=("shim",),
                           env_host={"OKP_TEST_CONN_REPORTS": "no"},
                           checks=[("log", "host", "the game left its lobby")]),
    # 12. in the lobby the joiner's PC freezes for 20 s, a minute after it joined, then goes on: the stream goes on
    #     (okplay.ini drop_after=40, so only okplay's own peer table could end it)
    "lobby_silence": dict(host=S("menu", "host", "lobby:1", "left:1", "--secs", "150"),
                          joins=[S("menu", "join:" + LO, "wait:63000", "say:freezeme", "wait:500", "ping", "leave")],
                          ini={"drop_after": "40"}, modes=("shim",), freeze=[dict(role="join1", on="EVENT say freezeme", secs=20)]),
    # 13. three players in the lobby; one leaves, enters the menu again and joins again
    "lobby3":     dict(host=S("menu", "host", "lobby:2", "left:3"),
                       joins=[S("menu", "join:" + LO, "wait:6000", "leave"),
                              S("menu", "join:" + LO, "wait:500", "leave", "back", "menu", "wait:1500", "join:" + LO, "wait:500", "leave")],
                       modes=("shim",)),
    # 14. the player limit (okplay.ini wsock_max_players=2): host + one joiner fill the room; a second joiner is refused
    #     by okplay before the host's game sees it, and gets "connection refused" at once
    "full":       dict(host=S("menu", "host", "lobby:1", "await:j2tried", "left:1"),
                       joins=[S("menu", "join:" + LO, "ping", "say:in1", "await:j2tried", "ping", "leave"),
                              S("menu", "await:in1", "wait:500", "joinfail:" + LO, "say:j2tried")],
                       ini={"wsock_max_players": "2"}, modes=("shim",),
                       checks=[("log", "host", FULL_REFUSED), ("outcount", "host", "EVENT accepted a connection", 1),
                               ("log", "join2", "the host says its game is full"), ("log", "join2", r"refused \(reason 4\)"),
                               ("outlt", "join2", r"EVENT refused.* after (\d+) ms", 5000)]),
    # 14b. ... the first joiner leaves: the room has space again, the second joiner gets in; the room server's list says
    #      2/2 while full and 1/2 after the leave
    "full_frees_online": dict(host=S("menu", "host", "lobby:1", "await:j2tried", "left:1", "lobby:2", "left:2"),
                              joins=[S("menu", "wait:3000", "join:" + LO, "ping", "say:in1", "await:j2tried", "wait:3000",
                                       "say:rooms_full", "leave", "wait:4000", "say:rooms_after", "say:j1left"),
                                     S("menu", "await:in1", "wait:500", "joinfail:" + LO, "say:j2tried", "await:j1left",
                                       "join:" + LO, "ping", "leave")],
                              ini={"wsock_max_players": "2"}, modes=("shim",), online=True,
                              checks=[("log", "host", FULL_REFUSED), ("outcount", "host", "EVENT accepted a connection", 2),
                                      ("players", "rooms_full", "2/2"), ("players", "rooms_after", "1/2")]),
    # 14c. ... a full room: its joiner closes and connects again at once - the same player, never refused
    "full_same_peer": dict(host=S("menu", "host", "lobby:2", "left:2"),
                           joins=[S("menu", "join:" + LO, "ping", "leave", "join:" + LO, "ping", "leave")],
                           ini={"wsock_max_players": "2"}, modes=("shim",),
                           checks=[("nolog", "host", "the game is full"), ("outcount", "host", "EVENT accepted a connection", 2)]),
    # 14d. a known build hosting Peer-to-Peer (the game's own type 0, no okplay.ini setting): 2 players, the third refused
    "full_p2p_known": dict(host=S("menu", "host", "lobby:1", "await:j2tried", "left:1"),
                           joins=[S("menu", "join:" + LO, "ping", "say:in1", "await:j2tried", "leave"),
                                  S("menu", "await:in1", "wait:500", "joinfail:" + LO, "say:j2tried")],
                           env_host={"TCPSIM_GAMETYPE": "0"}, known="modern", modes=("shim",),
                           checks=[("log", "host", FULL_REFUSED), ("log", "host", r"Host Game: this PC hosts \(game type 0\)"),
                                   ("outcount", "host", "EVENT accepted a connection", 1)]),
}


# Fixed folders, reused by every scenario: Windows Firewall asks once per program path, so a fresh temp folder
# per scenario put one prompt on the screen for each of them.
FOLDERS = os.path.join(HERE, "..", "..", "tests", "out", "tcp")
SLOT = [0]


def folder(mode, ini, dll=None):
    SLOT[0] += 1
    d = os.path.abspath(os.path.join(FOLDERS, f"{mode}_{SLOT[0]}"))
    for _ in range(50):   # (the last scenario's programs may still hold a file for a moment)
        shutil.rmtree(d, ignore_errors=True)
        if not os.path.exists(d): break
        time.sleep(0.1)
    os.makedirs(d, exist_ok=True)
    shutil.copy(os.path.join(BIN, "tcpsim.exe"), d)
    if mode == "shim":
        shutil.copy(dll or os.path.join(BIN, "wsock32.dll"), os.path.join(d, "wsock32.dll"))
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


class Freezer:
    """Stops a process (every thread, as a PC whose game froze) for `secs` once a process prints `on`."""
    def __init__(self, procs, role, on, secs):
        self.procs, self.role, self.on, self.secs, self.done = procs, role, on, secs, None
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        import ctypes
        t0 = time.time()
        while time.time() - t0 < 150 and not self.done:
            if any(any(self.on in l for l in list(p.lines)) for p in list(self.procs.values())):
                p = self.procs.get(self.role)
                if p is None or p.p.poll() is not None:
                    return
                nt = ctypes.windll.ntdll
                h = ctypes.c_void_p(int(p.p._handle))
                nt.NtSuspendProcess(h)
                time.sleep(self.secs)
                nt.NtResumeProcess(h)
                self.done = time.time()
                return
            time.sleep(0.05)


def standin_rooms():   # (number of rooms, "players/maxp" of each)
    import urllib.request, json
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{STANDIN_PORT}/okplay.php?a=list&fmt=json", timeout=5) as r:
            rooms = json.loads(r.read().decode())["rooms"]
            return len(rooms), ",".join(f"{x['players']}/{x['maxp']}" for x in rooms)
    except Exception as e:
        return f"? ({e})", ""


class Sampler:
    """The room server's list (the stand-in) each time a process prints "EVENT say rooms..."."""
    def __init__(self, procs):
        self.procs, self.samples, self.stop = procs, [], False
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        seen = set()
        while not self.stop:
            for p in list(self.procs.values()):
                for i, l in enumerate(list(p.lines)):
                    m = re.match(r"EVENT say (rooms\S*)", l)
                    if m and (p.name, i) not in seen:
                        seen.add((p.name, i))
                        self.samples.append((m.group(1),) + standin_rooms())
            time.sleep(0.05)


def run_scenario(sc, mode):
    SLOT[0] = 0
    d = folder(mode, sc.get("ini", {}))
    env = {k: v for k, v in os.environ.items() if not k.startswith("OKP_")}
    env.update(EXTRA_ENV)
    env["TCPSIM_SIGDIR"] = d   # (scripts' say/await, whatever folder each process runs in)
    if mode == "shim":
        env.update(OKP_WSOCK_MODE="lan", OKP_WSOCK_GAME="nfs3")
        kb = sc.get("known") or KNOWN["all"]
        if kb:
            env.update(OKP_TEST_EXEBUILD=BUILDS[kb][0], TCPSIM_GAMESTATE=BUILDS[kb][1])
        if sc.get("online"):
            env.update(OKP_WSOCK_MODE="online", OKP_SERVER=f"http://127.0.0.1:{STANDIN_PORT}/okplay.php", OKP_STUN="")
    env_host = dict(env, OKP_TEST_HOST_PASSWORD="<none>")
    # (a joiner that turns host gets a host's answer too: a test never shows a real box)
    env_join = dict(env, OKP_TEST_JOIN_PICK="cancel", OKP_TEST_BUILD_ANSWER="cancel", OKP_TEST_JOIN_PASSWORD="<cancel>",
                    OKP_TEST_HOST_PASSWORD="<none>")
    env_host2 = dict(env_host)
    if mode == "shim":
        env_host.update(sc.get("env_host", {}))
        env_join.update(sc.get("env_join", {}))
        env_host2.update(sc.get("env_host2", {}))
        for e in (env_host, env_join, env_host2):   # a process that says it is a known build plays that build's game state
            for fp, gs in BUILDS.values():
                if e.get("OKP_TEST_EXEBUILD") == fp:
                    e["TCPSIM_GAMESTATE"] = gs
    exe = os.path.join(d, "tcpsim.exe")
    subst = {"lo": "127.0.0.1", "lanip": lan_ip(), "menuport": "9803" if mode == "shim" else "0"}
    procs, dirs = {}, {}
    # --host-dll / --join-dll: another build of the shim on one side (old with new), each side in its own folder
    jd = folder(mode, sc.get("ini", {}), DLLS["join"]) if mode == "shim" and DLLS["join"] else d
    for role in ("host", "host2"):   # the hosts first, each listening before the joiners start
        if sc.get(role) is None:
            continue
        hd = folder(mode, sc.get("ini", {}), DLLS["host"]) if mode == "shim" and DLLS["host"] and role == "host" else d
        if role == "host2":   # another PC: its own folder (its own okplay.ini, e.g. another player name)
            hd = folder(mode, dict(sc.get("ini", {}), **sc.get("host2_ini", {})))
        dirs[role] = hd
        procs[role] = Proc(role, [os.path.join(hd, "tcpsim.exe")] + [x.format(**subst) for x in sc[role]], hd,
                           env_host if role == "host" else env_host2)
        tag = "standin" if role == "host" else "standin2"
        if any("{%s}" % tag in x for j in sc["joins"] for x in j):
            m = procs[role].wait_line(r"EVENT my address (\S+)", 15)
            subst[tag] = m.group(1) if m else "0.0.0.0"
        time.sleep(1.5)
    for i, ja in enumerate(sc["joins"]):
        procs[f"join{i + 1}"] = Proc(f"join{i + 1}", [os.path.join(jd, "tcpsim.exe")] + [x.format(**subst) for x in ja], jd, env_join)
        dirs[f"join{i + 1}"] = jd
    fr = [Freezer(procs, f["role"], f["on"], f["secs"]) for f in sc.get("freeze", [])]
    sampler = Sampler(procs) if sc.get("online") and mode == "shim" else None
    out = {n: p.finish(180) for n, p in procs.items() if n.startswith("join")}
    for role in ("host", "host2"):
        if role not in procs:
            continue
        if sc.get("kill_host"):
            time.sleep(1)
            if procs[role].p.poll() is None:
                procs[role].p.kill()
        out[role] = procs[role].finish(180)
    for f in fr:
        f.done = f.done or True
    if sampler:
        sampler.stop = True
        out["_rooms"] = sampler.samples
    out["_pids"] = {n: p.p.pid for n, p in procs.items()}
    out["_dirs"] = dirs
    out["_dir"] = d
    return out


def log_of(out, role):   # one process's okplay log
    logs = os.path.join(out["_dirs"].get(role, out["_dir"]), "logs")
    pid = out["_pids"].get(role)
    if not os.path.isdir(logs) or pid is None:
        return ""
    return "".join(open(os.path.join(logs, f), errors="replace").read() for f in sorted(os.listdir(logs))
                   if f.endswith(f"-{pid}.log"))


def script_checks(sc, out):   # what the okplay logs must (not) say, per scenario ("checks" in SCENARIOS)
    why = []
    for c in sc.get("checks", []):
        kind = c[0]
        if kind == "rooms":   # the room server's list at each "say <name>": c[2] rooms
            got = [n for name, n, _ in out.get("_rooms", []) if name == c[1]]
            if not got:
                why.append(f"no room list taken at '{c[1]}' (the step was not reached)")
            elif any(n != c[2] for n in got):
                why.append(f"the room server lists {got} room(s) at '{c[1]}', expected {c[2]}")
            continue
        if kind == "players":   # the room server's list at "say <name>": one room, its players/maxp c[2]
            got = [pm for name, _, pm in out.get("_rooms", []) if name == c[1]]
            if not got:
                why.append(f"no room list taken at '{c[1]}' (the step was not reached)")
            elif any(pm != c[2] for pm in got):
                why.append(f"the room server says {got} at '{c[1]}', expected {c[2]}")
            continue
        role = c[1]
        if kind in ("outcount", "outlt"):   # the process's own output: c[3] lines like c[2] / c[2]'s group 1 below c[3]
            outl = out.get(role, "").splitlines()
            got = [l for l in outl if re.search(c[2], l)]
            if kind == "outcount" and len(got) != c[3]:
                why.append(f"{role} printed /{c[2]}/ {len(got)} time(s), expected {c[3]}")
            if kind == "outlt" and (not got or int(re.search(c[2], got[-1]).group(1)) >= c[3]):
                why.append(f"{role}: {got[-1].strip() if got else 'nothing like /' + c[2] + '/'} (want below {c[3]})")
            continue
        lines = log_of(out, role).splitlines()
        hit = lambda pat, ls: [l for l in ls if re.search(pat, l)]
        if kind == "log" and not hit(c[2], lines):
            why.append(f"{role}'s log never says /{c[2]}/")
        elif kind == "nolog" and hit(c[2], lines):
            why.append(f"{role}'s log says /{c[2]}/: {hit(c[2], lines)[0].strip()[:120]}")
        elif kind == "count" and len(hit(c[2], lines)) != c[3]:   # exactly c[3] lines like c[2]
            why.append(f"{role}'s log says /{c[2]}/ {len(hit(c[2], lines))} time(s), expected {c[3]}")
        elif kind == "distinct":   # c[3] different values of c[2]'s group 1
            vals = {re.search(c[2], l).group(1) for l in hit(c[2], lines)}
            if len(vals) != c[3]:
                why.append(f"{role}: {len(vals)} different /{c[2]}/ ({', '.join(sorted(vals))}), expected {c[3]}")
        elif kind == "after":   # c[3] after the first line like c[2]
            at = [i for i, l in enumerate(lines) if re.search(c[2], l)]
            if not at:
                why.append(f"{role}'s log never says /{c[2]}/")
            elif not hit(c[3], lines[at[0] + 1:]):
                why.append(f"{role}: nothing like /{c[3]}/ after /{c[2]}/")
        elif kind == "between":   # nothing like c[4] from the first c[2] to the next c[3]
            at = [i for i, l in enumerate(lines) if re.search(c[2], l)]
            if not at:
                why.append(f"{role}'s log never says /{c[2]}/")
                continue
            end = next((i for i in range(at[0] + 1, len(lines)) if re.search(c[3], lines[i])), len(lines))
            bad = hit(c[4], lines[at[0] + 1:end])
            if bad:
                why.append(f"{role}: {len(bad)} x /{c[4]}/ after /{c[2]}/, first: {bad[0].strip()[:100]}")
    return why


def all_logs(out):   # every process's okplay log, whatever its folder
    return "".join(logs_of(x) for x in sorted(set(out["_dirs"].values()) | {out["_dir"]}))


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
        text = all_logs(out)
        for r, pid in out["_pids"].items():
            if not r.startswith("join") or r in sc.get("may_host", ()):
                continue
            # a joiner may look like a host for a moment (the game's calls for entering the TCP menu again are those of
            # Host Game), but its room must come off the list when the game connects out
            ls = [l for l in text.splitlines() if re.search(r"\[\s*%d\] " % pid, l)]
            hosted = [i for i, l in enumerate(ls) if re.search("hosting room|this PC hosts", l)]
            dropped = [i for i, l in enumerate(ls) if "comes off the list" in l]
            if hosted and (not dropped or dropped[-1] < hosted[-1]):
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
        text = all_logs(out)
        if not text:
            why.append("no okplay log: the shim did not load")
        if "LAN Play (okplay.ini" in text or "for a Winsock game" in text:
            why.append("the probes made okplay decide the mode (a box would have come)")
    if kind == "pw_wrong" and mode == "shim" and "turned away (wrong password)" not in all_logs(out):
        why.append("the host's log does not say it turned the joiner away")
    if mode == "shim":
        why += script_checks(sc, out)
    return why


SHOW = ("connected after", "refused", "gave up", "WOULDBLOCK", "resumed", "partial", "after the joiner", "then EOF",
        "select", "FIONREAD", "UDP", "failure", "accepted", "connection lost", "my address", "ends now", "Host Game", "at the menu",
        "EVENT step", "in the lobby", "left the lobby", "closed (", "player(s)", "ping answered")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="both", choices=["real", "shim", "both"])
    ap.add_argument("--only", default="")
    ap.add_argument("--env", action="append", default=[], help="K=V for every tcpsim process (e.g. OKP_DROP_PCT=10)")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-v", action="store_true", help="print every process's output")
    ap.add_argument("--host-dll", default="", help="the host runs this wsock32.dll (an older build, for old-with-new)")
    ap.add_argument("--join-dll", default="", help="the joiners run this wsock32.dll")
    ap.add_argument("--build", default="", choices=["", "modern", "oem", "cd"],
                    help="every scenario as this known build of the game (its own state decides Host Game)")
    a = ap.parse_args()
    if a.list:
        print("\n".join(SCENARIOS))
        return 0
    for kv in a.env:
        k, v = kv.split("=", 1)
        EXTRA_ENV[k] = v
    DLLS["host"], DLLS["join"] = a.host_dll or None, a.join_dll or None
    KNOWN["all"] = a.build or None
    names = [s for s in (a.only.split(",") if a.only else SCENARIOS) if s]
    modes = ["real", "shim"] if a.mode == "both" else [a.mode]
    standin = None
    if "shim" in modes and any(SCENARIOS[n].get("online") for n in names):
        php_server = os.environ.get("TCP_RUN_PHP_SERVER")   # a released okplay.php instead of the stand-in (old servers keep working)
        if php_server:
            d = os.path.abspath(os.path.join(FOLDERS, "php"))
            os.makedirs(d, exist_ok=True)
            shutil.copy(php_server, os.path.join(d, "okplay.php"))
            standin = subprocess.Popen([os.environ.get("TCP_RUN_PHP", "php"), "-S", f"127.0.0.1:{STANDIN_PORT}", "-t", d],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            standin = subprocess.Popen([sys.executable, STANDIN, "--port", str(STANDIN_PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
                xf = sc.get("xfail")
                rows.append((n, m, not why or bool(xf), bool(why and xf)))
                tag = "ok" if not why else ("FAIL (known, needs " + xf + "): " if xf else "FAIL: ") + "; ".join(why)
                print(f"{n:18} {m:5} {tag}", flush=True)
                if out.get("_rooms"):
                    print("    room server: " + ", ".join(f"{k} {v}" + (f" ({pm})" if pm else "") for k, v, pm in out["_rooms"]))
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
    known = [r[0] for r in rows if r[3]]
    print(f"\n{len(rows) - len(bad)}/{len(rows)} ok" + (f" (counted ok but known to fail: {', '.join(known)})" if known else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
