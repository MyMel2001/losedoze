/* testserver.c — host-side wl.c harness for w32host interop testing */
#include "lost.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <poll.h>

void wlog(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vfprintf(stderr, fmt, ap); va_end(ap);
  fputc('\n', stderr);
}

int main(void) {
  wlserver *s = wlserver_create("/tmp/wayland-test");
  if (!s) { fprintf(stderr, "server fail\n"); return 1; }
  setvbuf(stdout, NULL, _IONBF, 0);
  fprintf(stderr, "testserver: listening /tmp/wayland-test\n");
  struct pollfd p = { .fd = wlserver_fd(s), .events = POLLIN };
  for (;;) {
    p.fd = wlserver_fd(s);
    int pr = poll(&p, 1, 100);
    if (pr != 0) fprintf(stderr, "harness: poll=%d revents=0x%x\n", pr, p.revents);
    if (pr >= 0) wlserver_pump(s);
  }
}
