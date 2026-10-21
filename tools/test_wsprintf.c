/* test_wsprintf.c — host-side check for the Win32 wsprintf implementation.
 * Compares win/wsprintf.c against the host libc's snprintf for the same
 * format strings (the Win32 subset is printf-compatible except %p).
 *
 *   gcc -O2 -o /tmp/test_wsprintf tools/test_wsprintf.c win/wsprintf.c -Iwin -lm
 */
#include <stdio.h>
#include <string.h>

/* as an app sees them: variadic, Win64 ABI */
__attribute__((ms_abi)) int wsprintfA(char *out, const char *fmt, ...);
__attribute__((ms_abi)) int wsprintfW(unsigned short *out, const unsigned short *fmt, ...);

static int fails, checks;

#define CMP(fmt, ...)                                                        \
  do {                                                                       \
    char g_[512], w_[512];                                                   \
    wsprintfA(g_, fmt, ##__VA_ARGS__);                                         \
    snprintf(w_, sizeof w_, fmt, ##__VA_ARGS__);                               \
    checks++;                                                                \
    if (strcmp(g_, w_)) {                                                    \
      printf("FAIL  %-24s got [%s] want [%s]\n", fmt, g_, w_);               \
      fails++;                                                               \
    } else {                                                                 \
      printf("ok    %-24s [%s]\n", fmt, g_);                                  \
    }                                                                        \
  } while (0)

int main(void) {
  CMP("%d", 42);
  CMP("%d", -7);
  CMP("%i", 1234);
  CMP("%u", 4000000000u);
  CMP("%x", 255);
  CMP("%X", 255);
  CMP("%o", 64);
  CMP("%5d|%-5d|%05d", 42, 42, 42);
  CMP("%+d|% d|%#x|%#o", 5, 5, 255, 8);
  CMP("%.5d|%.0d", 42, 0);
  CMP("%s|%10s|%-10s|%.3s", "hello", "hi", "hi", "hello");
  CMP("%c%c%c", 'a', 'b', 'c');
  CMP("%08.3f", 3.14159);
  CMP("%.2f", 3.14159);
  CMP("%f", 2.5);
  CMP("%e", 1234.5);
  CMP("%E", 0.000123);
  CMP("%g", 0.0001);
  CMP("%g", 100000.0);
  CMP("%zu", (unsigned long)12345678901234UL);
  CMP("%lld", -1234567890123LL);
  CMP("%hd", (short)-300);
  CMP("%hhd", (signed char)-5);
  CMP("%lx", 0xdeadbeefUL);
  CMP("100%%");
  CMP("%.*s", 3, "abcdef");
  CMP("%*d", 6, 42);

  /* %p: MS prints 16 uppercase hex digits without 0x — checked by hand */
  {
    char g_[64];
    wsprintfA(g_, "%p", (void *)0x1234);
    if (strcmp(g_, "0000000000001234")) {
      printf("FAIL  %%p -> [%s] want [0000000000001234]\n", g_);
      fails++;
    } else {
      printf("ok    %%-24s [%s]\n", g_);
    }
    checks++;
  }

  /* wide version: %s takes a wide string (Win32 semantics) */
  {
    static const unsigned short wf[] = {'%', 'd', '/', '%', 's', '/', '%', 'c', 0};
    static const unsigned short ws[] = {'h', 'i', 0};
    unsigned short g_[64];
    wsprintfW(g_, wf, 7, ws, (int)'x');
    if (g_[0] != '7' || g_[1] != '/' || g_[2] != 'h' || g_[3] != 'i' ||
        g_[4] != '/' || g_[5] != 'x' || g_[6] != 0) {
      printf("FAIL  wsprintfW -> [%u %u %u %u %u %u %u]\n", g_[0], g_[1], g_[2], g_[3], g_[4], g_[5], g_[6]);
      fails++;
    } else {
      printf("ok    wsprintfW [%u%u%u%u%u%u]\n", g_[0], g_[1], g_[2], g_[3], g_[4], g_[5]);
    }
    checks++;
  }

  printf("\n%d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
