/*
 * snprintf & co. for the drop-in, the same on every Windows.
 *
 * MinGW's own C99 printf (libmingwex) is prebuilt for newer CPUs (cmov, SSE: it crashes a Pentium
 * MMX) and the system's msvcrt.dll on Windows 9x doesn't know "%ll" (okplay prints 64-bit peer IDs,
 * some of them to the room server). okp_stdio.h, force-included into the drop-in's sources, points
 * snprintf/vsnprintf/sprintf/vsprintf here.
 *
 * C99 semantics (always NUL-terminated, returns the full length). Supported: flags - + space # 0,
 * width and precision (digits or *), lengths hh h l ll L q I64 I32 I z j t, conversions
 * d i u o x X c s p %. Anything else is copied as it stands (the drop-in prints no floats).
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct out { char *buf; size_t cap, len; };

static void put(struct out *o, char c)
{
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void pad(struct out *o, char c, int n)
{
    while (n-- > 0) put(o, c);
}

int okp_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct out o = { buf, buf ? cap : 0, 0 };
    const char *p;

    for (p = fmt; *p; p++) {
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0, width = 0, prec = -1, size = 0;
        const char *start = p;
        char tmp[72], *digits;
        int ndigits, neg = 0;
        uint64_t v = 0;

        if (*p != '%') { put(&o, *p); continue; }
        p++;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '#') alt = 1;
            else if (*p == '0') zero = 1;
            else break;
        }
        if (*p == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } p++; }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') { prec = va_arg(ap, int); if (prec < 0) prec = -1; p++; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        /* size: 0 int, 1 long, 2 64-bit, -1 short, -2 char, 3 pointer-sized */
        if (*p == 'h') { size = -1; p++; if (*p == 'h') { size = -2; p++; } }
        else if (*p == 'l') { size = 1; p++; if (*p == 'l') { size = 2; p++; } }
        else if (*p == 'L' || *p == 'q') { size = 2; p++; }
        else if (*p == 'I') {
            if (p[1] == '6' && p[2] == '4') { size = 2; p += 3; }
            else if (p[1] == '3' && p[2] == '2') { size = 0; p += 3; }
            else { size = 3; p++; }
        }
        else if (*p == 'z' || *p == 't') { size = 3; p++; }
        else if (*p == 'j') { size = 2; p++; }

        switch (*p) {
        case '%':
            put(&o, '%');
            continue;
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left) pad(&o, ' ', width - 1);
            put(&o, c);
            if (left) pad(&o, ' ', width - 1);
            continue;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            int n = 0;
            if (!s) s = "(null)";
            while ((prec < 0 || n < prec) && s[n]) n++;
            if (!left) pad(&o, ' ', width - n);
            for (int i = 0; i < n; i++) put(&o, s[i]);
            if (left) pad(&o, ' ', width - n);
            continue;
        }
        case 'p':
            v = (uintptr_t)va_arg(ap, void *);
            size = 3;
            if (prec < 0) prec = (int)(sizeof(void *) * 2);   /* msvcrt style: 0012FF40 */
            goto hex_upper;
        case 'd': case 'i': {
            int64_t sv;
            if (size == 2) sv = va_arg(ap, int64_t);
            else if (size == 1) sv = va_arg(ap, long);
            else if (size == 3) sv = (intptr_t)va_arg(ap, intptr_t);
            else sv = va_arg(ap, int);
            if (size == -1) sv = (short)sv;
            if (size == -2) sv = (signed char)sv;
            neg = sv < 0;
            v = neg ? 0 - (uint64_t)sv : (uint64_t)sv;
            goto decimal;
        }
        case 'u': case 'o': case 'x': case 'X':
            if (size == 2) v = va_arg(ap, uint64_t);
            else if (size == 1) v = va_arg(ap, unsigned long);
            else if (size == 3) v = (uintptr_t)va_arg(ap, uintptr_t);
            else v = va_arg(ap, unsigned int);
            if (size == -1) v = (unsigned short)v;
            if (size == -2) v = (unsigned char)v;
            if (*p == 'u') goto decimal;
            if (*p == 'o') {
                digits = tmp + sizeof(tmp);
                do { *--digits = (char)('0' + (v & 7)); v >>= 3; } while (v);
                if (alt && *digits != '0') *--digits = '0';
                goto emit;
            }
            if (*p == 'X') goto hex_upper;
            {
                const char *hx = "0123456789abcdef";
                digits = tmp + sizeof(tmp);
                if (v || prec != 0) do { *--digits = hx[v & 15]; v >>= 4; } while (v);
                goto emit_hex;
            }
        default:
            /* unknown: copy the spec as it stands */
            while (start <= p && *start) put(&o, *start++);
            if (!*p) p--;
            continue;
        }

    hex_upper:
        {
            const char *hx = "0123456789ABCDEF";
            digits = tmp + sizeof(tmp);
            if (v || prec != 0) do { *--digits = hx[v & 15]; v >>= 4; } while (v);
        }
    emit_hex:
        ndigits = (int)(tmp + sizeof(tmp) - digits);
        {
            int pre = alt && ndigits && !(ndigits == 1 && *digits == '0') && *p != 'p' ? 2 : 0;
            int zeros = prec > ndigits ? prec - ndigits : 0;
            int total = pre + zeros + ndigits;
            if (prec < 0 && zero && !left && width > total) { zeros += width - total; total = width; }
            if (!left) pad(&o, ' ', width - total);
            if (pre) { put(&o, '0'); put(&o, *p == 'X' ? 'X' : 'x'); }
            pad(&o, '0', zeros);
            for (int i = 0; i < ndigits; i++) put(&o, digits[i]);
            if (left) pad(&o, ' ', width - total);
        }
        continue;

    decimal:
        digits = tmp + sizeof(tmp);
        if (v || prec != 0) do { *--digits = (char)('0' + v % 10); v /= 10; } while (v);
    emit:
        ndigits = (int)(tmp + sizeof(tmp) - digits);
        {
            char sign = neg ? '-' : plus && (*p == 'd' || *p == 'i') ? '+' : space && (*p == 'd' || *p == 'i') ? ' ' : 0;
            int zeros = prec > ndigits ? prec - ndigits : 0;
            int total = (sign ? 1 : 0) + zeros + ndigits;
            if (prec < 0 && zero && !left && width > total) { zeros += width - total; total = width; }
            if (!left) pad(&o, ' ', width - total);
            if (sign) put(&o, sign);
            pad(&o, '0', zeros);
            for (int i = 0; i < ndigits; i++) put(&o, digits[i]);
            if (left) pad(&o, ' ', width - total);
        }
    }
    if (o.cap) o.buf[o.len < o.cap ? o.len : o.cap - 1] = 0;
    return (int)o.len;
}

int okp_snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = okp_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int okp_vsprintf(char *buf, const char *fmt, va_list ap)
{
    return okp_vsnprintf(buf, (size_t)-1 / 2, fmt, ap);
}

int okp_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = okp_vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}
