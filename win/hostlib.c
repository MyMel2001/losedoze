/* hostlib.c — the Win32 layer as a Linux shared library (libwin32.so).
 *
 * Same user32/gdi32/wlclient sources the PE host uses, so managed apps
 * (.NET) and native tools drive the losedoze desktop through the Win32 API
 * by P/Invoke / linking: RegisterClassEx, CreateWindowEx, GDI painting, the
 * message loop. No Wine, no NT layer — our own Win32 implementation.
 *
 * The PE host (w32host.c) keeps its own main(); this file only provides the
 * glue the shared library needs: compositor callbacks + los_init().
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>

#include "host.h"

void host_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "win32: ");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

/* no PE image and no builtin dll table in the shared library */
HMODULE pe_module_main(void) { return NULL; }
HMODULE w32host_dll(LPCSTR n) { (void)n; return NULL; }

/* desktop GL is not wired into the shared library (ceiling: the PE host has
 * opengl32.dll over EGL; managed callers get CPU rendering only) */
int gl32_swap(W32Canvas *c) { (void)c; return 0; }

/* ---------------- compositor glue (same wiring as w32host.c) ------------- */
static void cb_configure(void *ud, WlcWin *w, int cw, int ch) {
  (void)ud;
  user32_configure_event(w, cw, ch);
}
static void cb_close(void *ud, WlcWin *w) {
  (void)ud;
  user32_close_event(w);
}
static void cb_key(void *ud, int code, int down, unsigned mods) {
  (void)ud;
  user32_key_event(code, down, mods);
}
static void cb_motion(void *ud, int x, int y) {
  (void)ud;
  user32_motion_event(x, y);
}
static void cb_button(void *ud, int button, int pressed, int x, int y) {
  (void)ud;
  user32_button_event(button, pressed, x, y);
}
static void cb_release(void *ud, WlcWin *w) {
  (void)ud;
  user32_release_event(w);
}

/* los_init: start the Win32 layer and connect to the compositor.
 * Returns 1 with a compositor, 0 for a console-only run. */
int los_init(void) {
  nt_init();
  k32_init_stdio();
  if (wlc_connect() == 0) {
    struct WlcCbs cb = {
        .configure = cb_configure,
        .close_req = cb_close,
        .key = cb_key,
        .motion = cb_motion,
        .button = cb_button,
        .buf_release = cb_release,
    };
    wlc_set_cbs(&cb);
    return 1;
  }
  return 0;
}
