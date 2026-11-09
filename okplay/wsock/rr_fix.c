/* okplay wsock - Road Rash Online (Steam/GoG/CD-Rom): fixes for the game's own netcode.
 *
 * Road Rash draws another player's bike by guessing forward from that player's last report. Over the internet
 * the guesses go wrong more often and the game snaps the bike to each new report; the two machines' race clocks
 * also start a few steps apart, so every guess is off by that much. And AI bikers, shared between the machines,
 * can be drawn far from the road ("flying"). Measured on a test bench (two Windows 98 PCs, simulated lag, loss
 * and dropouts) and with a replay simulator, these fixes help most:
 *
 *   rr_clock=1      clock correction: the two okplay builds tell each other how late each other's reports
 *                   arrive; half the difference is the clock offset, used once both sides agree (within a step).
 *   rr_slide=8      when a new report shows the guess was wrong, the bike glides to the right place (the error
 *                   shrinks by 1/K per game step) instead of jumping there. 0 = off, 2..32 = K.
 *                   Bench: 45-76 % fewer jumps at LAN, 200 ms, 300 ms ping and with dropouts.
 *   rr_air=6        an AI biker's upward speed is followed for at most this many steps past its last report
 *                   (the "flying" AI bikers); 0 = off.
 *   rr_air_players=20  the same for the players, past any normal ping: in a dropout the game lifts them hundreds
 *                   of metres; 0 = off.
 *   rr_history=1    each rider keeps its own count in the game's report history (the game shares one counter
 *                   between all riders, which mixes their reports up when several update at once).
 *   rr_ai_echo=1    reports for AI bikers this PC runs itself, sent back to it by the host, are ignored.
 *   rr_ai_owners=1  the host resends who runs which AI biker during a race (the game's resend never happens: its
 *                   step test can't be met on the steps it runs on), so both PCs agree on the AI bikers.
 *
 * The builds are recognised by their exe fingerprint (okp_exe_build); the hook points below are addresses in them,
 * learned by studying the game. Nothing of the game is copied here. Unknown builds: no changes at all. */
#include "okp.h"
#include "rr_fix.h"
#include <string.h>

typedef struct {
    const char *build, *name;
    uint32_t pred_site;   /* the call of the remote-rider guess: rider in CL, step in EDX, returns the pose (EAX) */
    uint32_t hist_site;   /* the call that files a received rider report: rider in CL, the report in EDX */
    uint32_t now;         /* the local step counter (60 per second) */
    uint32_t snaps;       /* each rider's three reports: + rider * 0xa8 + k * 0x38; dwords [3] height, [13] step,
                           * byte +0x30 bit 0 = filled */
    uint32_t fill;        /* the report history's fill counter (one for all riders) */
    uint32_t bcast;       /* the step value the owner-table resend waits for (a byte: 1 -> 0) */
} rrf_build;

static const rrf_build builds[] = {
    { "73d7f9744b5b1a94", "1996 CD",                       0x40a1c6, 0x43eafe, 0x4b7ecc, 0x498198, 0x46dddc, 0x43cf4b },
    { "9c32eadc287cc8eb", "1996 CD (okplay lab no-CD)",    0x40a1c6, 0x43eafe, 0x4b7ecc, 0x498198, 0x46dddc, 0x43cf4b },
    { "9e40e25737149ab0", "Steam",                         0x41a780, 0x4502b6, 0x6036e8, 0x5c8f68, 0x5c561c, 0x4544fe },
};

#define SNAP_STRIDE 0xa8
#define WRAP (1 << 22)        /* a move this big is a track-section change, not motion */

static const rrf_build *B;
static uintptr_t base;        /* where the exe really loaded (the Steam exe may move) */
static uintptr_t A(uint32_t a) { return base + (a - 0x400000u); }

static int clock_on = 1, slide_k = 8, air_steps = 6, air_players = 20, hist_on = 1, echo_on = 1, owners_on = 1;

