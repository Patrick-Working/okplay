/* rrf_sim: rr_fix.c (the shipping Road Rash fixes) built as a plain DLL for a replay simulator.
 *
 *   build: tests/build_rrf_sim.sh -> bin/rrf_sim.dll (64-bit, for a 64-bit Python with ctypes)
 *
 * The simulator plays the game's part: it keeps a stand-in for the game's memory (rrf_sim_mem, laid out at the
 * 1996 CD build's addresses), files each report there the way the game does and calls the same entry points the
 * game's hooks call (rrf_hist_pre/post around filing a report, rrf_pre/post around the guess), sends and receives
 * the 'R' clock messages (rrf_ctrl_due / rrf_ctrl) and sets the clock (rrf_sim_ms). Load one copy of the DLL per
 * simulated PC (each copy has its own state). Nothing here touches a real game. */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define MEM_SIZE 0xc0000   /* 0x400000 .. 0x4c0000 covers the CD build's report table and counters */
static uint8_t mem[MEM_SIZE];
static int log_on;

int rrf_sim_setup(void *mem, const char *ini);

__declspec(dllexport) uint8_t *rrf_sim_mem(void) { return mem; }
__declspec(dllexport) void rrf_sim_log(int on) { log_on = on; }
__declspec(dllexport) int rrf_sim_init(const char *ini) { memset(mem, 0, sizeof(mem)); return rrf_sim_setup(mem, ini); }

void okp_log(int level, const char *fmt, ...)
{
    if (!log_on) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    (void)level;
}
