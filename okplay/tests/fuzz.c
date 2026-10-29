/* fuzz - throw malformed inputs at every okplay parser that reads network or server data.
 *
 * Each input is copied so that it ends exactly at a PAGE_NOACCESS guard page: reading even one
 * byte past the input faults at once (a poor man's AddressSanitizer). A fault prints the target,
 * iteration and input (saved to tests/out/fuzz-crash-<target>.bin) and exits 1.
 *
 *   fuzz [iterations-per-target] [seed]
 *   fuzz --control     fuzzes the OLD pre-fix player-data parser; the trap must catch it (exit 1)
 */
#include "../src/net.c"
#include "../src/lan.c"
#include "../src/rdv.c"
#include <stdio.h>

void okp_self_probe(okp_ctx *c) { (void)c; }
void okp_inject_rooms(okp_ctx *c) { (void)c; }
void okp_capture_enum_reply(okp_ctx *c, const uint8_t *m, int l) { (void)c; (void)m; (void)l; }

/* ------------------------------------------------------------------ rng */
static uint64_t rs;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static int rndn(int n) { return n > 0 ? (int)(rnd() % (uint64_t)n) : 0; }

/* ------------------------------------------------------------------ guard page + crash report */
static uint8_t *guard_base;                 /* [4 data pages][1 no-access page] */
#define DATA_SPAN (4 * 4096)
static const char *cur_target = "?";
static const uint8_t *cur_in;
static int cur_len;
static long cur_iter;

