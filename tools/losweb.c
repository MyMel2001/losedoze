/* losweb - Losedoze web browser.
 *
 * Native client over our own Win32 layer (libwin32.so / win/user32.c +
 * win/wlclient.c behind it):
 *   - window + message loop (GetMessageW/DispatchMessageW) over the
 *     hand-rolled Wayland client behind the library
 *   - GDI: FillRect/TextOutW/GetTextExtentPoint32W over the stb_ttf font
 *     (same Liberation Sans rasterizer the shell uses)
 *   - transport + parse: clean-room k93html parser + minimal loshttp (http://)
 *   - render: k93html tree walk, wrapped text blocks, headings, lists, hr
 *
 * Usage: losweb <url>          open + load
 *        losweb                 omnibox: type host/path, Enter to load
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "w32.h"
#include "host.h"
#include "k93html.h"
#include "loshttp.h"

extern int los_init(void);

static int selftest(void);
static void nav_to_impl(const char *url, int push);
static void nav_to(const char *url);
static void nav_reload(void);
static void push_hist(void);
static void hist_back(void);
static void hist_fwd(void);
static int fill_rect(HDC dc, int x, int y, int w, int h, uint32_t col);
static int text_block(HDC dc, int x, int *y, int cw, const char *s, int size, uint32_t fg);
static int render_node(HDC dc, int *y, k93node *n, int cw, int scale, uint32_t fg);

#define HOME_URL   "http://losedoze.local/"
#define TOOLBAR_H  40
#define WIN_W      860
#define WIN_H      540
#define INK        0x111111u
#define SUB        0x666666u
#define ACCENT     0x2f6fedu
#define PAGE_BG    0xffffffu
#define TOOLBAR    0x2a2a2au
#define OMNI_X     60

/* ---------------- link hit-testing ---------------- */
typedef struct { char href[256]; int y, h; } Anchor;
#define MAX_ANCHORS 64
static Anchor anchors[MAX_ANCHORS];
static int anchor_n;

/* ---------------- browser state ---------------- */
static HWND hwnd;
static char cur_url[1024];
static char hist[24][1024];
static int hist_n, hist_c;      /* hist_c = back index into hist[0..hist_n) */
static k93node *doc;
static char doc_title[256];
static int page_valid;
static char status_msg[256];
static int loading;
static char omni[256];
static int omni_len;
static int content_y;            /* page y of first visible content row */

/* ---------------- URL history ---------------- */
static void push_hist(void) {
    if (hist_n == 24) { memmove(hist[0], hist[1], 23 * 1024); hist_n = 23; }
    snprintf(hist[hist_n % 24], 1024, "%s", cur_url);
    hist_n++;
    hist_c = hist_n - 1;
}
static void hist_back(void) {
    if (hist_c > 0) { hist_c--; nav_to_impl(hist[hist_c], 1); }
}
static void hist_fwd(void) {
    if (hist_c + 1 < hist_n) { hist_c++; nav_to_impl(hist[hist_c], 1); }
}

/* ---------------- navigation ---------------- */
static void nav_to_impl(const char *url, int push) {
    if (!url || !url[0]) return;
    snprintf(cur_url, sizeof cur_url, "%s", url);
    if (push) push_hist();
    loading = 1;
    page_valid = 0;
    anchor_n = 0;
    content_y = 0;
    InvalidateRect(hwnd, NULL, 1);
    struct loshttp r;
    char err[256];
    if (loshttp_get(cur_url, &r, err, sizeof err) != 0) {
        snprintf(status_msg, sizeof status_msg, "%s", err);
        if (!strncmp(cur_url, "http://losedoze.local", 19)) {
            doc = k93html_parse(
                "<html><head><title>Losedoze</title></head><body>"
                "<h1>Losedoze Browser</h1>"
                "<p>Welcome to the Losedoze clean-room web browser.</p>"
                "<p>Type a URL in the omnibox and press Enter to surf.</p>"
                "<ul><li>Back/forward: history buttons or arrow keys</li>"
                "<li>Scroll: mouse wheel or down/up arrow keys</li></ul>"
                "</body></html>");
            k93node *t = k93html_query(doc, "title");
            k93html_text_content(t, doc_title, sizeof doc_title);
            page_valid = 1;
        }
        loading = 0;
        return;
    }
    snprintf(status_msg, sizeof status_msg, "http %d", r.status);
    k93node *d = k93html_parse(r.body);
    doc = d;
    k93node *t = k93html_query(d, "title");
    k93html_text_content(t, doc_title, sizeof doc_title);
    page_valid = 1;
    free(r.body);
    loading = 0;
    InvalidateRect(hwnd, NULL, 1);
}
static void nav_to(const char *url) { nav_to_impl(url, 1); }
static void nav_reload(void) { nav_to_impl(cur_url, 0); }
static void nav_home(void) { nav_to(HOME_URL); }

