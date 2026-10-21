/* lost.h — shared types for loseshell's native DRM/KMS + evdev + wayland stack.
 * Graphics/input code adapted from K93OS-x86 (k93ui.c, GPL-2.0) — used with permission. */
#ifndef LOST_H
#define LOST_H
#include <stdint.h>
#include <stddef.h>

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int32_t i32;

#define RGB(r, g, b) ((u32)(r) << 16 | (u32)(g) << 8 | (u32)(b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* Canvas: ARGB8888 pixels, stride in pixels (scanout buffers may pad rows).
 * All drawing helpers live in loseshell.c (cv_*). */
typedef struct Canvas {
  u32 *px;
  int w, h, stride;
} Canvas;

/* Set/get pixel with bounds check (renamed from px to avoid clash with
 * loseshell's rgb color converter px(unsigned).) */
static inline void cpx(Canvas *c, int x, int y, u32 v) {
  if ((unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h) c->px[y * c->stride + x] = v;
}
static inline u32 cpx_get(Canvas *c, int x, int y) { return c->px[y * c->stride + x]; }

/* ---------- drm/kms (kms.c) ---------- */
struct card;

/* Open first usable KMS card (/dev/dri/card*), atomic modesetting. Fails on none. */
struct card *card_open(void);
/* Modeset to preferred (or forced) mode; creates two dumb buffers + shadow canvas.
 * card->shadow is where the shell draws; card->fbw/fbh/fbstride_px describe it. */
int card_modeset(struct card *c, int fbw, int fbh);
Canvas *card_shadow(struct card *c);
int card_fbwidth(struct card *c);
int card_fbheight(struct card *c);
/* Copy shadow to back buffer, atomic page flip, swap. Returns vblank sequence, <0 error. */
unsigned long card_flip(struct card *c);
const char *card_backend(struct card *c); /* "kms" or "fbdev" */
void card_close(struct card *c);
/* Which kernel driver bound the card, e.g. "bochs-drm", "i915", "amdgpu", "nouveau". */
const char *card_driver(struct card *c);

/* Optional GPU acceleration: EGL+GL on GBM over the same card. 0 on success.
 * card has an accelerated backend if card_gpu_ok(c). */
int card_egl_init(struct card *c);
int card_gpu_ok(struct card *c);
const char *card_gpu_renderer(struct card *c); /* GL_RENDERER string, or NULL */
/* Composite frame on GPU: wallpaper gradient (animated t) + overlay texture of the
 * CPU canvas region. Present into GBM front buffer + scanout. 0 on success. */
int card_gpu_frame(struct card *c, int x, int y, int w, int h, unsigned t_ms);
void card_gpu_shutdown(struct card *c);

/* Client dma-buf import (zwp_linux_dmabuf_v1): map the client's buffer for CPU
 * compositing and prove the PRIME import. 0 on success. c may be NULL (headless). */
int card_dmabuf_import(struct card *c, int fd, uint32_t format, uint32_t stride,
                       uint32_t length, void **px, int *w, int *h);

/* Host-side selftest: create GL context on a render node (no modeset), clear, read back. */
int kms_selftest(void);

/* ---------- input (input.c) ---------- */
/* evdev keycodes (linux/input-event-codes.h values). */
#define LKEY_ESC 1
#define LKEY_1 2
#define LKEY_0 11
#define LKEY_MINUS 12
#define LKEY_EQUAL 13
#define LKEY_BACKSPACE 14
#define LKEY_TAB 15
#define LKEY_ENTER 28
#define LKEY_LCTRL 29
#define LKEY_CAPSLOCK 58
#define LKEY_F1 59
#define LKEY_F12 87
#define LKEY_HOME 102
#define LKEY_UP 103
#define LKEY_PGUP 104
#define LKEY_LEFT 105
#define LKEY_RIGHT 106
#define LKEY_END 107
#define LKEY_DOWN 108
#define LKEY_PGDN 109
#define LKEY_INSERT 110
#define LKEY_DELETE 111
#define LKEY_LEFTMETA 125
#define LKEY_RIGHTMETA 126

typedef struct KeyInput {
  int code;    /* evdev keycode */
  int pressed; /* 1 down, 0 up, 2 autorepeat */
} KeyInput;

typedef struct MouseInput {
  int dx, dy, dw; /* rel deltas; dw +1 = wheel up */
  int buttons;    /* bit0 left bit1 right bit2 middle, current state */
  int changed;    /* nonzero if motion/button changed */
  int abs_x, abs_y, has_abs; /* absolute pointer (USB tablet) teleport, screen px */
} MouseInput;

/* Open all /dev/input/event* (USB HID + PS/2 keyboards/mice arrive via evdev).
 * Grabs each device (EVIOCGRAB) so the shell is sole consumer.
 * Returns number of fds, fills fds[]. */
int input_open(int *fds, int max);
/* 1 = pointer device (rel/abs mouse/tablet), 0 = keyboard/other. */
int input_is_pointer(int fd);
/* Screen size for absolute-pointer scaling. */
void input_set_screen(int w, int h);
/* Drain pending events from fd. Keys appended to keys[] (max keys_cap, count in *nkeys),
 * mouse deltas accumulated into m (m->changed set if anything). Returns 0 on ok. */
int input_read(int fd, KeyInput *keys, int *nkeys, int keys_cap, MouseInput *m);
/* Modifier/depressed state for wl_keyboard.modifiers: bit0 shift bit1 caps bit2 ctrl
 * bit3 alt bit4 logo bit5 num. */
unsigned input_mods(void);
/* US QWERTY printable translation for shell-side text entry.
 * Returns unicode codepoint or 0. */
u32 input_key_char(int code, int shift);

/* ---------- wayland (wl.c) ----------
 * Hand-rolled Wayland wire server (no libwayland-server): wl_display,
 * registry, compositor, shm(+pool/buffer), surface, region, seat(+keyboard,
 * +pointer), output, data-device(-manager) minimal, subcompositor, xdg-shell
 * (wm_base/surface/toplevel/popup/positioner), callback. xkb_v1 keymap fd
 * from embedded string (build-time xkbcomp output, keymap.c). */
struct wlserver;
typedef struct wlserver wlserver;
typedef struct WlWin WlWin;

/* Shell callbacks, invoked from wlserver_pump(). */
struct wlserver_cbs {
  void (*mapped)(void *ud, WlWin *w);        /* first commit with buffer */
  void (*unmapped)(void *ud, WlWin *w);      /* surface destroyed */
  void (*title)(void *ud, WlWin *w);         /* title/appid changed */
  void (*resized)(void *ud, WlWin *w, int bw, int bh); /* new buffer size */
  void (*close_req)(void *ud, WlWin *w);     /* xdg ping timeout n/a; popup grabs end */
  /* zwp_linux_dmabuf_v1: a client hands us a dma-buf fd. Returns 0 on success
   * and fills the CPU view (px, w, h) of the imported buffer. */
  int (*dmabuf_import)(void *ud, int fd, uint32_t format, uint32_t stride,
                        uint32_t length, void **px, int *w, int *h);
  void *ud;
};

/* Window as seen by the compositor (xdg_toplevel). Public fields are
 * read-mostly; positioning/activation go through the wlwin_* calls. */
struct WlWin {
  struct WlWin *next;  /* stack order: head = topmost */
  int mapped;
  int popup;           /* 1 = xdg_popup; composited above its parent stack */
  int x, y;            /* surface origin on screen (shell-set) */
  int cw, ch;          /* last configured content size */
  int bw, bh;          /* attached buffer size */
  int stride4;         /* buffer pixels per row */
  int alpha;           /* buffer format has real alpha (ARGB8888) vs XRGB8888 */
  const u32 *px;       /* attached buffer pixels (valid until ack_paint), or NULL */
  int activated;
  char title[256];
  char appid[128];
  /* internal (wl.c) */
  void *srv;
  uint32_t surf_id, xdg_id, toplevel_id;
};

wlserver *wlserver_create(const char *sockname);
int wlserver_fd(wlserver *w);                /* listen fd for poll */
void wlserver_pump(wlserver *w);             /* accept + dispatch pending wire msgs */
const char *wlserver_socket_path(wlserver *w);
void wlserver_set_cbs(wlserver *w, const struct wlserver_cbs *cb);
void wlserver_destroy(wlserver *w);

/* iteration: head of window stack (topmost first) */
WlWin *wlserver_wins(wlserver *w);
WlWin *wlwin_next(WlWin *win);

/* accessors */
void wlwin_size(WlWin *win, int *bw, int *bh);
const char *wlwin_title(WlWin *win);
const char *wlwin_appid(WlWin *win);
/* Content pixels of attached shm buffer (ARGB8888), stride in pixels.
 * NULL when nothing attached. Valid until wlserver_ack_paint. */
const u32 *wlwin_pixels(WlWin *win, int *stride4, int *bw, int *bh);

/* shell -> protocol (each call flushes) */
void wlwin_configure(WlWin *win, int cw, int ch);   /* xdg configure + resize */
void wlwin_set_pos(WlWin *win, int x, int y);
void wlwin_raise(WlWin *win);                       /* restack to head */
void wlwin_set_activated(WlWin *win, int on);       /* configure + kb focus */
void wlwin_close(WlWin *win);                       /* xdg_toplevel.close */
/* After compositing: wl_buffer.release + frame.done (also when px == NULL). */
void wlserver_ack_paint(wlserver *w, WlWin *win);

/* input routing: screen-absolute coords; wl.c picks surface + sends local */
WlWin *wlserver_pick(wlserver *w, int sx, int sy, int *lx, int *ly);
void wlserver_pointer_motion(wlserver *w, int sx, int sy);
void wlserver_pointer_button(wlserver *w, int button /*1/2/3*/, int pressed);
void wlserver_pointer_axis(wlserver *w, int dir /* +1 wheel up, -1 down */);
void wlserver_key(wlserver *w, int code /*evdev*/, int pressed, unsigned mods);

#endif