typedef struct {
    int have_tick;
    int32_t last_tick;            /* the newest report's step (the sender's clock) */
    int32_t lag16, dev16;         /* how late reports arrive, in steps x16: mean and mean deviation */
    uint32_t reports;
    int32_t height;               /* the newest report's height */
    int32_t pred_dt;              /* how far the last guess reached past the newest report */
    /* the slide */
    int s_have;
    uint32_t s_now, s_reports;
    int32_t clk_used, s_clk;      /* the clock steps taken off this guess, and off the last one the slide saw */
    int32_t s_steps;
    int32_t T[3], Tp[3];          /* the game's guess (across, along, height) for the last two steps */
    int32_t off[3];               /* what is left of the corrections */
} rrf_rider;
static rrf_rider riders[16];

/* clock offsets (my clock - theirs, x16), per rider; used once both sides agree */
static int32_t clk_off16[16];
static int clk_state[16];         /* 0 none, 1 my estimate, 2 agreed */
static DWORD sent_ms[16];         /* riders this PC sends reports for (its own; AI bikers it runs) */
static DWORD clk_due_ms, clk_log_ms;

static int32_t fill_level[16], fill_left = -1;

#ifdef RRF_SIM   /* tests/rrf_sim.c: this file run by the replay simulator, on its clock and a stand-in for the game's memory */
uint32_t rrf_sim_ms;
static DWORD ms_now(void) { return rrf_sim_ms; }
#else
static DWORD ms_now(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (DWORD)(c.QuadPart * 1000 / f.QuadPart);
}
#endif

static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

/* the whole steps applied: changed only when the estimate is more than 3/4 step away from them - rounding a
 * wobbling estimate (4.4, 4.6, ...) flipped between 4 and 5 steps, and each flip moved the bike a step's distance */
static int32_t clk_applied[16];
static int32_t off_steps(int rider)
{
    int32_t o = clk_off16[rider], a = clk_applied[rider];
    if (o - a * 16 > 12 || a * 16 - o > 12) clk_applied[rider] = a = o >= 0 ? (o + 8) / 16 : -((-o + 8) / 16);
    return a;
}

/* ---- the lab build's measurements (okplay/wsock/build.sh OKW_LAB_SRC=...): called around ours */
static rrf_observer obs;
void rrf_observe(const rrf_observer *o) { if (o) obs = *o; }

/* ---- the report history (the game's thread) */
void __cdecl rrf_hist_pre(int rider)
{
    volatile int32_t *fill = (volatile int32_t *)A(B->fill);
    if (rider < 0 || rider > 15) return;
    if (*fill != fill_left) for (int i = 0; i < 16; i++) fill_level[i] = *fill;   /* the game reset it: a race start */
    if (hist_on) *fill = fill_level[rider];
    if (obs.hist_pre) obs.hist_pre(rider);
}

void __cdecl rrf_hist_post(int rider)
{
    volatile int32_t *fill = (volatile int32_t *)A(B->fill);
    if (rider < 0 || rider > 15) return;
    fill_level[rider] = *fill;
    fill_left = *fill;
    /* the newest filed report: how late it arrived (for the clock offset), its height, and that it is new */
    const uint8_t *s = (const uint8_t *)A(B->snaps) + rider * SNAP_STRIDE;
    const int32_t *best = NULL;
    for (int k = 0; k < 3; k++) {
        const int32_t *sn = (const int32_t *)(s + k * 0x38);
        if ((s[k * 0x38 + 0x30] & 1) && (!best || sn[13] > best[13])) best = sn;
    }
    rrf_rider *r = &riders[rider];
    if (best && (!r->have_tick || best[13] != r->last_tick)) {
        if (r->have_tick && best[13] < r->last_tick - 100) { r->reports = 0; clk_state[rider] = 0; clk_applied[rider] = 0; }   /* a new race */
        r->have_tick = 1;
        r->last_tick = best[13];
        r->height = best[3];
        r->reports++;
        int32_t lag = *(volatile int32_t *)A(B->now) - best[13];
        if (lag >= -100 && lag < 200) {
            int32_t e = lag * 16 - r->lag16;
            r->lag16 += e / 16;
            r->dev16 += (iabs(e) - r->dev16) / 16;
        }
    }
    if (obs.hist_post) obs.hist_post(rider);
}

