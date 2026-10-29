/* Force-included (gcc -include) into every source of the drop-in: the printf family goes to
 * okp_printf.c - the same result on Windows 95 .. 11, and nothing prebuilt for newer CPUs. */
#ifndef OKP_STDIO_H
#define OKP_STDIO_H
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
int okp_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int okp_snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(gnu_printf, 3, 4)));
int okp_vsprintf(char *buf, const char *fmt, va_list ap);
int okp_sprintf(char *buf, const char *fmt, ...) __attribute__((format(gnu_printf, 2, 3)));
#undef snprintf
#undef vsnprintf
#undef sprintf
#undef vsprintf
#undef _snprintf
#undef _vsnprintf
#define snprintf okp_snprintf
#define vsnprintf okp_vsnprintf
#define sprintf okp_sprintf
#define vsprintf okp_vsprintf
#define _snprintf okp_snprintf
#define _vsnprintf okp_vsnprintf
#endif
