/* compat.c and okp_printf.c against Windows' own implementations (run on a new Windows):
 *   SHA-256 vs bcrypt, okp_ntop/okp_pton vs inet_ntop/inet_pton, okp_utf16_to_utf8 vs CP_UTF8,
 *   okp_snprintf vs the C runtime's C99 snprintf, okp_resolve, okp_random.
 * Prints each failure; exit 0 = all agree. */
#include "okp.h"
#include "okp_stdio.h"
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

#undef snprintf
#undef vsnprintf
#undef sprintf
static int fails, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", what); }
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void test_sha256(void)
{
    static uint8_t data[4096];
    for (int i = 0; i < (int)sizeof(data); i++) data[i] = (uint8_t)rnd();
    for (int len = 0; len <= 300; len++) {
        uint8_t a[32], b[32];
        okp_sha256_ctx s;
        BCRYPT_ALG_HANDLE alg;
        BCRYPT_HASH_HANDLE h;
        okp_sha256_init(&s);
        /* feed in uneven pieces to exercise the buffering */
        for (int off = 0; off < len;) {
            int take = 1 + (int)(rnd() % 70);
            if (take > len - off) take = len - off;
            okp_sha256_update(&s, data + off, take);
            off += take;
        }
        okp_sha256_final(&s, a);
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
        BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0);
        BCryptHashData(h, data, len, 0);
        BCryptFinishHash(h, b, 32, 0);
        BCryptDestroyHash(h);
        BCryptCloseAlgorithmProvider(alg, 0);
        char what[64];
        snprintf(what, sizeof(what), "sha256 len %d", len);
        check(!memcmp(a, b, 32), what);
    }
    /* the FIPS "abc" vector */
    uint8_t d[32];
    const void *parts[1] = { "abc" };
    size_t lens[1] = { 3 };
    okp_sha256_parts(parts, lens, 1, d);
    static const uint8_t abc[32] = { 0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
                                     0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
    check(!memcmp(d, abc, 32), "sha256 abc vector");
}