/* ---- the guess for a remote rider (the game's thread): before - the step to guess for; after - the pose */
uint32_t __cdecl rrf_pre(int rider, uint32_t now)
{
    if (rider < 0 || rider > 15) return now;
    rrf_rider *r = &riders[rider];
    r->clk_used = clock_on && clk_state[rider] == 2 ? off_steps(rider) : 0;
    now -= (uint32_t)r->clk_used;
    r->pred_dt = r->have_tick ? (int32_t)now - r->last_tick : 0;
    if (obs.pre) now = obs.pre(rider, now);
    return now;
}

static void slide(rrf_rider *r, int32_t *out, uint32_t now)
{
    int32_t T[3] = { out[0], out[2], out[1] };
    int32_t steps = (int32_t)(now - r->s_now);
    if (!r->s_have || steps < 0 || steps > 30) {   /* a start or a long pause: as the game has it */
        memset(r->off, 0, sizeof(r->off));
        r->s_have = 1; r->s_now = now; r->s_reports = r->reports; r->s_clk = r->clk_used; r->s_steps = 1;
        memcpy(r->T, T, sizeof(T)); memcpy(r->Tp, T, sizeof(T));
        return;
    }
    if (steps > 0) {
        /* a new report moved the guess, or the clock offset changed by a step (bench 11-08: its estimate wanders a
         * step or two over a race, and each change moved the bike a step's distance at once): glide it */
        if (r->reports != r->s_reports || r->clk_used != r->s_clk) {
            int big = 0;
            int32_t d[3];
            for (int i = 0; i < 3; i++) {
                int64_t v = (int64_t)(r->T[i] - r->Tp[i]) * steps / (r->s_steps > 0 ? r->s_steps : 1);
                int64_t dd = (int64_t)r->T[i] + v - T[i];
                if (dd > 8192 || dd < -8192) big = 1;   /* a track-section change or a real jump: no glide */
                d[i] = (int32_t)dd;
            }
            for (int i = 0; i < 3; i++) r->off[i] = big ? 0 : r->off[i] + d[i];
            r->s_reports = r->reports;
            r->s_clk = r->clk_used;
        }
        for (int k = 0; k < steps; k++)
            for (int i = 0; i < 3; i++) r->off[i] -= r->off[i] / slide_k + (r->off[i] > 0) - (r->off[i] < 0);
        memcpy(r->Tp, r->T, sizeof(T));
        r->s_steps = steps;
        r->s_now = now;
    }
    memcpy(r->T, T, sizeof(T));
    out[0] = T[0] + r->off[0];
    out[2] = T[1] + r->off[1];
    int32_t h = T[2] + r->off[2];
    out[1] = h < 0 ? 0 : h;
}

void __cdecl rrf_post(int rider, int32_t *out)   /* out: [0] across, [1] height, [2] along */
{
    if (rider < 0 || rider > 15 || !out) return;
    rrf_rider *r = &riders[rider];
    /* the game follows the last upward speed for as long as no report comes (in a dropout: bikes hundreds of metres
     * up). The rise stops growing after `lim` steps - for AI bikers after rr_air steps; for the players only after
     * rr_air_players (20: past any normal ping - limiting them at 6 flattened their jumps at high ping, bench 11-08) */
    int lim = rider >= 2 ? air_steps : air_players;
    if (lim > 0 && r->have_tick && r->pred_dt > lim && out[1] > r->height) {   /* (going up only: a fall is left alone) */
        int32_t h = r->height + (int32_t)((int64_t)(out[1] - r->height) * lim / r->pred_dt);
        out[1] = h < 0 ? 0 : h;
    }
    if (slide_k >= 2) slide(r, out, *(volatile uint32_t *)A(B->now));
    if (obs.post) obs.post(rider, out);
}

