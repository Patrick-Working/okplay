/* okplay wsock - Need for Speed III: okplay's choice in the game's own Network menu (nfs3_menu.c) [game: NFS3] */
#pragma once

void nfs3_menu_init(const char *ini);   /* DllMain, before the game's front end reads its menus */
int nfs3_menu_mode(void);               /* what the player picked in the game's menu: 0 = nothing (ask), 1 = TCP/IP (the
                                         * game's own), 2 = Oldkid.net Online, 3 = Oldkid.net LAN */
