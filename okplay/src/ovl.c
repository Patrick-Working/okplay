/* ovl - okplay's overlay: what okplay shows drawn INTO a game's own frames (universal, any game).
 *
 * Why: a window of ours over a full-screen game is only seen while the game keeps drawing - and the game stops
 * drawing while it waits in a network call (Need for Speed III portable with DDrawCompat, 2026-11-16 - 11-19:
 * docs/BOX_METHOD.md section 11).  Drawn into each frame the game shows, the overlay is part of the game's picture:
 * nothing competes with the game, a wrapper or Windows for the screen.  The frame hooks are per graphics API
 * (DirectDraw: wsock/ddshow.c, at the game's Flip); this file only draws, into a device context it is given.
 *
 * Milestone 1 (wsock 0.3.0): a banner for the first seconds of the game - proof that drawing into the frames works
 * on every setup.  Later: the panel (mode, password, rooms) and notices.  docs/OVERLAY.md. */
#include "okp.h"
#include <string.h>
#include "ovl.h"

static int ovl_on = 1;                 /* okplay.ini wsock_overlay=0: nothing drawn */
static DWORD ovl_banner_ms = 20000;    /* how long the banner stays (from the first frame) */
static DWORD ovl_t0;                   /* the first frame drawn */
static char ovl_text[160] = "Oldkid.net Online Play";
static int ovl_logged;
static volatile int ovl_paused;        /* ovl_pause: one of okplay's old boxes is open - the overlay stands aside */

void ovl_setup(int on, DWORD banner_ms, const char *text)
{
    ovl_on = on;
    if (banner_ms) ovl_banner_ms = banner_ms;
    if (text && text[0]) lstrcpynA(ovl_text, text, sizeof(ovl_text));
}

/* 1 only while there is something to draw: otherwise the game's frames are not touched at all (0.3.0 asked for a
 * device context of every frame, all game long - with DDrawCompat the game then hung at Host Game, 2026-11-19) */
int ovl_wants_frames(void)
{
    if (!ovl_on || ovl_paused) return 0;
    return !ovl_t0 || GetTickCount() - ovl_t0 <= ovl_banner_ms;
}

void ovl_pause(int on) { ovl_paused = on; }

/* Draw on the game's frame (dc: the frame's device context, w x h its size).  Called by a frame hook just before the
 * frame is shown, on the game's drawing thread.  Returns 1 if anything was drawn. */
int ovl_draw(HDC dc, int w, int h, int bpp)
{
    DWORD now = GetTickCount();
    if (!ovl_on || !dc) return 0;
    if (!ovl_t0) ovl_t0 = now | 1;
    if (now - ovl_t0 > ovl_banner_ms) return 0;
    /* the banner: a dark strip along the bottom, the text in it - sized from the frame (640x480 and up) */
    int fh = h >= 600 ? 16 : 13, pad = fh / 2;
    int bh = fh + 2 * pad;
    HFONT f = CreateFontA(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
    HGDIOBJ of = f ? SelectObject(dc, f) : NULL;
    RECT r = { 0, h - bh, w, h };
    HBRUSH b = CreateSolidBrush(RGB(16, 16, 24));
    if (b) { FillRect(dc, &r, b); DeleteObject(b); }
    RECT line = { 0, h - bh, w, h - bh + 1 };
    HBRUSH lb = CreateSolidBrush(RGB(220, 160, 40));
    if (lb) { FillRect(dc, &line, lb); DeleteObject(lb); }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 240, 240));
    RECT tr = { pad * 2, h - bh, w - pad * 2, h };
    DrawTextA(dc, ovl_text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (of) SelectObject(dc, of);
    if (f) DeleteObject(f);
    if (!ovl_logged) {
        ovl_logged = 1;
        okp_log(1, "overlay: drawn into the game's frame (%dx%d, %d bit) - shown %lu s from the first frame", w, h, bpp,
                (unsigned long)(ovl_banner_ms / 1000));
    }
    return 1;
}