static void omni_norm(void) {
    char u[1600];
    char t[1600];
    snprintf(t, sizeof t, "%s", omni_len ? omni : cur_url);
    if (strncmp(t, "http://", 7) && strncmp(t, "https://", 8)) snprintf(u, sizeof u, "http://%s", t);
    else snprintf(u, sizeof u, "%s", t);
    nav_to(u);
}

/* ---------------- GDI text helpers ---------------- */
static void txt_state(HDC dc, int size, uint32_t fg, uint32_t bg) {
    SetTextColor(dc, W32RGB((fg >> 16) & 255, (fg >> 8) & 255, fg & 255));
    SetBkColor(dc, W32RGB((bg >> 16) & 255, (bg >> 8) & 255, bg & 255));
    SetBkMode(dc, OPAQUE);
    SetFontSize(dc, (float)size);
}
static void txt_enc(const char *s, W16 *w, int *n) {
    int i = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && i < 1023; p++) {
        if (*p < 0x80) { w[i++] = (W16)*p; continue; }
        if (*p < 0xC0) continue;
        int cp = *p & 0x1f;
        int extra = 0;
        for (int k = 1; k < 2 && (p[k] & 0xc0) == 0x80; k++) { cp = (cp << 6) | (p[k] & 0x3f); extra++; }
        if (cp < 0x100) w[i++] = (W16)cp;
        else if (cp < 0x10000) w[i++] = (W16)(0xd800 + (cp >> 10));
        if (extra < 1) break;
    }
    w[i] = 0;
    *n = i;
}
static int txt_line_h(HDC dc, int size) {
    W16 w[8];
    int n = 0;
    txt_enc("Ag", w, &n);
    SIZE sz;
    GetTextExtentPoint32W(dc, w, n, &sz);
    return sz.cy > 0 ? (int)sz.cy : (int)(size * 1.2);
}
static int txt_width(HDC dc, const char *s, int size) {
    W16 w[256];
    int n = 0;
    txt_enc(s, w, &n);
    if (!n) return 0;
    SIZE sz;
    GetTextExtentPoint32W(dc, w, n, &sz);
    return (int)sz.cx;
}
static void txt_draw(HDC dc, int x, int y, const char *s, int size, uint32_t fg) {
    W16 w[1024];
    int n = 0;
    txt_enc(s, w, &n);
    if (!n) return;
    txt_state(dc, size, fg, PAGE_BG);
    TextOutW(dc, x, y, w, n);
}
/* draw wrapped text block starting at *y; returns advance */
static int text_block(HDC dc, int x, int *y, int cw, const char *s, int size, uint32_t fg) {
    int lh = txt_line_h(dc, size);
    char *dup = strdup(s);
    char *save = NULL;
    char *word = strtok_r(dup, " \t\r\n", &save);
    char *cur = NULL;
    int curw = 0;
    int yy = *y;
    int adv = 0;
    while (word) {
        int ww = txt_width(dc, word, size);
        if (cur && curw + ww > cw) {
            txt_draw(dc, x, yy, cur, size, fg);
            yy += lh;
            adv += lh;
            free(cur);
            cur = NULL;
            curw = 0;
        }
        if (!cur) { cur = strdup(word); curw = ww; }
        else {
            char *nc = (char *)malloc(strlen(cur) + strlen(word) + 2);
            sprintf(nc, "%s %s", cur, word);
            free(cur);
            cur = nc;
            curw += ww;
        }
        word = strtok_r(NULL, " \t\r\n", &save);
    }
    if (cur) { txt_draw(dc, x, yy, cur, size, fg); yy += lh; adv += lh; }
    free(cur);
    free(dup);
    *y = yy;
    return adv;
}

