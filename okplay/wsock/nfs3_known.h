/* okplay wsock - Need for Speed III: the hand-made address tables of the builds okplay knows. [game: NFS3]
 *
 * Learned by studying the game (nothing of it is copied here - only addresses and what they mean). Used first; any
 * other build goes to the finder (nfs3_find.c), whose results the scan tool (tests/nfs3scan.c) compares with these.
 * Define NFS3_KNOWN_MENU and/or NFS3_KNOWN_FIX before including it for the table(s) wanted. */
#pragma once
#include "nfs3_find.h"

#ifdef NFS3_KNOWN_MENU
/* the in-game menus: matched by the game's own data (the handler table entry pointing at "connect" with these
 * enter/leave handlers) and the exe's PE time stamp */
static const nm_build nm_builds[] = {
    { "1.6.x (Modern Patch 1.6.1, portable 1.6.2 Beta)", 0x555f00, 0x537d1c, 0x4405c0, 0x440730, 0x442a40, 0x43fe50,
      0x555ed0, 0x537d04, 0x4403f0, 0x4446d0, 0x444a80, 0x444c20, 0x4438b0, 0x4446c0, 0x444c10,
      0x558b18, 0x558b3c, 0x43f760, 0x43f680, 0x5f4064, 0x5f4074, 4, { 0x58137f6f, 0x5916ea74 } },
};
#endif

#ifdef NFS3_KNOWN_FIX
/* hosting state, Join box, player name: matched by the exe fingerprint (okp_exe_build) */
static const nfs3_build builds[] = {
    { "240a083a78ac3b83", "Modern Patch 1.6.1",              0x7a22e0, 0x7a22da, 0x6fbb7e, 0x6fd2fc },
    { "397dee58eb88822c", "portable 1.6.2 Beta",             0x7a22e0, 0x7a22da, 0x6fbb7e, 0x6fd2fc },
    { "b390be67925b2ad0", "1998 OEM release",                0x7a22e0, 0x7a22da, 0, 0 },   /* (Join box buffer not studied) */
    { "53aa0dfcf438788d", "1998 CD (unpacked in memory)",    0x7c92e0, 0x7c92da, 0x722b7e, 0x7242fc },   /* (packed on disk) */
};
#endif
