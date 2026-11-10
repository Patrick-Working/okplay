/* okplay wsock - Need for Speed III: what the game itself says about hosting (nfs3_fix.c) */
#pragma once

int nfs3_init(const char *ini);              /* DllMain (Need for Speed III only): starts finding the build */
int nfs3_hosting(int *net, int *server);     /* 1 = the game hosts (Host Game), 0 = it does not, -1 = not known;
                                              * net/server (may be NULL): the two values read, when 0 or 1 */
int nfs3_max_players(int *type);             /* the players the hosted game takes (host included): 2 Peer-to-Peer,
                                              * 8 Client-Server, 0 = not known; type (may be NULL): the raw value, -1 = not read */
void nfs3_join_prefill(int okplay_on);         /* the game listens on its port: pre-fill / un-fill the Join box */
int nfs3_player_name(char *out, int n);      /* the player's name in the game; 1 = read, 0 = not known */
