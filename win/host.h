/* host.h — internal interfaces of the w32host Win32 subsystem host. */
#ifndef W32HOST_H
#define W32HOST_H
#include "w32.h"

/* ---------------- PE loader (pe.c) ---------------- */
typedef struct PeImage PeImage;

/* Load a PE32 (i386) or PE32+ (AMD64) image from file; resolve imports against
 * the builtin export tables and PE DLLs loaded from disk; apply relocations.
 * Returns NULL + *err on failure. */
PeImage *pe_load(const char *path, const char **err);
void *pe_entry(PeImage *m);           /* NULL for 32-bit images (not runnable) */
const char *pe_refuse(PeImage *m);    /* why a loaded image cannot be run */
void pe_inspect(PeImage *m);
HMODULE pe_load_library(const char *name, const char **err);
BOOL pe_release(HMODULE h);
HMODULE pe_find_module(const char *name);
HMODULE pe_module(PeImage *m);
void pe_register_dll(const char *name, const char *const *funcs, void **fns);
HMODULE pe_dll_handle(const char *name);
FARPROC pe_get_export(HMODULE h, const char *name);
const char *pe_image_name(PeImage *m);


/* ---------------- wayland client (wlclient.c) ---------------- */
typedef struct WlcWin WlcWin;

struct WlcCbs {
  /* configure: compositor picked/changed content size (cw,ch; 0 = pick own) */
  void (*configure)(void *ud, WlcWin *win, int cw, int ch);
  void (*close_req)(void *ud, WlcWin *win);
  /* keyboard: evdev keycode; down 1/0/2(repeat); mods bit0 shift bit2 ctrl bit3 alt */
  void (*key)(void *ud, int code, int down, unsigned mods);
  /* pointer, content-local coords */
  void (*motion)(void *ud, int x, int y);
  void (*button)(void *ud, int button /*1,2,3*/, int pressed, int x, int y);
  /* buffer released by compositor: safe to reuse that slot */
  void (*buf_release)(void *ud, WlcWin *win);
  void *ud;
};

int wlc_connect(void);            /* 0 ok; tries XDG_RUNTIME_DIR/WAYLAND_DISPLAY */
int wlc_fd(void);
void wlc_set_cbs(const struct WlcCbs *cb);
void wlc_dispatch(void);          /* read+parse; invokes callbacks */
void wlc_flush(void);
WlcWin *wlc_toplevel(const char *title, const char *appid);
void wlc_win_surface(WlcWin *win, int cw, int ch); /* size/attach shm buffer (XRGB8888) */
W32 *wlc_win_pixels(WlcWin *w, int *stride4);
int wlc_win_uses_dmabuf(WlcWin *w);   /* 1 = buffer is a client GPU dma-buf */
void wlc_win_commit(WlcWin *w);   /* attach+damage+commit+flush */
int wlc_win_alive(WlcWin *w);     /* 0 after close_req handled/destroy */
int wlc_win_buf_busy(WlcWin *w);  /* 1 = no free buffer right now */
int wlc_win_configured(WlcWin *w);
void wlc_win_destroy(WlcWin *w);
int wlc_have_display(void);

/* ---------------- gdi objects (gdi32.c) ---------------- */
#define GDI_BRUSH 1
#define GDI_PEN 2
#define GDI_FONT 3
typedef struct W32GdiObj {
  W32 magic; int kind;
  W32 color;              /* brush/pen color (COLORREF) */
  int style, width;       /* pen */
} W32GdiObj;

/* drawing target behind an HDC */
typedef struct W32Canvas {
  W32 *px; int w, h, stride;
  W32 textcol, bkcol;
  int bkmode;             /* 1 transparent, 2 opaque */
  W32GdiObj *pen, *brush, *font;
  float font_px;          /* text rasterization height (Font.Size) */
  RECT clip;              /* current clip rect (exclusive right/bottom) */
  RECT clip_stack[8];     /* SaveDC/RestoreDC saves */
  int clip_sp;
} W32Canvas;

W32GdiObj *gdi_stock(int idx);
void gdi_textout(W32Canvas *c, int x, int y, const W16 *s, int n);

/* ---------------- desktop GL (gl32.c) ---------------- */
int gl32_swap(W32Canvas *c);   /* present the current GL context into a canvas */

/* ---------------- shared host state (w32host.c) ---------------- */
void host_log(const char *fmt, ...);void user32_key_event(int code, int down, unsigned mods); /* evdev -> MSGs */
void user32_char_event(unsigned cp);
void user32_motion_event(int x, int y);
void user32_button_event(int button, int pressed, int x, int y);
void user32_configure_event(WlcWin *w, int cw, int ch);
void user32_close_event(WlcWin *w);
void user32_release_event(WlcWin *w);
int user32_have_msg(void);        /* queued messages or pending paint */

/* ---------------- shared handle/string layer (ntdll.c) ---------------- */
#define W32_HANDLE_MAGIC 0x33484e57u
enum { HK_FD = 1, HK_STDIN, HK_STDOUT, HK_STDERR, HK_HEAP, HK_MODULE, HK_EVENT,
       HK_MEM, HK_SEM, HK_MUTEX, HK_TIMER, HK_SEC, HK_KEY, HK_PROC, HK_THREAD,
       HK_PORT, HK_JOB, HK_TOKEN, HK_WNF, HK_DEBUG };
typedef struct W32Handle {
  W32 magic;
  int kind;   /* HK_* (ntdll internal) */
  int fd;
  void *ptr;  /* heap block / module */
  W64 size;
} W32Handle;

W32Handle *w32_handle_new(int kind, int fd);
int w32_handle_kind(HANDLE h, int want);
int w32_handle_fd(HANDLE h);
W32Handle *w32_handle(HANDLE h);
int w16_to_utf8(const W16 *s, int n, char *out, size_t cap);
int nt_path_to_unix(const char *in, char *out, size_t cap);
void nt_init(void);
HMODULE pe_module_main(void);   /* w32host.c: the running EXE image */
HMODULE w32host_dll(LPCSTR n);  /* w32host.c: builtin dll handle by name */
void k32_init_stdio(void);      /* kernel32.c */
void k32_set_command_line(int argc, char **argv);

#endif
