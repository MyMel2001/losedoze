/* wsprintf.c — Win32 wsprintfA/wsprintfW for the losedoze Win32 subsystem.
 * Full printf-style formatting: flags (- + space # 0), width, precision,
 * length modifiers (h hh l ll z t j L) and conversions
 * (d i u o x X c s S p f e g %).
 *
 * GCC's ms_abi variadic support is broken (it builds a bogus register save
 * area, so va_start reads garbage) and clang refuses va_start in a Win64
 * function altogether. So the exported functions take the MS x64 varargs as
 * fixed parameters instead and pick them apart by argument position:
 *
 *   position 0,1  -> RCX, RDX      (the caller's first two arguments)
 *   position 2    -> R8            + XMM2 for a float
 *   position 3    -> R9            + XMM3 for a float
 *   position 4+  -> caller stack, 8 bytes per slot (rsp+0x28 at entry)
 *
 * A floating-point vararg is also copied by the caller into the general
 * purpose register of its own position, so a float can be read as raw bits
 * from the same slot an integer would use. (This is what clang emits for
 * x86_64-pc-windows-msvc variadic calls.)
 *
 * ponytail: 8 vararg slots; widen if an app ever needs more.
 *
 * i386: the exports are plain cdecl variadic (stdcall is ignored on variadic
 * functions, by both MSVC and GCC), so GCC's va_start works; the varargs are
 * pulled with their real types into the same positional gp[] slots the core
 * reads (see args_from_fmt()).
 */
#define W32_NO_VSPRINTF_DECL
#include "w32.h"

#include <string.h>
#include <math.h>
#if defined(__i386__)
#include <stdarg.h>
#endif

#define MAX_GP 8

typedef struct {
  W64 gp[MAX_GP]; /* argument positions 2..9 */
  int pos;        /* next argument position (2 = first vararg) */
} FmtArgs;

/* the raw 64 bits of the argument at the next position */
static W64 arg_raw(FmtArgs *a) {
  int i = a->pos++ - 2;
  if (i < 0 || i >= MAX_GP) return 0;
  return a->gp[i];
}

static int arg_int(FmtArgs *a) { return (int)(W32)arg_raw(a); }

static double arg_flt(FmtArgs *a) {
  W64 v = arg_raw(a);
  double d;
  memcpy(&d, &v, sizeof d);
  return d;
}

#if defined(__i386__)
/* Pull the varargs into gp[] in format order, each with its real type (i386
 * cdecl slots are type-sized, not fixed 8-byte like x64). The spec grammar
 * below must stay in sync with fmt_core()'s (flags, width, precision, length
 * modifier, conversion). */
static void args_from_fmt(FmtArgs *a, const char *fmt, va_list ap) {
  int k = 0;
  for (const char *p = fmt; *p; p++) {
    if (*p != '%') continue;
    p++;
    if (!*p) break;
    while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') p++;
    if (*p == '*') { if (k < MAX_GP) a->gp[k++] = (W64)(W64S)va_arg(ap, int); p++; }
    else while (*p >= '0' && *p <= '9') p++;
    if (*p == '.') {
      p++;
      if (*p == '*') { if (k < MAX_GP) a->gp[k++] = (W64)(W64S)va_arg(ap, int); p++; }
      else while (*p >= '0' && *p <= '9') p++;
    }
    int wide64 = 0; /* l ll z t j L -> one 64-bit argument */
    if (*p == 'h') { p++; if (*p == 'h') p++; }
    else if (*p == 'l' || *p == 'z' || *p == 't' || *p == 'j' || *p == 'L') {
      p++; wide64 = 1; if (*p == 'l') p++;
    }
    char c = *p;
    W64 v = 0;
    if (c == 'd' || c == 'i')
      v = (W64)(wide64 ? (W64S)va_arg(ap, long long) : (W64S)va_arg(ap, int));
    else if (c == 'u' || c == 'o' || c == 'x' || c == 'X')
      v = wide64 ? va_arg(ap, unsigned long long) : (W64)va_arg(ap, unsigned int);
    else if (c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G') {
      double d = va_arg(ap, double);
      memcpy(&v, &d, sizeof d);
    } else if (c == 'c' || c == 'C')
      v = (W64)(W64S)va_arg(ap, int);
    else if (c == 's' || c == 'S' || c == 'p')
      v = (W64)(size_t)va_arg(ap, void *);
    else continue; /* '%%' and unknown specs consume no argument */
    if (k < MAX_GP) a->gp[k++] = v;
  }
}
#endif /* __i386__ */

