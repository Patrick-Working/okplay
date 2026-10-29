/* Force-included when building Wine's dplayx as a Windows DLL with MinGW. */
#ifndef WINE_COMPAT_H
#define WINE_COMPAT_H
#define DECLSPEC_EXPORT
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
#endif
