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
int dds_box_over_game(void);         /* a box is open over a full-screen game */
HWND dds_game_window(void);         /* that game's window (NULL: no box over a full-screen game) */
int dds_on_game_window_thread(void); /* a box is open and the caller is the thread of the game window under it */
void dds_detach(void);               /* DLL unload (not process exit): the game windows get their own procedures back */
