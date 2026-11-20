/* okplay wsock - Need for Speed III: the addresses okplay uses, found in the loaded game (nfs3_find.h). [game: NFS3]
 *
 * Nothing of the game is in here: no byte patterns, no code. The finder knows only
 *  - generic x86 (instruction lengths, and what a few common forms do: call, push, mov, xor, cmp, ret n),
 *  - the game's own names: the menu names "connect" / "mainmult" and the codelink tokens "tcp", "SET_PLAYERNAME1",
 *    "SHOW_PLAYERNAME1", which the game's menu files use,
 *  - structure offsets and conventions: a menu handler entry is {name, -, -, enter, leave, idle}; a menu element's
 *    action callback is at +0x64 and its text pointer at +0x3c; the game's functions take eax, edx, ebx, ecx, then
 *    stack arguments the callee pops (ret n), and keep every register but eax.
 * From the two handler entries it follows the code: each function is read once, linearly, with a small register
 * tracker (constants, call results, stack arguments, loads), and the facts it finds (calls with their arguments,
 * stores, compares) lead to the next function. Each value must be reached by two routes that agree, or pass a check
 * of what it is (a getter reads the variable the popup sets; the box's last button has the Join box's Cancel text...).
 * Anything unexpected: "not found" (f->why says which check), and okplay keeps its Windows boxes. */
#include "nfs3_find.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- the image ---- */
#define SCN_CODE  0x00000020u
#define SCN_IDATA 0x00000040u
#define SCN_DISC  0x02000000u
#define SCN_EXEC  0x20000000u
#define SCN_WRITE 0x80000000u

typedef struct { uint32_t va, size, ch; } nf_sec;
typedef struct {
    nf_read_fn rdf;
    void *ctx;
    uint32_t base, stamp;
    int nsec;
    nf_sec sec[24];
} nf_img;

static int rd(const nf_img *im, uint32_t va, void *out, unsigned n) { return im->rdf(im->ctx, va, out, n); }

static const nf_sec *sec_of(const nf_img *im, uint32_t a)
{
    for (int i = 0; i < im->nsec; i++)
        if (a >= im->sec[i].va && a - im->sec[i].va < im->sec[i].size) return &im->sec[i];
    return NULL;
}
static int in_code(const nf_img *im, uint32_t a) { const nf_sec *s = sec_of(im, a); return s && (s->ch & (SCN_CODE | SCN_EXEC)); }
static int in_rw(const nf_img *im, uint32_t a) { const nf_sec *s = sec_of(im, a); return s && (s->ch & SCN_WRITE) && !(s->ch & SCN_DISC); }

static int read_pe(nf_img *im)
{
    uint32_t lfanew = 0, sig = 0;
    uint8_t fh[20], sh[40];
    if (!rd(im, im->base + 0x3c, &lfanew, 4) || lfanew > 0x1000 || !rd(im, im->base + lfanew, &sig, 4) || sig != 0x4550) return 0;
    if (!rd(im, im->base + lfanew + 4, fh, sizeof(fh))) return 0;
    int n = fh[2] | fh[3] << 8, opt = fh[16] | fh[17] << 8;
    memcpy(&im->stamp, fh + 4, 4);
    if (n < 1 || n > 24) return 0;
    for (int i = 0; i < n; i++) {
        if (!rd(im, im->base + lfanew + 24 + opt + 40u * i, sh, sizeof(sh))) return 0;
        uint32_t vs, va, rs, ch;
        memcpy(&vs, sh + 8, 4); memcpy(&va, sh + 12, 4); memcpy(&rs, sh + 16, 4); memcpy(&ch, sh + 36, 4);
        nf_sec *s = &im->sec[im->nsec++];
        s->va = im->base + va;
        s->size = vs > rs ? vs : rs;   /* (the game's linker leaves VirtualSize 0; a packed exe's raw size is the packed one) */
        s->ch = ch;
    }
    return 1;
}

static int read_str(const nf_img *im, uint32_t a, char *out, int n)   /* 1 = a NUL-ended string that fits */
{
    for (int i = 0; i < n; i++) {
        if (!rd(im, a + i, &out[i], 1)) return 0;
        if (!out[i]) return 1;
    }
    return 0;
}
static int str_is(const nf_img *im, uint32_t a, const char *s)
{
    char b[40];
    return a && read_str(im, a, b, sizeof(b)) && !strcmp(b, s);
}

/* ---- x86 (32-bit): instruction lengths, operands ---- */
enum { F_M = 1, F_B = 2, F_W = 4, F_Z = 8, F_X = 16, F_P = 32, F_A = 64 };
#define MB (F_M | F_B)
#define MZ (F_M | F_Z)
static const uint8_t op1[256] = {
    F_M, F_M, F_M, F_M, F_B, F_Z, 0, 0,   F_M, F_M, F_M, F_M, F_B, F_Z, 0, F_X,      /* 00 */
    F_M, F_M, F_M, F_M, F_B, F_Z, 0, 0,   F_M, F_M, F_M, F_M, F_B, F_Z, 0, 0,        /* 10 */
    F_M, F_M, F_M, F_M, F_B, F_Z, F_P, 0, F_M, F_M, F_M, F_M, F_B, F_Z, F_P, 0,      /* 20 */
    F_M, F_M, F_M, F_M, F_B, F_Z, F_P, 0, F_M, F_M, F_M, F_M, F_B, F_Z, F_P, 0,      /* 30 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                                  /* 40 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                                  /* 50 */
    0, 0, F_M, F_M, F_P, F_P, F_P, F_X, F_Z, MZ, F_B, MB, 0, 0, 0, 0,                /* 60 */
    F_B, F_B, F_B, F_B, F_B, F_B, F_B, F_B, F_B, F_B, F_B, F_B, F_B, F_B, F_B, F_B,  /* 70 */
    MB, MZ, MB, MB, F_M, F_M, F_M, F_M, F_M, F_M, F_M, F_M, F_M, F_M, F_M, F_M,      /* 80 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, F_X, 0, 0, 0, 0, 0,                                /* 90 */
    F_A, F_A, F_A, F_A, 0, 0, 0, 0, F_B, F_Z, 0, 0, 0, 0, 0, 0,                      /* a0 */
    F_B, F_B, F_B, F_B, F_B, F_B, F_B, F_B, F_Z, F_Z, F_Z, F_Z, F_Z, F_Z, F_Z, F_Z,  /* b0 */
    MB, MB, F_W, 0, F_M, F_M, MB, MZ, F_W | F_B, 0, F_W, 0, 0, F_B, 0, 0,            /* c0 */
    F_M, F_M, F_M, F_M, F_B, F_B, F_X, 0, F_M, F_M, F_M, F_M, F_M, F_M, F_M, F_M,    /* d0 */
    F_B, F_B, F_B, F_B, F_B, F_B, F_B, F_B, F_Z, F_Z, F_X, F_B, 0, 0, 0, 0,          /* e0 */
    F_P, F_X, F_P, F_P, 0, 0, F_M, F_M, 0, 0, 0, 0, 0, 0, F_M, F_M,                  /* f0 */
};

