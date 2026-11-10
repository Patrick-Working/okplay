/* okplay wsock - okplay's windows over a full-screen DirectDraw game (ddshow.c) */
#pragma once
#include <windows.h>

void dds_init(void);                 /* DllMain: watch the DirectDraw objects the game makes (the exe and the DLLs it loads) */
int dds_show_gdi(void);              /* before a box of ours: Windows' screen page in front (FlipToGDISurface) and the game
                                      * window kept from hearing it lost its place; returns how many DirectDraw objects */
void dds_guard_end(void);            /* the box closed: the game window in front again */
void dds_detach(void);               /* DLL unload (not process exit): the game windows get their own procedures back */
