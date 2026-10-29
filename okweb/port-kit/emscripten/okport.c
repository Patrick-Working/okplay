/* okport.c - see okport.h. */
#include "okport.h"
#include <emscripten.h>
#include <string.h>

static int frame_depth;

void okport_frame_enter(void) { frame_depth++; }
void okport_frame_leave(void) { frame_depth--; }

EM_JS(int, okport_js_stack_free, (void), {
    return (typeof Asyncify !== 'undefined' && Asyncify.state === 0 && !Asyncify.currData) ? 1 : 0;
});

int okport_can_wait(void) { return frame_depth == 1 && okport_js_stack_free(); }

EM_JS(int, okport_js_page_hidden, (void), { return document.hidden ? 1 : 0; });

/* until the browser has shown a frame (at most 100 ms) */
EM_ASYNC_JS(void, okport_js_show_frame, (void), {
    await new Promise((resolve) => {
        let done = false;
        const go = () => { if (!done) { done = true; resolve(); } };
        requestAnimationFrame(() => setTimeout(go, 0));
        setTimeout(go, 100);
    });
});

void okport_show_frame(void)
{
    /* (a hidden page shows nothing, and its timers can be held back for up to a minute) */
    if (okport_can_wait() && !okport_js_page_hidden()) okport_js_show_frame();
}

/* the file from the server next to the page, as named and in lower case (a Linux server's names are
   case-sensitive; ports usually store the game's folders in lower case); written where the game looks */
EM_ASYNC_JS(int, okport_js_fetch, (const char *path_ptr), {
    const path = UTF8ToString(path_ptr);
    if (Module.okportFetch) {
        try { return (await Module.okportFetch(path)) ? 1 : 0; } catch (e) { return 0; }
    }
    const names = [path, path.toLowerCase()].filter((p, i, all) => all.indexOf(p) === i);
    for (const name of names) {
        try {
            const res = await fetch(encodeURI(name));
            if (!res.ok) continue;
            const data = new Uint8Array(await res.arrayBuffer());
            const dirs = path.split('/').slice(0, -1);
            for (let i = 1; i <= dirs.length; i++) { try { FS.mkdir(dirs.slice(0, i).join('/')); } catch (e) {} }
            FS.writeFile(path, data);
            console.log('[okport] loaded ' + name + ' (' + data.length + ' bytes)');
            return 1;
        } catch (e) {}
    }
    console.warn('[okport] not on the server: ' + path);
    return 0;
});

int okport_fetch_file(const char *path)
{
    char p[512];
    size_t i, n;
    if (!path || !*path || !okport_can_wait()) return 0;
    /* Windows separators and a leading ".\" off: the server's (and the file system's) path */
    while (path[0] == '.' && (path[1] == '\\' || path[1] == '/')) path += 2;
    n = strlen(path);
    if (n >= sizeof(p)) return 0;
    for (i = 0; i <= n; i++) p[i] = path[i] == '\\' ? '/' : path[i];
    return okport_js_fetch(p);
}