static int op2(uint8_t c)   /* after 0F */
{
    if (c >= 0x80 && c <= 0x8f) return F_Z;                       /* jcc rel32 */
    if ((c >= 0x90 && c <= 0x9f) || (c >= 0x40 && c <= 0x4f)) return F_M;
    if (c <= 0x03) return F_M;
    if (c == 0x31 || c == 0xa2 || (c >= 0xc8 && c <= 0xcf) || c == 0xa0 || c == 0xa1 || c == 0xa8 || c == 0xa9) return 0;
    if (c == 0xa4 || c == 0xac || c == 0xba || c == 0xc2 || (c >= 0xc4 && c <= 0xc6) || (c >= 0x70 && c <= 0x73)) return MB;
    if (c == 0xa3 || c == 0xa5 || c == 0xab || c == 0xad || c == 0xaf || c == 0xb0 || c == 0xb1 || c == 0xb3 ||
        (c >= 0xb6 && c <= 0xb7) || (c >= 0xbb && c <= 0xbf) || c == 0xc0 || c == 0xc1) return F_M;
    if ((c >= 0x10 && c <= 0x17) || (c >= 0x28 && c <= 0x2f) || (c >= 0x50 && c <= 0x6f) || (c >= 0x74 && c <= 0x7f) ||
        c >= 0xd0) return F_M;
    return F_X;
}

typedef struct {
    uint32_t at, next;
    int p66, rep, op;          /* op: one byte, or 0x100 | the byte after 0F */
    int m, mod, reg, rm;       /* m: has a ModRM byte */
    int memabs, base;          /* memory operand [disp32] / [base + disp] (no index; base = -1 otherwise) */
    int32_t disp;
    uint32_t imm;
    uint32_t tgt;              /* call / jmp / jcc / loop: the target */
} nf_ins;

static int decode(const nf_img *im, uint32_t va, nf_ins *d)
{
    uint8_t b[16];
    int avail = 16, p = 0, f;
    if (!rd(im, va, b, 16)) { avail = 8; if (!rd(im, va, b, 8)) return 0; }
#define NEED(k) do { if (p + (k) > avail) return 0; } while (0)
    memset(d, 0, sizeof(*d));
    d->at = va;
    d->base = -1;
    for (;;) {
        NEED(1);
        uint8_t c = b[p];
        if (c == 0x66) d->p66 = 1;
        else if (c == 0xf2 || c == 0xf3) d->rep = 1;
        else if (c != 0xf0 && c != 0x26 && c != 0x2e && c != 0x36 && c != 0x3e && c != 0x64 && c != 0x65) break;
        if (++p > 4) return 0;
    }
    uint8_t c = b[p++];
    if (c == 0x0f) { NEED(1); d->op = 0x100 | b[p]; f = op2(b[p]); p++; }
    else { d->op = c; f = op1[c]; }
    if (f & (F_X | F_P)) return 0;
    if (f & F_M) {
        NEED(1);
        uint8_t m = b[p++];
        d->m = 1; d->mod = m >> 6; d->reg = (m >> 3) & 7; d->rm = m & 7;
        if (d->mod != 3) {
            int base = d->rm, idx = 0;
            if (d->rm == 4) {
                NEED(1);
                uint8_t s = b[p++];
                base = s & 7;
                idx = ((s >> 3) & 7) != 4;
                if (base == 5 && d->mod == 0) { NEED(4); memcpy(&d->disp, b + p, 4); p += 4; base = -1; }
            } else if (d->rm == 5 && d->mod == 0) { NEED(4); memcpy(&d->disp, b + p, 4); p += 4; base = -1; }
            if (d->mod == 1) { NEED(1); d->disp = (int8_t)b[p++]; }
            else if (d->mod == 2) { NEED(4); memcpy(&d->disp, b + p, 4); p += 4; }
            if (!idx) { if (base < 0) d->memabs = 1; else d->base = base; }
        }
    }
    if (d->op == 0xf6 && d->reg <= 1) f |= F_B;
    if (d->op == 0xf7 && d->reg <= 1) f |= F_Z;
    if (f & F_W) { NEED(2); d->imm = b[p] | b[p + 1] << 8; p += 2; }
    if (f & F_B) { NEED(1); if (!(f & F_W)) d->imm = b[p]; p++; }
    if (f & F_Z) {
        int rel = d->op == 0xe8 || d->op == 0xe9 || (d->op >= 0x180 && d->op <= 0x18f);
        if (d->p66 && rel) return 0;
        if (d->p66) { NEED(2); d->imm = b[p] | b[p + 1] << 8; p += 2; }
        else { NEED(4); memcpy(&d->imm, b + p, 4); p += 4; }
    }
    if (f & F_A) { NEED(4); memcpy(&d->disp, b + p, 4); p += 4; d->memabs = 1; }
#undef NEED
    d->next = va + p;
    if ((d->op >= 0x70 && d->op <= 0x7f) || d->op == 0xeb || (d->op >= 0xe0 && d->op <= 0xe3)) d->tgt = d->next + (int8_t)d->imm;
    else if (d->op == 0xe8 || d->op == 0xe9 || (d->op >= 0x180 && d->op <= 0x18f)) d->tgt = d->next + d->imm;
    return 1;
}

static int is_jcc(int op) { return (op >= 0x70 && op <= 0x7f) || (op >= 0x180 && op <= 0x18f) || (op >= 0xe0 && op <= 0xe3); }
static int is_jmp(int op) { return op == 0xe9 || op == 0xeb; }

/* ---- reading one function ---- */
enum { V_UNK, V_CONST, V_RET, V_ARG, V_MEM };
typedef struct { uint8_t k, sz; uint32_t v; } nv;   /* a tracked value: unknown, constant, result of call #v, stack
                                                     * argument #v, or the contents of [v] (sz bytes) */
#define NPUSH 16
#define NARG 10
typedef struct { nv r[8]; nv pend[NPUSH]; int npend; } nst;   /* registers; values pushed since the last call */
typedef struct { uint32_t at, tgt; int npush; nv arg[NARG]; nv r[4]; } ncall;   /* arg[0] = the first stack argument */
typedef struct { uint32_t addr; int32_t disp; nv base, val; uint8_t size, abs; } nstore;
typedef struct { uint32_t addr, imm; uint8_t size; } ncmp;
#define FN_CALL 64
#define FN_ST 96
#define FN_REF 128
#define FN_CMP 24
typedef struct {
    uint32_t start, end;
    int ok, retn, nret, ninsn, full;   /* retn: bytes the callee pops (-1 = not the same at every ret); full: a list ran over */
    nv reteax;
    int ncall; ncall call[FN_CALL];
    int nst; nstore st[FN_ST];
    int nref; uint32_t ref[FN_REF];    /* every absolute address operand and 32-bit immediate */
    int ncmp; ncmp cmp[FN_CMP];
} nf_fn;

#define NTG 384
#define NPEND 128
typedef struct {
    nf_img *im;
    int nfn;
    nf_fn *fn[96];
    uint32_t tg[NTG]; uint8_t tgback[NTG]; int ntg;          /* pass 1: branch targets */
    struct { uint32_t t; nst s; } pend[NPEND]; int npend;    /* pass 2: states waiting at forward targets */
} nf_ctx;

