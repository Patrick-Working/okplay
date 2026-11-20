/* okplay wsock tests - nfs3scan: runs the Need for Speed III address finder (nfs3_find.c) over an exe file and
 * prints what it found; for builds in the known tables (nfs3_known.h) it compares field by field. [game: NFS3]
 * Read-only: the exe is only read. Not part of build.sh; build it with the same i686 MinGW gcc:
 *   gcc -O2 -Wall -Wextra -I.. -o nfs3scan.exe nfs3scan.c ../nfs3_find.c -lbcrypt
 * Use: nfs3scan.exe <exe>[@<fingerprint>] ...   (@: compare as that build, e.g. an exe dumped from memory whose file
 * on disk is packed; exit code: 0 = every exe found and, where known, the same as the table) */
#define NFS3_KNOWN_MENU
#define NFS3_KNOWN_FIX
#include "nfs3_known.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *file; uint32_t size, base, hdr; int nsec; struct { uint32_t va, vsize, raw, rsize; } sec[24]; } pemap;

static int pe_read(void *ctx, uint32_t va, void *out, unsigned n)   /* a VA of the exe as loaded: from the file */
{
    pemap *m = (pemap *)ctx;
    uint8_t *o = (uint8_t *)out;
    for (unsigned i = 0; i < n; i++) {
        uint32_t a = va + i, rva = a - m->base;
        int done = 0;
        if (a < m->base) return 0;
        if (rva < m->hdr) { if (rva >= m->size) return 0; o[i] = m->file[rva]; continue; }
        for (int s = 0; s < m->nsec && !done; s++) {
            uint32_t vs = m->sec[s].vsize > m->sec[s].rsize ? m->sec[s].vsize : m->sec[s].rsize;
            if (rva < m->sec[s].va || rva - m->sec[s].va >= vs) continue;
            uint32_t off = rva - m->sec[s].va;
            o[i] = (m->sec[s].raw && off < m->sec[s].rsize && m->sec[s].raw + off < m->size) ? m->file[m->sec[s].raw + off] : 0;
            done = 1;
        }
        if (!done) return 0;
    }
    return 1;
}