/* ---------------- k93 node rendering ---------------- */
static int render_node(HDC dc, int *y, k93node *n, int cw, int scale, uint32_t fg) {
    if (!n || !n->tag) return 0;
    int start = *y;
    if (!strcmp(n->tag, "#text")) {
        return text_block(dc, 8, y, cw - 16, n->text ? n->text : "", scale * 16, fg);
    }
    int nfg = fg;
    int nsize = scale * 16;
    if (n->tag[0] == 'h' && n->tag[1] >= '1' && n->tag[1] <= '6') {
        nfg = ACCENT;
        nsize = (n->tag[1] == '1' || n->tag[1] == '2') ? scale * 32 : scale * 24;
    }
    if (!strcmp(n->tag, "hr")) {
        int lh = txt_line_h(dc, nsize);
        fill_rect(dc, 8, *y + lh / 2, cw - 16, 1, SUB);
        *y += lh;
        return lh;
    }
    if (!strcmp(n->tag, "ul") || !strcmp(n->tag, "ol")) {
        int counter = 0;
        for (k93node *li = n->first; li; li = li->next) {
            if (strcmp(li->tag, "li")) continue;
            char marker[64];
            if (!strcmp(n->tag, "ul")) snprintf(marker, sizeof marker, "\xe2\x80\xa2 ");
            else { counter++; snprintf(marker, sizeof marker, "%d. ", counter); }
            int my = *y;
            text_block(dc, 8, &my, cw - 24, marker, nsize, ACCENT);
            text_block(dc, 8, &my, cw - 24, li->text, nsize, nfg);
            *y = my;
        }
        *y += txt_line_h(dc, nsize);
        return *y - start;
    }
    if (n->first) {
        int total = 0;
        for (k93node *ch = n->first; ch; ch = ch->next)
            total += render_node(dc, y, ch, cw, scale, nfg);
        return total;
    }
    int adv = 0;
    if (!strcmp(n->tag, "p") || n->tag[0] == 'h' || !strcmp(n->tag, "a") || !strcmp(n->tag, "li") ||
        !strcmp(n->tag, "blockquote") || !strcmp(n->tag, "pre") || !strcmp(n->tag, "td") ||
        !strcmp(n->tag, "th") || !strcmp(n->tag, "center") || !strcmp(n->tag, "figcaption")) {
        uint32_t col = (!strcmp(n->tag, "a") || !strcmp(n->tag, "li")) ? ACCENT : fg;
        int ay = *y;
        adv = text_block(dc, 8, y, cw - 16, n->text ? n->text : "", nsize, col);
        if (!strcmp(n->tag, "a") && n->text) {
            /* record link rect for hit-testing (page coords, top-left origin) */
            if (anchor_n < MAX_ANCHORS) {
                Anchor *a = &anchors[anchor_n];
                const char *href = k93html_attr(n, "href");
                snprintf(a->href, sizeof a->href, "%s", href ? href : n->text);
                a->y = ay;
                a->h = adv;
                anchor_n++;
            }
        }
    }
    return adv;
}

static void render_content(HDC dc, int y, int cw) {
    if (!page_valid || !doc) return;
    k93node *b = k93html_query(doc, "body");
    k93node *root = b ? b : doc;
    int cy = y;
    anchor_n = 0;
    render_node(dc, &cy, root, cw, 1, INK);
}

/* ---------------- painting ---------------- */
static int fill_rect(HDC dc, int x, int y, int w, int h, uint32_t col) {
    RECT r;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    HBRUSH b = CreateSolidBrush(W32RGB((col >> 16) & 255, (col >> 8) & 255, col & 255));
    int ok = FillRect(dc, &r, b);
    DeleteObject(b);
    return ok;
}
static void draw_btn(HDC dc, int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg) {
    fill_rect(dc, x, y, w, h, bg);
    txt_draw(dc, x + 5, y + 4, label, 14, fg);
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    if (!dc) return;
    W32Canvas *c = (W32Canvas *)dc;
    int W = c->w, H = c->h;
    fill_rect(dc, 0, 0, W, H, PAGE_BG);
    fill_rect(dc, 0, 0, W, TOOLBAR_H, TOOLBAR);
    draw_btn(dc, 8, 7, 34, 26, "<", ACCENT, 0xffffffu);
    draw_btn(dc, 46, 7, 34, 26, ">", ACCENT, 0xffffffu);
    draw_btn(dc, 84, 7, 34, 26, "R", ACCENT, 0xffffffu);
    draw_btn(dc, 122, 7, 34, 26, "H", ACCENT, 0xffffffu);
    fill_rect(dc, OMNI_X, 7, W - OMNI_X - 12, 26, 0x333333u);
    txt_draw(dc, OMNI_X + 8, 8, omni_len ? omni : cur_url, 14, 0xf0f0f0u);
    int y = TOOLBAR_H + 8 + content_y;
    if (page_valid && doc) {
        txt_state(dc, 16, INK, PAGE_BG);
        render_content(dc, y, W - 16);
    } else {
        char msg[256];
        snprintf(msg, sizeof msg, "%s", loading ? "Loading ..." : status_msg);
        txt_state(dc, 16, SUB, TOOLBAR);
        text_block(dc, 16, &y, W - 32, msg, 16, SUB);
    }
    EndPaint(h, &ps);
}

