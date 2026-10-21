/* dmabuf_client.c — host test client for the client dma-buf path.
 *
 * Connects to a headless loseshell, opens a toplevel, and paints a fixed
 * pattern. The pixel checksum it prints must equal the one the compositor logs
 * from its own mmap of the exported dma-buf ("dmabuf: import WxH stride=S sum=X"),
 * which is the proof that the compositor read the client's GPU buffer. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <sys/mman.h>

#include "host.h"

#define TW 64
#define TH 48

static int configured, cw, ch;
static WlcWin *win;

static void on_configure(void *ud, WlcWin *w, int ncw, int nch) {
  (void)ud; (void)w;
  cw = ncw > 0 ? ncw : TW;
  ch = nch > 0 ? nch : TH;
  configured = 1;
}

static uint32_t fnv(const uint32_t *p, int stride4, int w, int h) {
  uint32_t sum = 2166136261u;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) sum = (sum ^ p[(size_t)y * stride4 + x]) * 16777619u;
  return sum;
}

int main(void) {
  struct WlcCbs cb = { .configure = on_configure };
  wlc_set_cbs(&cb);
  if (wlc_connect() != 0) { fprintf(stderr, "dmabuf_client: connect failed\n"); return 1; }
  win = wlc_toplevel("dmabuf-test", "dmabuf-test");
  if (!win) return 1;
  wlc_win_commit(win);                  /* empty commit -> compositor configures us */
  for (int i = 0; i < 200 && !configured; i++) {
    struct pollfd p = { .fd = wlc_fd(), .events = POLLIN };
    poll(&p, 1, 20);
    wlc_dispatch();
  }
  if (!configured) { fprintf(stderr, "dmabuf_client: no configure\n"); return 1; }
  wlc_win_surface(win, cw, ch);
  int stride4 = 0;
  W32 *px = wlc_win_pixels(win, &stride4);
  if (!px) { fprintf(stderr, "dmabuf_client: no buffer\n"); return 1; }
  for (int y = 0; y < ch; y++)
    for (int x = 0; x < cw; x++)
      px[(size_t)y * stride4 + x] = 0xff000000u | ((uint32_t)(x * 3 & 0xff) << 16) |
                                    ((uint32_t)(y * 5 & 0xff) << 8) | 0x40u;
  wlc_win_commit(win);
  wlc_flush();
  for (int i = 0; i < 50; i++) {
    struct pollfd p = { .fd = wlc_fd(), .events = POLLIN };
    poll(&p, 1, 20);
    wlc_dispatch();
  }
  printf("DMABUF CLIENT: dmabuf=%d %dx%d stride4=%d sum=%08x\n",
         wlc_win_uses_dmabuf(win), cw, ch, stride4, fnv(px, stride4, cw, ch));  return 0;
}
