/* okavi.h - a game's AVI movies in the browser (Indeo 5 through FFmpeg, PCM sound through WebAudio,
 * drawn on a canvas over the game's). See okavi.cpp. One movie at a time, as games play them. */
#ifndef OKAVI_H
#define OKAVI_H
#ifdef __cplusplus
extern "C" {
#endif

#define OKAVI_STOPPED 0
#define OKAVI_PLAYING 1
#define OKAVI_PAUSED 2

int okavi_available(void);                       /* the build has the Indeo 5 decoder */
int okavi_open(const char *file);                /* a file in Emscripten's file system; 1 = ready */
void okavi_size(int *w, int *h);                 /* the movie's own size */
void okavi_place(int x, int y, int w, int h, int screen_w, int screen_h);   /* where, in game pixels */
void okavi_play(void);
void okavi_stop(void);
void okavi_pause(void);
void okavi_resume(void);
int okavi_mode(void);                            /* OKAVI_*: STOPPED once the movie (and its sound) ends */
void okavi_tick(void);                           /* once per game frame */
void okavi_close(void);                          /* removes the canvas */

#ifdef __cplusplus
}
#endif
#endif