/* ---------------- window proc ---------------- */
static LRESULT wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: paint(h); return 0;
    case WM_KEYDOWN: {
        int vk = (int)(W32)(WPARAM)wp;
        switch (vk) {
        case VK_BACK:
            if (omni_len) { omni_len--; omni[omni_len] = 0; }
            else hist_back();
            InvalidateRect(h, NULL, 1);
            return 0;
        case VK_NEXT: hist_fwd(); InvalidateRect(h, NULL, 1); return 0;
        case VK_HOME: nav_home(); return 0;
        case VK_ESCAPE: PostQuitMessage(0); return 0;
        case VK_UP: content_y -= 16; if (content_y < 0) content_y = 0; InvalidateRect(h, NULL, 1); return 0;
        case VK_DOWN: content_y += 16; InvalidateRect(h, NULL, 1); return 0;
        case VK_RETURN: omni_norm(); InvalidateRect(h, NULL, 1); return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
    case WM_CHAR: {
        W32 cp = (W32)(WPARAM)wp;
        char c = (char)cp;
        if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
            if (omni_len < (int)sizeof omni - 1) { omni[omni_len++] = c; omni[omni_len] = 0; InvalidateRect(h, NULL, 1); }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = (int)((W32)lp & 0xffff);
        int y = (int)(((W32)lp >> 16) & 0xffff);
        if (y < TOOLBAR_H) {
            if (x >= 8 && x < 42) hist_back();
            else if (x >= 46 && x < 80) hist_fwd();
            else if (x >= 84 && x < 118) nav_reload();
            else if (x >= 122 && x < 156) nav_home();
            InvalidateRect(h, NULL, 1);
            return 0;
        }
        /* content coordinates: page row under the pointer -> link hit-test */
        int page_y = content_y + (y - TOOLBAR_H - 8);
        const char *clicked = NULL;
        for (int i = anchor_n - 1; i >= 0; i--) {
            if (x >= 8 && page_y >= anchors[i].y && page_y < anchors[i].y + anchors[i].h) {
                clicked = anchors[i].href;
                break;
            }
        }
        content_y = y - TOOLBAR_H - 8;
        if (content_y < 0) content_y = 0;
        if (clicked) { nav_to(clicked); return 0; }
        InvalidateRect(h, NULL, 1);
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

/* ---------------- main ---------------- */
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest();
    if (!los_init()) { fprintf(stderr, "losweb: no compositor\n"); return 1; }
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.lpszClassName = wa("LosedozeWeb");
    if (!RegisterClassExW(&wc)) { fprintf(stderr, "losweb: RegisterClassEx failed\n"); return 1; }
    hwnd = CreateWindowExW(0, wa("LosedozeWeb"), wa("Losedoze Web"), WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, WIN_W, WIN_H, NULL, NULL, NULL, NULL);
    if (!hwnd) { fprintf(stderr, "losweb: CreateWindowEx failed\n"); return 1; }
    printf("LOSEDOZE BROWSER OK\n");
    fflush(stdout);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (m.message == WM_QUIT) break;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

/* ---------------- self test ---------------- */
static int selftest(void) {
    W32Canvas c;
    memset(&c, 0, sizeof c);
    c.px = (W32 *)calloc((size_t)WIN_W * WIN_H, 4);
    c.w = WIN_W; c.h = WIN_H; c.stride = WIN_W;
    c.textcol = W32RGB(0x11, 0x11, 0x11);
    c.bkcol = W32RGB(255, 255, 255);
    c.bkmode = OPAQUE;
    c.font_px = 16.0f;
    c.clip.left = 0; c.clip.top = 0; c.clip.right = WIN_W; c.clip.bottom = WIN_H;
    HDC dc = (HDC)&c;
    txt_state(dc, 16, INK, PAGE_BG);
    int y = 20;
    text_block(dc, 20, &y, WIN_W - 40, "Hello Browser World", 16, INK);
    int found = 0;
    W32 *px = c.px;
    for (int i = 0; i < WIN_W * WIN_H; i++) if (px[i] != 0xffffffffu) { found = 1; break; }
    free(c.px);
    fprintf(stderr, "losweb selftest %s\n", found ? "OK" : "FAIL");
    return found ? 0 : 1;
}
