/* okplay wsock - okplay's windows over a full-screen DirectDraw game (ddshow.c) */
#pragma once
#include <windows.h>

void dds_init(void);                 /* DllMain: watch the DirectDraw objects the game makes (the exe and the DLLs it loads) */
int dds_show_gdi(void);              /* before a box of ours: Windows' screen page in front (FlipToGDISurface) and the game
                                      * window kept from hearing it lost its place; returns how many DirectDraw objects */
void dds_guard_end(void);            /* the box closed: the game window in front again */
void dds_set_keep(int keep);         /* 1 = the game window is not told it lost the front while a box is open (every Windows) */
void dds_set_stall_log(int on);      /* okplay.ini wsock_box_stall_log=1: a game window that stops reading its messages under a box
                                      * gets its threads' positions logged (they are held for a moment to read them) */
void dds_set_flip_hold(int on);      /* okplay.ini wsock_box_flip=hold: the game's page flips (its primary surface's Flip) do
                                      * nothing while a box is open; off by default. Callable before or after dds_init */
void dds_set_wrapper(int on);        /* okplay.ini wsock_box_wrapper: 1 = auto (default: a borderless DirectDraw wrapper such as
                                      * DDrawCompat gets plain boxes, none of the full-screen measures), 0 = off */
int dds_box_can_drag(void);          /* over a wrapper's window with the game's picture kept: the box may be dragged */
void dds_set_drag(int on);           /* okplay.ini wsock_box_drag=0: no dragging (and no picture kept) over a wrapper's window */
int dds_box_wrapped(void);           /* that game is shown by a DirectDraw wrapper: the box takes the front (no out-of-front box) */
int dds_box_over_game(void);         /* a box is open over a full-screen game */
HWND dds_game_window(void);         /* that game's window (NULL: no box over a full-screen game) */
int dds_on_game_window_thread(void); /* a box is open and the caller is the thread of the game window under it */
void dds_shot_start(const char *key); /* okplay.ini wsock_screenshot_key (default ctrl+f12, "off"): a key that saves the screen
                                      * with okplay's boxes as a BMP in okplay-screenshots beside the game */
int dds_shot_here(UINT m);           /* the box's message filter: 1 = m was a screenshot asked for on the box's thread (taken) */
void dds_detach(void);               /* DLL unload (not process exit): the game windows get their own procedures back */
