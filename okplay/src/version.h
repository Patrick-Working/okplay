/* single source of truth for the okplay version (C code, resource file, packaging) */
#ifdef OKP_BUILTIN
/* the drop-in edition: okplay built into Wine's dplayx.dll (winedplay/), no install */
#define OKP_VERSION_MAJOR 0
#define OKP_VERSION_MINOR 8
#define OKP_VERSION_PATCH 3
#define OKP_VERSION_STR "0.8.3"
#define OKP_EDITION "drop-in"
#elif defined(OKP_WSOCK)
/* okplay wsock (okplay/wsock/): a wsock32.dll for games that use Winsock directly */
#define OKP_VERSION_MAJOR 0
#define OKP_VERSION_MINOR 2
#define OKP_VERSION_PATCH 0
#define OKP_VERSION_STR "0.2.0"
#define OKP_EDITION "wsock"
#else
#define OKP_VERSION_MAJOR 0
#define OKP_VERSION_MINOR 2
#define OKP_VERSION_PATCH 0
#define OKP_VERSION_STR "0.2.0"
#define OKP_EDITION "installer"
#endif