static void *place(const void *src, int n)
{
    uint8_t *dst = guard_base + DATA_SPAN - n;
    memcpy(dst, src, n);
    return dst;
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    printf("CRASH in %s at iteration %ld: exception 0x%08lx at %p", cur_target, cur_iter,
           (unsigned long)er->ExceptionCode, er->ExceptionAddress);
    if (er->NumberParameters >= 2)
        printf(" (%s of %p; input ends at %p)", er->ExceptionInformation[0] ? "write" : "read",
               (void *)er->ExceptionInformation[1], (void *)(guard_base + DATA_SPAN));
    printf(", input %d bytes\n", cur_len);
    char path[128];
    snprintf(path, sizeof(path), "out/fuzz-crash-%s.bin", cur_target);
    FILE *f = fopen(path, "wb");
    if (f && cur_in) { fwrite(cur_in, 1, cur_len, f); fclose(f); printf("saved %s\n", path); }
    printf("RESULT FAIL\n");
    fflush(stdout);
    ExitProcess(1);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ------------------------------------------------------------------ fake dplayx */
static HRESULT STDMETHODCALLTYPE fake_handle(IDirectPlaySP *This, LPVOID body, DWORD n, LPVOID hdr)
{
    (void)This; (void)hdr;
    volatile uint8_t sum = 0;
    for (DWORD i = 0; i < n; i++) sum += ((uint8_t *)body)[i];   /* every byte we hand dplayx must be readable */
    return DP_OK;
}
static IDirectPlaySPVtbl fake_vt;
static IDirectPlaySP fake_sp = { &fake_vt };

/* ------------------------------------------------------------------ inputs */
static uint8_t scratch[DATA_SPAN];

static void rand_bytes(uint8_t *b, int n) { for (int i = 0; i < n; i++) b[i] = (uint8_t)rnd(); }

static void rand_from(SOCKADDR_IN6 *a)
{
    memset(a, 0, sizeof(*a));
    a->sin6_family = AF_INET6;
    a->sin6_port = (uint16_t)rnd();
    if (rndn(3)) {
        uint8_t v4[4];
        rand_bytes(v4, 4);
        okp_addr_from_v4(a, v4, a->sin6_port);
    } else {
        rand_bytes((uint8_t *)&a->sin6_addr, 16);
    }
}

static const uint64_t ids[] = { 0xAAAA0001ULL, 0xAAAA0002ULL, 0xAAAA0003ULL, 0xAAAA0004ULL, 0x1111ULL };

/* a plausible session record (enum reply) to mutate */
static int good_blob(uint8_t *b)
{
    memset(b, 0, 120);
    memcpy(b, "play", 4);
    b[4] = 1; b[6] = 14;
    uint32_t sz = 80, off = 92;
    memcpy(b + 8, &sz, 4);
    memcpy(b + 88, &off, 4);
    memcpy(b + 92, "N\0a\0m\0e\0\0\0", 10);
    return 102;
}

static void mutate(uint8_t *b, int *n, int cap)
{
    int k = 1 + rndn(8);
    while (k--) {
        int op = rndn(5);
        if (op == 0 && *n) b[rndn(*n)] = (uint8_t)rnd();                 /* byte */
        else if (op == 1 && *n) b[rndn(*n)] ^= (uint8_t)(1 << rndn(8));    /* bit */
        else if (op == 2) *n = rndn(*n + 1);                               /* truncate */
        else if (op == 3 && *n + 16 < cap) { rand_bytes(b + *n, 16); *n += 16; }   /* extend */
        else if (op == 4 && *n >= 4) {                                      /* interesting ints */
            static const uint32_t magic[] = { 0, 1, 0x7fffffff, 0xffffffff, 80, 92, 1024, 65535 };
            uint32_t v = magic[rndn(8)];
            memcpy(b + rndn(*n - 3), &v, 4);
        }
    }
}

/* ------------------------------------------------------------------ targets */
static okp_ctx *ctx;

static void reset_ctx(int mode)
{
    for (int i = 0; i < OKP_MAX_PEERS; i++) { okp_peer_free_bufs(&ctx->peers[i]); ctx->peers[i].used = 0; }
    ctx->mode = mode;
    ctx->mode_name = mode == OKP_MODE_LAN ? "LAN Play" : "Online Play";
    ctx->have_srflx = 0;
    ctx->nrooms = 0;
    ctx->host_peer = ids[rndn(5)];
}

static void t_packet(long iters)
{
    cur_target = "packet";
    for (cur_iter = 0; cur_iter < iters; cur_iter++) {
        if (cur_iter % 5000 == 0) reset_ctx(rndn(2) ? OKP_MODE_ONLINE : OKP_MODE_LAN);
        int n = rndn(1600);
        int kind = rndn(10);
        if (kind < 7 && n >= (int)sizeof(okp_wire)) {
            rand_bytes(scratch, n);
            okp_wire *w = (okp_wire *)scratch;
            w->m0 = 'O'; w->m1 = 'K'; w->ver = OKP_WIRE_VER;
            w->type = (uint8_t)rndn(9);
            w->room = rndn(8) ? ctx->room : (uint32_t)rnd();
            w->from = rndn(8) ? ids[rndn(4)] : rnd();
            w->to = rndn(8) ? ctx->self : rnd();
            if (w->type == OKP_T_RDATA && n >= (int)sizeof(okp_wire) + 4) {   /* keep seqs near the window */
                uint32_t seq = (uint32_t)rndn(40);
                memcpy(scratch + sizeof(okp_wire), &seq, 4);
            }
            if (rndn(4) == 0 && n > (int)sizeof(okp_wire) + 8) memcpy(scratch + sizeof(okp_wire) + (w->type == OKP_T_RDATA ? 4 : 0), "play", 4);
        } else if (kind < 9) {
            n = 20 + rndn(200);
            rand_bytes(scratch, n);
            scratch[0] = 0x01; scratch[1] = 0x01;
            uint32_t mc = htonl(STUN_MAGIC);
            memcpy(scratch + 4, &mc, 4);
            memcpy(scratch + 8, ctx->stun_txid, 12);
            for (int off = 20; off + 12 <= n; off += 4 + 8) {                   /* attribute-shaped junk */
                scratch[off] = 0x00; scratch[off + 1] = rndn(2) ? 0x20 : 0x01;
                scratch[off + 2] = 0; scratch[off + 3] = (uint8_t)rndn(40);
                scratch[off + 5] = (uint8_t)rndn(3);
            }
        } else {
            rand_bytes(scratch, n);
        }
        uint8_t *in = place(scratch, n);
        cur_in = in; cur_len = n;
        SOCKADDR_IN6 from;
        rand_from(&from);
        handle_packet(ctx, in, n, &from);
        if (cur_iter % 997 == 0) run_timers(ctx);   /* push the fuzzed state through the timers too */
    }
}

static void t_offer(long iters)
{
    cur_target = "lan_offer";
    for (cur_iter = 0; cur_iter < iters; cur_iter++) {
        if (cur_iter % 5000 == 0) reset_ctx(OKP_MODE_LAN);
        int n;
        if (rndn(4)) {
            uint8_t blob[1100];
            int bl = good_blob(blob);
            if (rndn(2)) mutate(blob, &bl, 1024);
            uint16_t el = (uint16_t)(rndn(6) ? bl : rnd());
            memcpy(scratch, &el, 2);
            memcpy(scratch + 2, blob, bl);
            n = 2 + bl;
            int nc = rndn(9);
            scratch[n++] = (uint8_t)nc;
            int body = rndn(nc + 2) * (int)sizeof(okp_ccand);
            rand_bytes(scratch + n, body);
            n += body;
            if (rndn(3) == 0) mutate(scratch, &n, 1500);
        } else {
            n = rndn(1400);
            rand_bytes(scratch, n);
        }
        uint8_t *in = place(scratch, n);
        cur_in = in; cur_len = n;
        okp_wire w;
        memset(&w, 0, sizeof(w));
        w.from = ids[rndn(4)];
        w.room = (uint32_t)rnd();
        w.to = ctx->self;
        SOCKADDR_IN from4;
        memset(&from4, 0, sizeof(from4));
        from4.sin_family = AF_INET;
        from4.sin_addr.s_addr = (uint32_t)rnd();
        take_offer(ctx, &w, in, n, &from4);
    }
}

static void t_pdata(long iters)
{
    cur_target = "player_data";
    for (cur_iter = 0; cur_iter < iters; cur_iter++) {
        int n = rndn(80);
        rand_bytes(scratch, n);
        if (n >= 4 && rndn(4)) { uint32_t m = OKP_PD_MAGIC; memcpy(scratch, &m, 4); }
        if (n > 12 && rndn(2)) scratch[12] = (uint8_t)rndn(5);
        uint8_t *in = place(scratch, n);
        cur_in = in; cur_len = n;
        uint64_t peer;
        SOCKADDR_IN6 out[OKP_PDATA_CANDS];
        okp_pdata_parse(in, (DWORD)n, &peer, out, OKP_PDATA_CANDS);
    }
}

static void t_blob(long iters)
{
    cur_target = "session_record";
    for (cur_iter = 0; cur_iter < iters; cur_iter++) {
        uint8_t blob[1200];
        int n = good_blob(blob);
        if (rndn(8)) mutate(blob, &n, 1100); else { n = rndn(1100); rand_bytes(blob, n); }
        uint8_t *in = place(blob, n);
        cur_in = in; cur_len = n;
        if (okp_enum_blob_ok(in, n)) {
            char name[256];
            DWORD pl, mx, fl;
            enum_blob_info(in, n, name, sizeof(name), &pl, &mx, &fl);
        }
    }
}

static const char *seed_text =
    "room 0000abcd 00000000000000ab 3 1.2.3.4:5000,[2001:db8::1]:5000 cGxheQEADgBQAAAARAAAAA== Ann%27s%20Game\n"
    "knock aaaa000000000002 5.6.7.8:6000,[::1]:7\n"
    "room 00001234 00000000000000cd 0 - AAAA -\n";

static void t_text(long iters)
{
    cur_target = "server_text";
    okp_room rooms[OKP_MAX_ROOMS];
    for (cur_iter = 0; cur_iter < iters; cur_iter++) {
        if (cur_iter % 5000 == 0) reset_ctx(OKP_MODE_ONLINE);
        int n;
        if (rndn(5)) {
            n = (int)strlen(seed_text);
            memcpy(scratch, seed_text, n);
            mutate(scratch, &n, 3000);
            if (rndn(6) == 0) { int k = rndn(2500); memset(scratch + n, 'A' + rndn(26), k); n += k; }   /* long token */
        } else {
            n = rndn(3000);
            rand_bytes(scratch, n);
        }
        scratch[n++] = 0;
        char *in = place(scratch, n);
        cur_in = (uint8_t *)in; cur_len = n;
        if (rndn(2)) parse_list(ctx, in, rooms); else parse_knocks(ctx, in);
    }
}

static void t_strings(long iters)
{
    cur_target = "string_helpers";
    for (cur_iter = 0; cur_iter < iters; cur_iter++) {
        int n = rndn(300);
        for (int i = 0; i < n; i++) scratch[i] = "0123456789abcdefABCDEF.:[],%+/=-_ \x01\xff"[rndn(38)];
        scratch[n++] = 0;
        char *in = place(scratch, n);
        cur_in = (uint8_t *)in; cur_len = n;
        SOCKADDR_IN6 c6[OKP_MAX_CANDS];
        uint8_t out[64];
        char txt[64];
        switch (rndn(5)) {
        case 0: okp_cands_from_str(in, c6, OKP_MAX_CANDS); break;
        case 1: okp_addr_parse(in, c6); break;
        case 2: okp_b64_decode(in, out, 1 + rndn(63)); break;
        case 3: okp_urldec(in, txt, 1 + rndn(63)); break;
        case 4: okp_urlenc(in, txt, 1 + rndn(63)); break;
        }
    }
}

/* the parser as it was before hardening: trusts that the record holds ncands addresses */
static int old_pdata_parse(const void *buf, DWORD sz)
{
    const okp_pdata *pd = (const okp_pdata *)buf;
    if (!pd || sz < 13 || pd->magic != OKP_PD_MAGIC) return -1;
    volatile int sum = 0;
    for (int i = 0; i < pd->ncands && i < OKP_PDATA_CANDS; i++) sum += pd->cands[i].addr[15];
    return sum;
}

int main(int argc, char **argv)
{
    long iters = 200000;
    uint64_t seed = 0x5EED0000ULL ^ GetTickCount();
    int control = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--control")) control = 1;
        else if (i == 1) iters = atol(argv[i]);
        else seed = strtoull(argv[i], NULL, 0);
    }
    rs = seed ? seed : 1;
    CreateDirectoryA("out", NULL);
    guard_base = (uint8_t *)VirtualAlloc(NULL, DATA_SPAN + 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    VirtualProtect(guard_base + DATA_SPAN, 4096, PAGE_NOACCESS, &old);
    SetUnhandledExceptionFilter(on_crash);
    AddVectoredExceptionHandler(1, (PVECTORED_EXCEPTION_HANDLER)on_crash);

    fake_vt.HandleMessage = fake_handle;
    ctx = (okp_ctx *)calloc(1, sizeof(okp_ctx));
    InitializeCriticalSection(&ctx->lock);
    ctx->isp = &fake_sp;
    ctx->sock = INVALID_SOCKET;             /* sendto fails: nothing ever leaves this PC */
    ctx->lan_q = ctx->lan_listen = INVALID_SOCKET;
    ctx->self = 0x1111ULL;
    ctx->room = 0x2222;
    for (int i = 0; i < 12; i++) ctx->stun_txid[i] = (uint8_t)(i * 7);
    okp_cfg.stun[0] = 0;                    /* no DNS lookups from the timers */
    okp_cfg.stats_ms = 1;

    printf("seed 0x%llx, %ld iterations per target\n", (unsigned long long)seed, iters);
    if (control) {
        cur_target = "control_old_player_data";
        for (cur_iter = 0; cur_iter < iters; cur_iter++) {
            int n = 13 + rndn(40);
            rand_bytes(scratch, n);
            uint32_t m = OKP_PD_MAGIC;
            memcpy(scratch, &m, 4);
            scratch[12] = 3;
            uint8_t *in = place(scratch, n);
            cur_in = in; cur_len = n;
            old_pdata_parse(in, (DWORD)n);
        }
        printf("control: the old parser was NOT caught - the guard page is not working\nRESULT FAIL\n");
        return 2;
    }

    struct { const char *name; void (*fn)(long); } targets[] = {
        { "packet", t_packet }, { "lan_offer", t_offer }, { "player_data", t_pdata },
        { "session_record", t_blob }, { "server_text", t_text }, { "string_helpers", t_strings } };
    for (int t = 0; t < 6; t++) {
        DWORD t0 = GetTickCount();
        targets[t].fn(iters);
        printf("  %-15s %ld inputs, no faults (%lu ms)\n", targets[t].name, iters, (unsigned long)(GetTickCount() - t0));
        fflush(stdout);
    }
    printf("RESULT ok\n");
    return 0;
}
