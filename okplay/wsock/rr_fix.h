/* okplay wsock - Road Rash Online (Steam/GoG/CD-Rom) netcode fixes (rr_fix.c) */
#pragma once
#include <stdint.h>

int rrf_init(const char *ini);                       /* 1 = a known build, the fixes are in */
void rrf_knobs(const char *ini);                     /* okplay.ini read again */
void rrf_game_out(const uint8_t *d, int len);        /* a game packet this PC sends */
int rrf_game_filter(const uint8_t *d, int len, uint8_t *copy);   /* 1 = deliver `copy` instead of `d` */
int rrf_ctrl_due(uint8_t *b, int max);               /* an 'R' message for every other player now, or 0 */
void rrf_ctrl(uint64_t from, const uint8_t *b, int n);   /* an 'R' message from another player */
int rrf_clock_applied(int rider);                    /* 1 = the clock correction is applied to this rider */

/* the lab build's measurements, called around the fixes */
typedef struct {
    uint32_t (__cdecl *pre)(int rider, uint32_t now);
    void (__cdecl *post)(int rider, int32_t *out);
    void (__cdecl *hist_pre)(int rider);
    void (__cdecl *hist_post)(int rider);
} rrf_observer;
void rrf_observe(const rrf_observer *o);
