/* loseset.c — Losedoze settings app.
 *
 * A desktop GUI (our Win32 layer over the compositor, libwin32.so) that writes
 * the shell's settings file (~/.config/losedoze.conf): network on/off, wifi
 * on/off, wallpaper preset, theme (dark/light) and brightness. The shell
 * reloads the file when it changes; the network toggle also runs
 * /usr/local/bin/losenet.
 *
 * Keyboard: up/down select, left/right or enter/space change, q or Esc quit.
 * Mouse: click a row to select, click the value area to change it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "w32.h"

extern int los_init(void);

/* ASCII -> W16 literal (the image has no wide literals; ASCII only here) */
static LPCWSTR wa(const char *s) {
  static W16 bufs[8][96];
  static int i;
  W16 *b = bufs[i = (i + 1) % 8];
  int n = 0;
  for (; s[n] && n < 95; n++) b[n] = (W8)s[n];
  b[n] = 0;
  return b;
}

/* ---------------- state ---------------- */
#define ROWS 5
enum { R_NET = 0, R_WIFI, R_WALL, R_THEME, R_BRIGHT };
static const char *row_name[ROWS] = { "Network", "WiFi", "Wallpaper",
                                      "Theme", "Brightness" };
static int sel, net_on = 1, wifi_on = 1, wallpaper, dark = 1, bright = 80;
static const int W = 460, H = 320;
static HWND hwnd;

static COLORREF rgb(int r, int g, int b) {
  return (COLORREF)((r << 16) | (g << 8) | b);
}

/* ---------------- config file ---------------- */
static const char *cfg_path(void) {
  static char p[512];
  const char *home = getenv("HOME");
  if (!home) home = "/root";
  snprintf(p, sizeof p, "%s/.config/losedoze.conf", home);
  return p;
}

/* rewrite the file: our keys first, everything else (vol, bt, pinned, ...) kept */
static void cfg_write(void) {
  char buf[4096], keep[4096];
  size_t n = 0, kn = 0;
  FILE *f = fopen(cfg_path(), "r");
  if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
  buf[n] = 0;
  for (char *line = buf; line && *line; ) {
    char *nl = strchr(line, '\n');
    size_t len = nl ? (size_t)(nl - line) + 1 : strlen(line);
    if (len < 256) {
      char one[256];
      memcpy(one, line, len);
      one[len] = 0;
      char *eq = strchr(one, '=');
      if (eq) *eq = 0;
      if (strcmp(one, "dark") && strcmp(one, "wifi") && strcmp(one, "net") &&
          strcmp(one, "wallpaper") && strcmp(one, "bright")) {
        memcpy(keep + kn, one, len);
        kn += len;
      }
    }
    line = nl ? nl + 1 : NULL;
  }
  keep[kn] = 0;

  char dir[512];
  snprintf(dir, sizeof dir, "%s/.config",
           getenv("HOME") ? getenv("HOME") : "/root");
  mkdir(dir, 0755);
  f = fopen(cfg_path(), "w");
  if (!f) return;
  fprintf(f, "dark=%d\nnet=%d\nwifi=%d\nwallpaper=%d\nbright=%d\n",
          dark, net_on, wifi_on, wallpaper, bright);
  fputs(keep, f);
  fclose(f);
}

/* ---------------- network ---------------- */
static void net_apply(void) {
  if (access("/usr/local/bin/losenet", X_OK) != 0) return;
  if (fork() == 0) {
    execl("/usr/local/bin/losenet", "losenet", net_on ? "up" : "down",
          (char *)NULL);
    _exit(127);
  }
  waitpid(-1, NULL, WNOHANG);
}

/* ---------------- values ---------------- */
static void row_change(int row, int dir) {
  switch (row) {
    case R_NET: net_on = !net_on; net_apply(); break;
    case R_WIFI: wifi_on = !wifi_on; net_apply(); break;
    case R_WALL: wallpaper = (wallpaper + (dir >= 0 ? 1 : 2)) % 3; break;
    case R_THEME: dark = !dark; break;
    case R_BRIGHT:
      bright += dir >= 0 ? 10 : -10;
      if (bright < 10) bright = 10;
      if (bright > 100) bright = 100;
      break;
  }
  cfg_write();
  InvalidateRect(hwnd, NULL, 1);
}

static void row_value(int row, char *out, size_t cap) {
  switch (row) {
    case R_NET: snprintf(out, cap, "%s", net_on ? "On" : "Off"); break;
    case R_WIFI: snprintf(out, cap, "%s", wifi_on ? "On" : "Off"); break;
    case R_WALL: snprintf(out, cap, "%s", wallpaper == 0 ? "Dusk" :
                                  wallpaper == 1 ? "Forest" : "Deep sea"); break;
    case R_THEME: snprintf(out, cap, "%s", dark ? "Dark" : "Light"); break;
    case R_BRIGHT: snprintf(out, cap, "%d%%", bright); break;
  }
}

