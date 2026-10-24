/* losterm.c — Losedoze terminal app.
 *
 * Native terminal over our own Win32 layer (libwin32.so / win/user32.c +
 * win/wlclient.c behind it): window, message loop, GDI text onto the canvas,
 * keyboard (WM_KEYDOWN/WM_CHAR from the Win32 input layer) + scrollback +
 * command execution via /bin/sh.
 *
 * Keys: type text (WM_CHAR), up/down history recall, back delete, enter run,
 * Esc/q quit. Commands run through /bin/sh -c, stdout/stderr captured to the
 * scrollback.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "w32.h"
#include "host.h"

extern int los_init(void);

static int selftest(void);

#define TERM_ROWS 64
#define TERM_COLS 256
#define TERM_FG   0xffffffu
#define TERM_BG   0x1e1e1eu
#define TERM_ACC  0x2f6fedu

static HWND hwnd;
static char scr[TERM_ROWS][TERM_COLS];
static int scr_n;
static char input[TERM_COLS];
static int ilen;
static char hist_cmds[16][TERM_COLS];
static int hist_n, hist_c;

/* ---------------- scrollback ---------------- */
static void push_line(const char *l) {
    if (scr_n >= TERM_ROWS) {
        for (int i = 1; i < TERM_ROWS; i++) memcpy(scr[i - 1], scr[i], TERM_COLS);
        scr_n = TERM_ROWS - 1;
    }
    snprintf(scr[scr_n], TERM_COLS, "%s", l);
    scr_n++;
}

/* ---------------- command execution ---------------- */
static void run_cmd(const char *cmd) {
    int fds[2];
    if (pipe(fds)) return;
    pid_t p = fork();
    if (p == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    char *all = (char *)calloc(65536, 1);
    size_t total = 0;
    for (;;) {
        ssize_t r = read(fds[0], all + total, 65536 - total - 1);
        if (r <= 0) break;
        total += (size_t)r;
    }
    close(fds[0]);
    int st;
    waitpid(p, &st, 0);
    all[total] = 0;
    char *line = all;
    char *nl;
    while ((nl = (char *)strchr(line, '\n'))) {
        *nl = 0;
        push_line(line);
        line = nl + 1;
    }
    if (*line) push_line(line);
    free(all);
}

static void submit_cmd(void) {
    char cmd[TERM_COLS];
    snprintf(cmd, sizeof cmd, "%s", input);
    if (ilen) input[ilen] = 0;
    ilen = 0;
    if (cmd[0]) {
        snprintf(hist_cmds[hist_n % 16], TERM_COLS, "%s", cmd);
        hist_n++;
        hist_c = hist_n - 1;
    }
    if (cmd[0]) run_cmd(cmd);
}

/* ---------------- GDI text helpers ---------------- */
static void txt_state(HDC dc, int size, uint32_t fg, uint32_t bg) {
    SetTextColor(dc, W32RGB((fg >> 16) & 255, (fg >> 8) & 255, fg & 255));
    SetBkColor(dc, W32RGB((bg >> 16) & 255, (bg >> 8) & 255, bg & 255));
    SetBkMode(dc, OPAQUE);
    SetFontSize(dc, (float)size);
}
static int txt_line_h(HDC dc, int size) {
    W16 w[8];
    int n = 0;
    w[0] = (W16)'A'; w[1] = (W16)'g'; w[2] = 0;
    n = 2;
    SIZE sz;
    GetTextExtentPoint32W(dc, w, n, &sz);
    return sz.cy > 0 ? (int)sz.cy : (int)(size * 1.2);
}
static void txt_draw(HDC dc, int x, int y, const char *s, int size, uint32_t fg) {
    W16 w[TERM_COLS];
    int i = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && i < TERM_COLS - 1; p++) {
        if (*p < 0x80) w[i++] = (W16)*p;
    }
    w[i] = 0;
    txt_state(dc, size, fg, TERM_BG);
    TextOutW(dc, x, y, w, i);
}