static const nv UNK = { V_UNK, 0, 0 };
static nv mk(int k, uint32_t v, int sz) { nv x; x.k = (uint8_t)k; x.sz = (uint8_t)sz; x.v = v; return x; }
static int nv_eq(nv a, nv b) { return a.k == b.k && a.v == b.v && a.sz == b.sz; }

static void st_unk(nst *s) { for (int i = 0; i < 8; i++) s->r[i] = UNK; s->npend = 0; }
static void st_merge(nst *d, const nst *s)
{
    for (int i = 0; i < 8; i++) if (!nv_eq(d->r[i], s->r[i])) d->r[i] = UNK;
    int same = d->npend == s->npend;
    for (int i = 0; same && i < d->npend; i++) same = nv_eq(d->pend[i], s->pend[i]);
    if (!same) d->npend = 0;
}
static void push(nst *s, nv v)
{
    if (s->npend == NPUSH) { memmove(s->pend, s->pend + 1, sizeof(nv) * (NPUSH - 1)); s->npend--; }
    s->pend[s->npend++] = v;
}
static nv pop(nst *s) { return s->npend ? s->pend[--s->npend] : UNK; }
static nv byte_of(const nst *s, int r8)   /* al..bl, ah..bh */
{
    nv v = s->r[r8 & 3];
    if (v.k != V_CONST) return UNK;
    return mk(V_CONST, r8 < 4 ? (v.v & 0xff) : ((v.v >> 8) & 0xff), 1);
}
static void set_byte(nst *s, int r8, uint32_t b)
{
    nv *v = &s->r[r8 & 3];
    if (v->k != V_CONST) { *v = UNK; return; }
    v->v = r8 < 4 ? ((v->v & ~0xffu) | (b & 0xff)) : ((v->v & ~0xff00u) | (b & 0xff) << 8);
}

static void add_ref(nf_fn *f, uint32_t a) { if (f->nref < FN_REF) f->ref[f->nref++] = a; else f->full = 1; }
static void add_store(nf_fn *f, const nf_ins *d, int size, nv val)
{
    if (!d->memabs && d->base < 0) return;
    if (f->nst == FN_ST) { f->full = 1; return; }
    nstore *t = &f->st[f->nst++];
    memset(t, 0, sizeof(*t));
    t->abs = (uint8_t)d->memabs;
    t->addr = d->memabs ? (uint32_t)d->disp : 0;
    t->disp = d->disp;
    t->size = (uint8_t)size;
    t->val = val;
    t->base = UNK;
    if (val.k == V_CONST) { t->val.sz = (uint8_t)size; if (size < 4) t->val.v &= size == 1 ? 0xffu : 0xffffu; }
}

/* pass 1: the function's extent (linear, until a ret / jmp past every forward branch target) and its targets */
static int extent(nf_ctx *c, uint32_t s, uint32_t *end)
{
    uint32_t a = s, last = s;
    nf_ins d;
    c->ntg = 0;
    for (;;) {
        if (a - s > 0x3000 || !decode(c->im, a, &d)) return 0;
        int flow_end = d.op == 0xc3 || d.op == 0xc2 || is_jmp(d.op) || (d.op == 0xff && d.m && (d.reg == 4 || d.reg == 5));
        if (is_jcc(d.op) || is_jmp(d.op)) {
            int inside = d.tgt >= s && d.tgt - s < 0x3000;
            if (inside) {   /* (a jmp elsewhere is a tail call) */
                if (c->ntg == NTG) return 0;
                c->tgback[c->ntg] = d.tgt <= a;
                c->tg[c->ntg++] = d.tgt;
                if (d.tgt > last) last = d.tgt;
            }
        }
        a = d.next;
        if (flow_end && a > last) { *end = a; return 1; }
    }
}

static void pend_add(nf_ctx *c, uint32_t t, const nst *s, nf_fn *f)
{
    for (int i = 0; i < c->npend; i++) if (c->pend[i].t == t) { st_merge(&c->pend[i].s, s); return; }
    if (c->npend == NPEND) { f->full = 1; return; }
    c->pend[c->npend].t = t;
    c->pend[c->npend++].s = *s;
}