#ifndef RRF_SIM
__attribute__((used)) static uintptr_t rrf_orig_pred, rrf_orig_hist;

__attribute__((naked)) static void pred_wrap(void)
{
    __asm__ volatile(
        "push   %%ecx\n\t"               /* the rider (CL), for after */
        "push   %%edx\n\t"
        "push   %%ecx\n\t"
        "movsx  %%cl, %%ecx\n\t"
        "push   %%edx\n\t"               /* the step */
        "push   %%ecx\n\t"               /* the rider */
        "call   _rrf_pre\n\t"
        "add    $8, %%esp\n\t"
        "pop    %%ecx\n\t"
        "pop    %%edx\n\t"
        "mov    %%eax, %%edx\n\t"        /* the step to guess for */
        "call   *_rrf_orig_pred\n\t"
        "test   %%eax, %%eax\n\t"        /* no pose: nothing to do */
        "jz     1f\n\t"
        "pushal\n\t"
        "push   %%eax\n\t"
        "movsbl 36(%%esp), %%ecx\n\t"    /* the rider pushed first (32 bytes of pushal + 4) */
        "push   %%ecx\n\t"
        "call   _rrf_post\n\t"
        "add    $8, %%esp\n\t"
        "popal\n\t"
        "1:\n\t"
        "add    $4, %%esp\n\t"
        "ret\n\t" ::: "memory");
}

__attribute__((naked)) static void hist_wrap(void)
{
    __asm__ volatile(
        "push   %%ecx\n\t"
        "pushal\n\t"
        "movsx  %%cl, %%ecx\n\t"
        "push   %%ecx\n\t"
        "call   _rrf_hist_pre\n\t"
        "add    $4, %%esp\n\t"
        "popal\n\t"
        "call   *_rrf_orig_hist\n\t"
        "pushal\n\t"
        "movsbl 32(%%esp), %%ecx\n\t"
        "push   %%ecx\n\t"
        "call   _rrf_hist_post\n\t"
        "add    $4, %%esp\n\t"
        "popal\n\t"
        "add    $4, %%esp\n\t"
        "ret\n\t" ::: "memory");
}

/* a call (opcode e8, rel32) at `site` now calls `to`; the address it called before goes to *orig */
static int hook_call(uintptr_t site, void *to, uintptr_t *orig)
{
    DWORD old;
    if (IsBadReadPtr((void *)site, 5) || *(const uint8_t *)site != 0xe8) return 0;
    int32_t rel;
    memcpy(&rel, (void *)(site + 1), 4);
    *orig = site + 5 + rel;
    rel = (int32_t)((uintptr_t)to - (site + 5));
    if (!VirtualProtect((void *)site, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)(site + 1), &rel, 4);
    VirtualProtect((void *)site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)site, 5);
    return 1;
}

static void set_byte(uintptr_t at, uint8_t from, uint8_t to)
{
    DWORD old;
    if (IsBadReadPtr((void *)at, 1) || *(volatile uint8_t *)at != from) return;
    if (!VirtualProtect((void *)at, 1, PAGE_EXECUTE_READWRITE, &old)) return;
    *(volatile uint8_t *)at = to;
    VirtualProtect((void *)at, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)at, 1);
}
#else
static void set_byte(uintptr_t at, uint8_t from, uint8_t to) { (void)at; (void)from; (void)to; }
#endif

