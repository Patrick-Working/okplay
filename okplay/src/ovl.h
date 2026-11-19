/* ovl - okplay's overlay, drawn into a game's own frames (ovl.c; docs/OVERLAY.md) */
#pragma once
#include <windows.h>

void ovl_setup(int on, DWORD banner_ms, const char *text);   /* okplay.ini: wsock_overlay, the banner's time and text */
int ovl_wants_frames(void);                                  /* 1: the frame hooks should call ovl_draw (only while drawing) */
void ovl_pause(int on);                                      /* 1 while one of okplay's old boxes is open: nothing drawn */
int ovl_draw(HDC dc, int w, int h, int bpp);                 /* on the frame about to be shown; 1 = something drawn */
