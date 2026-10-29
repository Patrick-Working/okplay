/* Feeds real enum replies through rdv.c's name decoder. */
#include "../src/rdv.c"
void okp_self_probe(okp_ctx *c) { (void)c; }
void okp_inject_rooms(okp_ctx *c) { (void)c; }
void okp_capture_enum_reply(okp_ctx *c, const uint8_t *m, int l) { (void)c; (void)m; (void)l; }

static int hexin(const char *h, uint8_t *out) { int n = 0; unsigned v; while (sscanf(h, "%2x", &v) == 1) { out[n++] = v; h += 2; while (*h == ' ') h++; } return n; }

int main(void)
{
    /* AoE2 host, from okplay-20260928-013115-15476.log */
    const char *aoe2 =
        "70 6c 61 79 01 00 0e 00 50 00 00 00 44 c0 00 00 b6 85 95 38 37 8a be 44 9d af 2e 5e c4 79 36 23 3f 3f e9 5d 90 fc e1 4e ae 5a 63 da fa 05 59 50 "
        "08 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00 de 71 28 43 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 5c 00 00 00 50 61 74 72 "
        "69 63 6b 27 73 20 47 61 6d 65 00 5b 50 61 74 72 69 63 6b 5d 00 00";
    uint8_t b[256]; char name[256]; DWORD pl, mx, fl;
    int n = hexin(aoe2, b);
    enum_blob_info(b, n, name, sizeof(name), &pl, &mx, &fl);
    printf("aoe2 (%d bytes): name='%s' players=%lu/%lu flags=0x%lx\n", n, name, pl, mx, fl);
    int ok = strcmp(name, "Ann's Game") == 0 && pl == 1 && mx == 8 && fl == 0xc044;

    /* dptest host (UTF-16 name): same header, name "dptest H" as UTF-16 */
    memcpy(b + 92, L"dptest H", 18);
    enum_blob_info(b, 92 + 18, name, sizeof(name), &pl, &mx, &fl);
    printf("utf16: name='%s'\n", name);
    ok = ok && strcmp(name, "dptest H") == 0;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return !ok;
}