/* pass 2: what the function does, instruction by instruction, with the register tracker */
static int analyse(nf_ctx *c, uint32_t start, nf_fn *f)
{
    memset(f, 0, sizeof(*f));
    f->start = start;
    f->retn = -2;
    if (!extent(c, start, &f->end)) return 0;
    nst S;
    st_unk(&S);
    int flow = 1, frame = 0;
    uint32_t argbase = 0;
    nf_ins d;
    c->npend = 0;
    for (uint32_t a = start; a < f->end; a = d.next) {
        if (!decode(c->im, a, &d)) return 0;
        f->ninsn++;
        int back = 0;
        for (int i = 0; i < c->ntg; i++) if (c->tg[i] == a && c->tgback[i]) back = 1;
        int got = !flow ? 0 : 1;
        for (int i = 0; i < c->npend; ) {
            if (c->pend[i].t != a) { i++; continue; }
            if (got) st_merge(&S, &c->pend[i].s); else { S = c->pend[i].s; got = 1; }
            c->pend[i] = c->pend[--c->npend];
        }
        if (!got || back) st_unk(&S);   /* (a loop head: what the loop changes is not followed) */
        flow = 1;
        int o = d.op, rr = d.reg, rm = d.rm, m3 = d.m && d.mod == 3;
        if (d.memabs) add_ref(f, (uint32_t)d.disp);
        if (o == 0xe8) {
            if (f->ncall == FN_CALL) { f->full = 1; S.r[0] = UNK; }
            else {
                ncall *k = &f->call[f->ncall];
                memset(k, 0, sizeof(*k));
                k->at = a; k->tgt = d.tgt; k->npush = S.npend;
                for (int i = 0; i < NARG && i < S.npend; i++) k->arg[i] = S.pend[S.npend - 1 - i];
                for (int i = 0; i < 4; i++) k->r[i] = S.r[i];
                S.r[0] = mk(V_RET, (uint32_t)f->ncall, 4);
                f->ncall++;
            }
            S.npend = 0;
        } else if (o == 0xff && d.m && (rr == 2 || rr == 3)) { S.r[0] = S.r[1] = S.r[2] = UNK; S.npend = 0; }
        else if (o == 0xc3 || o == 0xc2) {
            int n = o == 0xc2 ? (int)d.imm : 0;
            if (f->nret++ == 0) { f->retn = n; f->reteax = S.r[0]; }
            else { if (f->retn != n) f->retn = -1; if (!nv_eq(f->reteax, S.r[0])) f->reteax = UNK; }
            flow = 0;
        } else if (is_jmp(o) || (o == 0xff && d.m && (rr == 4 || rr == 5))) { if (is_jmp(o) && d.tgt > a && d.tgt < f->end) pend_add(c, d.tgt, &S, f); flow = 0; }
        else if (is_jcc(o)) { if (o >= 0xe0 && o <= 0xe2) S.r[1] = UNK; if (d.tgt > a && d.tgt < f->end) pend_add(c, d.tgt, &S, f); }
        else if (o >= 0x50 && o <= 0x57) push(&S, S.r[o - 0x50]);
        else if (o >= 0x58 && o <= 0x5f) { nv v = pop(&S); S.r[o - 0x58] = (o - 0x58 == 4 || o - 0x58 == 5) ? UNK : v; }
        else if (o == 0x68) { push(&S, mk(V_CONST, d.imm, 4)); add_ref(f, d.imm); }
        else if (o == 0x6a) push(&S, mk(V_CONST, (uint32_t)(int32_t)(int8_t)d.imm, 4));
        else if (o == 0x60) { for (int i = 0; i < 8; i++) push(&S, UNK); }
        else if (o == 0x61) { for (int i = 0; i < 8; i++) pop(&S); st_unk(&S); }
        else if (o == 0x9c || o == 0x06 || o == 0x0e || o == 0x16 || o == 0x1e || o == 0x1a0 || o == 0x1a8) push(&S, UNK);
        else if (o == 0x9d || o == 0x07 || o == 0x17 || o == 0x1f || o == 0x1a1 || o == 0x1a9) pop(&S);
        else if (o == 0x8f) { pop(&S); if (m3) S.r[rm] = UNK; }
        else if (o == 0xff && d.m && rr == 6) push(&S, d.memabs ? mk(V_MEM, (uint32_t)d.disp, 4) : m3 ? S.r[rm] : UNK);
        else if ((o == 0x89 && m3 && rr == 4 && rm == 5) || (o == 0x8b && m3 && rr == 5 && rm == 4)) {   /* mov ebp, esp */
            frame = 1; argbase = 4u * S.npend + 4; S.npend = 0;
        } else if (o >= 0xb8 && o <= 0xbf) { S.r[o - 0xb8] = d.p66 ? UNK : mk(V_CONST, d.imm, 4); if (!d.p66) add_ref(f, d.imm); }
        else if (o >= 0xb0 && o <= 0xb7) set_byte(&S, o - 0xb0, d.imm);
        else if (o == 0x89 || o == 0x88) {
            if (m3) { if (o == 0x89 && !d.p66) S.r[rm] = S.r[rr]; else S.r[rm & (o == 0x88 ? 3 : 7)] = UNK; }
            else add_store(f, &d, o == 0x88 ? 1 : d.p66 ? 2 : 4, o == 0x88 ? byte_of(&S, rr) : S.r[rr]);
        } else if (o == 0x8b || o == 0x8a) {
            nv v = UNK;
            if (!m3) {
                if (d.memabs) v = mk(V_MEM, (uint32_t)d.disp, o == 0x8a ? 1 : d.p66 ? 2 : 4);
                else if (frame && d.base == 5 && o == 0x8b && !d.p66 && d.disp >= (int32_t)argbase && !((d.disp - argbase) & 3))
                    v = mk(V_ARG, (uint32_t)(d.disp - argbase) / 4, 4);
            } else if (o == 0x8b && !d.p66) v = S.r[rm];
            if (o == 0x8b) S.r[rr] = d.p66 ? UNK : v;
            else S.r[rr & 3] = (rr == 0 && v.k == V_MEM) ? v : UNK;
        } else if (o == 0xa1) S.r[0] = d.p66 ? UNK : mk(V_MEM, (uint32_t)d.disp, 4);
        else if (o == 0xa0) S.r[0] = mk(V_MEM, (uint32_t)d.disp, 1);
        else if (o == 0xa3) add_store(f, &d, d.p66 ? 2 : 4, S.r[0]);
        else if (o == 0xa2) add_store(f, &d, 1, byte_of(&S, 0));
        else if (o == 0xc7 || o == 0xc6) {
            int sz = o == 0xc6 ? 1 : d.p66 ? 2 : 4;
            if (o == 0xc7 && !d.p66) add_ref(f, d.imm);
            if (m3) { if (sz == 4) S.r[rm] = mk(V_CONST, d.imm, 4); else if (sz == 1) set_byte(&S, rm, d.imm); else S.r[rm] = UNK; }
            else {
                int n0 = f->nst;
                add_store(f, &d, sz, mk(V_CONST, d.imm, sz));
                if (f->nst > n0 && d.base >= 0) f->st[f->nst - 1].base = S.r[d.base];
            }
        } else if ((o == 0x31 || o == 0x33 || o == 0x29 || o == 0x2b) && m3 && rr == rm) S.r[rr] = d.p66 ? UNK : mk(V_CONST, 0, 4);
        else if ((o == 0x83 || o == 0x81) && m3 && rm == 4) {   /* add / sub esp */
            if (rr == 0) for (uint32_t i = 0; i < (o == 0x83 ? (uint32_t)(int8_t)d.imm : d.imm) / 4 && i < NPUSH; i++) pop(&S);
            else S.npend = 0;
        } else if ((o == 0x80 || o == 0x81 || o == 0x83) && rr == 7) {   /* cmp */
            if (d.memabs && f->ncmp < FN_CMP) {
                ncmp *k = &f->cmp[f->ncmp++];
                k->addr = (uint32_t)d.disp;
                k->size = (uint8_t)(o == 0x80 ? 1 : d.p66 ? 2 : 4);
                k->imm = o == 0x83 || o == 0x80 ? (uint32_t)(int32_t)(int8_t)d.imm : d.imm;
            }
        } else if ((o >= 0x38 && o <= 0x3d) || o == 0x84 || o == 0x85 || o == 0xa8 || o == 0xa9 || ((o == 0xf6 || o == 0xf7) && rr <= 1) ||
                   o == 0x90 || o == 0x9b || o == 0x9e || o == 0xc9 || (o >= 0xf8 && o <= 0xfd) || o == 0xf5 || o == 0xcc) {
            /* (compare, test, nop, wait, sahf, leave, flags: no register written) */
        } else if (o >= 0xd8 && o <= 0xdf) { if (o == 0xdf && m3 && rr == 4) S.r[0] = UNK; }   /* x87 (fnstsw ax) */
        else if (o >= 0xa4 && o <= 0xaf) { S.r[6] = S.r[7] = UNK; if (d.rep) S.r[1] = UNK; if (o == 0xac || o == 0xad) S.r[0] = UNK; }
        else if (o >= 0x40 && o <= 0x4f) { nv *v = &S.r[o & 7]; if (v->k == V_CONST && !d.p66) v->v += o < 0x48 ? 1 : (uint32_t)-1; else *v = UNK; }
        else if (o >= 0x91 && o <= 0x97) { nv t = S.r[0]; S.r[0] = d.p66 ? UNK : S.r[o - 0x90]; S.r[o - 0x90] = d.p66 ? UNK : t; }
        else if (o == 0x98 || o == 0x9f || o == 0xd4 || o == 0xd5 || o == 0x27 || o == 0x2f || o == 0x37 || o == 0x3f ||
                 o == 0xe4 || o == 0xe5 || o == 0xec || o == 0xed) S.r[0] = UNK;
        else if (o == 0x99) S.r[2] = UNK;
        else if (o == 0x131) S.r[0] = S.r[2] = UNK;
        else if (o == 0x1a2) S.r[0] = S.r[1] = S.r[2] = S.r[3] = UNK;
        else if ((o & 0xc7) == 0x02 && o < 0x40) S.r[rr & 3] = UNK;   /* op r8, r/m8 */
        else if (((o & 0xc7) == 0x03 && o < 0x40) || o == 0x8d || o == 0x69 || o == 0x6b || o == 0x63 || o == 0x1af ||
                 o == 0x1b6 || o == 0x1b7 || o == 0x1be || o == 0x1bf || o == 0x1bc || o == 0x1bd || (o >= 0x140 && o <= 0x14f))
            S.r[rr] = UNK;                                              /* op r32, r/m32 */
        else if (((o & 0xc6) == 0x00 && o < 0x40) || o == 0x80 || o == 0x81 || o == 0x83 || o == 0xc0 || o == 0xc1 ||
                 (o >= 0xd0 && o <= 0xd3) || o == 0xfe || (o == 0xff && rr <= 1) || ((o == 0xf6 || o == 0xf7) && rr <= 3) ||
                 (o >= 0x190 && o <= 0x19f) || o == 0x1a3 || o == 0x1a4 || o == 0x1a5 || o == 0x1ab || o == 0x1ac ||
                 o == 0x1ad || o == 0x1b3 || o == 0x1ba || o == 0x1bb) {   /* op r/m, ...: a register only when mod = 3 */
            int byte = (o < 0x100 && !(o & 1)) || (o >= 0x190 && o <= 0x19f);
            if (m3) S.r[byte ? (rm & 3) : rm] = UNK;
        } else if ((o == 0xf6 || o == 0xf7) && rr >= 4) S.r[0] = S.r[2] = UNK;   /* mul / div */
        else if (o == 0x86 || o == 0x87 || o == 0x1c0 || o == 0x1c1) { S.r[o & 1 ? rr : rr & 3] = UNK; if (m3) S.r[o & 1 ? rm : rm & 3] = UNK; }
        else if (o == 0x1b0 || o == 0x1b1) { S.r[0] = UNK; if (m3) S.r[o & 1 ? rm : rm & 3] = UNK; }
        else st_unk(&S);   /* anything else: nothing is known afterwards */
    }
    f->ok = f->nret > 0;
    return f->ok;
}

