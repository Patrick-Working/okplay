"""One-off: add --cycles N to tools/dptest.c (host/join, Close, and again - same DirectPlay object)."""
import os
import sys

p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dptest.c")
s = open(p, encoding="utf-8", newline="").read()


def rep(old, new):
    global s
    if s.count(old) != 1:
        sys.exit("no unique match: " + old[:70])
    s = s.replace(old, new)


rep(" *   --features      also exercise what a lobby does:",
    " *   --cycles N      host/join, Close, and again N times with the SAME DirectPlay object\n"
    " *                   (leaving one game and joining/hosting another); cycle c's session is\n"
    " *                   named \"...#c\" and joiners look for exactly that one\n"
    " *   --features      also exercise what a lobby does:")
rep("typedef struct { int found; DPSESSIONDESC2 sd; char name[128]; int count; } enum_ctx;\n",
    "typedef struct { int found; DPSESSIONDESC2 sd; char name[128]; int count; } enum_ctx;\n"
    "static char want_suffix[16];   /* --cycles: only the session of this cycle */\n")
rep("    enum_ctx *e = (enum_ctx *)ctx;\n",
    "    enum_ctx *e = (enum_ctx *)ctx;\n"
    "    if (!(flags & DPESC_TIMEDOUT) && want_suffix[0]) {\n"
    "        const char *n = sd->lpszSessionNameA ? sd->lpszSessionNameA : \"\";\n"
    "        size_t ln = strlen(n), ls = strlen(want_suffix);\n"
    "        if (ln < ls || strcmp(n + ln - ls, want_suffix) != 0) return TRUE;   /* an older cycle's session */\n"
    "    }\n")
rep('        else if (!strcmp(argv[i], "--features")) features = 1;\n',
    '        else if (!strcmp(argv[i], "--features")) features = 1;\n'
    '        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = atoi(argv[++i]);\n')
rep("    int secs = 10, expect = 0;\n", "    int secs = 10, expect = 0, cycles = 1;\n")

rep('    char sessname[96];\n    snprintf(sessname, sizeof(sessname), "dptest %s", name);\n',
    '    int all_ok = 1;\n'
    '    for (int cyc = 1; cyc <= (cycles < 1 ? 1 : cycles); cyc++) {\n'
    '    char sessname[96];\n'
    '    if (cycles > 1) {\n'
    '        snprintf(sessname, sizeof(sessname), "dptest %s#%d", name, cyc);\n'
    '        snprintf(want_suffix, sizeof(want_suffix), "#%d", cyc);\n'
    '        nrem = 0;\n'
    '        memset(rem, 0, sizeof(rem));\n'
    '        out("EVENT cycle %d", cyc);\n'
    '    } else\n'
    '        snprintf(sessname, sizeof(sessname), "dptest %s", name);\n')
rep("    IDirectPlayX_Close(dp);\n    IDirectPlayX_Release(dp);\n    out(\"RESULT %s\", ok ? \"ok\" : \"fail\");\n    return ok ? 0 : 1;\n}",
    "    if (cycles > 1) out(\"STAT cycle %d max_players=%d peers_answering=%d %s\", cyc, max_players, with_pongs, ok ? \"ok\" : \"FAIL\");\n"
    "    if (!ok) all_ok = 0;\n"
    "    hr = IDirectPlayX_Close(dp);\n"
    "    if (cycles > 1) out(\"EVENT closed 0x%lx\", (unsigned long)hr);\n"
    "    }\n"
    "    IDirectPlayX_Release(dp);\n    out(\"RESULT %s\", all_ok ? \"ok\" : \"fail\");\n    return all_ok ? 0 : 1;\n}")
open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
