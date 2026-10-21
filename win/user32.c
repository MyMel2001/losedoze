/* user32.c — homemade user32 for the losedoze Win32 subsystem.
 * Windows, classes, message queue, painting — backed by wlclient toplevels
 * (shell draws decorations; we own the content buffer). Original code. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

/* wsprintf* live in win/wsprintf.c. */
#include "host.h"

#define WND_MAGIC 0x334e4457u
#define QCAP 512

unsigned input_key_char(int code, int shift); /* shell/input.c (evdev table) */

typedef struct W32Wnd {
  W32 magic;
  WlcWin *wl;
  WNDPROC proc;
  DWORD style;
  int cw, ch;                 /* client (buffer) size */
  char title[256];
  W32 *shadow;                /* offscreen client buffer (ARGB) */
  W32Canvas canvas;
  int need_paint, commit_pending, destroyed, visible;
  struct W32Wnd *next;
} W32Wnd;

typedef struct W32Class {
  char name[128];
  WNDPROC proc;
  struct W32Class *next;
} W32Class;

static W32Wnd *wins;
static W32Class *classes;
static MSG q[QCAP];
static int qh, qt, qn;

/* ---------------- queue ---------------- */
static void post(const MSG *m) {
  if (qn >= QCAP) return;
  q[qt] = *m;
  qt = (qt + 1) % QCAP;
  qn++;
}
static int take(MSG *m) {
  if (!qn) return 0;
  *m = q[qh];
  qh = (qh + 1) % QCAP;
  qn--;
  return 1;
}

static void post_simple(W32Wnd *w, UINT msg, WPARAM wp, LPARAM lp) {
  MSG m;
  memset(&m, 0, sizeof m);
  m.hwnd = (HWND)w;
  m.message = msg;
  m.wParam = wp;
  m.lParam = lp;
  post(&m);
}

void PostQuitMessage(int code) {
  MSG m;
  memset(&m, 0, sizeof m);
  m.message = WM_QUIT;
  m.wParam = (WPARAM)code;
  post(&m);
}

BOOL PostMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  post_simple((W32Wnd *)h, msg, wp, lp);
  return W32_TRUE;
}

LRESULT SendMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  W32Wnd *w = (W32Wnd *)h;
  if (!w || w->magic != WND_MAGIC || !w->proc) return 0;
  return w->proc(h, msg, wp, lp);
}

int user32_have_msg(void) {
  if (qn) return 1;
  for (W32Wnd *w = wins; w; w = w->next)
    if (w->need_paint) return 1;
  return 0;
}

/* ---------------- painting plumbing ---------------- */
static void win_commit_shadow(W32Wnd *w) {
  if (!w->wl || !wlc_win_alive(w->wl)) return;
  int stride4;
  W32 *dst = wlc_win_pixels(w->wl, &stride4);
  if (dst) {
    for (int y = 0; y < w->ch && y < w->canvas.h; y++)
      memcpy(dst + (size_t)y * stride4, w->shadow + (size_t)y * w->cw,
             (size_t)w->cw * 4);
    w->commit_pending = 0;
  } else {
    w->commit_pending = 1;   /* buffer comes after configure; commit empty now */
  }
  wlc_win_commit(w->wl);
}

static void win_paint(W32Wnd *w) {
  w->need_paint = 0;
  SendMessageW((HWND)w, WM_PAINT, 0, 0);
}

static void win_ensure_buffer(W32Wnd *w) {
  if (w->wl) wlc_win_surface(w->wl, w->cw, w->ch);
}

/* wlclient callbacks (glued in w32host.c) ---------------- */
void user32_configure_event(WlcWin *wl, int cw, int ch) {
  for (W32Wnd *w = wins; w; w = w->next)
    if (w->wl == wl) {
      if (cw > 0 && ch > 0 && (cw != w->cw || ch != w->ch)) {
        W32 *ns = malloc((size_t)cw * (size_t)ch * 4);
        if (!ns) return;
        free(w->shadow);
        w->shadow = ns;
        memset(ns, 0xff, (size_t)cw * (size_t)ch * 4);
        w->cw = cw; w->ch = ch;
        w->canvas.px = ns;
        w->canvas.w = cw;
        w->canvas.h = ch;
        w->canvas.stride = cw;
      }
      win_ensure_buffer(w);   /* create shm pool now that configure arrived */
      w->need_paint = 1;
      return;
    }
}