static nf_fn *fn_get(nf_ctx *c, uint32_t a)
{
    if (!a || !in_code(c->im, a)) return NULL;
    for (int i = 0; i < c->nfn; i++) if (c->fn[i]->start == a) return c->fn[i]->ok ? c->fn[i] : NULL;
    if (c->nfn == (int)(sizeof(c->fn) / sizeof(c->fn[0]))) return NULL;
    nf_fn *f = (nf_fn *)malloc(sizeof(nf_fn));
    if (!f) return NULL;
    c->fn[c->nfn++] = f;
    if (!analyse(c, a, f)) f->ok = 0;
    return f->ok ? f : NULL;
}

/* a getter: no calls, no stores, a few instructions, returns [v] or a constant */
static int getter(nf_ctx *c, uint32_t a, nv *out)
{
    nf_fn *f = fn_get(c, a);
    if (!f || f->ncall || f->nst || f->ninsn > 8 || f->retn != 0 || (f->reteax.k != V_MEM && f->reteax.k != V_CONST)) return 0;
    *out = f->reteax;
    return 1;
}

static int has_ref(const nf_fn *f, uint32_t a) { for (int i = 0; i < f->nref; i++) if (f->ref[i] == a) return 1; return 0; }

/* the one absolute store of value v (size sz) in f: its address; 0 = none or more than one */
static uint32_t store_of(const nf_fn *f, nv v, int sz)
{
    uint32_t at = 0;
    for (int i = 0; i < f->nst; i++) {
        const nstore *t = &f->st[i];
        if (!t->abs || t->size != sz || !nv_eq(t->val, v)) continue;
        if (at && at != t->addr) return 0;
        at = t->addr;
    }
    return at;
}

/* the text index (eax of the call) whose result f stores at [a]; -1 = none */
static int text_at(const nf_fn *f, uint32_t a, uint32_t *lookup)
{
    for (int i = f->nst - 1; i >= 0; i--) {
        const nstore *t = &f->st[i];
        if (!t->abs || t->addr != a || t->size != 4) continue;
        if (t->val.k != V_RET) return -1;
        const ncall *k = &f->call[t->val.v];
        if (k->r[0].k != V_CONST) return -1;
        if (lookup) *lookup = k->tgt;
        return (int)k->r[0].v;
    }
    return -1;
}

/* a call in f with edx = the string tok whose result gets a store [result + disp] = imm: the call's target, imm */
static int token_store(nf_ctx *c, const nf_fn *f, const char *tok, int32_t disp, uint32_t *fn, uint32_t *imm)
{
    int n = 0;
    for (int i = 0; i < f->ncall; i++) {
        const ncall *k = &f->call[i];
        if (k->r[2].k != V_CONST || !str_is(c->im, k->r[2].v, tok)) continue;
        for (int j = 0; j < f->nst; j++) {
            const nstore *t = &f->st[j];
            if (t->abs || t->disp != disp || t->size != 4 || t->base.k != V_RET || t->base.v != (uint32_t)i || t->val.k != V_CONST) continue;
            if (n && (*fn != k->tgt || *imm != t->val.v)) return 0;
            *fn = k->tgt; *imm = t->val.v; n++;
        }
    }
    return n > 0;
}

/* functions reachable from root within depth: direct calls (and, with cbs, code pointers passed as call arguments) */
static int reach(nf_ctx *c, uint32_t root, int depth, int cbs, const uint32_t *skip, int nskip, uint32_t *out, int max)
{
    int n = 0, lo = 0;
    out[n++] = root;
    for (int dpt = 0; dpt < depth; dpt++) {
        int hi = n;
        for (int i = lo; i < hi; i++) {
            nf_fn *f = fn_get(c, out[i]);
            if (!f) continue;
            for (int j = 0; j < f->ncall; j++) {
                uint32_t cand[NARG + 1];
                int nc = 0;
                cand[nc++] = f->call[j].tgt;
                for (int k = 0; cbs && k < NARG && k < f->call[j].npush; k++)
                    if (f->call[j].arg[k].k == V_CONST && in_code(c->im, f->call[j].arg[k].v)) cand[nc++] = f->call[j].arg[k].v;
                for (int k = 0; k < nc; k++) {
                    int seen = 0;
                    for (int q = 0; q < n && !seen; q++) seen = out[q] == cand[k];
                    for (int q = 0; q < nskip && !seen; q++) seen = skip[q] == cand[k];
                    if (!seen && n < max && in_code(c->im, cand[k])) out[n++] = cand[k];
                }
            }
        }
        lo = hi;
    }
    return n;
}