/* ---------------- painting ---------------- */
static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    if (!dc) return;
    W32Canvas *c = (W32Canvas *)dc;
    int W = c->w, H = c->h;
    int fsz = (int)(c->font_px > 0 ? c->font_px : 16.0f);
    int pitch = txt_line_h(dc, fsz);
    if (pitch < fsz) pitch = fsz;
    HBRUSH b = CreateSolidBrush(W32RGB((TERM_BG >> 16) & 255, (TERM_BG >> 8) & 255, TERM_BG & 255));
    RECT all;
    all.left = 0; all.top = 0; all.right = W; all.bottom = H;
    FillRect(dc, &all, b);
    DeleteObject(b);
    int top = 4;
    int maxrows = (H - top - 40) / pitch;
    if (maxrows > TERM_ROWS) maxrows = TERM_ROWS;
    if (maxrows < 1) maxrows = 1;
    for (int i = 0; i < scr_n && i < maxrows; i++) {
        const char *ln = scr[scr_n - 1 - i];
        if (ln[0]) txt_draw(dc, 8, top + i * pitch, ln, fsz, TERM_FG);
    }
    int iy = top + (maxrows + 1) * pitch;
    char row[TERM_COLS + 64];
    int p = snprintf(row, sizeof row, "losedoze> %s", input);
    txt_draw(dc, 8, iy, row, fsz, TERM_FG);
    EndPaint(h, &ps);
}

/* ---------------- window proc ---------------- */
static LRESULT wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: paint(h); return 0;
    case WM_KEYDOWN:
        switch ((int)(W32)(WPARAM)wp) {
        case VK_UP: {
            if (hist_n) { hist_c = hist_c > 0 ? hist_c - 1 : 0; snprintf(input, sizeof input, "%s", hist_cmds[hist_c]); ilen = (int)strlen(input); InvalidateRect(h, NULL, 1); }
            return 0;
        }
        case VK_DOWN: {
            if (hist_n) { hist_c = hist_c + 1 < hist_n - 1 ? hist_c + 1 : hist_n - 1; snprintf(input, sizeof input, "%s", hist_cmds[hist_c]); InvalidateRect(h, NULL, 1); }
            return 0;
        }
        case VK_ESCAPE: PostQuitMessage(0); return 0;
        }
        break;
    case WM_CHAR: {
        W32 cp = (W32)(WPARAM)wp;
        char c = (char)cp;
        if (cp == 13) { submit_cmd(); InvalidateRect(h, NULL, 1); return 0; }
        if (cp == 8) {
            if (ilen > 0) { ilen--; input[ilen] = 0; InvalidateRect(h, NULL, 1); }
            return 0;
        }
        if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
            if (ilen < (int)sizeof input - 1) { input[ilen++] = c; input[ilen] = 0; InvalidateRect(h, NULL, 1); }
        }
        return 0;
    }
    case WM_CLOSE: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---------------- ASCII -> W16 literal (the image has no wide literals; ASCII only here) */
static LPCWSTR wa(const char *s) {
    static W16 bufs[8][96];
    static int i;
    W16 *b = bufs[i = (i + 1) % 8];
    int n = 0;
    for (; s[n] && n < 95; n++) b[n] = (W8)s[n];
    b[n] = 0;
    return b;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest();
    if (!los_init()) { fprintf(stderr, "losterm: no compositor\n"); return 1; }
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.lpszClassName = wa("LosedozeTerminal");
    if (!RegisterClassExW(&wc)) { fprintf(stderr, "losterm: RegisterClassEx failed\n"); return 1; }
    hwnd = CreateWindowExW(0, wa("LosedozeTerminal"), wa("Terminal"),
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    if (!hwnd) { fprintf(stderr, "losterm: CreateWindowEx failed\n"); return 1; }
    printf("LOSEDOZE TERMINAL OK\n");
    fflush(stdout);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (m.message == WM_QUIT) break;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

static int selftest(void) {
    W32Canvas c;
    memset(&c, 0, sizeof c);
    c.px = (W32 *)calloc((size_t)640 * 480, 4);
    c.w = 640; c.h = 480; c.stride = 640;
    c.textcol = W32RGB(0, 0, 0);
    c.bkcol = W32RGB(255, 255, 255);
    c.bkmode = OPAQUE;
    c.font_px = 16.0f;
    c.clip.left = 0; c.clip.top = 0; c.clip.right = 640; c.clip.bottom = 480;
    HDC dc = (HDC)&c;
    txt_state(dc, 16, TERM_FG, TERM_BG);
    int y = 20;
    txt_draw(dc, 20, y, "losedoze> hello", 16, TERM_FG);
    int found = 0;
    W32 *px = c.px;
    for (int i = 0; i < 640 * 480; i++) if (px[i] != 0xffffffffu) { found = 1; break; }
    free(c.px);
    fprintf(stderr, "losterm selftest %s\n", found ? "OK" : "FAIL");
    return found ? 0 : 1;
}