void user32_release_event(WlcWin *wl) {
  for (W32Wnd *w = wins; w; w = w->next)
    if (w->wl == wl) {
      if (w->commit_pending) win_commit_shadow(w);
      return;
    }
}

void user32_close_event(WlcWin *wl) {
  for (W32Wnd *w = wins; w; w = w->next)
    if (w->wl == wl) {
      SendMessageW((HWND)w, WM_CLOSE, 0, 0);
      return;
    }
}

static void win_destroy(W32Wnd *w) {
  if (w->destroyed) return;
  w->destroyed = 1;
  SendMessageW((HWND)w, WM_DESTROY, 0, 0);
  W32Wnd **pp = &wins;
  while (*pp && *pp != w) pp = &(*pp)->next;
  if (*pp == w) *pp = w->next;
  free(w->shadow);
  w->magic = 0;
  free(w);
}

/* ---------------- input events (evdev codes via wlclient) ------------- */
static int kb_shift;

static W32Wnd *focused(void) { return wins; /* head = topmost window */ }

static int evdev_to_vk(int code) {
  switch (code) {
  case 14: return VK_BACK;
  case 15: return VK_TAB;
  case 28: return VK_RETURN;
  case 1: return VK_ESCAPE;
  case 105: return VK_LEFT;
  case 106: return VK_RIGHT;
  case 103: return VK_UP;
  case 108: return VK_DOWN;
  case 102: return VK_HOME;
  case 107: return VK_END;
  case 104: return VK_PRIOR;
  case 109: return VK_NEXT;
  case 111: return VK_DELETE;
  case 57: return VK_SPACE;
  case 29: return VK_CONTROL;
  case 42: case 54: return VK_SHIFT;
  case 56: return VK_MENU;
  default:
    if (code >= 59 && code <= 68) return VK_F1 + (code - 59);
    if (code >= 2 && code <= 11) return '0' + (code - 2);
    if (code >= 16 && code <= 25) return 'q' + (code - 16);
    if (code >= 30 && code <= 38) return 'a' + (code - 30);
    if (code >= 44 && code <= 50) return 'z' + (code - 44);
    return 0;
  }
}

void user32_key_event(int code, int down, unsigned mods) {
  kb_shift = (mods & 1) != 0;
  W32Wnd *w = focused();
  if (!w || down == 0) return; /* key-up not delivered (ceiling) */
  int vk = evdev_to_vk(code);
  if (!vk) return;
  post_simple(w, WM_KEYDOWN, (WPARAM)vk, 0);
  unsigned cp = input_key_char(code, kb_shift);
  if (cp) post_simple(w, WM_CHAR, (WPARAM)cp, 0);
  else if (code == 28) post_simple(w, WM_CHAR, 13, 0);
  else if (code == 14) post_simple(w, WM_CHAR, 8, 0);
  else if (code == 15) post_simple(w, WM_CHAR, 9, 0);
}

void user32_char_event(unsigned cp) {
  W32Wnd *w = focused();
  if (w) post_simple(w, WM_CHAR, (WPARAM)cp, 0);
}

void user32_motion_event(int x, int y) {
  W32Wnd *w = focused();
  if (w) post_simple(w, WM_MOUSEMOVE, 0, (LPARAM)((W32)x | ((W32)y << 16)));
}

void user32_button_event(int button, int pressed, int x, int y) {
  W32Wnd *w = focused();
  if (!w) return;
  UINT msg = 0;
  WPARAM mk = 0;
  if (button == 1) { msg = pressed ? WM_LBUTTONDOWN : WM_LBUTTONUP; mk = 1; }
  else if (button == 3) { msg = pressed ? WM_RBUTTONDOWN : WM_RBUTTONUP; mk = 2; }
  if (msg) post_simple(w, msg, mk, (LPARAM)((W32)x | ((W32)y << 16)));
}

