/* okplay wsock - Need for Speed III: the addresses okplay uses, found in the loaded game at launch (nfs3_find.c).
 * [game: NFS3]
 *
 * The known builds have hand-made tables (nfs3_known.h); any other build is read by the finder: it starts from the
 * game's own data (the menu handler table entries named "connect" and "mainmult") and follows the code from there.
 * Every value is cross-checked; one failed check and the finder says "not found" (okplay keeps its Windows boxes). */
#pragma once
#include <stdint.h>

/* the in-game menus (nfs3_menu.c) */
typedef struct {
    const char *name;
    uint32_t hentry;       /* the connect menu's entry in the handler table: {name, -, -, enter, leave, idle} */
    uint32_t name_str;     /* "connect" */
    uint32_t enter, leave; /* its enter / leave handlers */
    uint32_t find;         /* element of a menu by codelink token: eax = menu, edx = token -> eax = element or 0 */
    uint32_t tcp_cb;       /* the game's TCP callback (its "TCP Connection" box): eax = element */
    uint32_t lobby_entry, lobby_name, lobby_idle;   /* the lobby menu (mainmult): its handler entry, "mainmult", its idle */
    uint32_t pop_choice, pop_notice, pop_text;      /* the game's popups: choice, notice, text entry (open) */
    uint32_t pop_active, pop_button, pop_edit;      /* is one open (al), the button chosen, the text entry's buffer */
    uint32_t pop_cbvar, pop_btnvar;                 /* the open popup's callback, its chosen button (variables) */
    uint32_t tcp_choice_cb, join_cb;                /* the "TCP Connection" box's callback; its Join box's callback */
    uint32_t tcp_btns, tcp_help;                    /* that box's button texts and help lines (char *[]; help 0 = none) */
    uint32_t tcp_nbtn;     /* that box's buttons: 4 = Host, Join, Port, Cancel; 3 = Host, Join, Cancel (no Port) */
    uint32_t pe_time[2];   /* known table only: the exes' PE time stamps (the handler table alone can't tell the
                            * 1998 OEM exe from 1.6.x: same menu code, a different TCP box) */
} nm_build;

/* what the game says about hosting, the Join box, the player's name (nfs3_fix.c) */
typedef struct {
    const char *build, *name;
    uint32_t net;      /* DWORD: the network game state; the DWORD after it (net + 4): the game type */
    uint32_t server;   /* WORD: this PC is the server */
    uint32_t join;     /* char[30]: the Join box's address, typed last (0 = not pinned down for this build) */
    uint32_t pname;    /* char[]: the player's name as the menus show it ("Player 1 Name"); 0 = not pinned down */
} nfs3_build;

typedef struct {
    int ok;               /* 1 = every field below was found and cross-checked */
    char why[160];        /* ok = 0: the first check that failed */
    nm_build nm;          /* (nm.name = "found at launch"; pe_time[0] = this exe's) */
    nfs3_build fx;        /* (fx.build = NULL) */
    uint32_t join_len;    /* the Join box's max length (characters) */
} nfs3_found;

/* A way to read the loaded image: 1 = all n bytes at va read into out. */
typedef int (*nf_read_fn)(void *ctx, uint32_t va, void *out, unsigned n);

/* The finder over any image (base = its load address, 0x400000 for the game). Fills *f; returns f->ok. */
int nfs3_find_image(nf_read_fn rd, void *ctx, uint32_t base, nfs3_found *f);

/* The running game (nfs3.exe at 0x400000, read with readability checks): found once, then kept. NULL = not found
 * (or not at 0x400000). Safe from any thread. */
const nfs3_found *nfs3_find_running(void);
const char *nfs3_find_why(void);   /* why nfs3_find_running gave NULL ("" while not asked) */

/* The found table as text, for the log (and the scan tool): "field=value ..." in a few lines. */
int nfs3_found_text(const nfs3_found *f, char *out, int n);