static void read_ini(const char *ini)
{
    clock_on = GetPrivateProfileIntA("okplay", "rr_clock", 1, ini);
    slide_k = GetPrivateProfileIntA("okplay", "rr_slide", 8, ini);
    if (slide_k > 32) slide_k = 32;
    air_steps = GetPrivateProfileIntA("okplay", "rr_air", 6, ini);
    air_players = GetPrivateProfileIntA("okplay", "rr_air_players", 20, ini);
    hist_on = GetPrivateProfileIntA("okplay", "rr_history", 1, ini);
    echo_on = GetPrivateProfileIntA("okplay", "rr_ai_echo", 1, ini);
    int ow = GetPrivateProfileIntA("okplay", "rr_ai_owners", 1, ini);
    if (B && ow != owners_on) {
        if (ow) set_byte(A(B->bcast), 1, 0); else set_byte(A(B->bcast), 0, 1);
    }
    owners_on = ow;
}

#ifndef RRF_SIM
int rrf_init(const char *ini)   /* DllMain (Road Rash only): 1 = this build is known and the fixes are in */
{
    char build[17] = "", exe[64] = "";
    okp_exe_build(build, exe, sizeof(exe));
    for (unsigned i = 0; i < sizeof(builds) / sizeof(builds[0]) && !B; i++)
        if (!strcmp(build, builds[i].build)) B = &builds[i];
    if (!B) {
        okp_log(1, "rr: %s build %s is not one okplay knows - its netcode is left as it is", exe, build[0] ? build : "?");
        return 0;
    }
    base = (uintptr_t)GetModuleHandleA(NULL);
    if (!hook_call(A(B->pred_site), (void *)pred_wrap, &rrf_orig_pred) ||
        !hook_call(A(B->hist_site), (void *)hist_wrap, &rrf_orig_hist)) {
        okp_log(0, "rr: %s (%s): the hook points are not as expected - netcode left as it is", exe, B->name);
        B = NULL;
        return 0;
    }
    owners_on = 0;   /* (read_ini applies it) */
    read_ini(ini);
    okp_log(1, "rr: Road Rash %s - clock correction %s, slide %s, height guess %s, own history per rider %s, "
               "AI echoes %s, AI owner resend %s", B->name, clock_on ? "on" : "off",
            slide_k >= 2 ? "on" : "off", air_steps > 0 ? "limited" : "as the game", hist_on ? "on" : "off",
            echo_on ? "ignored" : "kept", owners_on ? "on" : "off");
    return 1;
}
#else
int rrf_sim_setup(void *mem, const char *ini)   /* mem: the CD build's addresses 0x400000.. */
{
    B = &builds[0];
    base = (uintptr_t)mem;
    read_ini(ini);
    return 1;
}
#endif

void rrf_knobs(const char *ini) { if (B) read_ini(ini); }   /* okplay.ini read again (every 2 s) */

/* ---- the clock-offset exchange, through okplay's control messages ('R') */
void rrf_game_out(const uint8_t *d, int len)   /* a race packet this PC sends: whose riders are ours */
{
    if (!B || len < 2 || d[0] != 10) return;
    DWORD ms = ms_now();
    for (int slot = 0; slot < 3; slot++) {
        int off = 2 + 24 * slot;
        if (!(d[1] & (1 << slot)) || off + 5 > len) continue;
        if ((d[off] >> 4) == 1) sent_ms[d[off] & 15] = ms;
    }
}

static uint16_t sent_mask(void)
{
    DWORD ms = ms_now();
    uint16_t m = 0;
    for (int i = 0; i < 16; i++) if (sent_ms[i] && ms - sent_ms[i] < 3000) m |= (uint16_t)(1 << i);
    return m;
}