/* ---------------- message loop ---------------- */
static void pump(int timeout_ms) {
  if (!wlc_have_display()) {
    usleep((useconds_t)(timeout_ms > 20 ? 20 : timeout_ms) * 1000);
    return;
  }
  struct pollfd pfd = { .fd = wlc_fd(), .events = POLLIN };
  poll(&pfd, 1, timeout_ms);
  wlc_flush();
  wlc_dispatch();
}

static int next_msg(MSG *m, HWND hwnd, UINT min, UINT max) {
  for (;;) {
    MSG t;
    if (take(&t)) {
      if (t.message == WM_QUIT) { *m = t; return 1; }
      if (!hwnd || t.hwnd == hwnd) {
        if ((!min || t.message >= min) && (!max || t.message <= max)) {
          *m = t;
          return 1;
        }
      }
      continue;
    }
    for (W32Wnd *w = wins; w; w = w->next)
      if (w->need_paint) {
        win_paint(w);
        break;
      }
    if (take(&t)) {
      if (t.message == WM_QUIT) { *m = t; return 1; }
      *m = t;
      return 1;
    }
    pump(50);
  }
}

BOOL GetMessageW(MSG *m, HWND hwnd, UINT min, UINT max) {
  if (!m) return (BOOL)-1;
  return (BOOL)next_msg(m, hwnd, min, max);
}
BOOL GetMessageA(MSG *m, HWND hwnd, UINT min, UINT max) {
  return GetMessageW(m, hwnd, min, max);
}

BOOL PeekMessageW(MSG *m, HWND hwnd, UINT min, UINT max, UINT flags) {
  (void)hwnd; (void)min; (void)max; (void)flags;
  MSG t;
  for (W32Wnd *w = wins; w; w = w->next)
    if (w->need_paint) {
      win_paint(w);
      break;
    }
  if (take(&t)) {
    if (m) *m = t;
    return W32_TRUE;
  }
  pump(0);
  return W32_FALSE;
}

BOOL TranslateMessage(const MSG *m) { (void)m; return W32_TRUE; }

LRESULT DispatchMessageW(const MSG *m) {
  W32Wnd *w = m ? (W32Wnd *)m->hwnd : NULL;
  if (!w || w->magic != WND_MAGIC || !w->proc) return 0;
  return w->proc(m->hwnd, m->message, m->wParam, m->lParam);
}
LRESULT DispatchMessageA(const MSG *m) { return DispatchMessageW(m); }

/* ---------------- classes / windows ---------------- */
static void utf8_of_w16(LPCWSTR s, char *out, size_t cap) {
  w16_to_utf8(s ? s : (LPCWSTR)L"", s ? lstrlenW(s) : 0, out, cap);
}

ATOM RegisterClassExW(const WNDCLASSW *wc) {
  if (!wc || !wc->lpfnWndProc || !wc->lpszClassName) return 0;
  W32Class *c = calloc(1, sizeof *c);
  if (!c) return 0;
  utf8_of_w16(wc->lpszClassName, c->name, sizeof c->name);
  c->proc = wc->lpfnWndProc;
  c->next = classes;
  classes = c;
  return 1;
}

ATOM RegisterClassA(const WNDCLASSA *wc) {
  if (!wc || !wc->lpfnWndProc || !wc->lpszClassName) return 0;
  W32Class *c = calloc(1, sizeof *c);
  if (!c) return 0;
  snprintf(c->name, sizeof c->name, "%s", wc->lpszClassName);
  c->proc = wc->lpfnWndProc;
  c->next = classes;
  classes = c;
  return 1;
}