/* the handler table entry of menu name: {name, -, -, enter, leave, idle}; need = which handlers must be code */
static int find_entry(nf_img *im, const char *name, int need_leave, int need_idle, uint32_t *entry, uint32_t *str)
{
    uint8_t buf[4096 + 32];
    size_t nl = strlen(name);
    uint32_t sv[8];
    int ns = 0, ne = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < im->nsec; i++) {
            const nf_sec *s = &im->sec[i];
            if (!(s->ch & (SCN_CODE | SCN_IDATA)) || (s->ch & SCN_DISC)) continue;   /* (not .bss, not .reloc) */
            uint32_t keep = 0;   /* (pass 0: the last bytes of the page before, for a name across two pages) */
            for (uint32_t off = 0; off < s->size; off += 4096) {   /* (all of it: unpacked in memory, a packed exe's
                                                                      * data is beyond its raw size) */
                uint32_t n = s->size - off < 4096 ? s->size - off : 4096;
                if (!rd(im, s->va + off, buf + keep, n)) { keep = 0; continue; }
                if (pass == 0) {
                    uint32_t tot = keep + n;
                    for (uint32_t j = 0; j + nl + 2 <= tot; j++) {
                        if (buf[j] || memcmp(buf + j + 1, name, nl) || buf[j + 1 + nl]) continue;
                        uint32_t va = s->va + off - keep + j + 1;
                        int dup = 0;
                        for (int q = 0; q < ns; q++) dup |= sv[q] == va;
                        if (!dup && ns < 8) sv[ns++] = va;
                    }
                    keep = tot < 16 ? tot : 16;
                    memmove(buf, buf + tot - keep, keep);
                } else {
                    for (uint32_t j = 0; j + 24 <= n; j += 4) {
                        uint32_t v;
                        memcpy(&v, buf + j, 4);
                        for (int q = 0; q < ns; q++) {
                            if (v != sv[q]) continue;
                            uint32_t e[6];
                            if (!rd(im, s->va + off + j, e, sizeof(e)) || !in_code(im, e[3])) continue;
                            if ((need_leave && !in_code(im, e[4])) || (need_idle && !in_code(im, e[5]))) continue;
                            if (ne++) return 0;   /* more than one: not sure which */
                            *entry = s->va + off + j;
                            *str = v;
                        }
                    }
                }
            }
        }
        if (!ns) return 0;
    }
    return ne == 1;
}

