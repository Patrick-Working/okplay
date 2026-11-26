/* The 64-bit okplay wsock build (build_proxy.sh 64, for 64-bit dethrace) has no game patches: Road Rash's and Need
 * for Speed III's (rr_fix.c, nfs3_*.c) work on those games' 32-bit exes, with 32-bit code. Their entry points, empty,
 * so wsock.c's profile table links unchanged. */
#include <stdint.h>
#include "rr_fix.h"
#include "nfs3_fix.h"
#include "nfs3_menu.h"

int rrf_init(const char *ini) { (void)ini; return 0; }
void rrf_knobs(const char *ini) { (void)ini; }
void rrf_game_out(const uint8_t *d, int len) { (void)d; (void)len; }
int rrf_game_filter(const uint8_t *d, int len, uint8_t *copy) { (void)d; (void)len; (void)copy; return 0; }
int rrf_ctrl_due(uint8_t *b, int max) { (void)b; (void)max; return 0; }
void rrf_ctrl(uint64_t from, const uint8_t *b, int n) { (void)from; (void)b; (void)n; }

int nfs3_init(const char *ini) { (void)ini; return 0; }
int nfs3_hosting(int *net, int *server) { (void)net; (void)server; return -1; }
int nfs3_max_players(int *type) { (void)type; return 0; }
void nfs3_join_prefill(int okplay_on) { (void)okplay_on; }
int nfs3_player_name(char *out, int n) { (void)out; (void)n; return 0; }
int nfs3_menu_mode(void) { return 0; }