static void test_addr_text(void)
{
    for (int i = 0; i < 20000; i++) {
        uint8_t a6[16], back[16];
        char mine[64], sys[64];
        int kind = i % 5;
        memset(a6, 0, 16);
        for (int k = 0; k < 16; k++) a6[k] = (uint8_t)rnd();
        if (kind == 1) { int z = rnd() % 8, n = 1 + rnd() % (8 - z); memset(a6 + 2 * z, 0, 2 * n); }
        if (kind == 2) { memset(a6, 0, 10); a6[10] = a6[11] = 0xff; }        /* v4-mapped */
        if (kind == 3) { for (int k = 0; k < 16; k += 2) if (rnd() % 2) a6[k] = a6[k + 1] = 0; }
        okp_ntop(AF_INET6, a6, mine, sizeof(mine));
        inet_ntop(AF_INET6, a6, sys, sizeof(sys));
        if (strcmp(mine, sys)) { printf("  ntop6 mine=%s sys=%s\n", mine, sys); check(0, "ntop6"); continue; }
        check(okp_pton(AF_INET6, sys, back) == 1 && !memcmp(back, a6, 16), "pton6 round trip");
        okp_ntop(AF_INET, a6 + 12, mine, sizeof(mine));
        inet_ntop(AF_INET, a6 + 12, sys, sizeof(sys));
        check(!strcmp(mine, sys), "ntop4");
        uint8_t b4[4], s4[4];
        check(okp_pton(AF_INET, sys, b4) == 1 && inet_pton(AF_INET, sys, s4) == 1 && !memcmp(b4, s4, 4), "pton4");
    }
    const char *badv4[] = { "", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1..2.3", "a.b.c.d", "1.2.3.4 ", "01234.1.1.1" };
    for (unsigned i = 0; i < sizeof(badv4) / sizeof(*badv4); i++) {
        uint8_t x[4];
        check(okp_pton(AF_INET, badv4[i], x) == 0, "pton4 rejects junk");
    }
    const char *badv6[] = { "", ":", ":::", "1::2::3", "12345::", "g::", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7" };
    for (unsigned i = 0; i < sizeof(badv6) / sizeof(*badv6); i++) {
        uint8_t x[16];
        check(okp_pton(AF_INET6, badv6[i], x) == 0, "pton6 rejects junk");
    }
}

static void test_utf8(void)
{
    for (int i = 0; i < 5000; i++) {
        WCHAR w[40];
        char mine[200], sys[200];
        int n = rnd() % 39;
        for (int k = 0; k < n; k++) {
            int kind = rnd() % 6;
            w[k] = kind == 0 ? (WCHAR)(0x20 + rnd() % 0x5f) : kind == 1 ? (WCHAR)(0x80 + rnd() % 0x780)
                 : kind == 2 ? (WCHAR)(0x800 + rnd() % 0xD000) : kind == 3 ? (WCHAR)(0xE000 + rnd() % 0x2000)
                 : kind == 4 ? (WCHAR)(0xD800 + rnd() % 0x400) : (WCHAR)(0xDC00 + rnd() % 0x400);
            if (w[k] == 0) w[k] = 'x';
        }
        w[n] = 0;
        int a = okp_utf16_to_utf8(w, mine, sizeof(mine));
        int b = WideCharToMultiByte(CP_UTF8, 0, w, -1, sys, sizeof(sys), NULL, NULL);
        check(a >= 0 && b == a + 1 && !memcmp(mine, sys, b), "utf16->utf8 matches CP_UTF8");
    }
}

#define PF(...) do { char a_[256], b_[256]; int ra_ = okp_snprintf(a_, sizeof(a_), __VA_ARGS__); \
    int rb_ = __mingw_snprintf(b_, sizeof(b_), __VA_ARGS__); \
    if (ra_ != rb_ || strcmp(a_, b_)) printf("  printf %s: mine=[%s]%d crt=[%s]%d\n", #__VA_ARGS__, a_, ra_, b_, rb_); \
    check(ra_ == rb_ && !strcmp(a_, b_), "printf " #__VA_ARGS__); } while (0)

static void test_printf(void)
{
    uint64_t big = 0x0123456789abcdefULL;
    PF("%d %i %u", -5, 42, 3000000000u);
    PF("%5d|%-5d|%05d|%+d|% d", 42, 42, 42, 42, 42);
    PF("%x %X %#x %#X %08x %#010x", 0xbeef, 0xbeef, 0xbeef, 0xbeef, 0xbeef, 0xbeef);
    PF("%o %#o %lu %ld %li", 8u, 8u, 4000000000ul, -7l, 7l);
    PF("%llx %016llx %llu %lld %I64x %I64d", big, big, big, (long long)-12345678901LL, big, (long long)-5);
    PF("%s|%10s|%-10s|%.3s|%.*s|%*s", "abc", "abc", "abc", "abcdef", 2, "xyz", 6, "q");
    PF("%c%c%5c%-3c|", 'a', 'b', 'c', 'd');
    PF("%%|%.0d|%.0x|%.5d|%8.3d|%-8.3d|", 0, 0, 42, 7, 7);
    PF("%hhu %hu %hd %zu %zd", 300, 70000, -3, (size_t)12, (ptrdiff_t)-12);
    PF("%u.%u.%u.%u:%u", 192u, 168u, 1u, 183u, 2300u);
    PF("%08lx-%04x-%04x-%02x%02x", 0x66d8dc59ul, 0x28e8, 0x4d3e, 0xba, 0x93);
    /* truncation: C99 length and a terminated prefix */
    char small[6];
    int r = okp_snprintf(small, sizeof(small), "%s", "0123456789");
    check(r == 10 && !strcmp(small, "01234"), "printf truncation");
    check(okp_snprintf(NULL, 0, "%d", 12345) == 5, "printf size query");
}

static void test_resolve_random(void)
{
    SOCKADDR_IN6 a[4];
    char s[80];
    int n = okp_resolve("127.0.0.1", htons(80), 4, a, 4);
    check(n == 1 && !strcmp(okp_addr_str(&a[0], s, sizeof(s)), "127.0.0.1:80"), "resolve literal");
    n = okp_resolve("localhost", htons(80), 4, a, 4);
    check(n >= 1, "resolve localhost");
    uint8_t r1[40], r2[40];
    okp_random(r1, sizeof(r1));
    okp_random(r2, sizeof(r2));
    check(memcmp(r1, r2, sizeof(r1)) != 0, "random differs");
}

int main(void)
{
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    const char *e = getenv("OKP_EMULATE_9X");
    char sys[80];
    okp_sys_init();
    printf("test_compat on %s%s\n", okp_sys_name(sys, sizeof(sys)), e && *e == '1' ? " (OKP_EMULATE_9X)" : "");
    test_sha256();
    test_addr_text();
    test_utf8();
    test_printf();
    test_resolve_random();
    printf("%d checks, %d failed\nRESULT %s\n", checks, fails, fails ? "fail" : "ok");
    return fails != 0;
}