static int pe_map(const char *path, pemap *m)
{
    FILE *f = fopen(path, "rb");
    memset(m, 0, sizeof(*m));
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    m->file = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (!m->file || fread(m->file, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
    fclose(f);
    m->size = (uint32_t)n;
    uint32_t lf;
    memcpy(&lf, m->file + 0x3c, 4);
    if (lf + 0x100 > m->size || memcmp(m->file + lf, "PE\0\0", 4)) return 0;
    uint16_t ns, opt;
    memcpy(&ns, m->file + lf + 6, 2);
    memcpy(&opt, m->file + lf + 20, 2);
    memcpy(&m->base, m->file + lf + 24 + 28, 4);
    memcpy(&m->hdr, m->file + lf + 24 + 60, 4);
    for (int i = 0; i < ns && i < 24; i++) {
        const uint8_t *s = m->file + lf + 24 + opt + 40 * i;
        memcpy(&m->sec[i].vsize, s + 8, 4); memcpy(&m->sec[i].va, s + 12, 4);
        memcpy(&m->sec[i].rsize, s + 16, 4); memcpy(&m->sec[i].raw, s + 20, 4);
        m->nsec++;
    }
    return 1;
}

static void fingerprint(const pemap *m, char out[17])   /* as okplay's okp_exe_build: SHA-256 of the file, first 8 bytes */
{
    BCRYPT_ALG_HANDLE a;
    BCRYPT_HASH_HANDLE h;
    UCHAR d[32];
    out[0] = 0;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return;
    if (!BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0)) {
        BCryptHashData(h, m->file, m->size, 0);
        BCryptFinishHash(h, d, sizeof(d), 0);
        BCryptDestroyHash(h);
        for (int i = 0; i < 8; i++) sprintf(out + 2 * i, "%02x", d[i]);
    }
    BCryptCloseAlgorithmProvider(a, 0);
}

static int bad;
static void cmp(const char *what, uint32_t known, uint32_t found)
{
    const char *r = known == found ? "same" : known == 0 ? "NEW (the table has 0: not pinned down)" : "DIFFERENT";
    if (known != found && known) bad = 1;
    printf("    %-14s known 0x%08x  found 0x%08x  %s\n", what, known, found, r);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "use: nfs3scan <exe>[@<fingerprint>] ...\n"); return 2; }
    for (int i = 1; i < argc; i++) {
        pemap m;
        char fp[17];
        nfs3_found f;
        char txt[1024];
        char path[MAX_PATH], *as;
        lstrcpynA(path, argv[i], sizeof(path));
        as = strrchr(path, '@');
        if (as) *as++ = 0;
        printf("== %s%s%s\n", path, as ? " as build " : "", as ? as : "");
        if (!pe_map(path, &m)) { printf("   not a PE file\n"); bad = 1; continue; }
        fingerprint(&m, fp);
        if (as) lstrcpynA(fp, as, sizeof(fp));
        DWORD t0 = GetTickCount();
        int ok = nfs3_find_image(pe_read, &m, m.base, &f);
        printf("   fingerprint %s, PE time 0x%08x, finder %s (%lu ms)%s%s\n", fp, f.nm.pe_time[0], ok ? "found" : "NOT FOUND",
               (unsigned long)(GetTickCount() - t0), ok ? "" : ": ", ok ? "" : f.why);
        nfs3_found_text(&f, txt, sizeof(txt));
        printf("   %s\n", txt);
        if (!ok) bad = 1;
        for (unsigned k = 0; k < sizeof(nm_builds) / sizeof(nm_builds[0]); k++) {
            const nm_build *b = &nm_builds[k], *g = &f.nm;
            int data = g->hentry == b->hentry && g->name_str == b->name_str && g->enter == b->enter && g->leave == b->leave;
            if (g->pe_time[0] != b->pe_time[0] && g->pe_time[0] != b->pe_time[1]) {
                if (!data) continue;
                printf("   menu table \"%s\": its handler-entry data check alone would take this exe (the PE time says no)"
                       " - what it would use:\n", b->name);
                int keep = bad;
                cmp("tcp_btns", b->tcp_btns, g->tcp_btns); cmp("tcp_help", b->tcp_help, g->tcp_help);
                cmp("tcp_nbtn", b->tcp_nbtn, g->tcp_nbtn);
                bad = keep;
                continue;
            }
            printf("   menu table \"%s\":\n", b->name);
            cmp("hentry", b->hentry, g->hentry); cmp("name_str", b->name_str, g->name_str);
            cmp("enter", b->enter, g->enter); cmp("leave", b->leave, g->leave); cmp("find", b->find, g->find);
            cmp("tcp_cb", b->tcp_cb, g->tcp_cb); cmp("lobby_entry", b->lobby_entry, g->lobby_entry);
            cmp("lobby_name", b->lobby_name, g->lobby_name); cmp("lobby_idle", b->lobby_idle, g->lobby_idle);
            cmp("pop_choice", b->pop_choice, g->pop_choice); cmp("pop_notice", b->pop_notice, g->pop_notice);
            cmp("pop_text", b->pop_text, g->pop_text); cmp("pop_active", b->pop_active, g->pop_active);
            cmp("pop_button", b->pop_button, g->pop_button); cmp("pop_edit", b->pop_edit, g->pop_edit);
            cmp("pop_cbvar", b->pop_cbvar, g->pop_cbvar); cmp("pop_btnvar", b->pop_btnvar, g->pop_btnvar);
            cmp("tcp_choice_cb", b->tcp_choice_cb, g->tcp_choice_cb); cmp("join_cb", b->join_cb, g->join_cb);
            cmp("tcp_btns", b->tcp_btns, g->tcp_btns); cmp("tcp_help", b->tcp_help, g->tcp_help);
            cmp("tcp_nbtn", b->tcp_nbtn, g->tcp_nbtn);
        }
        for (unsigned k = 0; k < sizeof(builds) / sizeof(builds[0]); k++) {
            const nfs3_build *b = &builds[k];
            if (strcmp(b->build, fp)) continue;
            printf("   state table \"%s\" (%s):\n", b->name, b->build);
            cmp("net", b->net, f.fx.net); cmp("server", b->server, f.fx.server);
            cmp("join", b->join, f.fx.join); cmp("pname", b->pname, f.fx.pname);
        }
        free(m.file);
    }
    return bad;
}