HWND CreateWindowExW(DWORD ex, LPCWSTR classn, LPCWSTR title, DWORD style,
                     int x, int y, int w, int h, HWND parent, HMENU menu,
                     HINSTANCE inst, LPVOID param) {
  (void)ex; (void)x; (void)y; (void)parent; (void)menu; (void)inst;
  (void)param;
  char cls[128], ttl[256];
  utf8_of_w16(classn, cls, sizeof cls);
  utf8_of_w16(title, ttl, sizeof ttl);
  W32Class *c = classes;
  while (c && strcmp(c->name, cls)) c = c->next;
  if (!c) return NULL;
  W32Wnd *wn = calloc(1, sizeof *wn);
  if (!wn) return NULL;
  wn->magic = WND_MAGIC;
  wn->proc = c->proc;
  wn->style = style;
  wn->cw = w == CW_USEDEFAULT || w <= 0 ? 640 : w;
  wn->ch = h == CW_USEDEFAULT || h <= 0 ? 420 : h;
  snprintf(wn->title, sizeof wn->title, "%s", ttl);
  wn->shadow = malloc((size_t)wn->cw * (size_t)wn->ch * 4);
  if (!wn->shadow) { free(wn); return NULL; }
  memset(wn->shadow, 0xff, (size_t)wn->cw * (size_t)wn->ch * 4);
  wn->canvas.px = wn->shadow;
  wn->canvas.w = wn->cw;
  wn->canvas.h = wn->ch;
  wn->canvas.stride = wn->cw;
  wn->canvas.textcol = W32RGB(0, 0, 0);
  wn->canvas.bkcol = W32RGB(255, 255, 255);
  wn->canvas.bkmode = OPAQUE;
  wn->next = wins;
  wins = wn;
  if (wlc_have_display()) {
    wn->wl = wlc_toplevel(wn->title, cls);
    win_ensure_buffer(wn);
  }
  SendMessageW((HWND)wn, WM_CREATE, 0, 0);
  wn->need_paint = 1;
  if (style & WS_VISIBLE) ShowWindow((HWND)wn, SW_SHOW);
  return (HWND)wn;
}

HWND CreateWindowExA(DWORD ex, LPCSTR classn, LPCSTR title, DWORD style,
                     int x, int y, int w, int h, HWND parent, HMENU menu,
                     HINSTANCE inst, LPVOID param) {
  W16 cls[128], ttl[256];
  int i = 0;
  for (; classn && classn[i] && i < 127; i++) cls[i] = (W8)classn[i];
  cls[i] = 0;
  i = 0;
  for (; title && title[i] && i < 255; i++) ttl[i] = (W8)title[i];
  ttl[i] = 0;
  return CreateWindowExW(ex, cls, ttl, style, x, y, w, h, parent, menu, inst,
                         param);
}

BOOL ShowWindow(HWND h, int cmd) {
  W32Wnd *w = (W32Wnd *)h;
  if (!w || w->magic != WND_MAGIC) return W32_FALSE;
  int was = w->visible;
  w->visible = cmd != SW_HIDE;
  if (w->visible && !was) {
    win_ensure_buffer(w);
    w->need_paint = 1;
    win_paint(w);
    win_commit_shadow(w);
  }
  return !was;
}

BOOL UpdateWindow(HWND h) {
  W32Wnd *w = (W32Wnd *)h;
  if (!w || w->magic != WND_MAGIC) return W32_FALSE;
  if (w->need_paint) win_paint(w);
  return W32_TRUE;
}

BOOL IsWindow(HWND h) {
  W32Wnd *w = (W32Wnd *)h;
  return w && w->magic == WND_MAGIC;
}

/* ---------------- def window proc ---------------- */
LRESULT DefWindowProcW(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  W32Wnd *w = (W32Wnd *)h;
  switch (msg) {
  case WM_CLOSE:
    if (w && w->magic == WND_MAGIC) win_destroy(w);
    return 0;
  case WM_ERASEBKGND:
    return 1;
  case WM_GETTEXTLENGTH:
    return w ? (LRESULT)strlen(w->title) : 0;
  case WM_GETTEXT:
    if (w && wp) {
      LPSTR o = (LPSTR)lp;
      snprintf(o, (size_t)wp, "%s", w->title);
      return (LRESULT)strlen(o);
    }
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    BeginPaint(h, &ps);
    EndPaint(h, &ps);
    return 0;
  }
  default:
    return 0;
  }
}
LRESULT DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  return DefWindowProcW(h, msg, wp, lp);
}

