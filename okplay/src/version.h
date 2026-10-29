/* single source of truth for the okplay version (C code, resource file, packaging) */
#ifdef OKP_BUILTIN
/* the drop-in edition: okplay built into Wine's dplayx.dll (winedplay/), no install */
#define OKP_VERSION_MAJOR 0
#define OKP_VERSION_MINOR 8
#define OKP_VERSION_PATCH 2
#define OKP_VERSION_STR "0.8.2"
#define OKP_EDITION "drop-in"
#else
#define OKP_VERSION_MAJOR 0
#define OKP_VERSION_MINOR 2
#define OKP_VERSION_PATCH 0
#define OKP_VERSION_STR "0.2.0"
#define OKP_EDITION "installer"
#endif