/* every second: [my riders, 2 bytes] [count], then per remote rider [index] [lag x16, 2] [my offset x16, 2] */
int rrf_ctrl_due(uint8_t *b, int max)
{
    if (!B || !clock_on || max < 3 + 16 * 5) return 0;
    DWORD ms = ms_now();
    if (ms - clk_due_ms < 1000) return 0;
    clk_due_ms = ms;
    uint16_t mine = sent_mask();
    if (!mine) return 0;
    int n = 0, at = 3;
    for (int i = 0; i < 16; i++) {
        rrf_rider *r = &riders[i];
        if ((mine >> i) & 1 || r->reports < 8) continue;
        int32_t o = clk_state[i] ? clk_off16[i] : 0x7fff;
        b[at] = (uint8_t)i;
        b[at + 1] = (uint8_t)(r->lag16 & 255); b[at + 2] = (uint8_t)((r->lag16 >> 8) & 255);
        b[at + 3] = (uint8_t)(o & 255); b[at + 4] = (uint8_t)((o >> 8) & 255);
        at += 5;
        n++;
    }
    if (!n) return 0;
    b[0] = (uint8_t)(mine & 255); b[1] = (uint8_t)(mine >> 8); b[2] = (uint8_t)n;
    return at;
}

void rrf_ctrl(uint64_t from, const uint8_t *b, int n)
{
    if (!B || n < 3) return;
    uint16_t theirs = (uint16_t)(b[0] | b[1] << 8), mine = sent_mask();
    int cnt = b[2];
    int32_t them_sum = 0, them_n = 0, me_sum = 0, me_n = 0, their_off = 0x7fff;
    for (int k = 0; k < cnt && 3 + k * 5 + 4 < n; k++) {   /* how late my reports reach them */
        const uint8_t *e = b + 3 + k * 5;
        int16_t lag16 = (int16_t)(e[1] | e[2] << 8), o16 = (int16_t)(e[3] | e[4] << 8);
        if ((mine >> (e[0] & 15)) & 1) { them_sum += lag16; them_n++; if (o16 != 0x7fff) their_off = o16; }
    }
    for (int i = 0; i < 16; i++)                             /* how late theirs reach me */
        if ((theirs >> i) & 1 && riders[i].reports >= 8) { me_sum += riders[i].lag16; me_n++; }
    if (!them_n || !me_n) return;
    int32_t off16 = (me_sum / me_n - them_sum / them_n) / 2;
    int agree = their_off != 0x7fff && off16 + their_off <= 16 && off16 + their_off >= -16;
    for (int i = 0; i < 16; i++)
        if ((theirs >> i) & 1) {
            if (agree) { clk_off16[i] = off16; clk_state[i] = 2; }
            else if (clk_state[i] != 2) { clk_off16[i] = off16; clk_state[i] = 1; }
        }
    DWORD ms = ms_now();
    if (ms - clk_log_ms >= 30000) {
        clk_log_ms = ms;
        okp_log(2, "rr: clock offset to %016llx: %d/16 steps (%s)", (unsigned long long)from, off16,
                agree ? "agreed, in use" : "not agreed yet");
    }
}

/* AI bikers this PC runs, echoed back by the host (it relays to every slot but the one numbered like the rider):
 * taken out of the packet before the game sees it. 1 = deliver `copy` instead. */
int rrf_game_filter(const uint8_t *d, int len, uint8_t *copy)
{
    if (!B || !echo_on || len < 2 || d[0] != 10) return 0;
    DWORD ms = ms_now();
    uint8_t mask = d[1];
    for (int slot = 0; slot < 3; slot++) {
        int off = 2 + 24 * slot;
        if (!(d[1] & (1 << slot)) || off + 5 > len) continue;
        int idx = d[off] & 15;
        if ((d[off] >> 4) == 1 && idx >= 2 && sent_ms[idx] && ms - sent_ms[idx] < 200) mask &= (uint8_t)~(1 << slot);
    }
    if (mask == d[1]) return 0;
    memcpy(copy, d, len);
    copy[1] = mask;
    return 1;
}

int rrf_clock_applied(int rider)   /* 1 = the guess for this rider already gets the corrected step */
{
    return B && clock_on && rider >= 0 && rider < 16 && clk_state[rider] == 2;
}