int nfs3_find_image(nf_read_fn rdf, void *ctx, uint32_t base, nfs3_found *out)
{
    nfs3_found *f = out;
    nf_img im;
    nf_ctx *c = (nf_ctx *)calloc(1, sizeof(nf_ctx));
    memset(f, 0, sizeof(*f));
    memset(&im, 0, sizeof(im));
    im.rdf = rdf; im.ctx = ctx; im.base = base;
#define FAIL(...) do { snprintf(f->why, sizeof(f->why), __VA_ARGS__); goto out; } while (0)
    if (!c) FAIL("no memory");
    c->im = &im;
    if (!read_pe(&im)) FAIL("no PE header");
    nm_build *nb = &f->nm;
    nb->name = "found at launch";
    nb->pe_time[0] = im.stamp;

    /* 1. the handler table: the entries named "connect" (enter, leave) and "mainmult" (enter, idle) */
    if (!find_entry(&im, "connect", 1, 0, &nb->hentry, &nb->name_str)) FAIL("no single handler entry \"connect\"");
    if (!find_entry(&im, "mainmult", 0, 1, &nb->lobby_entry, &nb->lobby_name)) FAIL("no single handler entry \"mainmult\"");
    uint32_t he[6], le[6];
    if (!rd(&im, nb->hentry, he, sizeof(he)) || !rd(&im, nb->lobby_entry, le, sizeof(le))) FAIL("handler entries unreadable");
    if ((nb->hentry > nb->lobby_entry ? nb->hentry - nb->lobby_entry : nb->lobby_entry - nb->hentry) % 24 ||
        (nb->hentry > nb->lobby_entry ? nb->hentry - nb->lobby_entry : nb->lobby_entry - nb->hentry) > 24 * 256)
        FAIL("the two handler entries are not in one table");
    nb->enter = he[3]; nb->leave = he[4]; nb->lobby_idle = le[5];

    /* 2. the connect menu's enter handler: the element finder (edx = "tcp") and the TCP element's callback (+0x64) */
    nf_fn *E = fn_get(c, nb->enter);
    if (!E) FAIL("connect enter handler not readable");
    if (!token_store(c, E, "tcp", 0x64, &nb->find, &nb->tcp_cb) || !in_code(&im, nb->tcp_cb)) FAIL("connect enter: no \"tcp\" callback");
    int nfind = 0;
    for (int i = 0; i < E->ncall; i++) if (E->call[i].tgt == nb->find && E->call[i].r[2].k == V_CONST) nfind++;
    if (nfind < 3) FAIL("connect enter: the element finder is called %d times", nfind);

    /* 3. the lobby: its enter handler binds the name editor (+0x64) and the name text (+0x3c) with the same finder;
     *    its idle uses the finder too, and tests the "server" flag */
    nf_fn *L = fn_get(c, le[3]), *I = fn_get(c, nb->lobby_idle);
    uint32_t fn2 = 0, fn3 = 0, setname = 0;
    if (!L || !I) FAIL("lobby handlers not readable");
    if (!token_store(c, L, "SET_PLAYERNAME1", 0x64, &fn2, &setname) || fn2 != nb->find || !in_code(&im, setname)) FAIL("lobby: no name editor");
    if (!token_store(c, L, "SHOW_PLAYERNAME1", 0x3c, &fn3, &f->fx.pname) || fn3 != nb->find || !in_rw(&im, f->fx.pname)) FAIL("lobby: no name text");
    int idle_finds = 0;
    for (int i = 0; i < I->ncall; i++) if (I->call[i].tgt == nb->find) idle_finds++;
    if (!idle_finds) FAIL("lobby idle: does not use the element finder");

    /* 4. the TCP callback opens the choice popup: the one call to a function popping 4 stack arguments */
    nf_fn *T = fn_get(c, nb->tcp_cb);
    if (!T) FAIL("TCP callback not readable");
    const ncall *cc = NULL;
    for (int i = 0; i < T->ncall; i++) {
        nf_fn *p = fn_get(c, T->call[i].tgt);
        if (!p || p->retn != 0x10) continue;
        if (cc && cc->tgt != T->call[i].tgt) FAIL("TCP callback: two popups");
        cc = &T->call[i];
    }
    if (!cc || cc->npush < 4) FAIL("TCP callback: no choice popup");
    if (cc->arg[3].k != V_CONST || !in_code(&im, cc->arg[3].v) || cc->r[3].k != V_CONST || cc->r[1].k != V_CONST ||
        cc->r[2].k != V_CONST || cc->arg[0].k != V_CONST || !in_rw(&im, cc->r[1].v) || !in_rw(&im, cc->r[2].v))
        FAIL("TCP callback: the choice popup's arguments are not as expected");
    nb->pop_choice = cc->tgt;
    nb->tcp_choice_cb = cc->arg[3].v;
    nb->tcp_nbtn = cc->r[3].v;
    nb->tcp_btns = cc->r[1].v;
    nb->tcp_help = cc->arg[0].v;
    if (nb->tcp_nbtn != 3 && nb->tcp_nbtn != 4) FAIL("TCP box: %u buttons", nb->tcp_nbtn);
    if (nb->tcp_help && !in_rw(&im, nb->tcp_help)) FAIL("TCP box: help lines not in data");
    int btxt[4];
    uint32_t lookup = 0, lk;
    for (uint32_t i = 0; i < nb->tcp_nbtn; i++) {
        int seen = 0;
        for (int j = 0; j < T->nst; j++) seen |= T->st[j].abs && T->st[j].addr == nb->tcp_btns + 4 * i;
        if (!seen) FAIL("TCP box: button %u's text is not set by the callback", i);
        btxt[i] = text_at(T, nb->tcp_btns + 4 * i, &lk);
        if (btxt[i] >= 0) { if (lookup && lk != lookup) FAIL("TCP box: two text lookups"); lookup = lk; }
    }
    if (nb->tcp_help) {
        int seen = 0;
        for (int j = 0; j < T->nst; j++) seen |= T->st[j].abs && T->st[j].addr >= nb->tcp_help && T->st[j].addr < nb->tcp_help + 4 * nb->tcp_nbtn;
        if (!seen) FAIL("TCP box: help lines not set by the callback");
    }

    /* 5. the choice popup: callback (4th stack argument) and default button (2nd) go into the popup's variables */
    nf_fn *P = fn_get(c, nb->pop_choice);
    if (!P) FAIL("choice popup not readable");
    nb->pop_cbvar = store_of(P, mk(V_ARG, 3, 4), 4);
    nb->pop_btnvar = store_of(P, mk(V_ARG, 1, 4), 4);
    if (!in_rw(&im, nb->pop_cbvar) || !in_rw(&im, nb->pop_btnvar)) FAIL("choice popup: callback / button variables not found");

    /* 6. the TCP box's callback: the chosen-button getter, and the Join box opener (a text-entry popup: 5 stack
     *    arguments, the 5th its callback, the 2nd the buffer) */
    nf_fn *C = fn_get(c, nb->tcp_choice_cb);
    if (!C) FAIL("TCP box callback not readable");
    uint32_t jopen = 0;
    const ncall *jc = NULL;
    int jtitle = -1, jcancel = -1;
    for (int i = 0; i < C->ncall; i++) {
        uint32_t t = C->call[i].tgt;
        nv g;
        if (getter(c, t, &g)) {
            if (g.k == V_MEM && g.sz == 4 && g.v == nb->pop_btnvar) {
                if (nb->pop_button && nb->pop_button != t) FAIL("two chosen-button getters");
                nb->pop_button = t;
            }
            continue;
        }
        nf_fn *X = fn_get(c, t);
        if (!X) continue;
        for (int j = 0; j < X->ncall; j++) {
            const ncall *k = &X->call[j];
            nf_fn *tp = fn_get(c, k->tgt);
            if (!tp || tp->retn != 0x14 || k->npush < 5) continue;
            if (k->arg[4].k != V_CONST || !in_code(&im, k->arg[4].v) || k->arg[1].k != V_CONST || !in_rw(&im, k->arg[1].v) ||
                k->arg[2].k != V_CONST || k->arg[2].v < 1 || k->arg[2].v > 255 || k->r[3].k != V_CONST || k->r[3].v < 1 ||
                k->r[3].v > 9 || k->r[1].k != V_CONST || k->r[2].k != V_CONST)
                continue;
            if (jopen && jopen != t) FAIL("TCP box callback: two text-entry boxes");
            jopen = t; jc = k;
            jtitle = text_at(X, k->r[2].v, &lk);
            if (jtitle >= 0 && lk != lookup) jtitle = -1;
            jcancel = text_at(X, k->r[1].v + 4 * (k->r[3].v - 1), &lk);
            if (jcancel >= 0 && lk != lookup) jcancel = -1;
        }
    }
    if (!nb->pop_button) FAIL("TCP box callback: no chosen-button getter");
    if (!jc) FAIL("TCP box callback: no Join box");
    nb->pop_text = jc->tgt;
    nb->join_cb = jc->arg[4].v;
    f->fx.join = jc->arg[1].v;
    f->join_len = jc->arg[2].v;
    /* the buttons say what they should: index 1 has the Join box's title text (Join), the last one the Join box's
     * last button text (Cancel) - so a 4-button box has its extra button (Port) at index 2 */
    if (jtitle < 0 || btxt[1] != jtitle) FAIL("TCP box: button 1 is not the Join box's text");
    if (jcancel < 0 || btxt[nb->tcp_nbtn - 1] != jcancel) FAIL("TCP box: the last button is not the Join box's Cancel");

    /* 7. the text-entry popup: callback (5th) and default button (1st) into the same variables as the choice popup */
    nf_fn *X = fn_get(c, nb->pop_text);
    if (!X) FAIL("text-entry popup not readable");
    if (store_of(X, mk(V_ARG, 4, 4), 4) != nb->pop_cbvar || store_of(X, mk(V_ARG, 0, 4), 4) != nb->pop_btnvar)
        FAIL("text-entry popup: not the choice popup's variables");
    /* the name editor opens the same popup on the name the lobby shows */
    nf_fn *SN = fn_get(c, setname);
    int named = 0;
    for (int i = 0; SN && i < SN->ncall; i++) {
        const ncall *k = &SN->call[i];
        if (k->tgt == nb->pop_text && k->npush >= 5 && k->arg[1].k == V_CONST && k->arg[1].v == f->fx.pname) named = 1;
    }
    if (!named) FAIL("the name editor does not edit the lobby's name text");

    /* 8. the popup's "open" flag: the byte both popups set */
    uint32_t active = 0;
    int nact = 0;
    for (int i = 0; i < P->nst; i++) {
        if (!P->st[i].abs || P->st[i].size != 1) continue;
        for (int j = 0; j < X->nst; j++)
            if (X->st[j].abs && X->st[j].size == 1 && X->st[j].addr == P->st[i].addr && P->st[i].addr != active) { active = P->st[i].addr; nact++; }
    }
    if (nact != 1 || !in_rw(&im, active)) FAIL("popup open flag: %d candidates", nact);

    /* 9. the Join box's callback: edit-buffer getter (an address the text-entry popup uses), the chosen-button getter
     *    again, the join buffer, and the join itself (stores 3 into the network state, 0 into the server flag) */
    nf_fn *J = fn_get(c, nb->join_cb);
    if (!J) FAIL("Join box callback not readable");
    if (!has_ref(J, f->fx.join)) FAIL("Join box callback: does not use the join buffer");
    uint32_t netfn = 0;
    for (int i = 0; i < J->ncall; i++) {
        uint32_t t = J->call[i].tgt;
        nv g;
        if (getter(c, t, &g)) {
            if (g.k == V_CONST && has_ref(X, g.v)) {
                if (nb->pop_edit && nb->pop_edit != t) FAIL("two edit-buffer getters");
                nb->pop_edit = t;
            } else if (g.k == V_MEM && g.v == nb->pop_btnvar && t != nb->pop_button) FAIL("Join box callback: another button getter");
            continue;
        }
        nf_fn *Y = fn_get(c, t);
        if (!Y || t == netfn) continue;
        /* the join: one DWORD set to 3 (the network state), and a WORD set to 0 that the lobby's idle tests for 0
         * (the server flag) */
        uint32_t n3 = store_of(Y, mk(V_CONST, 3, 4), 4), s0 = 0;
        int ns0 = 0;
        for (int j = 0; n3 && j < Y->nst; j++) {
            const nstore *t2 = &Y->st[j];
            if (!t2->abs || t2->size != 2 || !nv_eq(t2->val, mk(V_CONST, 0, 2)) || t2->addr == s0) continue;
            for (int q = 0; q < I->ncmp; q++)
                if (I->cmp[q].addr == t2->addr && I->cmp[q].size == 2 && I->cmp[q].imm == 0) { s0 = t2->addr; ns0++; break; }
        }
        if (!n3 || ns0 != 1) continue;
        if (netfn) FAIL("Join box callback: two joins");
        netfn = t; f->fx.net = n3; f->fx.server = s0;
    }
    if (!nb->pop_edit) FAIL("Join box callback: no edit-buffer getter");
    if (!netfn) FAIL("Join box callback: no join (network state / server flag tested by the lobby)");
    if (!in_rw(&im, f->fx.net) || !in_rw(&im, f->fx.server)) FAIL("network state not in data");

    /* 10. popup open getter and the notice popup: near the two callbacks (two calls deep) */
    {
        uint32_t r[96], skip[4] = { nb->pop_choice, nb->pop_text, 0, 0 };
        int n = reach(c, nb->join_cb, 2, 0, skip, 2, r, 48);
        n += reach(c, nb->tcp_choice_cb, 2, 0, skip, 2, r + n, 48);
        for (int i = 0; i < n; i++) {
            nv g;
            if (getter(c, r[i], &g)) {
                if (g.k == V_MEM && g.sz == 1 && g.v == active) {
                    if (nb->pop_active && nb->pop_active != r[i]) FAIL("two popup-open getters");
                    nb->pop_active = r[i];
                }
                continue;
            }
            nf_fn *Y = fn_get(c, r[i]);
            if (!Y || Y->retn != 4 || store_of(Y, mk(V_ARG, 0, 4), 4) != nb->pop_cbvar) continue;
            int sets = 0;
            for (int j = 0; j < Y->nst; j++) sets |= Y->st[j].abs && Y->st[j].size == 1 && Y->st[j].addr == active;
            if (!sets) continue;
            if (nb->pop_notice && nb->pop_notice != r[i]) FAIL("two notice popups");
            nb->pop_notice = r[i];
        }
        if (!nb->pop_active) FAIL("no popup-open getter");
        if (!nb->pop_notice) FAIL("no notice popup");
    }

    /* 11. hosting stores the same network state: from the TCP box's callback, not through the Join box */
    {
        uint32_t r[64], skip[4] = { jopen, nb->join_cb, nb->pop_choice, nb->pop_text };
        int n = reach(c, nb->tcp_choice_cb, 3, 1, skip, 4, r, 64), host = 0;
        for (int i = 0; i < n && !host; i++) {
            nf_fn *Y = fn_get(c, r[i]);
            if (!Y || r[i] == netfn) continue;
            for (int j = 0; j < Y->nst && !host; j++)
                host = Y->st[j].abs && Y->st[j].addr == f->fx.net && Y->st[j].size == 4 && nv_eq(Y->st[j].val, mk(V_CONST, 3, 4));
        }
        if (!host) FAIL("hosting does not set the network state found by the join");
    }
    f->ok = 1;
out:
    if (c) { for (int i = 0; i < c->nfn; i++) free(c->fn[i]); free(c); }
    return f->ok;   /* (not ok: the fields found so far stay, for the scan tool; nothing uses them) */
#undef FAIL
}