/* ---------------- painting API ---------------- */
HDC BeginPaint(HWND h, PAINTSTRUCT *ps) {
  W32Wnd *w = (W32Wnd *)h;
  if (!w || w->magic != WND_MAGIC) return NULL;
  w->need_paint = 0;
  if (ps) {
    memset(ps, 0, sizeof *ps);
    ps->hdc = (HDC)&w->canvas;
    ps->fErase = W32_TRUE;
    ps->rcPaint.left = 0;
    ps->rcPaint.top = 0;
    ps->rcPaint.right = w->cw;
    ps->rcPaint.bottom = w->ch;
  }
  return (HDC)&w->canvas;
}

BOOL EndPaint(HWND h, const PAINTSTRUCT *ps) {
  W32Wnd *w = (W32Wnd *)h;
  (void)ps;
  if (!w || w->magic != WND_MAGIC) return W32_FALSE;
  win_commit_shadow(w);
  return W32_TRUE;
}

HDC GetDC(HWND h) {
  W32Wnd *w = h ? (W32Wnd *)h : focused();
  if (!w || w->magic != WND_MAGIC) return NULL;
  return (HDC)&w->canvas;
}
int ReleaseDC(HWND h, HDC dc) { (void)h; (void)dc; return 1; }

BOOL InvalidateRect(HWND h, const RECT *r, BOOL erase) {
  W32Wnd *w = (W32Wnd *)h;
  (void)r; (void)erase;
  if (!w || w->magic != WND_MAGIC) return W32_FALSE;
  w->need_paint = 1;
  return W32_TRUE;
}
BOOL ValidateRect(HWND h, const RECT *r) {
  W32Wnd *w = (W32Wnd *)h;
  (void)r;
  if (!w || w->magic != WND_MAGIC) return W32_FALSE;
  w->need_paint = 0;
  return W32_TRUE;
}

BOOL GetClientRect(HWND h, RECT *r) {
  W32Wnd *w = (W32Wnd *)h;
  if (!w || w->magic != WND_MAGIC || !r) return W32_FALSE;
  r->left = 0; r->top = 0; r->right = w->cw; r->bottom = w->ch;
  return W32_TRUE;
}
BOOL GetWindowRect(HWND h, RECT *r) {
  W32Wnd *w = (W32Wnd *)h;
  if (!w || w->magic != WND_MAGIC || !r) return W32_FALSE;
  /* decorations are compositor-side: report content at origin (ceiling) */
  r->left = 0; r->top = 0; r->right = w->cw; r->bottom = w->ch;
  return W32_TRUE;
}

BOOL SetWindowTextW(HWND h, LPCWSTR t) {
  W32Wnd *w = (W32Wnd *)h;
  if (!w || w->magic != WND_MAGIC) return W32_FALSE;
  utf8_of_w16(t, w->title, sizeof w->title);
  return W32_TRUE;
}
int GetWindowTextW(HWND h, LPWSTR o, int cap) {
  W32Wnd *w = (W32Wnd *)h;
  if (!w || w->magic != WND_MAGIC || cap <= 0) return 0;
  int n = 0;
  for (; w->title[n] && n < cap - 1; n++) o[n] = (W8)w->title[n];
  o[n] = 0;
  return n;
}

/* ---------------- misc ---------------- */
int MessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT flags) {
  (void)h; (void)flags;
  char t[512], c[256];
  utf8_of_w16(text, t, sizeof t);
  utf8_of_w16(caption, c, sizeof c);
  /* ceiling: no modal dialog box yet — logged, auto-OK */
  host_log("MessageBox: [%s] %s", c, t);
  return IDOK;
}
int MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT flags) {
  (void)h; (void)flags;
  host_log("MessageBox: [%s] %s", caption ? caption : "", text ? text : "");
  return IDOK;
}

static int dummy_icon;
HCURSOR LoadCursorW(HINSTANCE i, LPCWSTR n) {
  (void)i; (void)n;
  return (HCURSOR)&dummy_icon; /* shell owns the pointer */
}
HICON LoadIconW(HINSTANCE i, LPCWSTR n) {
  (void)i; (void)n;
  return (HICON)&dummy_icon;
}

/* wsprintfA/wsprintfW live in win/wsprintf.c (full printf-style
 * formatting, standalone testable: tools/test_wsprintf.c). */
