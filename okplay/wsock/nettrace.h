/* okplay wsock - network timing trace (nettrace.c): okplay.ini wsock_nettrace=1 */
#ifndef OKW_NETTRACE_H
#define OKW_NETTRACE_H
#include <stdint.h>
void nt_init(const char *ini);
void nt_mode(const char *mode);                       /* "okplay online", "okplay lan", "direct" */
void nt_event(char kind, uintptr_t sock, int bytes);  /* S game send, R game recv, A okplay got from lane, W okplay sent */
void nt_closed(uintptr_t sock);
int nt_enabled(void);
#endif