/* ---------------- UTF-8 <-> UTF-16 (local: this file stands alone) ------ */
static int utf8_put(char *o, int n, unsigned cp) {
  if (cp < 0x80) { o[n++] = (char)cp; }
  else if (cp < 0x800) { o[n++] = (char)(0xc0 | (cp >> 6)); o[n++] = (char)(0x80 | (cp & 0x3f)); }
  else if (cp < 0x10000) {
    o[n++] = (char)(0xe0 | (cp >> 12));
    o[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
    o[n++] = (char)(0x80 | (cp & 0x3f));
  } else {
    o[n++] = (char)(0xf0 | (cp >> 18));
    o[n++] = (char)(0x80 | ((cp >> 12) & 0x3f));
    o[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
    o[n++] = (char)(0x80 | (cp & 0x3f));
  }
  return n;
}

static int utf8_from_w16(char *o, const W16 *s, int cap) {
  int n = 0;
  if (!s) return 0;
  for (int i = 0; s[i] && n < cap - 4; i++) {
    unsigned cp = s[i];
    if (cp >= 0xd800 && cp < 0xdc00 && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
      cp = 0x10000 + ((cp - 0xd800) << 10) + (s[i + 1] - 0xdc00);
      i++;
    }
    n = utf8_put(o, n, cp);
  }
  return n;
}

static int utf16_from_utf8(W16 *o, const char *s, int cap) {
  int n = 0;
  if (!s) return 0;
  while (*s && n < cap - 2) {
    unsigned char c = (unsigned char)*s++;
    unsigned cp = c;
    int extra = 0;
    if (c >= 0xf0) { cp &= 0x07; extra = 3; }
    else if (c >= 0xe0) { cp &= 0x0f; extra = 2; }
    else if (c >= 0xc0) { cp &= 0x1f; extra = 1; }
    for (int i = 0; i < extra && *s; i++) cp = (cp << 6) | ((unsigned char)*s++ & 0x3f);
    if (cp >= 0x10000 && n < cap - 2) {
      cp -= 0x10000;
      o[n++] = (W16)(0xd800 + (cp >> 10));
      o[n++] = (W16)(0xdc00 + (cp & 0x3ff));
    } else {
      o[n++] = (W16)cp;
    }
  }
  if (n < cap) o[n] = 0;
  return n;
}

/* ---------------- number helpers --------------------------------------- */
static int utoa(char *buf, W64 v, int base, int upper) {
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = digits[v % (W64)base];
    v /= (W64)base;
  } while (v);
  for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
  return n;
}

/* integer conversion with flags/width/precision */
static int fmt_int(char *o, W64 uv, int neg, int base, int upper, int plus,
                   int space, int alt, int width, int prec, int zero, int left) {
  char digits[32];
  int nd = utoa(digits, uv, base, upper);
  char pre[3];
  int np = 0;
  if (neg) pre[np++] = '-';
  else if (plus) pre[np++] = '+';
  else if (space) pre[np++] = ' ';
  if (alt && base == 16 && uv) {
    pre[np++] = '0';
    pre[np++] = upper ? 'X' : 'x';
  } else if (alt && base == 8) {
    pre[np++] = '0';
  }
  if (prec == 0 && uv == 0) nd = 0;
  if (prec > nd) {
    int extra = prec - nd;
    for (int i = nd - 1; i >= 0; i--) digits[i + extra] = digits[i];
    for (int i = 0; i < extra; i++) digits[i] = '0';
    nd = prec;
  }
  int total = np + nd;
  int pad = width > total ? width - total : 0;
  int n = 0;
  if (!left && !(zero && prec < 0))
    for (int i = 0; i < pad; i++) o[n++] = ' ';
  for (int i = 0; i < np; i++) o[n++] = pre[i];
  if (!left && zero && prec < 0)
    for (int i = 0; i < pad; i++) o[n++] = '0';
  for (int i = 0; i < nd; i++) o[n++] = digits[i];
  if (left)
    for (int i = 0; i < pad; i++) o[n++] = ' ';
  return n;
}

/* |v| with prec fractional digits; returns chars written */
static int fmt_fixed_digits(char *o, double av, int prec) {
  double scale = 1.0;
  for (int i = 0; i < prec; i++) scale *= 10.0;
  char tmp[512];
  int t;
  double scaled = av * scale;
  if (scaled < 1.8e19 && scaled >= 0.0) {
    t = utoa(tmp, (W64)(scaled + 0.5), 10, 0);
  } else {
    t = utoa(tmp, (W64)av, 10, 0);
    for (int i = 0; i < prec; i++) tmp[t++] = '0';
  }
  int n = 0;
  if (prec == 0) {
    memcpy(o, tmp, (size_t)t);
    return t;
  }
  int intlen = t - prec;
  if (intlen <= 0) {
    o[n++] = '0';
    o[n++] = '.';
    for (int i = 0; i < -intlen; i++) o[n++] = '0';
    memcpy(o + n, tmp, (size_t)t);
    n += t;
  } else {
    memcpy(o, tmp, (size_t)intlen);
    n = intlen;
    o[n++] = '.';
    memcpy(o + n, tmp + intlen, (size_t)prec);
    n += prec;
  }
  return n;
}

static int fmt_float(char *o, double v, int prec, int e_flag, int g_flag, int upper,
                     int plus, int space, int width, int left, int alt, int zero) {
  char body[560];
  int bn = 0;
  int neg = v < 0.0;
  double av = neg ? -v : v;
  if (prec < 0) prec = 6;
  if (e_flag || g_flag) {
    int exp10 = 0;
    if (av > 0.0) {
      exp10 = (int)floor(log10(av));
      double p = pow(10.0, (double)exp10);
      while (av / p >= 10.0) { p *= 10.0; exp10++; }
      while (av / p < 1.0) { p /= 10.0; exp10--; }
    }
    if (g_flag && !alt && exp10 < -4) e_flag = 1;
    if (g_flag && !alt && exp10 >= prec) e_flag = 1;
    if (g_flag && !alt && !e_flag) prec = prec - 1 - exp10;
    if (g_flag && !alt && prec < 1) prec = 1;
    if (e_flag) {
      double mant = av;
      if (exp10 > 0) mant /= pow(10.0, (double)exp10);
      else if (exp10 < 0) mant *= pow(10.0, (double)-exp10);
      bn = fmt_fixed_digits(body, mant, prec);
      if (bn >= 2 && body[1] == '.') {
        /* keep the point: %e always shows prec+1 significant digits */
      }
      body[bn++] = upper ? 'E' : 'e';
      if (exp10 < 0) { body[bn++] = '-'; exp10 = -exp10; }
      else body[bn++] = '+';
      char eb[8];
      int en = utoa(eb, (W64)exp10, 10, 0);
      for (int i = en; i < 2; i++) body[bn++] = '0';
      for (int i = 0; i < en; i++) body[bn++] = eb[i];
    } else {
      bn = fmt_fixed_digits(body, av, prec);
      if (!alt) {
        while (bn > 0 && body[bn - 1] == '0') bn--;
        if (bn > 0 && body[bn - 1] == '.') bn--;
      }
    }
  } else {
    bn = fmt_fixed_digits(body, av, prec);
  }
  char pre[2];
  int np = 0;
  if (neg) pre[np++] = '-';
  else if (plus) pre[np++] = '+';
  else if (space) pre[np++] = ' ';
  int total = np + bn;
  int pad = width > total ? width - total : 0;
  int n = 0;
  if (!left && !zero)
    for (int i = 0; i < pad; i++) o[n++] = ' ';
  for (int i = 0; i < np; i++) o[n++] = pre[i];
  if (!left && zero)
    for (int i = 0; i < pad; i++) o[n++] = '0';
  for (int i = 0; i < bn; i++) o[n++] = body[i];
  if (left)
    for (int i = 0; i < pad; i++) o[n++] = ' ';
  return n;
}

/* ---------------- core formatter ---------------------------------------- */
static int fmt_core(char *out, const char *fmt, FmtArgs *a, int wide) {
  int n = 0;
  for (const char *p = fmt; *p; p++) {
    if (*p != '%') {
      out[n++] = *p;
      continue;
    }
    p++;
    if (!*p) break;
    int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
    for (;;) {
      if (*p == '-') left = 1;
      else if (*p == '+') plus = 1;
      else if (*p == ' ') space = 1;
      else if (*p == '#') alt = 1;
      else if (*p == '0') zero = 1;
      else break;
      p++;
    }
    int width = -1;
    if (*p == '*') {
      width = arg_int(a);
      if (width < 0) { left = 1; width = -width; }
      p++;
    } else if (*p >= '0' && *p <= '9') {
      width = 0;
      while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
    }
    int prec = -1;
    if (*p == '.') {
      p++;
      if (*p == '*') {
        prec = arg_int(a);
        if (prec < 0) prec = -1;
        p++;
      } else {
        prec = 0;
        while (*p >= '0' && *p <= '9') { prec = prec * 10 + (*p - '0'); p++; }
      }
    }
    int lmod = 0; /* 0 = int, 1 = 64-bit, 2 = short, 3 = char */
    if (*p == 'h') {
      p++;
      lmod = 2;
      if (*p == 'h') { p++; lmod = 3; }
    } else if (*p == 'l' || *p == 'z' || *p == 't' || *p == 'j') {
      p++;
      lmod = 1;
      if (*p == 'l') p++;
    } else if (*p == 'L') {
      p++;
      lmod = 1;
    }
    char conv = *p;
    switch (conv) {
      case 'd':
      case 'i': {
        W64S v = (W64S)arg_raw(a);
        if (lmod == 2) v = (W64S)(W16S)(W16)v;
        else if (lmod == 3) v = (W64S)(signed char)(W8)v;
        else if (lmod == 0) v = (W64S)(W32S)(W32)v;
        int neg = v < 0;
        W64 uv = neg ? (W64)(-(v + 1)) + 1 : (W64)v;
        n += fmt_int(out + n, uv, neg, 10, 0, plus, space, alt, width, prec, zero, left);
        break;
      }
      case 'u':
      case 'o':
      case 'x':
      case 'X': {
        W64 v = arg_raw(a);
        if (lmod == 2) v = (W64)(W16)v;
        else if (lmod == 3) v = (W64)(W8)v;
        else if (lmod == 0) v = (W64)(W32)v;
        int base = conv == 'u' ? 10 : conv == 'o' ? 8 : 16;
        n += fmt_int(out + n, v, 0, base, conv == 'X', plus, space, alt, width, prec, zero, left);
        break;
      }
      case 'p': {
        W64 v = arg_raw(a);
        n += fmt_int(out + n, v, 0, 16, 1, 0, 0, 0, 16, -1, 1, 0);
        break;
      }
      case 'c': {
        if (wide) {
          unsigned cp = (unsigned)arg_int(a) & 0xffffu;
          n = utf8_put(out, n, cp);
        } else if (conv == 'c') {
          out[n++] = (char)arg_int(a);
        }
        break;
      }
      case 'C': {
        unsigned cp = (unsigned)arg_int(a) & 0xffffu;
        n = utf8_put(out, n, cp);
        break;
      }
      case 's':
      case 'S': {
        W64 av = arg_raw(a);
        const char *s = (const char *)av;
        const W16 *ws = (const W16 *)av;
        char tmp[1024];
        int tl = 0;
        if (wide) {
          if (conv == 's') tl = utf8_from_w16(tmp, ws, (int)sizeof tmp);
          else if (s) { tl = (int)strlen(s); if (tl > (int)sizeof tmp - 1) tl = (int)sizeof tmp - 1; memcpy(tmp, s, (size_t)tl); }
        } else {
          if (conv == 's') { if (s) { tl = (int)strlen(s); if (tl > (int)sizeof tmp - 1) tl = (int)sizeof tmp - 1; memcpy(tmp, s, (size_t)tl); } }
          else tl = utf8_from_w16(tmp, ws, (int)sizeof tmp);
        }
        if (prec >= 0 && tl > prec) tl = prec;
        int pad = width > tl ? width - tl : 0;
        if (!left) for (int i = 0; i < pad; i++) out[n++] = ' ';
        memcpy(out + n, tmp, (size_t)tl);
        n += tl;
        if (left) for (int i = 0; i < pad; i++) out[n++] = ' ';
        break;
      }
      case 'f':
      case 'F':
      case 'e':
      case 'E':
      case 'g':
      case 'G': {
        double v = arg_flt(a);
        n += fmt_float(out + n, v, prec, conv == 'e' || conv == 'E',
                       conv == 'g' || conv == 'G', conv == 'E' || conv == 'G',
                       plus, space, width, left, alt, zero);
        break;
      }
      case '%':
        out[n++] = '%';
        break;
      case 0:
        break;
      default:
        out[n++] = '%';
        out[n++] = conv;
        break;
    }
  }
  out[n] = 0;
  return n;
}

/* ---------------- exported entry points ---------------------------------- */
#if defined(__i386__)
/* cdecl variadic (stdcall is ignored on variadic, both sides): the caller
 * keeps the stack, so plain va_start works and the callee returns with ret */
int wsprintfA(LPSTR out, LPCSTR fmt, ...) {
  FmtArgs a = {{0}, 2};
  va_list ap;
  va_start(ap, fmt);
  args_from_fmt(&a, fmt ? fmt : "", ap);
  va_end(ap);
  return fmt_core(out, fmt ? fmt : "", &a, 0);
}

int wsprintfW(LPWSTR out, LPCWSTR fmt, ...) {
  char fbuf[1024], buf[2048];
  int fn = 0;
  if (fmt) fn = utf8_from_w16(fbuf, fmt, (int)sizeof fbuf);
  fbuf[fn] = 0;
  FmtArgs a = {{0}, 2};
  va_list ap;
  va_start(ap, fmt);
  args_from_fmt(&a, fbuf, ap);
  va_end(ap);
  int n = fmt_core(buf, fbuf, &a, 1);
  utf16_from_utf8(out, buf, 2048);
  return n;
}
#else
W32API int wsprintfA(LPSTR out, LPCSTR fmt, W64 a1, W64 a2, W64 a3, W64 a4,
                      W64 a5, W64 a6, W64 a7, W64 a8) {
  FmtArgs a;
  a.gp[0] = a1; a.gp[1] = a2; a.gp[2] = a3; a.gp[3] = a4;
  a.gp[4] = a5; a.gp[5] = a6; a.gp[6] = a7; a.gp[7] = a8;
  a.pos = 2;
  return fmt_core(out, fmt ? fmt : "", &a, 0);
}

W32API int wsprintfW(LPWSTR out, LPCWSTR fmt, W64 a1, W64 a2, W64 a3, W64 a4,
                      W64 a5, W64 a6, W64 a7, W64 a8) {
  char fbuf[1024], buf[2048];
  FmtArgs a;
  a.gp[0] = a1; a.gp[1] = a2; a.gp[2] = a3; a.gp[3] = a4;
  a.gp[4] = a5; a.gp[5] = a6; a.gp[6] = a7; a.gp[7] = a8;
  a.pos = 2;
  int fn = 0;
  if (fmt) {
    const W16 *s = fmt;
    int cap = (int)sizeof fbuf - 1;
    while (s[fn] && fn < cap) {
      unsigned cp = s[fn];
      if (cp >= 0xd800 && cp < 0xdc00 && s[fn + 1] >= 0xdc00 && s[fn + 1] < 0xe000) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (s[fn + 1] - 0xdc00);
        fn++;
      }
      fn = utf8_put(fbuf, fn, cp);
    }
    fbuf[fn] = 0;
  }
  int n = fmt_core(buf, fbuf, &a, 1);
  int wn = utf16_from_utf8(out, buf, 2048);
  (void)wn;
  return n;
}
#endif /* !__i386__ */
