/* okport.h - small helpers for porting a 1990s Windows game to the browser with Emscripten and
 * Asyncify (which a DirectPlay game needs anyway: DirectPlay's calls wait for the network).
 *
 * Asyncify lets C code wait for the browser in the middle of a frame - but only when nothing else is
 * waiting, and only from the game's own frame: a call the page makes into the game (a test console,
 * an audio callback) must never wait, and must never run game code while a frame is suspended
 * (Asyncify then keeps "rewinding" and every later call traps). These helpers keep to that:
 *
 *   okport_frame_enter() / okport_frame_leave()   around the game's frame (the main-loop callback)
 *   okport_can_wait()                              inside the game's own frame, nothing else waiting
 *   okport_show_frame()                            let the browser show what the game drew (a status
 *                                                  screen drawn before long work, as UpdateWindow does
 *                                                  on Windows); skipped while the page is hidden
 *   okport_fetch_file(path)                        one of the game's files the page didn't load at start
 *                                                  (music, a movie, a taunt): fetched from the server
 *                                                  next to the page into the file system; 1 = there now
 *
 * Link with -sASYNCIFY. A page can replace the fetch with its own (cache, progress):
 * Module.okportFetch = async (path) => true/false. Also set SDL_HINT_EMSCRIPTEN_ASYNCIFY to "0" before
 * SDL_Init when the game runs on emscripten_set_main_loop: SDL otherwise yields in every RenderPresent. */
#ifndef OKPORT_H
#define OKPORT_H
#ifdef __cplusplus
extern "C" {
#endif

void okport_frame_enter(void);
void okport_frame_leave(void);
int okport_can_wait(void);
void okport_show_frame(void);
int okport_fetch_file(const char *path);   /* path as the game writes it ("sound\\stream\\x.mp3" is fine) */

#ifdef __cplusplus
}
#endif
#endif