/* ---- the running game ---- */
static uint32_t rr_lo, rr_hi;   /* the last region found readable */
static int run_read(void *ctx, uint32_t va, void *out, unsigned n)
{
    const DWORD rdbl = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    uint32_t a = va, end = va + n;
    (void)ctx;
    if (end < va) return 0;
    while (a < end) {
        if (a >= rr_lo && a < rr_hi) { a = rr_hi; continue; }
        MEMORY_BASIC_INFORMATION mb;
        if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mb, sizeof(mb))) return 0;
        if (mb.State != MEM_COMMIT || (mb.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mb.Protect & rdbl)) return 0;
        rr_lo = (uint32_t)(uintptr_t)mb.BaseAddress;
        rr_hi = rr_lo + (uint32_t)mb.RegionSize;
        if (rr_hi < rr_lo) rr_hi = 0xffffffffu;
        a = rr_hi;
        if (a <= rr_lo) break;
    }
    memcpy(out, (const void *)(uintptr_t)va, n);
    return 1;
}

static volatile LONG fr_state;   /* 0 = not asked, 1 = finding, 2 = done */
static nfs3_found fr;

const nfs3_found *nfs3_find_running(void)
{
    if (InterlockedCompareExchange(&fr_state, 1, 0) == 0) {
        if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000u) { memset(&fr, 0, sizeof(fr)); snprintf(fr.why, sizeof(fr.why), "the exe is not at 0x400000"); }
        else nfs3_find_image(run_read, NULL, 0x400000u, &fr);
        InterlockedExchange(&fr_state, 2);
    } else while (fr_state != 2) Sleep(1);
    return fr.ok ? &fr : NULL;
}

const char *nfs3_find_why(void) { return fr_state == 2 ? fr.why : ""; }

int nfs3_found_text(const nfs3_found *f, char *out, int n)
{
    const nm_build *b = &f->nm;
    return snprintf(out, (size_t)n,
        "menu { hentry 0x%x, name 0x%x, enter 0x%x, leave 0x%x, find 0x%x, tcp_cb 0x%x, lobby_entry 0x%x, lobby_name 0x%x, "
        "lobby_idle 0x%x, pop_choice 0x%x, pop_notice 0x%x, pop_text 0x%x, pop_active 0x%x, pop_button 0x%x, pop_edit 0x%x, "
        "pop_cbvar 0x%x, pop_btnvar 0x%x, tcp_choice_cb 0x%x, join_cb 0x%x, tcp_btns 0x%x, tcp_help 0x%x, tcp_nbtn %u, "
        "pe_time 0x%x } state { net 0x%x, server 0x%x, join 0x%x (%u chars), pname 0x%x }",
        b->hentry, b->name_str, b->enter, b->leave, b->find, b->tcp_cb, b->lobby_entry, b->lobby_name, b->lobby_idle,
        b->pop_choice, b->pop_notice, b->pop_text, b->pop_active, b->pop_button, b->pop_edit, b->pop_cbvar, b->pop_btnvar,
        b->tcp_choice_cb, b->join_cb, b->tcp_btns, b->tcp_help, b->tcp_nbtn, b->pe_time[0],
        f->fx.net, f->fx.server, f->fx.join, f->join_len, f->fx.pname);
}