/* ---------------- painting ---------------- */
static void paint(HWND h) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  if (!dc) return;
  RECT all = { 0, 0, W, H };
  int base = dark ? 0x1f : 0xf3;
  HBRUSH bg = CreateSolidBrush(rgb(base, base, base));
  FillRect(dc, &all, bg);
  DeleteObject(bg);
  SetBkMode(dc, 1 /* transparent */);
  int fg = dark ? 0xff : 0x1a;
  SetTextColor(dc, rgb(fg, fg, fg));
  TextOutW(dc, 24, 20, wa("Settings"), -1);
  SetTextColor(dc, rgb(0x9a, 0x9a, 0x9a));
  TextOutW(dc, 24, 44, wa("applied live: the shell reloads this file"), -1);

  for (int i = 0; i < ROWS; i++) {
    int y = 84 + i * 38;
    if (i == sel) {
      RECT hl = { 16, y - 8, W - 16, y + 26 };
      HBRUSH hb = CreateSolidBrush(rgb(0x2f, 0x6f, 0xed));
      FillRect(dc, &hl, hb);
      DeleteObject(hb);
    }
    SetTextColor(dc, rgb(0xff, 0xff, 0xff));
    TextOutW(dc, 28, y, wa(row_name[i]), -1);
    char v[64];
    row_value(i, v, sizeof v);
    SetTextColor(dc, rgb(0xdd, 0xdd, 0xdd));
    TextOutW(dc, W - 150, y, wa(v), -1);
  }

  HPEN pen = CreatePen(0, 1, rgb(0x3a, 0x3a, 0x3a));
  HGDIOBJ old = SelectObject(dc, pen);
  int sep_y = 84 + ROWS * 38 - 10;
  MoveToEx(dc, 24, sep_y, NULL);
  LineTo(dc, W - 24, sep_y);
  SelectObject(dc, old);
  DeleteObject(pen);
  SetTextColor(dc, rgb(0x9a, 0x9a, 0x9a));
  TextOutW(dc, 24, sep_y + 16,
           wa("up/down select    left/right change    q quit"), -1);
  EndPaint(h, &ps);
}

/* ---------------- window proc ---------------- */
static LRESULT wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: paint(h); return 0;
    case WM_KEYDOWN:
      switch ((int)wp) {
        case VK_UP: sel = (sel + ROWS - 1) % ROWS; InvalidateRect(h, NULL, 1); return 0;
        case VK_DOWN: sel = (sel + 1) % ROWS; InvalidateRect(h, NULL, 1); return 0;
        case VK_LEFT: row_change(sel, -1); return 0;
        case VK_RIGHT: case VK_RETURN: case VK_SPACE: row_change(sel, 1); return 0;
        case 'q': case 'Q': case VK_ESCAPE: PostQuitMessage(0); return 0;
      }
      break;
    case WM_LBUTTONDOWN: {
      int x = (int)(lp & 0xffff), y = (int)((lp >> 16) & 0xffff);
      for (int i = 0; i < ROWS; i++) {
        int ry = 84 + i * 38;
        if (y >= ry - 8 && y < ry + 26) {
          sel = i;
          if (x > W - 170) row_change(i, 1);
          else InvalidateRect(h, NULL, 1);
          break;
        }
      }
      return 0;
    }
    case WM_CLOSE: PostQuitMessage(0); break;   /* DefWindowProc destroys it */
  }
  return DefWindowProcW(h, msg, wp, lp);
}

int main(void) {
  if (!los_init()) {
    fprintf(stderr, "loseset: no compositor\n");
    return 1;
  }
  FILE *f = fopen(cfg_path(), "r");
  if (f) {                                  /* start from the current settings */
    char line[512];
    while (fgets(line, sizeof line, f)) {
      char *eq = strchr(line, '=');
      if (!eq) continue;
      *eq = 0;
      int v = atoi(eq + 1);
      if (!strcmp(line, "dark")) dark = v;
      else if (!strcmp(line, "wifi")) wifi_on = v;
      else if (!strcmp(line, "net")) net_on = v;
      else if (!strcmp(line, "wallpaper")) wallpaper = v;
      else if (!strcmp(line, "bright")) bright = v;
    }
    fclose(f);
  }

  WNDCLASSW wc;
  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = wndproc;
  wc.lpszClassName = wa("LosedozeSettings");
  if (!RegisterClassExW(&wc)) {
    fprintf(stderr, "loseset: RegisterClassEx failed\n");
    return 1;
  }
  hwnd = CreateWindowExW(0, wa("LosedozeSettings"), wa("Settings"),
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H,
                         NULL, NULL, NULL, NULL);
  if (!hwnd) {
    fprintf(stderr, "loseset: CreateWindowEx failed\n");
    return 1;
  }
  printf("LOSEDOZE SETTINGS OK\n");
  fflush(stdout);

  MSG m;
  while (GetMessageW(&m, NULL, 0, 0)) {
    if (m.message == WM_QUIT) break;
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  return 0;
}
