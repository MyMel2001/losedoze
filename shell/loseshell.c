/* loseshell - desktop shell + window manager for Losedoze.
 *
 * Original code, original iconography; look inspired by, legally distinct
 * from, commercial desktop environments. NO X11: raw DRM/KMS scanout (kernel
 * UAPI ioctls), evdev input, hand-rolled Wayland server for the Win32 subsystem
 * (w32host / win/wlclient.c).
 *
 * Layout of this file:
 *   1. globals/theme/config     2. canvas + text + cursor sprite
 *   3. shell-window mgmt        4. app index (.desktop) + launcher
 *   5. wallpaper                6. window management (Wayland toplevels)
 *   7. panels draw + click      8. keys, toasts, composite, main loop
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <ctype.h>
#include <stdint.h>
#include <poll.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/types.h>

#include "lost.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define SHELL_VER "1.0"
#define TB_H 48                 /* taskbar height */
#define NDESK 4
#define FB 1                    /* frame side border */
#define TITLE_H 32              /* titlebar height */
#define POP_R 8                 /* popup corner radius */
#define FRAME_R 8               /* window frame corner radius */

#define START_W 600
#define START_H 640
#define QUICK_W 360
#define QUICK_H 430
#define CAL_W 340
#define CAL_H 470
#define RUN_W 480
#define RUN_H 130
#define NOTIFY_FIFO "/run/losedoze/notify"
#define LOGPATH "/var/log/losedoze.log"

static struct card *card;
static int sw, sh;               /* framebuffer size */
static int use_gpu;              /* EGL/GBM accelerated present path */
static struct wlserver *wl;      /* hand-rolled Wayland server */
static Canvas wall;              /* wallpaper layer (full screen) */
static int mouse_x, mouse_y;     /* evdev cursor position, screen px */

/* ---------------- logging ---------------- */
static FILE *lg;
static void logmsg(const char *fmt, ...) {
    va_list ap;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; localtime_r(&ts.tv_sec, &tm);
    char head[32]; strftime(head, sizeof head, "%H:%M:%S", &tm);
    fprintf(lg ? lg : stderr, "[%s] ", head);
    va_start(ap, fmt); vfprintf(lg ? lg : stderr, fmt, ap); va_end(ap);
    fputc('\n', lg ? lg : stderr);
    if (lg) fflush(lg);
}

/* ---------------- theme ---------------- */
typedef struct {
    unsigned taskbar, panel, panel2, field;
    unsigned text, text2, accent, accent_text, border;
    unsigned hover, pressed, danger, ok, warn;
} Theme;
static Theme th;
static int dark_mode = 1;

static void set_theme(void) {
    if (dark_mode) {
        th = (Theme){ 0x1f1f1f, 0x272727, 0x202020, 0x333333,
                      0xffffff, 0x9a9a9a, 0x2f6fed, 0xffffff, 0x3d3d3d,
                      0x3a3a3a, 0x464646, 0xe81123, 0x2cc758, 0xf7b500 };
    } else {
        th = (Theme){ 0xf3f3f3, 0xfbfbfb, 0xf0f0f0, 0xffffff,
                      0x1a1a1a, 0x666666, 0x2f6fed, 0xffffff, 0xd8d8d8,
                      0xe9e9e9, 0xdfdfdf, 0xe81123, 0x1f8f3a, 0xb8860b };
    }
}

/* ---------------- pixel + canvas ---------------- */
/* XRGB8888 scanout: R at bit16, G at bit8, B at bit0 (8 bits each) */
static int g_red_sh = 16, g_red_po = 8, g_grn_sh = 8, g_grn_po = 8, g_blu_sh = 0, g_blu_po = 8;

static void init_masks(void) { /* format fixed by KMS XRGB8888 — nothing to probe */ }
static unsigned long px(unsigned rgb) {
    unsigned r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    /* alpha forced opaque: compositor treats top-byte-0 as transparent key */
    return 0xff000000ul | ((unsigned long)r << g_red_sh) | ((unsigned long)g << g_grn_sh) | ((unsigned long)b << g_blu_sh);
}
static unsigned mix(unsigned a, unsigned b, int t /*0..255*/) {
    int r = ((int)((a >> 16) & 255) * (255 - t) + (int)((b >> 16) & 255) * t) / 255;
    int g = ((int)((a >> 8) & 255) * (255 - t) + (int)((b >> 8) & 255) * t) / 255;
    int c = ((int)(a & 255) * (255 - t) + (int)(b & 255) * t) / 255;
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)c;
}

/* Canvas comes from lost.h: { u32 *px; int w, h, stride; } — same layout the
 * KMS scanout buffer uses, so shadow canvases may alias dumb buffers. */

static void cv_init(Canvas *c, int w, int h) {
    c->w = w; c->h = h; c->stride = w;
    c->px = calloc((size_t)w * h, 4);   /* ARGB8888, transparent-zero */
    if (!c->px) { fprintf(stderr, "canvas alloc failed\n"); exit(1); }
}
static void cv_free(Canvas *c) {
    free(c->px); c->px = NULL;
}
static inline void cv_px(Canvas *c, int x, int y, unsigned rgb) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    c->px[y * c->stride + x] = (u32)px(rgb);
}
static void cv_fill(Canvas *c, int x, int y, int w, int h, unsigned rgb) {
    u32 p = (u32)px(rgb);
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= c->h) continue;
        u32 *row = c->px + (size_t)j * c->stride;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= c->w) continue;
            row[i] = p;
        }
    }
}
static void cv_grad(Canvas *c, int x, int y, int w, int h, unsigned c1, unsigned c2) {
    for (int j = y; j < y + h; j++) {
        int t = h > 1 ? (j - y) * 255 / (h - 1) : 0;
        unsigned col = mix(c1, c2, t);
        u32 p = (u32)px(col);
        if (j < 0 || j >= c->h) continue;
        u32 *row = c->px + (size_t)j * c->stride;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= c->w) continue;
            row[i] = p;
        }
    }
}
static int in_round(int px_, int py_, int x, int y, int w, int h, int r) {
    if (px_ < x || py_ < y || px_ >= x + w || py_ >= y + h) return 0;
    if (r <= 0) return 1;
    int dx = 0, dy = 0;
    if (px_ < x + r) dx = x + r - px_ - 1;
    else if (px_ >= x + w - r) dx = px_ - (x + w - r);
    if (py_ < y + r) dy = y + r - py_ - 1;
    else if (py_ >= y + h - r) dy = py_ - (y + h - r);
    if (dx && dy) return dx * dx + dy * dy <= r * r;
    return 1;
}
static void cv_round(Canvas *c, int x, int y, int w, int h, int r, unsigned rgb) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (in_round(i, j, x, y, w, h, r)) cv_px(c, i, j, rgb);
}
static void cv_round_border(Canvas *c, int x, int y, int w, int h, int r, int t, unsigned rgb) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) {
            if (!in_round(i, j, x, y, w, h, r)) continue;
            if (in_round(i, j, x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0)) continue;
            cv_px(c, i, j, rgb);
        }
}
static void cv_hline(Canvas *c, int x, int y, int w, unsigned rgb) { cv_fill(c, x, y, w, 1, rgb); }

/* geometric icon strokes: little filled discs along a segment */
static void disc(Canvas *c, int cx, int cy, int r, unsigned rgb) {
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++)
        if (i * i + j * j <= r * r) cv_px(c, cx + i, cy + j, rgb);
}
static void stroke(Canvas *c, int x1, int y1, int x2, int y2, int t, unsigned rgb) {
    int dx = abs(x2 - x1), dy = abs(y2 - y1);
    int n = dx > dy ? dx : dy; if (n < 1) n = 1;
    for (int i = 0; i <= n; i++)
        disc(c, x1 + (x2 - x1) * i / n, y1 + (y2 - y1) * i / n, t > 1 ? t / 2 : 0, rgb);
}
static void circle_out(Canvas *c, int cx, int cy, int r, int t, unsigned rgb) {
    for (int a = 0; a < 360; a += 2) {
        double rad = a * 3.14159265 / 180.0;
        int x = cx + (int)(r * cos(rad)), y = cy + (int)(r * sin(rad));
        disc(c, x, y, t > 1 ? t / 2 : 0, rgb);
    }
}
static void circle_arc(Canvas *c, int cx, int cy, int r, int a0, int a1, int t, unsigned rgb) {
    for (int a = a0; a <= a1; a += 2) {
        double rad = a * 3.14159265 / 180.0;
        disc(c, cx + (int)(r * cos(rad)), cy + (int)(r * sin(rad)), t > 1 ? t / 2 : 0, rgb);
    }
}

/* ---------------- text (stb_truetype) ---------------- */
static stbtt_fontinfo f_reg, f_bold;
static int font_ok;

typedef struct { int used, w, h, xoff, yoff; unsigned char *bm; } Glyph;
#define GLYPH_MAX 4096
static Glyph glyphs[GLYPH_MAX];

static unsigned glyph_key(int bold, int size, int cp) {
    return ((unsigned)bold << 30) | ((unsigned)(size & 63) << 24) | ((unsigned)cp & 0xfffff);
}
static int load_fonts(void) {
    const char *paths[] = {
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf", NULL
    };
    FILE *f = NULL;
    for (int i = 0; i < 2; i++)
        if ((f = fopen(paths[i], "rb"))) break;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { }
    fclose(f);
    if (!stbtt_InitFont(&f_reg, buf, stbtt_GetFontOffsetForIndex(buf, 0))) return 0;
    f_bold = f_reg;
    for (int i = 2; i < 4; i++)
        if ((f = fopen(paths[i], "rb"))) break;
    if (f) {
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *b2 = malloc(n);
        if (fread(b2, 1, (size_t)n, f) != (size_t)n) { }
        fclose(f);
        if (!stbtt_InitFont(&f_bold, b2, stbtt_GetFontOffsetForIndex(b2, 0))) f_bold = f_reg;
    }
    font_ok = 1;
    return 1;
}
static int glyph_advance(int bold, int size, int cp) {
    stbtt_fontinfo *fi = bold ? &f_bold : &f_reg;
    float scale = stbtt_ScaleForPixelHeight(fi, (float)size);
    int adv, lsb; stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
    return (int)(adv * scale + 0.5f);
}
static int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int cp;
    if (p[0] < 0x80) cp = p[0];
    else if ((p[0] & 0xe0) == 0xc0) cp = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
    else if ((p[0] & 0xf0) == 0xe0) cp = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
    else { *s += 1; return 0xfffd; }
    *s += (p[0] < 0x80) ? 1 : (p[0] < 0xe0 ? 2 : 3);
    return cp;
}
static int g_widths[GLYPH_MAX];

static int text_w(const char *s, int size, int bold) {
    if (!font_ok) return (int)strlen(s) * size * 6 / 10;
    int w = 0;
    while (*s) { int cp = utf8_next(&s); if (cp == ' ') { w += size / 3; continue; } w += glyph_advance(bold, size, cp); }
    return w;
}
static void text_draw(Canvas *c, int x, int y, const char *s, int size, int bold, unsigned rgb) {
    if (!font_ok) { /* fallback: raw pixels not drawn; keep text visible via nothing */ return; }
    stbtt_fontinfo *fi = bold ? &f_bold : &f_reg;
    float scale = stbtt_ScaleForPixelHeight(fi, (float)size);
    int a, d, g; stbtt_GetFontVMetrics(fi, &a, &d, &g);
    int base = y + (int)(a * scale);
    int pen = x;
    int rr = (rgb >> 16) & 255, rg = (rgb >> 8) & 255, rb = rgb & 255;
    while (*s) {
        int cp = utf8_next(&s);
        if (cp == ' ') { pen += size / 3; continue; }
        unsigned key = glyph_key(bold, size, cp);
        int slot = key % GLYPH_MAX; Glyph *gl = NULL; int slotfound = -1;
        for (int probe = 0; probe < 64; probe++) {
            int idx = (slot + probe) % GLYPH_MAX;
            if (glyphs[idx].used && glyphs[idx].w == (int)key) { gl = &glyphs[idx]; slotfound = idx; break; }
            if (!glyphs[idx].used) {
                int adv, lsb; stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
                int xoff, yoff, w, h;
                unsigned char *bm = stbtt_GetCodepointBitmap(fi, 0, scale, cp, &w, &h, &xoff, &yoff);
                glyphs[idx].used = 1; glyphs[idx].w = (int)key; glyphs[idx].bm = bm;
                glyphs[idx].xoff = xoff; glyphs[idx].yoff = yoff;
                g_widths[idx] = w;
                gl = &glyphs[idx]; slotfound = idx;
                break;
            }
        }
        if (!gl) break;
        int gw = g_widths[slotfound];
        for (int j = 0; j < gl->h; j++)
            for (int i = 0; i < gw; i++) {
                int al = gl->bm[j * gw + i];
                if (!al) continue;
                int X = pen + gl->xoff + i, Y = base + gl->yoff + j;
                if (X < 0 || Y < 0 || X >= c->w || Y >= c->h) continue;
                unsigned cur = c->px[(size_t)Y * c->stride + X];
                unsigned long pm = px(rgb);
                /* blend manually in rgb space using stored channel positions */
                (void)cur; (void)pm;
                int cr = (cur >> g_red_sh) & ((1u << g_red_po) - 1);
                int cg = (cur >> g_grn_sh) & ((1u << g_grn_po) - 1);
                int cb = (cur >> g_blu_sh) & ((1u << g_blu_po) - 1);
                cr = (cr * (255 - al) + (rr >> (8 - g_red_po)) * al) / 255;
                cg = (cg * (255 - al) + (rg >> (8 - g_grn_po)) * al) / 255;
                cb = (cb * (255 - al) + (rb >> (8 - g_blu_po)) * al) / 255;
                unsigned long np = ((unsigned long)cr << g_red_sh) | ((unsigned long)cg << g_grn_sh) | ((unsigned long)cb << g_blu_sh) | 0xff000000ul;
                c->px[(size_t)Y * c->stride + X] = (u32)np;
            }
        pen += glyph_advance(bold, size, cp);
    }
}

/* ---------------- misc util ---------------- */
static unsigned long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void strlcpy_(char *d, const char *s, size_t n) {
    if (!n) return; strncpy(d, s, n - 1); d[n - 1] = 0;
}
static int in_r(int px_, int py_, int x, int y, int w, int h) {
    return px_ >= x && py_ >= y && px_ < x + w && py_ < y + h;
}

/* ---------------- cursors (embedded XBM; stamped into the framebuffer) ---------------- */
enum { CUR_ARROW = 0, CUR_HAND, CUR_H, CUR_V };
static int cur_shape = CUR_ARROW;
static int cur_x = -1, cur_y = -1;
static int cur_shown;
static unsigned char cur_save[16 * 16 * 4];
static int cur_sx, cur_sy;   /* where the saved/stamped rect lives */

static const char arrow_b[] = {0x80,0x00,0xC0,0x00,0xE0,0x00,0xF0,0x00,0xF8,0x00,0xFC,0x00,
    0xFE,0x00,0xFF,0x00,0xF8,0x00,0xD8,0x00,0x8C,0x00,0x0C,0x00,0x06,0x00,0x06,0x00,0x03,0x00,0x03,0x00};
static const char arrow_m[] = {0x80,0x00,0xC0,0x00,0xE0,0x00,0xF0,0x00,0xF8,0x00,0xFC,0x00,
    0xFE,0x00,0xFF,0x80,0xF8,0x00,0xD8,0x00,0x8C,0x00,0x0C,0x00,0x06,0x00,0x06,0x00,0x03,0x00,0x03,0x00};
static const char hand_b[] = {0x00,0x00,0x18,0x00,0x18,0x00,0x18,0x00,0x18,0x00,0x18,0x00,
    0x18,0x00,0x1F,0x80,0xFF,0xC0,0xFF,0xE0,0x7F,0xC0,0x3F,0x80,0x1F,0x00,0x0E,0x00,0x00,0x00,0x00,0x00};
static const char hand_m[] = {0x18,0x00,0x18,0x00,0x18,0x00,0x18,0x00,0x18,0x00,0x18,0x00,
    0x1F,0x80,0xFF,0xC0,0xFF,0xE0,0xFF,0xC0,0x7F,0x80,0x3F,0x00,0x1E,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const char h_b[] = {0x00,0x00,0x10,0x00,0x38,0x00,0x7C,0x00,0xFE,0x00,0xFF,0x80,0xFF,0xC0,0xFF,0xE0,
    0xFF,0xC0,0xFF,0x80,0xFE,0x00,0x7C,0x00,0x38,0x00,0x10,0x00,0x00,0x00,0x00,0x00};
static const char v_b[] = {0x00,0x00,0x18,0x00,0x1C,0x00,0x1E,0x00,0x1F,0x00,0xFF,0x80,0xFF,0xC0,0xFF,0xE0,
    0xFF,0xC0,0xFF,0x80,0x1F,0x00,0x1E,0x00,0x1C,0x00,0x18,0x00,0x00,0x00};
static const char h_m[32] = {0xFF,0xFF,0xF8,0xFF,0xFC,0xFF,0xFE,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,0xFF,0xFC,0xFF,0xF8,0xFF,0xFF,0xFF};
static const char v_m[32] = {0xFF,0x00,0xFF,0x80,0xFF,0xC0,0xFF,0xE0,0xFF,0xF0,0xFF,0xF8,0xFF,0xFC,0xFF,0xFE,
    0xFE,0xFF,0xFC,0xFF,0xF8,0xFF,0xF0,0xFF,0xE0,0xFF,0xC0,0xFF,0x80,0xFF,0x00,0xFF};

static const char *cur_b_bits[4], *cur_m_bits[4];

static void make_cursors(void) {
    cur_b_bits[CUR_ARROW] = arrow_b; cur_m_bits[CUR_ARROW] = arrow_m;
    cur_b_bits[CUR_HAND] = hand_b;   cur_m_bits[CUR_HAND] = hand_m;
    cur_b_bits[CUR_H] = h_b;         cur_m_bits[CUR_H] = h_m;
    cur_b_bits[CUR_V] = v_b;         cur_m_bits[CUR_V] = v_m;
}

/* k93-style: save the rect under the cursor, stamp, restore, restamp.
 * Pure framebuffer ops — no server involvement. */
static void cursor_erase(void) {
    Canvas *sc = card_shadow(card);
    if (!cur_shown || cur_sx < 0) return;
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 16; i++) {
            int x = cur_sx + i, y = cur_sy + j;
            if ((unsigned)x >= (unsigned)sc->w || (unsigned)y >= (unsigned)sc->h) continue;
            sc->px[y * sc->stride + x] = *(uint32_t *)(cur_save + (j * 16 + i) * 4);
        }
}
static void cursor_stamp(void) {
    Canvas *sc = card_shadow(card);
    if (cur_x < 0) return;
    cur_sx = cur_x; cur_sy = cur_y;
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 16; i++) {
            int x = cur_sx + i, y = cur_sy + j;
            if ((unsigned)x >= (unsigned)sc->w || (unsigned)y >= (unsigned)sc->h) continue;
            *(uint32_t *)(cur_save + (j * 16 + i) * 4) = sc->px[y * sc->stride + x];
            int byte = j * 2 + (i >> 3), bit = 7 - (i & 7);
            int vis = (cur_m_bits[cur_shape][byte] >> bit) & 1;
            if (!vis) continue;
            int fg = (cur_b_bits[cur_shape][byte] >> bit) & 1;
            sc->px[y * sc->stride + x] = fg ? 0xff000000u : 0xffffffffu;
        }
    cur_shown = 1;
}
static void cursor_set_shape(int sh_) { if (sh_ != cur_shape) { cursor_erase(); cur_shape = sh_; cur_shown = 0; } }
static void cursor_set_pos(int x, int y) {
    if (x == cur_x && y == cur_y) return;
    cursor_erase();
    cur_x = x; cur_y = y;
    cur_shown = 0;
}

/* ---------------- shell surfaces ---------------- */
enum { SW_TASKBAR = 1, SW_START, SW_QUICK, SW_CAL, SW_ALT, SW_TASKVIEW,
       SW_RUN, SW_LOCK, SW_MENU, SW_TOAST, SW_SNAP, SW_DESKICON, SW_FILES };

typedef struct ShellWin ShellWin;
struct ShellWin {
    int type; int idx;                 /* idx: deskicon slot / menu id etc */
    void *win; int x, y, w, h;         /* win: unused token (was X window) */
    Canvas cv; int visible;
    ShellWin *next;
};
static ShellWin *swins;
static ShellWin *popup;                /* open transient surface */
static int popup_kbd;                  /* popup holds keyboard focus */
static int needs_composite;            /* screen must be rebuilt + presented */

static void draw_taskbar(ShellWin *), draw_start(ShellWin *), draw_quick(ShellWin *),
             draw_cal(ShellWin *), draw_alt(ShellWin *), draw_taskview(ShellWin *),
             draw_run(ShellWin *), draw_lock(ShellWin *), draw_menu(ShellWin *),
             draw_toast(ShellWin *), draw_snap(ShellWin *), draw_deskicon(ShellWin *),
             draw_files(ShellWin *);
static int click_taskbar(ShellWin *, int, int, int), click_start(ShellWin *, int, int, int),
           click_quick(ShellWin *, int, int, int), click_cal(ShellWin *, int, int, int),
           click_alt(ShellWin *, int, int, int), click_taskview(ShellWin *, int, int, int),
           click_run(ShellWin *, int, int, int), click_lock(ShellWin *, int, int, int),
           click_menu(ShellWin *, int, int, int), click_toast(ShellWin *, int, int, int),
           click_snap(ShellWin *, int, int, int), click_deskicon(ShellWin *, int, int, int),
           click_files(ShellWin *, int, int, int);

static void sw_draw(ShellWin *s) {
    switch (s->type) {
    case SW_TASKBAR: draw_taskbar(s); break;
    case SW_START: draw_start(s); break;
    case SW_QUICK: draw_quick(s); break;
    case SW_CAL: draw_cal(s); break;
    case SW_ALT: draw_alt(s); break;
    case SW_TASKVIEW: draw_taskview(s); break;
    case SW_RUN: draw_run(s); break;
    case SW_LOCK: draw_lock(s); break;
    case SW_MENU: draw_menu(s); break;
    case SW_TOAST: draw_toast(s); break;
    case SW_SNAP: draw_snap(s); break;
    case SW_DESKICON: draw_deskicon(s); break;
    case SW_FILES: draw_files(s); break;
    }
    needs_composite = 1;
}
static int sw_click(ShellWin *s, int btn, int x, int y) {
    switch (s->type) {
    case SW_TASKBAR: return click_taskbar(s, btn, x, y);
    case SW_START: return click_start(s, btn, x, y);
    case SW_QUICK: return click_quick(s, btn, x, y);
    case SW_CAL: return click_cal(s, btn, x, y);
    case SW_ALT: return click_alt(s, btn, x, y);
    case SW_TASKVIEW: return click_taskview(s, btn, x, y);
    case SW_RUN: return click_run(s, btn, x, y);
    case SW_LOCK: return click_lock(s, btn, x, y);
    case SW_MENU: return click_menu(s, btn, x, y);
    case SW_TOAST: return click_toast(s, btn, x, y);
    case SW_SNAP: return click_snap(s, btn, x, y);
    case SW_DESKICON: return click_deskicon(s, btn, x, y);
    case SW_FILES: return click_files(s, btn, x, y);
    }
    return 0;
}
static void sw_apply_shape(ShellWin *s, int r) {
    /* No X shape extension: rounded corners are alpha-blended at composite time.
     * r is recorded implicitly by the draw callbacks (in_round). */
    (void)s; (void)r;
}
static ShellWin *sw_get(int type, int idx) {
    for (ShellWin *s = swins; s; s = s->next)
        if (s->type == type && s->idx == idx) return s;
    ShellWin *s = calloc(1, sizeof *s);
    s->type = type; s->idx = idx;
    s->w = 1; s->h = 1;
    cv_init(&s->cv, 1, 1);
    s->next = swins; swins = s;
    return s;
}
static void sw_resize(ShellWin *s, int w, int h) {
    if (s->w == w && s->h == h) return;
    cv_free(&s->cv);
    s->w = w; s->h = h;
    cv_init(&s->cv, w, h);
    if (s->type != SW_TASKBAR && s->type != SW_DESKICON) sw_apply_shape(s, POP_R);
    if (s->visible) sw_draw(s);
}
static void popup_close(void);
static void snap_dismiss(void);
static int snap_open;   /* snap layout picker visible */
static void key_run_open(void);
static void taskbar_dirty(void);
static void desk_switch(int d);
static int frame_hover_btn;   /* tentative; set in part4 */
static int qdrag;             /* popup_close clears slider drag */
static void key_start_toggle(void);
static void key_start_open_search(void);
static void key_quick_toggle(void);
static void key_cal_toggle(void);
static void key_alt_open(int dir);
static void key_taskview_open(void);
static void key_lock(void);
static void menu_open_power(int x, int y);
static void files_open(const char *path);
static void redraw_frames(void);
static void toast_show(const char *fmt, ...);
static void sw_show(ShellWin *s, int x, int y) {
    s->x = x; s->y = y;
    s->visible = 1;
    sw_draw(s);
    int is_popup = s->type != SW_TASKBAR && s->type != SW_DESKICON &&
                   s->type != SW_SNAP && s->type != SW_TOAST;
    if (is_popup) {
        if (popup && popup != s) popup_close();
        popup = s;                     /* input router: outside click closes */
        if (s->type == SW_START || s->type == SW_RUN || s->type == SW_ALT || s->type == SW_LOCK)
            popup_kbd = 1;             /* keys routed to popup handler */
    }
    needs_composite = 1;
}
static void sw_hide(ShellWin *s) {
    if (!s->visible) return;
    s->visible = 0;
    needs_composite = 1;
}
static void popup_close(void) {
    if (!popup) return;
    ShellWin *s = popup;
    popup = NULL;
    qdrag = 0;
    sw_hide(s);
    if (popup_kbd) popup_kbd = 0;
    if (s->type == SW_SNAP) snap_open = 0;
    snap_dismiss();
}
static void redraw_all(void) {
    for (ShellWin *s = swins; s; s = s->next)
        if (s->visible) sw_draw(s);
}
/* ---------------- config ---------------- */
static char cfg_path[512];
static int cfg_vol = 60, cfg_bright = 80;
static int cfg_wifi = 1, cfg_bt = 0, cfg_air = 0, cfg_night = 0;
static int cfg_wallpaper = 0;
#define MAX_PINNED 8
static char cfg_pinned[MAX_PINNED][128];
static int cfg_npinned;

static void cfg_defaults(void) {
    const char *d[] = { "Files", "Notepad", NULL };
    cfg_npinned = 0;
    for (int i = 0; d[i] && cfg_npinned < MAX_PINNED; i++)
        strlcpy_(cfg_pinned[cfg_npinned++], d[i], 128);
}
static void cfg_load(void) {
    const char *home = getenv("HOME") ? getenv("HOME") : "/root";
    snprintf(cfg_path, sizeof cfg_path, "%s/.config/losedoze.conf", home);
    cfg_defaults();
    FILE *f = fopen(cfg_path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; const char *k = line, *v = eq + 1;
        if (!strcmp(k, "dark")) dark_mode = atoi(v);
        else if (!strcmp(k, "vol")) cfg_vol = atoi(v);
        else if (!strcmp(k, "bright")) cfg_bright = atoi(v);
        else if (!strcmp(k, "wifi")) cfg_wifi = atoi(v);
        else if (!strcmp(k, "bt")) cfg_bt = atoi(v);
        else if (!strcmp(k, "airplane")) cfg_air = atoi(v);
        else if (!strcmp(k, "nightlight")) cfg_night = atoi(v);
        else if (!strcmp(k, "wallpaper")) cfg_wallpaper = atoi(v);
        else if (!strcmp(k, "pinned")) {
            cfg_npinned = 0;
            char *tok = strtok((char *)v, ";");
            while (tok && cfg_npinned < MAX_PINNED) { strlcpy_(cfg_pinned[cfg_npinned++], tok, 128); tok = strtok(NULL, ";"); }
        }
    }
    fclose(f);
}
static void cfg_save(void) {
    FILE *f = fopen(cfg_path, "w");
    if (!f) return;
    fprintf(f, "dark=%d\nvol=%d\nbright=%d\nwifi=%d\nbt=%d\nairplane=%d\nnightlight=%d\nwallpaper=%d\n",
            dark_mode, cfg_vol, cfg_bright, cfg_wifi, cfg_bt, cfg_air, cfg_night, cfg_wallpaper);
    fprintf(f, "pinned=");
    for (int i = 0; i < cfg_npinned; i++) fprintf(f, "%s%s", cfg_pinned[i], i + 1 < cfg_npinned ? ";" : "");
    fprintf(f, "\n");
    fclose(f);
}

/* ---------------- app index ---------------- */
typedef struct App App;
struct App { char name[128], exec[512]; App *next; };
static App *apps;
static void app_add(const char *name, const char *exec) {
    App *a = malloc(sizeof *a);
    strlcpy_(a->name, name, sizeof a->name);
    strlcpy_(a->exec, exec, sizeof a->exec);
    a->next = apps; apps = a;
}
static void app_scan_dir(const char *dir) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 9 || strcmp(e->d_name + l - 8, ".desktop")) continue;
        char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "r"); if (!f) continue;
        char line[1024], nm[128] = "", ex[512] = "";
        int skip = 0, in_app = 0;
        while (fgets(line, sizeof line, f)) {
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            if (line[0] == '[') { in_app = !strcmp(line, "[Desktop Entry]"); continue; }
            if (!in_app) continue;
            if (!strncmp(line, "Name=", 5) && !nm[0]) strlcpy_(nm, line + 5, sizeof nm);
            else if (!strncmp(line, "Exec=", 5) && !ex[0]) strlcpy_(ex, line + 5, sizeof ex);
            else if (!strncmp(line, "NoDisplay=true", 14) || !strncmp(line, "Hidden=true", 11)) skip = 1;
        }
        fclose(f);
        if (!skip && nm[0] && ex[0]) app_add(nm, ex);
    }
    closedir(d);
}
static void apps_load(void) {
    app_scan_dir("/usr/share/applications");
    const char *home = getenv("HOME") ? getenv("HOME") : "/root";
    char p[512]; snprintf(p, sizeof p, "%s/.local/share/applications", home);
    app_scan_dir(p);
    if (!apps) { /* builtin fallbacks so the menu is never empty */
        app_add("Notepad", "w32host /opt/losedoze/apps/notes.exe");
        app_add("Hello", "w32host /opt/losedoze/apps/hello.exe");
    }
    /* builtins are always available even if .desktop files exist */
    app_add("Files", "__files__");
    app_add("Run dialog...", "__run__");
}
static App *app_find(const char *name) {
    for (App *a = apps; a; a = a->next)
        if (!strcasecmp(a->name, name)) return a;
    return NULL;
}

/* ---------------- launcher ---------------- */
static void launch(const char *cmd) {
    if (!strcmp(cmd, "__run__")) { key_run_open(); return; }
    if (!strcmp(cmd, "__files__")) { files_open("/"); return; }
    logmsg("launch: %s", cmd);
    pid_t p = fork();
    if (p == 0) {
        setsid();
        int fd = open(LOGPATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}
static void files_browse(const char *unixpath) {
    if (unixpath) files_open(unixpath);
    else files_open("/");
}

/* ---------------- wallpaper ---------------- */
static const unsigned wall_presets[3][2] = {
    { 0x0b2545, 0x1f6f8b }, { 0x2b1d4f, 0x8a3b6b }, { 0x143022, 0x4f8f6b }
};
static void wallpaper_set(void) {
    if (!wall.px) cv_init(&wall, sw, sh);
    unsigned a = wall_presets[cfg_wallpaper % 3][0], b = wall_presets[cfg_wallpaper % 3][1];
    cv_grad(&wall, 0, 0, sw, sh, a, b);
    /* soft light bands for depth (procedural, original artwork) */
    for (int k = 0; k < 4; k++) {
        int cx = sw / 4 * (k + 1), cy = sh / 5 * ((k % 3) + 1);
        for (int r = 380; r > 0; r -= 4) {
            int al = (380 - r) / 6;
            circle_arc(&wall, cx, cy, r, 0, 359, 2, mix(a, b, 40 + (k * 17 + al) % 60));
        }
    }
    needs_composite = 1;
}
static void wallpaper_next(void) { cfg_wallpaper = (cfg_wallpaper + 1) % 3; wallpaper_set(); cfg_save(); }

/* == PART3: WM (Wayland toplevels; no EWMH) == */

/* ---------------- clients ---------------- */
typedef struct Client Client;
struct Client {
    WlWin *wl;
    int x, y, w, h;          /* frame geometry (surface origin = x+FB, y+TITLE_H) */
    int fx, fy, fw, fh;      /* saved geometry before max/full */
    int max, full, min;
    int self_unmap;
    int mapped;
    int desktop;
    char title[256], cls[128];
    Canvas fc;               /* frame chrome (title + borders + margins) */
    Client *next;
};
static Client *clients;
static Client *focused;
static int cur_desk;

static void workarea(int *x, int *y, int *w, int *h) { *x = 0; *y = 0; *w = sw; *h = sh - TB_H; }

static Client *client_of_wl(WlWin *w) {
    for (Client *c = clients; c; c = c->next)
        if (c->wl == w) return c;
    return NULL;
}

static void get_title_class(Client *c) {
    strlcpy_(c->title, wlwin_title(c->wl) ? wlwin_title(c->wl) : "(no title)", sizeof c->title);
    if (!c->title[0]) strlcpy_(c->title, "(no title)", sizeof c->title);
    strlcpy_(c->cls, wlwin_appid(c->wl) ? wlwin_appid(c->wl) : "", sizeof c->cls);
}

/* ---------------- frame drawing (into c->fc; composited onto screen) ---- */
static void draw_frame_title(Client *c) {
    if (!c->fc.px || c->fc.w != c->w || c->fc.h != c->h) {
        cv_free(&c->fc);
        cv_init(&c->fc, c->w, c->h);
    }
    Canvas *cv = &c->fc;
    int focused_ = (focused == c);
    unsigned bg = focused_ ? mix(th.panel2, th.accent, 14) : th.panel2;
    /* margins + title bar */
    cv_fill(cv, 0, 0, c->w, c->h, bg);
    cv_fill(cv, 0, 0, c->w, 1, th.border);
    cv_fill(cv, 0, TITLE_H, c->w, 1, th.border);
    cv_fill(cv, 0, 0, FB, TITLE_H, th.border);
    cv_fill(cv, c->w - FB, 0, FB, TITLE_H, th.border);
    if (c->h > TITLE_H + 8) {
        cv_fill(cv, 0, TITLE_H + 1, FB, c->h - TITLE_H - 1, th.border);
        cv_fill(cv, c->w - FB, TITLE_H + 1, FB, c->h - TITLE_H - 1, th.border);
        cv_fill(cv, 0, c->h - FB, c->w, FB, th.border);
    }
    unsigned tcol = focused_ ? th.text : th.text2;
    text_draw(cv, 12, 9, c->title, 12, 0, tcol);
    if (c->w > 300) {
        int bw = 46;
        unsigned glyph = tcol;
        if (frame_hover_btn == 1 || frame_hover_btn == 2)
            cv_fill(cv, c->w - bw * frame_hover_btn, 0, bw, TITLE_H, mix(bg, th.text, 22));
        else if (frame_hover_btn == 3) {
            cv_fill(cv, c->w - bw, 0, bw, TITLE_H, th.danger);
            glyph = 0xffffff;
        }
        int minx = c->w - bw * 3, maxx = c->w - bw * 2, clx = c->w - bw;
        stroke(cv, minx + 17, 16, minx + 29, 16, 2, glyph);            /* minimize */
        cv_round_border(cv, maxx + 17, 11, 12, 11, 1, 1, glyph);        /* maximize */
        stroke(cv, clx + 17, 10, clx + 28, 21, 2, glyph);               /* close */
        stroke(cv, clx + 28, 10, clx + 17, 21, 2, glyph);
    }
    needs_composite = 1;
}

/* ---------------- geometry / state ops ---------------- */
static void apply_geom(Client *c, int x, int y, int w, int h) {
    if (w < 120) w = 120;
    if (h < TITLE_H + FB + 40) h = TITLE_H + FB + 40;
    c->x = x; c->y = y; c->w = w; c->h = h;
    cv_free(&c->fc);                       /* frame canvas re-sized on next draw */
    wlwin_configure(c->wl, w - 2 * FB, h - TITLE_H - FB);   /* content size */
}
static void set_max(Client *c, int on) {
    if (on == c->max) return;
    if (on) {
        c->fx = c->x; c->fy = c->y; c->fw = c->w; c->fh = c->h;
        int x, y, w, h; workarea(&x, &y, &w, &h);
        apply_geom(c, x, y, w, h);
    } else {
        apply_geom(c, c->fx, c->fy, c->fw, c->fh);
    }
    c->max = on;
    draw_frame_title(c);
}
enum { SNAP_L, SNAP_R, SNAP_T, SNAP_MAX, SNAP_TL, SNAP_TR, SNAP_BL, SNAP_BR };
static void snap_apply(Client *c, int zone) {
    int x, y, w, h; workarea(&x, &y, &w, &h);
    int hw = w / 2, hh = h / 2;
    if (!c->max) { c->fx = c->x; c->fy = c->y; c->fw = c->w; c->fh = c->h; }
    c->max = 0;
    switch (zone) {
    case SNAP_L: apply_geom(c, x, y, hw, h); break;
    case SNAP_R: apply_geom(c, x + hw, y, w - hw, h); break;
    case SNAP_T: apply_geom(c, x, y, w, h); set_max(c, 1); return;
    case SNAP_MAX: set_max(c, 1); return;
    case SNAP_TL: apply_geom(c, x, y, hw, hh); break;
    case SNAP_TR: apply_geom(c, x + hw, y, w - hw, hh); break;
    case SNAP_BL: apply_geom(c, x, y + hh, hw, h - hh); break;
    case SNAP_BR: apply_geom(c, x + hw, y + hh, w - hw, h - hh); break;
    }
    draw_frame_title(c);
}
static void close_client(Client *c) {
    wlwin_close(c->wl);                    /* xdg_toplevel.close */
}
static void iconify(Client *c) {
    c->min = 1;
    c->self_unmap++;
    wlwin_set_activated(c->wl, 0);
    if (focused == c) { focused = NULL; }
    taskbar_dirty();
    needs_composite = 1;
}

/* ---------------- focus / lists ---------------- */
static void focus_client(Client *c) {
    if (!c) return;
    if (c->min) {
        c->min = 0;
        c->mapped = 1;
    }
    if (c->desktop != cur_desk) desk_switch(c->desktop);
    Client *old = focused;
    focused = c;
    /* restack: move c to head of wl stack */
    wlwin_raise(c->wl);
    for (Client *cc = clients; cc; cc = cc->next)
        wlwin_set_activated(cc->wl, cc == c);
    if (old && old != c) draw_frame_title(old);
    draw_frame_title(c);
    taskbar_dirty();
    needs_composite = 1;
}

/* ---------------- manage / unmanage ---------------- */
static int cascade_n;
static void manage(WlWin *w) {
    if (client_of_wl(w)) return;
    if (w->popup) { needs_composite = 1; return; }  /* xdg_popup: no frame, 3b pass */
    Client *c = calloc(1, sizeof *c);
    c->wl = w;
    c->desktop = cur_desk;
    int bw, bh;
    wlwin_size(w, &bw, &bh);
    if (bw <= 0 || bh <= 0) { bw = 640; bh = 400; }
    c->w = bw + 2 * FB; c->h = bh + TITLE_H + FB;
    c->x = 40 + (cascade_n % 7) * 28;
    c->y = 30 + (cascade_n % 7) * 26;
    cascade_n++;
    if (c->w < 320) c->w = 420;
    if (c->h < 200) c->h = 300;
    get_title_class(c);
    c->mapped = 1;
    c->next = clients; clients = c;
    wlwin_set_pos(w, c->x + FB, c->y + TITLE_H);
    focus_client(c);
    logmsg("manage: '%s' %dx%d+%d+%d desk %d", c->title, c->w, c->h, c->x, c->y, c->desktop);
    needs_composite = 1;
}
/* client buffer (re)committed after configure: repaint + frame follows content */
static void client_resized(WlWin *w, int bw, int bh) {
    Client *c = client_of_wl(w);
    if (!c) return;
    needs_composite = 1;
    if (c->max || c->full) return;
    int nw = bw + 2 * FB, nh = bh + TITLE_H + FB;
    if (nw == c->w && nh == c->h) return;
    c->w = nw; c->h = nh;
    cv_free(&c->fc);
    draw_frame_title(c);
}
static void unmanage(Client *c, int dead) {
    (void)dead;
    Client **pp = &clients;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    if (focused == c) focused = NULL;
    cv_free(&c->fc);
    free(c);
    taskbar_dirty();
    needs_composite = 1;
}
static void desk_switch(int d) {
    if (d < 0 || d >= NDESK || d == cur_desk) return;
    cur_desk = d;
    for (Client *c = clients; c; c = c->next) {
        int on = (c->desktop == d) && !c->min;
        if (on != c->mapped) {
            c->mapped = on;
            wlwin_set_activated(c->wl, 0);   /* focus re-picked below */
        }
    }
    Client *top = NULL;
    for (Client *c = clients; c; c = c->next)
        if (c->desktop == d && !c->min) { top = c; break; }
    if (top) { focused = NULL; focus_client(top); }
    else { focused = NULL; needs_composite = 1; }
    taskbar_dirty();
}

/* ---------------- static-desk state for show-desktop ---------------- */
static int all_minimized;
static void show_desktop_toggle(void) {
    if (!all_minimized) {
        for (Client *c = clients; c; c = c->next)
            if (c->mapped && !c->min) iconify(c);
        all_minimized = 1;
    } else {
        for (Client *c = clients; c; c = c->next)
            if (c->min) focus_client(c); /* un-minimizes */
        all_minimized = 0;
    }
    focused = NULL;
    taskbar_dirty();
}

/* Wayland: no EWMH state to publish. */
static void set_root_cursor(void) { cursor_set_shape(CUR_ARROW); }

/* == PART4: panels == */

/* ---------------- small icon system ---------------- */
enum { IC_FOLDER, IC_DOC, IC_GEAR, IC_TERM, IC_GLOBE, IC_USER, IC_POWER, IC_LOCK,
       IC_WIFI, IC_BT, IC_PLANE, IC_MOON, IC_SUN, IC_VOL, IC_BAT, IC_SEARCH,
       IC_TASKVIEW, IC_CHEV, IC_BIN };
static void icon_logo(Canvas *c, int x, int y, int s, unsigned col, unsigned bg) {
    disc(c, x + s / 2, y + s / 2, s / 2 - 1, col);
    int t = s / 8 ? s / 8 : 1;
    cv_fill(c, x + s / 2 - t / 2, y, t, s, bg);
    cv_fill(c, x, y + s / 2 - t / 2, s, t, bg);
}
static void icon_letter_tile(Canvas *c, int x, int y, int s, unsigned col, const char *L) {
    cv_round(c, x + 2, y + 2, s - 4, s - 4, 8, col);
    int tw = text_w(L, s / 2, 1);
    text_draw(c, x + (s - tw) / 2, y + (s - s / 2) / 2 - 1, L, s / 2, 1, 0xffffff);
}
static void icon_draw(Canvas *c, int x, int y, int s, int type, unsigned col) {
    int cx = x + s / 2, cy = y + s / 2;
    switch (type) {
    case IC_FOLDER:
        cv_round(c, x + 2, y + s / 3, s - 4, s / 2 - 1, 2, col);
        cv_round(c, x + 2, y + s / 4, s / 2, s / 6 + 2, 2, col);
        break;
    case IC_DOC:
        cv_round(c, x + s / 4, y + 2, s / 2, s - 4, 2, col);
        cv_fill(c, x + s / 4 + 3, y + s / 3, s / 2 - 6, 1, 0xffffff);
        cv_fill(c, x + s / 4 + 3, y + s / 2, s / 2 - 6, 1, 0xffffff);
        break;
    case IC_GEAR:
        circle_out(c, cx, cy, s / 3, 2, col);
        for (int a = 0; a < 360; a += 45) {
            double r = a * 3.14159265 / 180.0;
            stroke(c, cx + (int)(s / 3 * cos(r)), cy + (int)(s / 3 * sin(r)),
                       cx + (int)(s / 2.2 * cos(r)), cy + (int)(s / 2.2 * sin(r)), 3, col);
        }
        break;
    case IC_TERM:
        cv_round_border(c, x + 2, y + s / 4, s - 4, s / 2, 3, 1, col);
        stroke(c, x + s / 4, y + s / 2 - 2, x + s / 3, y + s / 2 + 2, 2, col);
        stroke(c, x + s / 3, y + s / 2 + 2, x + s / 4 + 2, y + s / 2 + 5, 2, col);
        stroke(c, x + s / 2 + 2, y + s / 2 + 5, x + s * 2 / 3, y + s / 2 + 5, 2, col);
        break;
    case IC_GLOBE:
        circle_out(c, cx, cy, s / 2 - 2, 1, col);
        cv_fill(c, x + 2, cy - 1, s - 4, 2, col);
        stroke(c, cx, y + 3, cx, y + s - 3, 1, col);
        circle_out(c, cx, cy, s / 3, 1, col);
        break;
    case IC_USER:
        disc(c, cx, y + s / 3, s / 5, col);
        circle_arc(c, cx, y + s + s / 8, s / 2, 200, 340, 3, col);
        break;
    case IC_POWER:
        circle_arc(c, cx, cy, s / 3, -60, 240, 2, col);
        cv_fill(c, cx - 1, y + 2, 3, s / 3, col);
        break;
    case IC_LOCK:
        cv_round_border(c, x + s / 4, y + s / 3, s / 2, s / 2 - 1, 3, 2, col);
        circle_arc(c, cx, y + s / 3, s / 6, 180, 360, 2, col);
        break;
    case IC_WIFI:
        for (int r = s / 3; r >= s / 10; r -= (s / 6 ? s / 6 : 3))
            circle_arc(c, cx, y + s - 4, r, 210, 330, 2, col);
        disc(c, cx, y + s - 5, 2, col);
        break;
    case IC_BT:
        stroke(c, cx, y + 3, cx + s / 4, cy, 1, col);
        stroke(c, cx, y + s - 3, cx + s / 4, cy, 1, col);
        stroke(c, cx, y + 3, cx, y + s - 3, 1, col);
        stroke(c, cx, y + 3, cx - s / 6, cy - s / 8, 1, col);
        stroke(c, cx, y + s - 3, cx - s / 6, cy + s / 8, 1, col);
        break;
    case IC_PLANE:
        stroke(c, x + 3, cy, x + s - 3, cy, 2, col);
        stroke(c, cx - s / 5, cy + s / 5, cx + s / 8, cy, 2, col);
        stroke(c, cx - s / 5, cy - s / 5, cx + s / 8, cy, 2, col);
        stroke(c, x + 3, cy, cx - s / 6, cy - s / 6, 2, col);
        break;
    case IC_MOON:
        circle_out(c, cx, cy, s / 3, 2, col);
        disc(c, cx + s / 8, cy - s / 10, s / 4, th.panel);
        break;
    case IC_SUN:
        circle_out(c, cx, cy, s / 5, 2, col);
        for (int a = 0; a < 360; a += 45) {
            double r = a * 3.14159265 / 180.0;
            stroke(c, cx + (int)(s / 3.2 * cos(r)), cy + (int)(s / 3.2 * sin(r)),
                       cx + (int)(s / 2.3 * cos(r)), cy + (int)(s / 2.3 * sin(r)), 2, col);
        }
        break;
    case IC_VOL:
        cv_fill(c, x + s / 5, cy - s / 6, s / 5, s / 3, col);
        stroke(c, x + s / 5 + s / 5 - 1, cy - s / 6, x + s / 2, cy - s / 3, 2, col);
        stroke(c, x + s / 5 + s / 5 - 1, cy + s / 6, x + s / 2, cy + s / 3, 2, col);
        circle_arc(c, x + s / 2, cy, s / 4, -60, 60, 1, col);
        circle_arc(c, x + s / 2, cy, s / 2.6, -60, 60, 1, col);
        break;
    case IC_BAT:
        cv_round_border(c, x + 2, cy - s / 5, s - 6, s / 2.5, 2, 1, col);
        cv_fill(c, x + s - 4, cy - s / 8, 2, s / 4, col);
        cv_fill(c, x + 4, cy - s / 5 + 2,
                ((s - 10) * cfg_vol / 100) > 2 ? (s - 10) * cfg_vol / 100 : 2, s / 2.5 - 4, col);
        break;
    case IC_SEARCH:
        circle_out(c, x + s / 2 - 1, y + s / 2 - 1, s / 3, 2, col);
        stroke(c, x + s / 2 + s / 5, y + s / 2 + s / 5, x + s - 3, y + s - 3, 2, col);
        break;
    case IC_TASKVIEW:
        cv_round_border(c, x + 2, y + s / 3, s / 2 - 2, s / 2 - 1, 2, 1, col);
        cv_round_border(c, x + s / 2, y + s / 4, s / 2 - 2, s / 2 + 1, 2, 1, col);
        break;
    case IC_CHEV:
        stroke(c, x + 4, y + s / 2 + 2, cx, y + s / 2 - 2, 2, col);
        stroke(c, cx, y + s / 2 - 2, x + s - 4, y + s / 2 + 2, 2, col);
        break;
    }
}
static int app_color(const char *name) {
    unsigned h = 5381;
    for (const char *p = name; *p; p++) h = h * 33 + (unsigned char)*p;
    static const unsigned pal[] = { 0x2f6fed, 0x8a3bd8, 0xd83b3b, 0x2fa84f, 0xd89b2f, 0x2fb0b0, 0xd8572f, 0x5b5fc7 };
    return pal[h % 8];
}
static int app_icon_type(const App *a) {
    char buf[192]; snprintf(buf, sizeof buf, "%s %s", a->name, a->exec);
    if (strcasestr(buf, "folder") || strcasestr(buf, "files")) return IC_FOLDER;
    if (strcasestr(buf, "notepad") || strcasestr(buf, "editor") || strcasestr(buf, "writer")) return IC_DOC;
    if (strcasestr(buf, "settings") || strcasestr(buf, "control")) return IC_GEAR;
    if (strcasestr(buf, "console") || strcasestr(buf, "terminal") || strcasestr(buf, "cmd") ||
        strcasestr(buf, "hello")) return IC_TERM;
    if (strcasestr(buf, "browser") || strcasestr(buf, "firefox") || strcasestr(buf, "edge")) return IC_GLOBE;
    return -1;
}
static void app_icon(Canvas *c, int x, int y, int s, const App *a) {
    int t = app_icon_type(a);
    if (t >= 0) icon_draw(c, x, y, s, t, mix(th.accent, 0xffffff, 40));
    else {
        char L[2] = { (char)toupper((unsigned char)a->name[0]), 0 };
        icon_letter_tile(c, x, y, s, app_color(a->name), L);
    }
}

/* ---------------- native file manager (SW_FILES popup) --------------- */
#define FM_MAX 64
#define FM_ROWS 12
static char fm_path[512];
static char fm_ent[FM_MAX][64];
static int  fm_isdir[FM_MAX];
static int  fm_n, fm_sel;
static char fm_last[64];
static unsigned long fm_last_t;

static void files_open(const char *path) {
    char dir[512];
    if (!path || !path[0]) path = "/";
    if (!strcmp(path, "C:")) path = "/";
    strlcpy_(dir, path, sizeof dir);
    size_t l = strlen(dir);
    while (l > 1 && dir[l - 1] == '/') dir[--l] = 0;
    DIR *d = opendir(dir);
    if (!d) { toast_show("Cannot open %s", dir); return; }
    fm_n = 0;
    if (strcmp(dir, "/")) {                       /* parent entry */
        strlcpy_(fm_ent[0], "..", 64);
        fm_isdir[0] = 1;
        fm_n = 1;
    }
    struct dirent *e;
    while ((e = readdir(d)) && fm_n < FM_MAX) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        strlcpy_(fm_ent[fm_n], e->d_name, 64);
        fm_isdir[fm_n] = 0;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (!stat(full, &st) && S_ISDIR(st.st_mode)) fm_isdir[fm_n] = 1;
        fm_n++;
    }
    closedir(d);
    strlcpy_(fm_path, dir, sizeof fm_path);
    fm_sel = 0;
    fm_last[0] = 0;
    ShellWin *s = sw_get(SW_FILES, 0);
    sw_resize(s, 440, 380);
    sw_show(s, (sw - 440) / 2, (sh - 380) / 2 - 40);
}
static void draw_files(ShellWin *s) {
    Canvas *c = &s->cv;
    cv_fill(c, 0, 0, s->w, s->h, th.panel);
    cv_round_border(c, 0, 0, s->w, s->h, POP_R, 1, th.border);
    text_draw(c, 16, 12, fm_path, 12, 0, th.text);
    cv_round(c, 12, 34, s->w - 24, s->h - 82, 6, th.field);
    int y = 42;
    for (int i = 0; i < fm_n && i < FM_ROWS; i++) {
        if (i == fm_sel) cv_round(c, 18, y - 3, s->w - 36, 22, 4, mix(th.panel, th.accent, 60));
        icon_draw(c, 22, y, 16, fm_isdir[i] ? IC_FOLDER : IC_DOC,
                  fm_isdir[i] ? th.accent : th.text2);
        text_draw(c, 46, y + 2, fm_ent[i], 12, 0, th.text);
        y += 24;
    }
    if (fm_n > FM_ROWS) {
        char m[64];
        snprintf(m, sizeof m, "... %d more", fm_n - FM_ROWS);
        text_draw(c, 22, y + 2, m, 11, 0, th.text2);
    }
    text_draw(c, 16, s->h - 34, "double-click a folder to open it", 11, 0, th.text2);
    cv_round(c, s->w - 84, s->h - 40, 64, 26, 5, mix(th.panel, th.text, 12));
    text_draw(c, s->w - 74, s->h - 34, "Close", 11, 0, th.text);
}
static int click_files(ShellWin *s, int btn, int x, int y) {
    (void)btn;
    if (in_r(x, y, s->w - 84, s->h - 40, 64, 26)) { popup_close(); return 1; }
    if (y < 40 || y >= 40 + 24 * FM_ROWS) return 1;
    int row = (y - 40) / 24;
    if (row < 0 || row >= fm_n || row >= FM_ROWS) return 1;
    fm_sel = row;
    unsigned long now = now_ms();
    if (fm_isdir[row] && !strcmp(fm_last, fm_ent[row]) && now - fm_last_t < 500) {
        char full[1024];
        if (!strcmp(fm_ent[row], "..")) {
            char p[512];
            strlcpy_(p, fm_path, sizeof p);
            char *sl = strrchr(p, '/');
            if (sl) { if (sl == p) p[1] = 0; else *sl = 0; }
            files_open(p);
        } else {
            snprintf(full, sizeof full, "%s/%s", fm_path, fm_ent[row]);
            files_open(full);
        }
        return 1;
    }
    strlcpy_(fm_last, fm_ent[row], 64);
    fm_last_t = now;
    sw_draw(s);
    return 1;
}

/* ---------------- recents + launcher glue ---------------- */
#define NREC_ 6
static char recent[NREC_][128];
static int nrec;
static void rec_add(const char *name) {
    for (int i = 0; i < nrec; i++)
        if (!strcmp(recent[i], name)) return;
    if (nrec < NREC_) { strlcpy_(recent[nrec++], name, 128); return; }
    memmove(recent[0], recent[1], sizeof(recent[0]) * (NREC_ - 1));
    strlcpy_(recent[NREC_ - 1], name, 128);
}
static void launch_app(App *a) {
    rec_add(a->name);
    if (!strcmp(a->exec, "__files__")) files_open("/");
    else launch(a->exec);
}

/* ---------------- panel state ---------------- */
static char start_q[128];
static int start_allapps, start_sel;
static char run_q[256];
#define ALT_MAX 16
static Client *alt_list[ALT_MAX];
static int alt_n, alt_sel;
static Client *snap_client;
static int tb_hover_key = -1;
static int frame_hover_btn;
static Client *menu_client;
#define MENU_MAX 20
static struct { char label[64]; int act; int data; } menu_items[MENU_MAX];
static int menu_n;
static int qdrag;            /* 1=volume 2=brightness */
static int cal_m, cal_y;
static char toast_t[128], toast_b[192];
static unsigned long toast_until;
static int deskicon_hover = -1, deskicon_sel = -1;

enum { MI_NONE, MI_LOCK, MI_RESTART, MI_POWEROFF, MI_SLEEP, MI_ABOUT,
       MI_WIN_CLOSE, MI_WIN_MIN, MI_WIN_MAX, MI_WIN_RESTORE,
       MI_DESK_NEW, MI_DESK_CLOSE, MI_DESK_GOTO, MI_WALLPAPER, MI_THEME,
       MI_EXIT, MI_EMPTY_TRASH, MI_RUN };

/* ---------------- tray queries ---------------- */
static int net_ok(void) {
    DIR *d = opendir("/sys/class/net"); if (!d) return 0;
    struct dirent *e; int ok = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!strcmp(e->d_name, "lo")) continue;
        char p[256], st[32] = "";
        snprintf(p, sizeof p, "/sys/class/net/%s/operstate", e->d_name);
        FILE *f = fopen(p, "r");
        if (f) { if (fgets(st, sizeof st, f)) st[strcspn(st, "\n")] = 0; fclose(f); }
        if (!strcmp(st, "up")) ok = 1;
    }
    closedir(d);
    return ok;
}
static int bat_pct(void) {
    DIR *d = opendir("/sys/class/power_supply"); if (!d) return -1;
    struct dirent *e; int pct = -1;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "BAT", 3)) continue;
        char p[256]; snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
        FILE *f = fopen(p, "r");
        if (f) { if (fscanf(f, "%d", &pct) != 1) pct = -1; fclose(f); }
    }
    closedir(d);
    return pct;
}
static void brightness_set(int v) {
    DIR *d = opendir("/sys/class/backlight"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[300]; snprintf(p, sizeof p, "/sys/class/backlight/%s/max_brightness", e->d_name);
        int max = 100; FILE *f = fopen(p, "r");
        if (f) { if (fscanf(f, "%d", &max) != 1) max = 100; fclose(f); }
        snprintf(p, sizeof p, "/sys/class/backlight/%s/brightness", e->d_name);
        f = fopen(p, "w");
        if (f) { fprintf(f, "%d\n", v * max / 100); fclose(f); }
    }
    closedir(d);
}

/* ---------------- time helper ---------------- */
static void timestr(char *d, size_t n) { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(d, n, "%H:%M", &tm); }
static void datestr(char *d, size_t n) { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(d, n, "%m/%d/%Y", &tm); }

/* ---------------- taskbar ---------------- */
typedef struct { int kind; intptr_t id; int x, w; } TBItem;
enum { TBI_START, TBI_SEARCH, TBI_TASKVIEW, TBI_PIN, TBI_RUNWIN, TBI_CHEV,
       TBI_NET, TBI_VOL, TBI_BAT, TBI_CLOCK, TBI_SHOWDESK };
#define TB_MAXI 48
static int tb_build(TBItem *it) {
    int n = 0, w = 48 + 220 + 48;
    int np = 0;
    for (int i = 0; i < cfg_npinned; i++)
        if (app_find(cfg_pinned[i])) { np++; w += 48; }
    int nrun = 0;
    for (Client *c = clients; c; c = c->next)
        if (c->desktop == cur_desk && !c->min && c->mapped) nrun++;
    w += nrun * 48;
    int x0 = (sw - w) / 2;
    if (x0 < 8) x0 = 8;
    it[n++] = (TBItem){ TBI_START, 0, x0, 48 };
    it[n++] = (TBItem){ TBI_SEARCH, 0, x0 + 48, 220 };
    it[n++] = (TBItem){ TBI_TASKVIEW, 0, x0 + 276, 48 };
    int x = x0 + 332;
    for (int i = 0; i < np; i++) { it[n].kind = TBI_PIN; it[n].id = i; it[n].x = x; it[n].w = 48; x += 48; n++; }
    for (Client *c = clients; c && n < TB_MAXI - 12; c = c->next)
        if (c->desktop == cur_desk && !c->min && c->mapped) {
            it[n].kind = TBI_RUNWIN; it[n].id = (intptr_t)c; it[n].x = x; it[n].w = 48; x += 48; n++;
        }
    /* tray from right: [chev][net][vol][bat?] clock |showdesk */
    int rx = sw - 6 - 96;
    int bat = bat_pct() >= 0;
    int chevx = rx - (bat ? 4 : 3) * 32;
    it[n++] = (TBItem){ TBI_CHEV, 0, chevx, 24 };
    it[n++] = (TBItem){ TBI_NET, 0, chevx + 32, 24 };
    it[n++] = (TBItem){ TBI_VOL, 0, chevx + 64, 24 };
    if (bat) it[n++] = (TBItem){ TBI_BAT, 0, chevx + 96, 24 };
    it[n++] = (TBItem){ TBI_CLOCK, 0, rx, 96 };
    it[n++] = (TBItem){ TBI_SHOWDESK, 0, sw - 6, 6 };
    return n;
}
static void draw_taskbar(ShellWin *s) {
    Canvas *c = &s->cv;
    cv_fill(c, 0, 0, s->w, s->h, th.taskbar);
    cv_hline(c, 0, 0, s->w, mix(th.taskbar, th.text, 18));
    TBItem it[TB_MAXI];
    int n = tb_build(it);
    for (int i = 0; i < n; i++) {
        TBItem *t = &it[i];
        int hov = (tb_hover_key == t->kind * 100 + (int)t->id) && t->kind != TBI_SHOWDESK && t->kind != TBI_CLOCK;
        if (hov) cv_round(c, t->x, 6, t->w, s->h - 12, 6, mix(th.taskbar, th.text, dark_mode ? 22 : 14));
        switch (t->kind) {
        case TBI_START:
            icon_logo(c, t->x + 10, 10, 28, th.accent, hov ? mix(th.taskbar, th.text, 22) : th.taskbar);
            break;
        case TBI_SEARCH: {
            int bx = t->x + 8, bw = t->w - 16, by = 8, bh = s->h - 16;
            cv_round(c, bx, by, bw, bh, 6, mix(th.taskbar, th.text, dark_mode ? 16 : 10));
            icon_draw(c, bx + 8, by + 8, 20, IC_SEARCH, th.text2);
            text_draw(c, bx + 34, by + 10, "Type here to search", 12, 0, th.text2);
            break;
        }
        case TBI_TASKVIEW:
            icon_draw(c, t->x + 12, 12, 24, IC_TASKVIEW, th.text);
            break;
        case TBI_PIN: {
            App *a = app_find(cfg_pinned[t->id]);
            if (a) {
                app_icon(c, t->x + 8, 10, 28, a);
                cv_round(c, t->x + t->w / 2 - 8, s->h - 4, 16, 3, 2, th.accent);
            }
            break;
        }
        case TBI_RUNWIN: {
            Client *cl = (Client *)(intptr_t)t->id;
            App probe = { 0 };
            strlcpy_(probe.name, cl->cls[0] ? cl->cls : cl->title, sizeof probe.name);
            strlcpy_(probe.exec, cl->cls, sizeof probe.exec);
            int t2 = app_icon_type(&probe);
            if (t2 >= 0) icon_draw(c, t->x + 8, 10, 28, t2, mix(th.accent, 0xffffff, 40));
            else {
                char L[2] = { (char)toupper((unsigned char)(cl->cls[0] ? cl->cls[0] : cl->title[0])), 0 };
                icon_letter_tile(c, t->x + 8, 10, 28, app_color(cl->cls[0] ? cl->cls : cl->title), L);
            }
            unsigned ind = (focused == cl) ? th.accent : mix(th.text, th.taskbar, 60);
            cv_round(c, t->x + t->w / 2 - 8, s->h - 4, 16, 3, 2, ind);
            break;
        }
        case TBI_CHEV: icon_draw(c, t->x, 12, 24, IC_CHEV, th.text2); break;
        case TBI_NET: icon_draw(c, t->x, 12, 24, IC_WIFI, net_ok() && !cfg_air ? th.text : th.text2); break;
        case TBI_VOL: icon_draw(c, t->x, 12, 24, IC_VOL, th.text); break;
        case TBI_BAT: icon_draw(c, t->x, 12, 24, IC_BAT, bat_pct() < 20 ? th.danger : th.text); break;
        case TBI_CLOCK: {
            char tt[16], dd[16]; timestr(tt, sizeof tt); datestr(dd, sizeof dd);
            text_draw(c, t->x + 8, 8, tt, 12, 0, th.text);
            text_draw(c, t->x + 8, 25, dd, 11, 0, th.text);
            break;
        }
        case TBI_SHOWDESK:
            cv_fill(c, t->x, 4, 1, s->h - 8, mix(th.text, th.taskbar, 40));
            break;
        }
    }
}
static int click_taskbar(ShellWin *s, int btn, int x, int y) {
    (void)s; (void)btn;
    TBItem it[TB_MAXI];
    int n = tb_build(it);
    for (int i = 0; i < n; i++) {
        TBItem *t = &it[i];
        if (!in_r(x, y, t->x, 0, t->w, TB_H)) continue;
        switch (t->kind) {
        case TBI_START: key_start_toggle(); break;
        case TBI_SEARCH: key_start_open_search(); break;
        case TBI_TASKVIEW: key_taskview_open(); break;
        case TBI_PIN: { App *a = app_find(cfg_pinned[t->id]); if (a) launch_app(a); break; }
        case TBI_RUNWIN: {
            Client *cl = (Client *)(intptr_t)t->id;
            if (focused == cl && cl->mapped && !cl->min) iconify(cl);
            else focus_client(cl);
            break;
        }
        case TBI_NET: case TBI_VOL: case TBI_BAT: case TBI_CHEV: key_quick_toggle(); break;
        case TBI_CLOCK: key_cal_toggle(); break;
        case TBI_SHOWDESK: show_desktop_toggle(); break;
        }
        return 1;
    }
    return 0;
}
static void taskbar_dirty(void) {
    ShellWin *t = sw_get(SW_TASKBAR, 0);
    if (t->visible) sw_draw(t);
}

/* ---------------- helpers ---------------- */
static void text_clip(Canvas *c, int x, int y, int maxw, const char *s, int size, int bold, unsigned col) {
    char buf[256];
    strlcpy_(buf, s, sizeof buf);
    if (text_w(buf, size, bold) > maxw) {
        int l = (int)strlen(buf);
        while (l > 1 && text_w(buf, size, bold) > maxw - 10) buf[--l] = 0;
        strcat(buf, "...");
    }
    text_draw(c, x, y, buf, size, bold, col);
}
#define NOTIF_MAX 6
static char notifs[NOTIF_MAX][160];
static int nnotifs;
static void notif_add(const char *line) {
    memmove(notifs[0], notifs[1], sizeof(notifs[0]) * (NOTIF_MAX - 1));
    strlcpy_(notifs[0], line, 160);
    if (nnotifs < NOTIF_MAX) nnotifs++;
}
static int isleap(int y) { return (y % 4 == 0 && y % 100) || y % 400 == 0; }
static int dim(int y, int m) {
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return d[m] + (m == 1 && isleap(y) ? 1 : 0);
}

/* ---------------- start menu ---------------- */
static int start_filtered(App **out, int max) {
    int n = 0;
    if (start_q[0]) {
        for (App *a = apps; a && n < max; a = a->next)
            if (strcasestr(a->name, start_q) || strcasestr(a->exec, start_q))
                out[n++] = a;
    } else {
        for (int i = 0; i < cfg_npinned && n < max; i++) {
            App *a = app_find(cfg_pinned[i]);
            if (a) out[n++] = a;
        }
    }
    return n;
}
static int start_all_list(App **out, int max) {
    int n = 0;
    for (App *a = apps; a && n < max; a = a->next) out[n++] = a;
    return n;
}
static void draw_start(ShellWin *s) {
    Canvas *c = &s->cv;
    cv_fill(c, 0, 0, s->w, s->h, th.panel);
    cv_round_border(c, 0, 0, s->w, s->h, POP_R, 1, th.border);
    /* search field */
    cv_round(c, 24, 22, s->w - 48, 38, 6, th.field);
    cv_round_border(c, 24, 22, s->w - 48, 38, 6, 1, mix(th.field, th.accent, start_q[0] ? 120 : 0));
    icon_draw(c, 34, 31, 20, IC_SEARCH, th.text2);
    if (start_q[0]) text_draw(c, 62, 31, start_q, 13, 0, th.text);
    else text_draw(c, 62, 31, "Type here to search", 13, 0, th.text2);
    App *list[64];
    int n;
    if (start_q[0]) {
        text_draw(c, 32, 76, "Best match", 12, 1, th.text);
        n = start_filtered(list, 8);
        for (int i = 0; i < n; i++) {
            int y = 104 + i * 48;
            if (i == start_sel) cv_round(c, 24, y - 4, s->w - 48, 44, 6, th.hover);
            app_icon(c, 34, y, 32, list[i]);
            text_clip(c, 78, y + 8, s->w - 130, list[i]->name, 13, 0, th.text);
        }
        if (!n) text_draw(c, 32, 112, "No results for your search.", 12, 0, th.text2);
    } else if (start_allapps) {
        text_draw(c, 32, 76, "All apps", 12, 1, th.text);
        text_draw(c, s->w - 120, 76, "< Back", 12, 0, th.accent);
        n = start_all_list(list, 64);
        for (int i = 0; i < n && 104 + i * 44 < s->h - 80; i++) {
            int y = 104 + i * 44;
            if (i == start_sel) cv_round(c, 24, y - 2, s->w - 48, 40, 6, th.hover);
            app_icon(c, 34, y, 30, list[i]);
            text_clip(c, 76, y + 7, s->w - 130, list[i]->name, 13, 0, th.text);
        }
    } else {
        text_draw(c, 32, 76, "Pinned", 12, 1, th.text);
        text_draw(c, s->w - 130, 76, "All apps  >", 12, 0, th.text2);
        n = start_filtered(list, 18);
        for (int i = 0; i < n; i++) {
            int col = i % 6, row = i / 6;
            int x = 28 + col * 92, y = 104 + row * 92;
            if (i == start_sel) cv_round(c, x - 4, y - 4, 88, 88, 6, th.hover);
            app_icon(c, x + 16, y + 6, 40, list[i]);
            text_clip(c, x - 6, y + 54, 84, list[i]->name, 11, 0, th.text);
        }
        int yrec = 104 + 3 * 92 + 4;
        text_draw(c, 32, yrec, "Recommended", 12, 1, th.text);
        if (nrec) {
            for (int i = 0; i < nrec && i < 6; i++) {
                int col = i % 2, row = i / 2;
                int x = 28 + col * 272, y = yrec + 26 + row * 52;
                App *a = app_find(recent[i]);
                if (!a) continue;
                app_icon(c, x, y, 32, a);
                text_clip(c, x + 40, y + 2, 220, a->name, 12, 0, th.text);
                text_clip(c, x + 40, y + 18, 220, "Recently opened", 10, 0, th.text2);
            }
        } else {
            text_draw(c, 32, yrec + 28, "No recent items yet.", 12, 0, th.text2);
        }
    }
    /* bottom bar */
    cv_fill(c, 24, s->h - 66, s->w - 48, 1, th.border);
    icon_draw(c, 32, s->h - 52, 36, IC_USER, th.text);
    const char *user = getenv("USER") ? getenv("USER") : "user";
    text_draw(c, 78, s->h - 42, user, 13, 0, th.text);
    icon_draw(c, s->w - 64, s->h - 46, 26, IC_POWER, th.text);
}
static int click_start(ShellWin *s, int btn, int x, int y) {
    (void)btn;
    if (in_r(x, y, s->w - 130, 70, 130, 26) && start_allapps) { start_allapps = 0; start_sel = -1; sw_draw(s); return 1; }
    if (in_r(x, y, s->w - 130, 70, 130, 26) && !start_allapps && !start_q[0]) { start_allapps = 1; start_sel = -1; sw_draw(s); return 1; }
    if (in_r(x, y, s->w - 64, s->h - 52, 40, 40)) {
        menu_client = NULL;
        menu_open_power(x - 140, s->y + s->h - 190);
        return 1;
    }
    App *list[64]; int n;
    if (start_q[0]) {
        n = start_filtered(list, 8);
        for (int i = 0; i < n; i++)
            if (in_r(x, y, 24, 104 + i * 48 - 4, s->w - 48, 44)) { launch_app(list[i]); popup_close(); return 1; }
    } else if (start_allapps) {
        n = start_all_list(list, 64);
        for (int i = 0; i < n && 104 + i * 44 < s->h - 80; i++)
            if (in_r(x, y, 24, 104 + i * 44 - 2, s->w - 48, 40)) { launch_app(list[i]); popup_close(); return 1; }
    } else {
        n = start_filtered(list, 18);
        for (int i = 0; i < n; i++) {
            int col = i % 6, row = i / 6;
            int gx = 28 + col * 92, gy = 104 + row * 92;
            if (in_r(x, y, gx - 4, gy - 4, 88, 88)) { launch_app(list[i]); popup_close(); return 1; }
        }
        int yrec = 104 + 3 * 92 + 4;
        for (int i = 0; i < nrec && i < 6; i++) {
            int col = i % 2, row = i / 2;
            int gx = 28 + col * 272, gy = yrec + 26 + row * 52;
            if (in_r(x, y, gx, gy, 260, 44)) {
                App *a = app_find(recent[i]);
                if (a) { launch_app(a); popup_close(); }
                return 1;
            }
        }
    }
    return 1; /* consume clicks inside start menu */
}

/* ---------------- quick settings ---------------- */
typedef struct { const char *label; int icon; } QTog;
static const QTog qtogs[5] = { { "Wi-Fi", IC_WIFI }, { "Bluetooth", IC_BT }, { "Airplane", IC_PLANE },
                               { "Night light", IC_MOON }, { "Dark theme", IC_SUN } };
static int qtog_state(int i) {
    switch (i) {
    case 0: return cfg_wifi; case 1: return cfg_bt; case 2: return cfg_air;
    case 3: return cfg_night; case 4: return dark_mode;
    }
    return 0;
}
static void draw_quick(ShellWin *s) {
    Canvas *c = &s->cv;
    cv_fill(c, 0, 0, s->w, s->h, th.panel);
    cv_round_border(c, 0, 0, s->w, s->h, POP_R, 1, th.border);
    text_draw(c, 20, 14, "Quick settings", 13, 1, th.text);
    int bp = bat_pct();
    if (bp >= 0) {
        char b[32]; snprintf(b, sizeof b, "%d%%", bp);
        text_draw(c, s->w - 52, 15, b, 11, 0, th.text2);
        icon_draw(c, s->w - 90, 12, 22, IC_BAT, th.text2);
    }
    for (int i = 0; i < 5; i++) {
        int col = i % 3, row = i / 3;
        int x = 16 + col * 112, y = 52 + row * 92;
        int on = qtog_state(i);
        unsigned circ = on ? th.accent : mix(th.panel, th.text, 12);
        disc(c, x + 26, y + 26, 24, circ);
        icon_draw(c, x + 13, y + 13, 26, qtogs[i].icon, on ? th.accent_text : th.text);
        text_clip(c, x, y + 58, 100, qtogs[i].label, 11, 0, th.text);
    }
    /* volume */
    icon_draw(c, 20, 258, 24, IC_VOL, th.text);
    cv_round(c, 56, 268, s->w - 110, 4, 2, mix(th.panel, th.text, 18));
    cv_round(c, 56, 268, (s->w - 110) * cfg_vol / 100, 4, 2, th.accent);
    disc(c, 56 + (s->w - 110) * cfg_vol / 100, 270, 6, th.accent_text);
    { char v[32]; snprintf(v, sizeof v, "%d%%", cfg_vol); text_draw(c, s->w - 44, 261, v, 11, 0, th.text2); }
    /* brightness */
    icon_draw(c, 20, 308, 24, IC_SUN, th.text);
    cv_round(c, 56, 318, s->w - 110, 4, 2, mix(th.panel, th.text, 18));
    cv_round(c, 56, 318, (s->w - 110) * cfg_bright / 100, 4, 2, th.accent);
    disc(c, 56 + (s->w - 110) * cfg_bright / 100, 320, 6, th.accent_text);
    { char v[32]; snprintf(v, sizeof v, "%d%%", cfg_bright); text_draw(c, s->w - 44, 311, v, 11, 0, th.text2); }
    cv_fill(c, 20, 352, s->w - 40, 1, th.border);
    text_draw(c, 20, 366, net_ok() && !cfg_air ? "Network: connected" : "Network: unavailable", 11, 0, th.text2);
    text_draw(c, 20, 386, "Losedoze shell " SHELL_VER, 11, 0, th.text2);
    text_draw(c, s->w - 130, 386, "All settings >", 11, 0, th.accent);
}
static void qtog_apply(ShellWin *s, int i) {
    switch (i) {
    case 0: cfg_wifi = !cfg_wifi; toast_show("Wi-Fi %s", cfg_wifi ? "on" : "off"); break;
    case 1: cfg_bt = !cfg_bt; toast_show("Bluetooth %s (no adapter)", cfg_bt ? "on" : "off"); break;
    case 2: cfg_air = !cfg_air; toast_show("Airplane mode %s", cfg_air ? "on" : "off"); break;
    case 3:
        cfg_night = !cfg_night;
        toast_show("Night light %s (color ramp needs XRandR gamma)", cfg_night ? "on" : "off");
        break;
    case 4:
        dark_mode = !dark_mode;
        set_theme();
        redraw_all();
        redraw_frames();
        break;
    }
    cfg_save();
    sw_draw(s);
}
static int click_quick(ShellWin *s, int btn, int x, int y) {
    (void)btn;
    for (int i = 0; i < 5; i++) {
        int col = i % 3, row = i / 3;
        int qx = 16 + col * 112, qy = 52 + row * 92;
        if (in_r(x, y, qx, qy, 104, 84)) { qtog_apply(s, i); return 1; }
    }
    if (in_r(x, y, 50, 252, s->w - 100, 24)) { qdrag = 1; cfg_vol = (x - 56) * 100 / (s->w - 110); if (cfg_vol < 0) cfg_vol = 0; if (cfg_vol > 100) cfg_vol = 100; sw_draw(s); return 1; }
    if (in_r(x, y, 50, 302, s->w - 100, 24)) { qdrag = 2; cfg_bright = (x - 56) * 100 / (s->w - 110); if (cfg_bright < 0) cfg_bright = 0; if (cfg_bright > 100) cfg_bright = 100; brightness_set(cfg_bright); sw_draw(s); return 1; }
    if (in_r(x, y, s->w - 140, 380, 140, 24)) { toast_show("Settings app not bundled yet"); return 1; }
    return 1;
}

/* ---------------- calendar / notifications ---------------- */
static void draw_cal(ShellWin *s) {
    Canvas *c = &s->cv;
    cv_fill(c, 0, 0, s->w, s->h, th.panel);
    cv_round_border(c, 0, 0, s->w, s->h, POP_R, 1, th.border);
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char big[64]; strftime(big, sizeof big, "%A", &tm);
    text_draw(c, 20, 14, big, 16, 1, th.text);
    char sm[64]; strftime(sm, sizeof sm, "%B %d, %Y", &tm);
    text_draw(c, 20, 38, sm, 12, 0, th.text2);
    text_draw(c, 20, 66, "Notifications", 12, 1, th.text);
    if (nnotifs == 0) text_draw(c, 20, 90, "No new notifications", 11, 0, th.text2);
    for (int i = 0; i < nnotifs && i < 3; i++) {
        int y = 88 + i * 30;
        cv_round(c, 16, y, s->w - 32, 26, 4, mix(th.panel, th.text, 8));
        text_clip(c, 24, y + 6, s->w - 48, notifs[i], 11, 0, th.text);
    }
    int cy0 = 88 + 3 * 30 + 16;
    cv_fill(c, 20, cy0 - 10, s->w - 40, 1, th.border);
    static const char *mons[] = { "January","February","March","April","May","June","July",
                                  "August","September","October","November","December" };
    char mt[40]; snprintf(mt, sizeof mt, "%s %d", mons[cal_m], cal_y);
    text_draw(c, 20, cy0 + 4, mt, 12, 1, th.text);
    text_draw(c, s->w - 60, cy0 + 4, "<", 13, 0, th.text2);
    text_draw(c, s->w - 36, cy0 + 4, ">", 13, 0, th.text2);
    static const char *wd[] = { "S","M","T","W","T","F","S" };
    int gx = 20, gy = cy0 + 30, cw = (s->w - 40) / 7;
    for (int i = 0; i < 7; i++)
        text_draw(c, gx + i * cw + cw / 2 - 3, gy, wd[i], 10, 0, th.text2);
    struct tm first = { 0 };
    first.tm_year = cal_y - 1900; first.tm_mday = 1; first.tm_mon = cal_m;
    mktime(&first);
    int dow = first.tm_wday, nd = dim(cal_y, cal_m);
    int today = (cal_y == tm.tm_year + 1900 && cal_m == tm.tm_mon) ? tm.tm_mday : -1;
    for (int d = 1; d <= nd; d++) {
        int idx = dow + d - 1;
        int col = idx % 7, row = idx / 7;
        int x = gx + col * cw, y = gy + 20 + row * 30;
        if (d == today) {
            disc(c, x + cw / 2 - 1, y + 9, 11, th.accent);
            char dd[8]; snprintf(dd, sizeof dd, "%d", d);
            text_draw(c, x + cw / 2 - text_w(dd, 11, 0) / 2, y + 2, dd, 11, 0, th.accent_text);
        } else {
            char dd[8]; snprintf(dd, sizeof dd, "%d", d);
            text_draw(c, x + cw / 2 - text_w(dd, 11, 0) / 2, y + 2, dd, 11, 0, th.text);
        }
    }
}
static int click_cal(ShellWin *s, int btn, int x, int y) {
    (void)btn;
    int cy0 = 88 + 3 * 30 + 16;
    if (in_r(x, y, s->w - 64, cy0, 24, 24)) {
        if (--cal_m < 0) { cal_m = 11; cal_y--; }
        sw_draw(s); return 1;
    }
    if (in_r(x, y, s->w - 40, cy0, 24, 24)) {
        if (++cal_m > 11) { cal_m = 0; cal_y++; }
        sw_draw(s); return 1;
    }
    return 1;
}

/* ---------------- alt-tab ---------------- */
static void draw_alt(ShellWin *s) {
    Canvas *c = &s->cv;
    cv_fill(c, 0, 0, s->w, s->h, th.panel);
    cv_round_border(c, 0, 0, s->w, s->h, POP_R, 1, th.border);
    for (int i = 0; i < alt_n; i++) {
        int x = 20 + i * ((s->w - 40) / (alt_n ? alt_n : 1));
        int cw = (s->w - 40) / (alt_n ? alt_n : 1);
        if (i == alt_sel) cv_round(c, x - 4, 12, cw - 4, s->h - 24, 6, mix(th.panel, th.accent, 30));
        Client *cl = alt_list[i];
        App probe = { 0 };
        strlcpy_(probe.name, cl->cls[0] ? cl->cls : cl->title, sizeof probe.name);
        strlcpy_(probe.exec, cl->cls, sizeof probe.exec);
        int t = app_icon_type(&probe);
        if (t >= 0) icon_draw(c, x + cw / 2 - 20, 30, 40, t, mix(th.accent, 0xffffff, 40));
        else {
            char L[2] = { (char)toupper((unsigned char)cl->title[0]), 0 };
            icon_letter_tile(c, x + cw / 2 - 20, 30, 40, app_color(cl->title), L);
        }
        text_clip(c, x, 86, cw - 12, cl->title, 11, 0, th.text);
    }
}
static int click_alt(ShellWin *s, int btn, int x, int y) {
    (void)s; (void)btn;
    int cw = (alt_n ? (s->w - 40) / alt_n : s->w);
    for (int i = 0; i < alt_n; i++)
        if (in_r(x, y, 20 + i * cw, 12, cw, s->h - 24)) {
            Client *cl = alt_list[i];
            popup_close();
            focus_client(cl);
            return 1;
        }
    return 1;
}

/* ---------------- task view ---------------- */
static void draw_taskview(ShellWin *s) {
    Canvas *c = &s->cv;
    cv_fill(c, 0, 0, s->w, s->h, mix(th.taskbar, 0x000000, 45));
    int n = NDESK + 1, cw = 150, gap = 12;
    int x0 = (s->w - (n * cw + (n - 1) * gap)) / 2;
    for (int d = 0; d < NDESK; d++) {
        int x = x0 + d * (cw + gap);
        cv_round(c, x, 20, cw, 86, 6, mix(th.panel, 0x000000, 20));
        if (d == cur_desk) cv_round_border(c, x, 20, cw, 86, 6, 2, th.accent);
        cv_grad(c, x + 8, 28, cw - 16, 42, wall_presets[d % 3][0], wall_presets[d % 3][1]);
        char lb[24]; snprintf(lb, sizeof lb, "Desktop %d", d + 1);
        text_draw(c, x + 8, 76, lb, 11, 0, th.text);
    }
    int xn = x0 + NDESK * (cw + gap);
    cv_round_border(c, xn, 20, 120, 86, 6, 1, mix(th.text, th.panel, 50));
    stroke(c, xn + 60, 48, xn + 60, 70, 2, th.text2);
    stroke(c, xn + 49, 59, xn + 71, 59, 2, th.text2);
    text_draw(c, xn + 26, 84, "New desktop", 10, 0, th.text2);
    /* window cards */
    int idx = 0;
    for (Client *cl = clients; cl; cl = cl->next) {
        if (cl->desktop != cur_desk) continue;
        int col = idx % 4, row = idx / 4;
        int x = (s->w - 4 * 210 - 3 * 16) / 2 + col * 226;
        int y = 170 + row * 150;
        cv_round(c, x, y, 200, 130, 6, th.panel2);
        cv_round_border(c, x, y, 200, 130, 6, 1, th.border);
        cv_fill(c, x, y, 200, 24, mix(th.panel2, th.text, 10));
        text_clip(c, x + 8, y + 5, 150, cl->title, 10, 0, th.text);
        /* fake X close */
        stroke(c, x + 182, y + 8, x + 190, y + 16, 1, th.text2);
        stroke(c, x + 190, y + 8, x + 182, y + 16, 1, th.text2);
        App probe = { 0 };
        strlcpy_(probe.name, cl->cls[0] ? cl->cls : cl->title, sizeof probe.name);
        int t = app_icon_type(&probe);
        if (t >= 0) icon_draw(c, x + 86, y + 44, 28, t, mix(th.accent, 0xffffff, 40));
        else {
            char L[2] = { (char)toupper((unsigned char)cl->title[0]), 0 };
            icon_letter_tile(c, x + 86, y + 44, 28, app_color(cl->title), L);
        }
        text_clip(c, x + 8, y + 104, 184, cl->title, 10, 0, th.text2);
        idx++;
    }
    if (!idx) text_draw(c, s->w / 2 - 60, s->h / 2, "No open windows", 13, 0, th.text2);
}
static int click_taskview(ShellWin *s, int btn, int x, int y) {
    (void)btn;
    int n = NDESK + 1, cw = 150, gap = 12;
    int x0 = (s->w - (n * cw + (n - 1) * gap)) / 2;
    for (int d = 0; d < NDESK; d++)
        if (in_r(x, y, x0 + d * (cw + gap), 20, cw, 86)) {
            popup_close();
            desk_switch(d);
            return 1;
        }
    int xn = x0 + NDESK * (cw + gap);
    if (in_r(x, y, xn, 20, 120, 86)) { toast_show("%d virtual desktops available", NDESK); return 1; }
    int idx = 0;
    for (Client *cl = clients; cl; cl = cl->next) {
        if (cl->desktop != cur_desk) continue;
        int col = idx % 4, row = idx / 4;
        int cx_ = (s->w - 4 * 210 - 3 * 16) / 2 + col * 226;
        int cy_ = 170 + row * 150;
        if (in_r(x, y, cx_ + 174, cy_, 26, 24)) { close_client(cl); sw_draw(s); return 1; }
        if (in_r(x, y, cx_, cy_, 200, 130)) { popup_close(); focus_client(cl); return 1; }
        idx++;
    }
    popup_close();
    return 1;
}

/* ---------------- run dialog ---------------- */
static void draw_run(ShellWin *s) {
    Canvas *c = &s->cv;
    cv_fill(c, 0, 0, s->w, s->h, th.panel);
    cv_round_border(c, 0, 0, s->w, s->h, POP_R, 1, th.border);
    text_draw(c, 20, 16, "Open:", 12, 0, th.text);
    cv_round(c, 20, 40, s->w - 40, 34, 6, th.field);
    cv_round_border(c, 20, 40, s->w - 40, 34, 6, 1, th.border);
    if (run_q[0]) text_draw(c, 30, 48, run_q, 12, 0, th.text);
    else text_draw(c, 30, 48, "command...", 12, 0, th.text2);
    int by = s->h - 40;
    int okx = s->w - 20 - 64, cancelx = okx - 72;
    cv_round(c, cancelx, by, 64, 26, 5, mix(th.panel, th.text, 12));
    text_draw(c, cancelx + 10, by + 6, "Cancel", 11, 0, th.text);
    cv_round(c, okx, by, 64, 26, 5, th.accent);
    text_draw(c, okx + 22, by + 6, "OK", 11, 0, th.accent_text);
}
static int click_run(ShellWin *s, int btn, int x, int y) {
    (void)btn;
    int by = s->h - 40, okx = s->w - 20 - 64, cancelx = okx - 72;
    if (in_r(x, y, cancelx, by, 64, 26)) { popup_close(); return 1; }
    if (in_r(x, y, okx, by, 64, 26)) {
        if (run_q[0]) { launch(run_q); run_q[0] = 0; }
        popup_close();
        return 1;
    }
    return 1;
}

/* ---------------- lock screen ---------------- */
static void draw_lock(ShellWin *s) {
    Canvas *c = &s->cv;
    cv_fill(c, 0, 0, s->w, s->h, mix(th.taskbar, 0x000000, 75));
    icon_logo(c, s->w / 2 - 28, s->h / 4 - 28, 56, th.accent, mix(th.taskbar, 0x000000, 75));
    char tt[16]; timestr(tt, sizeof tt);
    int tw = text_w(tt, 64, 1);
    text_draw(c, (s->w - tw) / 2, s->h / 2 - 40, tt, 64, 1, th.text);
    char dd[64]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    strftime(dd, sizeof dd, "%A, %B %d", &tm);
    int dw = text_w(dd, 18, 0);
    text_draw(c, (s->w - dw) / 2, s->h / 2 + 50, dd, 18, 0, th.text);
    const char *hint = "Press any key or click to unlock";
    int hw = text_w(hint, 12, 0);
    text_draw(c, (s->w - hw) / 2, s->h - 80, hint, 12, 0, th.text2);
}
static int click_lock(ShellWin *s, int btn, int x, int y) {
    (void)s; (void)btn; (void)x; (void)y;
    popup_close();
    return 1;
}

/* ---------------- context menu ---------------- */
static int menu_hover = -1;
static void menu_do(int act, int data);
static void s_show_toast(ShellWin *t);
static void redraw_frames(void);

static void menu_open(int x, int y) {
    int w = 200, h = menu_n * 34 + 12;
    for (int i = 0; i < menu_n; i++) {
        int tw = text_w(menu_items[i].label, 12, 0) + 60;
        if (tw > w) w = tw;
    }
    ShellWin *m = sw_get(SW_MENU, 0);
    sw_resize(m, w, h);
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    if (x + w > sw) x = sw - w - 4;
    if (y + h > sh) y = sh - h - 4;
    sw_show(m, x, y);
}
static void menu_add(const char *label, int act, int data) {
    if (menu_n >= MENU_MAX) return;
    strlcpy_(menu_items[menu_n].label, label, 64);
    menu_items[menu_n].act = act;
    menu_items[menu_n].data = data;
    menu_n++;
}
static void menu_open_power(int x, int y) {
    menu_n = 0;
    menu_add("Lock", MI_LOCK, 0);
    menu_add("Sleep", MI_SLEEP, 0);
    menu_add("Restart", MI_RESTART, 0);
    menu_add("Shut down", MI_POWEROFF, 0);
    menu_open(x, y);
}
static void menu_open_win(int x, int y, Client *cl) {
    menu_client = cl;
    menu_n = 0;
    menu_add("Restore", MI_WIN_RESTORE, 0);
    menu_add("Minimize", MI_WIN_MIN, 0);
    menu_add(cl->max ? "Restore down" : "Maximize", MI_WIN_MAX, 0);
    menu_add("Close", MI_WIN_CLOSE, 0);
    menu_open(x, y);
}
static void draw_menu(ShellWin *s) {
    Canvas *c = &s->cv;
    cv_fill(c, 0, 0, s->w, s->h, th.panel);
    cv_round_border(c, 0, 0, s->w, s->h, POP_R, 1, th.border);
    for (int i = 0; i < menu_n; i++) {
        int y = 6 + i * 34;
        if (menu_hover == i) cv_round(c, 6, y, s->w - 12, 30, 4, th.hover);
        text_clip(c, 20, y + 8, s->w - 36, menu_items[i].label, 12, 0, th.text);
    }
}
static int click_menu(ShellWin *s, int btn, int x, int y) {
    (void)btn;
    for (int i = 0; i < menu_n; i++)
        if (in_r(x, y, 0, 6 + i * 34, s->w, 34)) {
            int act = menu_items[i].act, data = menu_items[i].data;
            popup_close();
            menu_do(act, data);
            return 1;
        }
    return 1;
}

/* ---------------- toast ---------------- */
static void toast_show(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast_t, sizeof toast_t, fmt, ap);
    va_end(ap);
    strlcpy_(toast_b, "Losedoze", sizeof toast_b);
    toast_until = now_ms() + 6000;
    ShellWin *t = sw_get(SW_TOAST, 0);
    if (t->w != 340 || t->h != 90) sw_resize(t, 340, 90);
    s_show_toast(t);
}
static void s_show_toast(ShellWin *t) {
    t->x = sw - 340 - 12; t->y = sh - TB_H - 90 - 12;
    t->visible = 1;
    sw_draw(t);
    needs_composite = 1;
}
static void draw_toast(ShellWin *s) {
    Canvas *c = &s->cv;
    cv_fill(c, 0, 0, s->w, s->h, th.panel);
    cv_round_border(c, 0, 0, s->w, s->h, POP_R, 1, th.border);
    icon_logo(c, 14, 16, 24, th.accent, th.panel);
    text_clip(c, 48, 12, s->w - 60, toast_t, 12, 1, th.text);
    text_clip(c, 48, 34, s->w - 60, toast_b, 11, 0, th.text2);
    text_clip(c, 48, 56, s->w - 60, "now", 10, 0, th.text2);
}
static int click_toast(ShellWin *s, int btn, int x, int y) {
    (void)btn; (void)x; (void)y;
    sw_hide(s);
    toast_until = 0;
    (void)s;
    return 1;
}

/* ---------------- snap layout picker ---------------- */
static const int snap_zones[6] = { SNAP_L, SNAP_MAX, SNAP_R, SNAP_TL, SNAP_BL, SNAP_BR };
static int snap_hover = -1;
static void draw_snap(ShellWin *s) {
    Canvas *c = &s->cv;
    cv_fill(c, 0, 0, s->w, s->h, th.panel);
    cv_round_border(c, 0, 0, s->w, s->h, POP_R, 1, th.border);
    for (int i = 0; i < 6; i++) {
        int col = i % 3, row = i / 3;
        int x = 8 + col * 66, y = 8 + row * 44;
        if (snap_hover == i) cv_round(c, x, y, 60, 40, 4, th.hover);
        cv_round_border(c, x + 8, y + 6, 44, 28, 3, 1, th.text2);
        int zx = x + 10, zy = y + 8, zw = 40, zh = 24;
        switch (snap_zones[i]) {
        case SNAP_L: cv_fill(c, zx, zy, zw / 2, zh, th.accent); break;
        case SNAP_R: cv_fill(c, zx + zw / 2, zy, zw / 2, zh, th.accent); break;
        case SNAP_MAX: cv_fill(c, zx, zy, zw, zh, th.accent); break;
        case SNAP_TL: cv_fill(c, zx, zy, zw / 2, zh / 2, th.accent); break;
        case SNAP_BL: cv_fill(c, zx, zy + zh / 2, zw / 2, zh / 2, th.accent); break;
        case SNAP_BR: cv_fill(c, zx + zw / 2, zy + zh / 2, zw / 2, zh / 2, th.accent); break;
        }
    }
}
static int click_snap(ShellWin *s, int btn, int x, int y) {
    (void)btn;
    for (int i = 0; i < 6; i++) {
        int col = i % 3, row = i / 3;
        if (in_r(x, y, 8 + col * 66, 8 + row * 44, 60, 40)) {
            if (snap_client) snap_apply(snap_client, snap_zones[i]);
            popup_close();
            return 1;
        }
    }
    (void)s;
    return 1;
}

/* ---------------- desktop icons ---------------- */
static const char *desk_names[3] = { "This PC", "Home", "Trash" };
static void draw_deskicon(ShellWin *s) {
    Canvas *c = &s->cv;
    /* approximate wallpaper gradient behind the icon (no compositor: opaque fill) */
    unsigned a = wall_presets[cfg_wallpaper % 3][0], b = wall_presets[cfg_wallpaper % 3][1];
    int t0 = s->y * 255 / (sh > 1 ? sh : 1), t1 = (s->y + s->h) * 255 / (sh > 1 ? sh : 1);
    cv_grad(c, 0, 0, s->w, s->h, mix(a, b, t0), mix(a, b, t1));
    int i = s->idx;
    if (deskicon_hover == i || deskicon_sel == i)
        cv_round(c, 4, 4, s->w - 8, s->h - 8, 6, mix(th.taskbar, th.text, dark_mode ? 24 : 14));
    int t = i == 0 ? IC_GLOBE : i == 1 ? IC_FOLDER : IC_BIN;
    icon_draw(c, s->w / 2 - 22, 12, 44, t, i == 2 ? mix(th.text, th.taskbar, 30) : mix(th.accent, 0xffffff, 30));
    const char *nm = desk_names[i];
    int tw = text_w(nm, 11, 0);
    text_draw(c, (s->w - tw) / 2, 66, nm, 11, 0, th.text);
}
static int desk_last_click[3];
static int click_deskicon(ShellWin *s, int btn, int x, int y) {
    (void)x; (void)y;
    int i = s->idx;
    deskicon_sel = i;
    if (btn == 3) {
        menu_n = 0;
        menu_add("Open", MI_RUN, 100 + i);
        if (i == 2) menu_add("Empty Recycle Bin", MI_EMPTY_TRASH, 0);
        menu_add("Properties", MI_ABOUT, 1);
        menu_open(mouse_x, mouse_y);      /* current evdev cursor position */
        return 1;
    }
    unsigned long now = now_ms();
    if (now - (unsigned long)desk_last_click[i] < 450) {
        desk_last_click[i] = 0;
        if (i == 0) files_browse("C:");
        else if (i == 2) {
            char p[512];
            const char *home = getenv("HOME") ? getenv("HOME") : "/root";
            snprintf(p, sizeof p, "%s/.local/share/Trash", home);
            files_browse(p);
        } else {
            const char *home = getenv("HOME") ? getenv("HOME") : "/root";
            files_browse(home);
        }
        return 1;
    }
    desk_last_click[i] = (int)now;
    return 1;
}

/* == PART5: input, events, main == */

static int super_armed;
static time_t last_minute;
static int quit_flag;
static int fifo_fd = -1;

static void snap_dismiss(void) {
    if (!snap_open) return;
    ShellWin *s = sw_get(SW_SNAP, 0);
    sw_hide(s);
    snap_open = 0;
    snap_hover = -1;
}
static void redraw_frames(void) {
    for (Client *c = clients; c; c = c->next) draw_frame_title(c);
}

/* ---------------- menu actions ---------------- */
static void menu_do(int act, int data) {
    switch (act) {
    case MI_LOCK: key_lock(); break;
    case MI_RESTART: launch("reboot -f"); break;
    case MI_POWEROFF: launch("poweroff -f"); break;
    case MI_SLEEP: toast_show("Suspend not wired (no power backend)"); break;
    case MI_ABOUT:
        if (data == 1) toast_show("Item properties not available in this build");
        else toast_show("Losedoze " SHELL_VER " — original desktop shell");
        break;
    case MI_WIN_CLOSE: if (menu_client) close_client(menu_client); break;
    case MI_WIN_MIN: if (menu_client) iconify(menu_client); break;
    case MI_WIN_MAX: if (menu_client) set_max(menu_client, !menu_client->max); break;
    case MI_WIN_RESTORE:
        if (menu_client) {
            set_max(menu_client, 0);
            if (menu_client->min) focus_client(menu_client);
        }
        break;
    case MI_DESK_NEW: toast_show("%d virtual desktops available", NDESK); break;
    case MI_DESK_CLOSE: toast_show("Desktop merge not implemented"); break;
    case MI_DESK_GOTO: desk_switch(data); break;
    case MI_WALLPAPER:
        wallpaper_next();
        redraw_all();
        redraw_frames();
        for (int i = 0; i < 3; i++) sw_draw(sw_get(SW_DESKICON, i));
        toast_show("Wallpaper changed");
        break;
    case MI_THEME:
        dark_mode = !dark_mode;
        set_theme();
        redraw_all();
        redraw_frames();
        break;
    case MI_EXIT: quit_flag = 1; break;
    case MI_EMPTY_TRASH:
        launch("rm -rf \"$HOME/.local/share/Trash/files\"/* \"$HOME/.local/share/Trash/info\"/* 2>/dev/null; true");
        toast_show("Recycle Bin emptied");
        break;
    case MI_RUN:
        if (data >= 100) {
            int i = data - 100;
            const char *home = getenv("HOME") ? getenv("HOME") : "/root";
            if (i == 0) files_browse("C:");
            else if (i == 2) {
                char p[512];
                snprintf(p, sizeof p, "%s/.local/share/Trash", home);
                files_browse(p);
            } else files_browse(home);
        } else key_run_open();
        break;
    }
}

/* ---------------- key actions ---------------- */
static void start_show(void) {
    ShellWin *s = sw_get(SW_START, 0);
    if (s->w != START_W || s->h != START_H) sw_resize(s, START_W, START_H);
    sw_show(s, (sw - START_W) / 2, sh - TB_H - START_H - 8);
}
static void key_start_toggle(void) {
    snap_dismiss();
    ShellWin *s = sw_get(SW_START, 0);
    if (popup == s) { popup_close(); return; }
    start_q[0] = 0; start_allapps = 0; start_sel = -1;
    start_show();
}
static void key_start_open_search(void) {
    snap_dismiss();
    ShellWin *s = sw_get(SW_START, 0);
    if (popup == s) return;
    start_q[0] = 0; start_allapps = 0; start_sel = -1;
    start_show();
}
static void key_quick_toggle(void) {
    snap_dismiss();
    ShellWin *s = sw_get(SW_QUICK, 0);
    if (popup == s) { popup_close(); return; }
    if (s->w != QUICK_W || s->h != QUICK_H) sw_resize(s, QUICK_W, QUICK_H);
    sw_show(s, sw - QUICK_W - 12, sh - TB_H - QUICK_H - 8);
}
static void key_cal_toggle(void) {
    snap_dismiss();
    ShellWin *s = sw_get(SW_CAL, 0);
    if (popup == s) { popup_close(); return; }
    time_t t = time(NULL); struct tm tm;
    localtime_r(&t, &tm);
    cal_m = tm.tm_mon; cal_y = tm.tm_year + 1900;
    if (s->w != CAL_W || s->h != CAL_H) sw_resize(s, CAL_W, CAL_H);
    sw_show(s, sw - CAL_W - 12, sh - TB_H - CAL_H - 8);
}
static void key_run_open(void) {
    snap_dismiss();
    ShellWin *s = sw_get(SW_RUN, 0);
    if (popup == s) { popup_close(); return; }
    run_q[0] = 0;
    if (s->w != RUN_W || s->h != RUN_H) sw_resize(s, RUN_W, RUN_H);
    sw_show(s, (sw - RUN_W) / 2, (sh - TB_H - RUN_H) / 2);
}
static void key_lock(void) {
    popup_close();
    snap_dismiss();
    ShellWin *s = sw_get(SW_LOCK, 0);
    if (s->w != sw || s->h != sh) sw_resize(s, sw, sh);
    sw_show(s, 0, 0);
}
static void key_taskview_open(void) {
    snap_dismiss();
    ShellWin *s = sw_get(SW_TASKVIEW, 0);
    if (popup == s) { popup_close(); return; }
    if (s->w != sw || s->h != sh - TB_H) sw_resize(s, sw, sh - TB_H);
    sw_show(s, 0, 0);
}

/* ---------------- alt-tab ---------------- */
static void alt_build(void) {
    alt_n = 0;
    for (Client *c = clients; c && alt_n < ALT_MAX; c = c->next)
        if (c->desktop == cur_desk) alt_list[alt_n++] = c;
}
static void key_alt_open(int dir) {
    snap_dismiss();
    popup_close();
    alt_build();
    if (!alt_n) return;
    alt_sel = dir > 0 ? 0 : alt_n - 1;
    ShellWin *s = sw_get(SW_ALT, 0);
    int w = 40 + alt_n * 116;
    if (w > sw - 100) w = sw - 100;
    if (w < 300) w = 300;
    if (s->w != w || s->h != 180) sw_resize(s, w, 180);
    sw_show(s, (sw - w) / 2, (sh - 180) / 2 - 40);
}
static void alt_commit(void) {
    if (!popup || popup->type != SW_ALT) return;
    Client *cl = (alt_sel >= 0 && alt_sel < alt_n) ? alt_list[alt_sel] : NULL;
    popup_close();
    if (cl) focus_client(cl);
}

/* ---------------- key routing (evdev codes; replaced X grabs) ---------- */
typedef struct { int down; int code; unsigned mods; u32 ch; } KeyEvent;
/* input_mods(): bit0 shift, bit1 caps, bit2 ctrl, bit3 alt, bit4 logo, bit5 num */
#define M_SHIFT 0x1u
#define M_CTRL  0x4u
#define M_ALT   0x8u
#define M_LOGO  0x10u

static void alt_key(KeyEvent *e) {
    if (!e->down) return;
    if (e->code == KEY_ESC) { popup_close(); return; }
    if (e->code == KEY_TAB) {
        int dir = (e->mods & M_SHIFT) ? -1 : 1;
        if (alt_n) alt_sel = (alt_sel + dir + alt_n) % alt_n;
        sw_draw(sw_get(SW_ALT, 0));
        return;
    }
    if (e->code == KEY_ENTER || e->code == KEY_KPENTER) alt_commit();
}

/* ---------------- popup typing ---------------- */
static void popup_key(KeyEvent *e) {
    ShellWin *s = popup;
    if (!s) return;
    if (!e->down) {
        if (s->type == SW_ALT && (e->code == KEY_LEFTALT || e->code == KEY_RIGHTALT)) alt_commit();
        return;
    }
    if (s->type == SW_LOCK) { popup_close(); return; }
    if (s->type == SW_ALT) { alt_key(e); return; }
    if (s->type == SW_START || s->type == SW_RUN) {
        char *buf = s->type == SW_START ? start_q : run_q;
        size_t cap = s->type == SW_START ? sizeof start_q : sizeof run_q;
        if (e->code == KEY_ESC) { popup_close(); return; }
        if (e->code == KEY_ENTER || e->code == KEY_KPENTER) {
            if (s->type == SW_RUN) {
                if (run_q[0]) { launch(run_q); run_q[0] = 0; }
                popup_close();
                return;
            }
            App *list[64];
            int n = start_q[0] ? start_filtered(list, 8)
                  : start_allapps ? start_all_list(list, 64)
                  : start_filtered(list, 18);
            int sel = (start_sel >= 0 && start_sel < n) ? start_sel : 0;
            if (n > 0) { launch_app(list[sel]); popup_close(); }
            return;
        }
        if (e->code == KEY_BACKSPACE) {
            size_t l = strlen(buf);
            if (l) buf[--l] = 0;
            start_sel = -1;
            sw_draw(s);
            return;
        }
        if (e->code == KEY_UP || e->code == KEY_DOWN || e->code == KEY_LEFT || e->code == KEY_RIGHT) {
            if (s->type != SW_START) return;
            App *list[64];
            int n = start_q[0] ? start_filtered(list, 8)
                  : start_allapps ? start_all_list(list, 64)
                  : start_filtered(list, 18);
            if (!n) return;
            int step = (e->code == KEY_DOWN || e->code == KEY_RIGHT) ? 1 : -1;
            start_sel = (start_sel < 0) ? (step > 0 ? 0 : n - 1)
                                        : (start_sel + step + n) % n;
            sw_draw(s);
            return;
        }
        if (e->ch >= 32 && e->ch < 127 && strlen(buf) + 1 < cap) {
            char ch[2] = { (char)e->ch, 0 };
            if (s->type == SW_START) { start_q[0] = 0; start_allapps = 0; start_sel = -1; }
            strcat(buf, ch);
            sw_draw(s);
        }
        return;
    }
    if (e->code == KEY_ESC) popup_close();
}

/* ---------------- global keys ---------------- */
/* Returns 1 when the key was consumed as a shell shortcut (do not forward). */
static int root_key(KeyEvent *e) {
    if (e->code == KEY_LEFTMETA || e->code == KEY_RIGHTMETA) {
        if (e->down) { super_armed = 1; return 1; }
        if (super_armed) {
            super_armed = 0;
            key_start_toggle();
        }
        return 1;
    }
    if (!e->down) return 0;
    super_armed = 0;
    unsigned st = e->mods & (M_SHIFT | M_CTRL | M_ALT | M_LOGO);
    snap_dismiss();
    if (st == M_LOGO) {
        switch (e->code) {
        case KEY_A: key_quick_toggle(); return 1;
        case KEY_N: key_cal_toggle(); return 1;
        case KEY_S: key_start_open_search(); return 1;
        case KEY_R: key_run_open(); return 1;
        case KEY_L: key_lock(); return 1;
        case KEY_D: show_desktop_toggle(); return 1;
        case KEY_E: files_browse(NULL); return 1;
        case KEY_TAB: key_taskview_open(); return 1;
        case KEY_LEFT: if (focused) snap_apply(focused, SNAP_L); return 1;
        case KEY_RIGHT: if (focused) snap_apply(focused, SNAP_R); return 1;
        case KEY_UP: if (focused && !focused->max) set_max(focused, 1); return 1;
        case KEY_DOWN:
            if (focused) {
                if (focused->max) set_max(focused, 0);
                else iconify(focused);
            }
            return 1;
        }
        return 1;                       /* Super+other: swallow */
    }
    if (st == (M_LOGO | M_CTRL)) {
        if (e->code == KEY_LEFT) { desk_switch((cur_desk + NDESK - 1) % NDESK); return 1; }
        if (e->code == KEY_RIGHT) { desk_switch((cur_desk + 1) % NDESK); return 1; }
        return 1;
    }
    if (st == M_ALT) {
        if (e->code == KEY_F4) { if (focused) close_client(focused); return 1; }
        if (e->code == KEY_TAB) { key_alt_open(1); return 1; }
        return 1;
    }
    if (st == (M_ALT | M_SHIFT) && e->code == KEY_TAB) { key_alt_open(-1); return 1; }
    return 0;
}
/* input_mods() layout -> wl_keyboard xkb depressed mask */
static unsigned wl_mods(unsigned m) {
    return (m & 0x0Fu) | ((m & 0x10u) ? 0x40u : 0u) | ((m & 0x20u) ? 0x80u : 0u);
}

/* ---------------- frame (client window) input ---------------- */
static int drag_mode;   /* 1 move, 2 resize */
static Client *dragc;
static int drag_dir;
static int drag_ox, drag_oy, drag_gx, drag_gy, drag_gw, drag_gh;
static unsigned long last_click_t;
static Client *last_click_c;

enum { FE_PRESS = 1, FE_MOTION, FE_RELEASE };

static void frame_event(Client *c, int type, int sx, int sy, int btn) {
    int x = sx - c->x, y = sy - c->y;   /* frame-local coords */
    switch (type) {
    case FE_PRESS: {
        snap_dismiss();
        int bw = c->w > 300 ? 46 : 0;
        if (bw && y < TITLE_H) {
            if (x >= c->w - bw) { focus_client(c); close_client(c); return; }
            if (x >= c->w - 2 * bw) { focus_client(c); set_max(c, !c->max); return; }
            if (x >= c->w - 3 * bw) { focus_client(c); iconify(c); return; }
        }
        focus_client(c);
        if (btn == 3 && y < TITLE_H) {
            menu_open_win(sx - 80, sy, c);
            return;
        }
        if (btn != 1) return;
        if (y < TITLE_H) {
            unsigned long n = now_ms();
            if (last_click_c == c && n - last_click_t < 450) {
                last_click_t = 0;
                set_max(c, !c->max);
            } else {
                last_click_c = c;
                last_click_t = n;
                if (c->max) {
                    set_max(c, 0);
                    apply_geom(c, sx - c->w / 2, sy - 16, c->w, c->h);
                }
                dragc = c;
                drag_mode = 1;
                drag_ox = sx - c->x;
                drag_oy = sy - c->y;
                cursor_set_shape(CUR_ARROW);
            }
            return;
        }
        int dir = 0;
        if (!c->max) {
            if (x < 6) dir |= 1;
            else if (x > c->w - 6) dir |= 2;
            if (y < 6) dir |= 4;
            else if (y > c->h - 6) dir |= 8;
        }
        if (dir) {
            dragc = c;
            drag_mode = 2;
            drag_dir = dir;
            drag_gx = c->x; drag_gy = c->y; drag_gw = c->w; drag_gh = c->h;
            drag_ox = sx;
            drag_oy = sy;
            cursor_set_shape(((dir & 3) && (dir & 12)) ? CUR_ARROW : ((dir & 3) ? CUR_H : CUR_V));
        }
        return;
    }
    case FE_MOTION: {
        if (dragc == c) {
            if (drag_mode == 1) {
                int nx = sx - drag_ox, ny = sy - drag_oy;
                c->x = nx; c->y = ny;
                wlwin_set_pos(c->wl, nx + FB, ny + TITLE_H);
                needs_composite = 1;
            } else {
                int dx = sx - drag_ox, dy = sy - drag_oy;
                int x2 = drag_gx, y2 = drag_gy, w2 = drag_gw, h2 = drag_gh;
                if (drag_dir & 1) { x2 = drag_gx + dx; w2 = drag_gw - dx; }
                if (drag_dir & 2) { w2 = drag_gw + dx; }
                if (drag_dir & 4) { y2 = drag_gy + dy; h2 = drag_gh - dy; }
                if (drag_dir & 8) { h2 = drag_gh + dy; }
                apply_geom(c, x2, y2, w2, h2);
                needs_composite = 1;
            }
            return;
        }
        int newhb = 0;
        if (y < TITLE_H && c->w > 300 && !c->max) {
            int bw = 46;
            if (x >= c->w - bw) newhb = 3;
            else if (x >= c->w - 2 * bw) newhb = 2;
            else if (x >= c->w - 3 * bw) newhb = 1;
            if (newhb == 2 && !popup && !snap_open) {
                ShellWin *sp = sw_get(SW_SNAP, 0);
                if (sp->w != 210 || sp->h != 104) sw_resize(sp, 210, 104);
                snap_client = c;
                snap_open = 1;
                int sxx = c->x + c->w - 46 * 2 - 105, syy = c->y + TITLE_H + 2;
                if (sxx < 4) sxx = 4;
                if (syy < 4) syy = 4;
                sw_show(sp, sxx, syy);
            }
        }
        if (newhb != frame_hover_btn) {
            frame_hover_btn = newhb;
            draw_frame_title(c);
        }
        return;
    }
    case FE_RELEASE:
        if (dragc == c) {
            if (drag_mode == 1) {
                if (sy <= 2) snap_apply(c, SNAP_MAX);
                else if (sx <= 2 && sy < sh / 3) snap_apply(c, SNAP_TL);
                else if (sx <= 2 && sy > sh * 2 / 3) snap_apply(c, SNAP_BL);
                else if (sx >= sw - 3 && sy < sh / 3) snap_apply(c, SNAP_TR);
                else if (sx >= sw - 3 && sy > sh * 2 / 3) snap_apply(c, SNAP_BR);
                else if (sx <= 2) snap_apply(c, SNAP_L);
                else if (sx >= sw - 3) snap_apply(c, SNAP_R);
            }
            dragc = NULL;
        }
        return;
    default:
        return;
    }
}

/* ---------------- notifications fifo ---------------- */
static void notify_open(void) {
    mkdir("/run/losedoze", 0755);
    if (mkfifo(NOTIFY_FIFO, 0666) < 0 && errno != EEXIST)
        logmsg("mkfifo: %s", strerror(errno));
    fifo_fd = open(NOTIFY_FIFO, O_RDONLY | O_NONBLOCK);
}
static void notify_read(void) {
    static char acc[8192];
    static int have;
    char tmp[1024];
    for (;;) {
        ssize_t r = read(fifo_fd, tmp, sizeof tmp);
        if (r <= 0) break;
        if (have + (int)r > (int)sizeof acc - 1) have = 0;
        memcpy(acc + have, tmp, (size_t)r);
        have += (int)r;
        acc[have] = 0;
    }
    char *nl;
    while ((nl = strchr(acc, '\n'))) {
        *nl = 0;
        if (acc[0]) {
            notif_add(acc);
            toast_show("%s", acc);
        }
        memmove(acc, nl + 1, strlen(nl + 1) + 1);
        have = (int)strlen(acc);
    }
}

/* ---------------- pointer/key routing (replaces X dispatch) ------------- */
static void snap_dismiss_if_other(ShellWin *s);

/* topmost client frame containing screen point */
static Client *client_at(int sx, int sy) {
    for (Client *c = clients; c; c = c->next)
        if (c->mapped && !c->min && c->desktop == cur_desk &&
            in_r(sx, sy, c->x, c->y, c->w, c->h))
            return c;
    return NULL;
}
static int client_in_content(Client *c, int sx, int sy) {
    int x = sx - c->x, y = sy - c->y;
    return !c->full && x >= FB && y >= TITLE_H && x < c->w - FB && y < c->h - FB;
}
/* visible popup-type surface containing point (or the active popup) */
static ShellWin *shell_hit_popup(int sx, int sy) {
    if (popup && popup->visible && in_r(sx, sy, popup->x, popup->y, popup->w, popup->h))
        return popup;
    for (ShellWin *s = swins; s; s = s->next)
        if (s->visible && s != popup && in_r(sx, sy, s->x, s->y, s->w, s->h) &&
            (s->type == SW_SNAP || s->type == SW_TOAST))
            return s;
    return NULL;
}
/* non-popup shell surfaces: taskbar, deskicons */
static ShellWin *shell_hit_normal(int sx, int sy) {
    for (ShellWin *s = swins; s; s = s->next)
        if (s->visible && (s->type == SW_TASKBAR || s->type == SW_DESKICON) &&
            in_r(sx, sy, s->x, s->y, s->w, s->h))
            return s;
    return NULL;
}
static void shell_hover(ShellWin *s, int x, int y) { /* ported hover logic */
    if (s->type == SW_TASKBAR) {
        TBItem it[TB_MAXI];
        int n = tb_build(it), key = -1;
        for (int i = 0; i < n; i++)
            if (in_r(x, y, it[i].x, 0, it[i].w, TB_H))
                key = it[i].kind * 100 + (int)it[i].id;
        if (key != tb_hover_key) { tb_hover_key = key; sw_draw(s); }
    } else if (s->type == SW_QUICK && qdrag) {
        int v = (x - 56) * 100 / (s->w - 110);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        if (qdrag == 1) cfg_vol = v;
        else { cfg_bright = v; brightness_set(v); }
        sw_draw(s);
    } else if (s->type == SW_MENU) {
        int hv = -1;
        for (int i = 0; i < menu_n; i++)
            if (in_r(x, y, 0, 6 + i * 34, s->w, 34)) hv = i;
        if (hv != menu_hover) { menu_hover = hv; sw_draw(s); }
    } else if (s->type == SW_SNAP) {
        int hv = -1;
        for (int i = 0; i < 6; i++) {
            int col = i % 3, row = i / 3;
            if (in_r(x, y, 8 + col * 66, 8 + row * 44, 60, 40)) hv = i;
        }
        if (hv != snap_hover) { snap_hover = hv; sw_draw(s); }
    }
}

static void on_motion(int sx, int sy) {
    mouse_x = sx; mouse_y = sy;
    if (dragc) { frame_event(dragc, FE_MOTION, sx, sy, 0); return; }
    ShellWin *s = shell_hit_popup(sx, sy);
    if (!s) s = shell_hit_normal(sx, sy);
    if (s) {
        shell_hover(s, sx - s->x, sy - s->y);
    } else {
        Client *c = client_at(sx, sy);
        if (c && !client_in_content(c, sx, sy)) frame_event(c, FE_MOTION, sx, sy, 0);
        else if (!c || client_in_content(c, sx, sy)) {
            if (!dragc) cursor_set_shape(CUR_ARROW);
        }
    }
    wlserver_pointer_motion(wl, sx, sy);   /* picks surface, sends enter/leave/local motion */
}

static void on_press(int btn, int sx, int sy) {
    mouse_x = sx; mouse_y = sy;
    ShellWin *s = shell_hit_popup(sx, sy);
    if (s) {
        if (popup && popup != s && s->type != SW_SNAP && s->type != SW_TOAST) popup_close();
        snap_dismiss_if_other(s);
        sw_click(s, btn, sx - s->x, sy - s->y);
        return;
    }
    if (popup) { popup_close(); return; }      /* click outside active popup */
    snap_dismiss();
    s = shell_hit_normal(sx, sy);
    if (s) { sw_click(s, btn, sx - s->x, sy - s->y); return; }
    Client *c = client_at(sx, sy);
    if (c) {
        if (client_in_content(c, sx, sy)) {
            focus_client(c);
            wlserver_pointer_button(wl, btn, 1);
            return;
        }
        frame_event(c, FE_PRESS, sx, sy, btn);
        return;
    }
    /* desktop root click */
    if (focused) {
        focused = NULL;
        for (Client *cc = clients; cc; cc = cc->next) wlwin_set_activated(cc->wl, 0);
        redraw_frames();
        taskbar_dirty();
    }
    deskicon_sel = -1;
}

static void on_release(int btn, int sx, int sy) {
    mouse_x = sx; mouse_y = sy;
    if (dragc) { frame_event(dragc, FE_RELEASE, sx, sy, btn); return; }
    ShellWin *s = shell_hit_popup(sx, sy);
    if (s && s->type == SW_QUICK && qdrag) {
        qdrag = 0;
        cfg_save();
        sw_draw(s);
        return;
    }
    Client *c = client_at(sx, sy);
    if (c && client_in_content(c, sx, sy)) wlserver_pointer_button(wl, btn, 0);
}

static void on_key(KeyEvent *e) {
    if (popup && popup_kbd) { popup_key(e); return; }
    if (root_key(e)) return;
    if (!popup && focused && focused->mapped && !focused->min)
        wlserver_key(wl, e->code, e->down, wl_mods(e->mods));
}
static void snap_dismiss_if_other(ShellWin *s) {
    ShellWin *sp = sw_get(SW_SNAP, 0);
    if (snap_open && s != sp) snap_dismiss();
}

/* ---------------- main ---------------- */
static void on_chld(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
}

/* One runnable check: pure logic + font metrics, no X server needed. */
static int selftest(void) {
    int fail = 0;
#define CHK(cond, name) do { if (!(cond)) { fprintf(stderr, "selftest FAIL: %s\n", name); fail = 1; } } while (0)
    CHK(load_fonts(), "fonts load (Liberation Sans)");
    CHK(in_round(40, 1, 0, 0, 80, 40, 8), "rounded rect top-center inside");
    CHK(!in_round(0, 0, 0, 0, 80, 40, 8), "rounded rect TL corner cut");
    CHK(in_round(0, 20, 0, 0, 80, 40, 8), "rounded rect left-middle inside");
    CHK(in_round(10, 10, 0, 0, 80, 40, 0), "square rect accepts corner");
    CHK(isleap(2000) && !isleap(1900) && isleap(2024) && !isleap(2023), "isleap rules");
    CHK(dim(2024, 1) == 29 && dim(2023, 1) == 28 && dim(2024, 3) == 30 && dim(2024, 11) == 31,
        "days per month");
    int wr = text_w("Losedoze 11", 13, 0);
    CHK(wr > 50 && wr < 200, "regular text width sane");
    CHK(text_w("MMM", 13, 1) >= text_w("MMM", 13, 0), "bold width >= regular");
    CHK(text_w("WW", 13, 0) > text_w("ii", 13, 0), "glyph advances differ");
    CHK(app_color("Files") == app_color("Files"), "app_color deterministic");
    const char *p = "héllo";
    CHK(utf8_next(&p) == 'h' && utf8_next(&p) == 0xe9 && *p == 'l', "utf8 decode");
    CHK(mix(0xffffff, 0x000000, 128) == mix(0xffffff, 0x000000, 128) &&
        mix(0xffffff, 0x000000, 0) == 0xffffff && mix(0xffffff, 0x000000, 255) == 0,
        "color mix endpoints");
#undef CHK
    fprintf(stderr, "selftest %s\n", fail ? "FAIL" : "OK");
    return fail;
}

/* ---------------- composite + present ---------------- */
/* Copy src canvas over dst at dx,dy (top byte 0 = transparent key, blend else). */
static void blit_canvas(Canvas *d, const Canvas *s, int dx, int dy) {
    for (int y = 0; y < s->h; y++) {
        int Y = dy + y;
        if (Y < 0 || Y >= d->h) continue;
        const u32 *srow = s->px + (size_t)y * s->stride;
        u32 *drow = d->px + (size_t)Y * d->stride + (dx < 0 ? 0 : dx);
        int x0 = dx < 0 ? -dx : 0;
        int x1 = dx + s->w > d->w ? d->w - dx : s->w;
        for (int x = x0; x < x1; x++) {
            u32 v = srow[x];
            unsigned a = v >> 24;
            if (!a) continue;
            if (a == 255) { drow[x] = v; continue; }
            u32 bg = drow[x];
            unsigned r = (((v >> 16) & 255) * a + ((bg >> 16) & 255) * (255 - a)) / 255;
            unsigned g = (((v >> 8) & 255) * a + ((bg >> 8) & 255) * (255 - a)) / 255;
            unsigned b = ((v & 255) * a + (bg & 255) * (255 - a)) / 255;
            drow[x] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
    }
}

static void composite(void) {
    Canvas *sc = card_shadow(card);
    cursor_erase();
    /* 1. wallpaper (opaque full-screen copy) */
    int wrow = wall.w < sc->w ? wall.w : sc->w;
    for (int y = 0; y < wall.h && y < sc->h; y++)
        memcpy(sc->px + (size_t)y * sc->stride, wall.px + (size_t)y * wall.stride, (size_t)wrow * 4);
    /* 2. desktop icons + taskbar */
    for (ShellWin *s = swins; s; s = s->next)
        if (s->visible && (s->type == SW_TASKBAR || s->type == SW_DESKICON))
            blit_canvas(sc, &s->cv, s->x, s->y);
    /* 3. client windows, bottom-up (clients list head = topmost) */
    Client *arr[64];
    int ncl = 0;
    for (Client *c = clients; c && ncl < 64; c = c->next)
        if (c->mapped && !c->min && c->desktop == cur_desk && c->fc.px) arr[ncl++] = c;
    for (int i = ncl - 1; i >= 0; i--) {
        Client *c = arr[i];
        /* frame chrome with rounded-corner cut (alpha key below corners) */
        for (int y = 0; y < c->h; y++) {
            int Y = c->y + y;
            if (Y < 0 || Y >= sc->h) break;
            int x0 = c->x < 0 ? -c->x : 0;
            int x1 = c->x + c->w > sc->w ? sc->w - c->x : c->w;
            u32 *drow = sc->px + (size_t)Y * sc->stride + (c->x < 0 ? 0 : c->x);
            const u32 *srow = c->fc.px + (size_t)y * c->fc.stride;
            if (y >= FRAME_R && y < c->h - FRAME_R)
                memcpy(drow, srow + x0, (size_t)(x1 - x0) * 4);
            else
                for (int x = x0; x < x1; x++)
                    if (in_round(x, y, 0, 0, c->w, c->h, FRAME_R)) drow[x] = srow[x];
        }
        /* client content straight from its shm buffer */
        int st4, bw2, bh2;
        const u32 *cp = wlwin_pixels(c->wl, &st4, &bw2, &bh2);
        if (cp) {
            int dx = c->x + FB, dy = c->y + TITLE_H;
            for (int y = 0; y < bh2; y++) {
                int Y = dy + y;
                if (Y < 0 || Y >= sc->h) break;
                const u32 *srow = cp + (size_t)y * st4;
                u32 *drow = sc->px + (size_t)Y * sc->stride + (dx < 0 ? 0 : dx);
                int x0 = dx < 0 ? -dx : 0;
                int x1 = dx + bw2 > sc->w ? sc->w - dx : bw2;
                if (!c->wl->alpha) { memcpy(drow, srow + x0, (size_t)(x1 - x0) * 4); continue; }
                for (int x = x0; x < x1; x++) {
                    u32 v = srow[x];
                    unsigned a = v >> 24;
                    if (!a) continue;
                    if (a == 255) { drow[x] = v; continue; }
                    u32 bg = drow[x];
                    unsigned r = (((v >> 16) & 255) * a + ((bg >> 16) & 255) * (255 - a)) / 255;
                    unsigned g = (((v >> 8) & 255) * a + ((bg >> 8) & 255) * (255 - a)) / 255;
                    unsigned b = ((v & 255) * a + (bg & 255) * (255 - a)) / 255;
                    drow[x] = 0xff000000u | (r << 16) | (g << 8) | b;
                }
            }
        }
    }
    /* 3b. xdg_popup client surfaces (menus/combos), bottom-up above parents */
    {
        WlWin *parr[32];
        int np = 0;
        for (WlWin *w = wlserver_wins(wl); w && np < 32; w = wlwin_next(w))
            if (w->popup && w->mapped && w->px) parr[np++] = w;
        for (int i = np - 1; i >= 0; i--) {
            WlWin *w = parr[i];
            int st4, bw2, bh2;
            const u32 *cp = wlwin_pixels(w, &st4, &bw2, &bh2);
            if (!cp) continue;
            int dx = w->x, dy = w->y;
            for (int y = 0; y < bh2; y++) {
                int Y = dy + y;
                if (Y < 0 || Y >= sc->h) break;
                const u32 *srow = cp + (size_t)y * st4;
                u32 *drow = sc->px + (size_t)Y * sc->stride + (dx < 0 ? 0 : dx);
                int x0 = dx < 0 ? -dx : 0;
                int x1 = dx + bw2 > sc->w ? sc->w - dx : bw2;
                if (!w->alpha) { memcpy(drow, srow + x0, (size_t)(x1 - x0) * 4); continue; }
                for (int x = x0; x < x1; x++) {
                    u32 v = srow[x];
                    unsigned a = v >> 24;
                    if (!a) continue;
                    if (a == 255) { drow[x] = v; continue; }
                    u32 bg = drow[x];
                    unsigned r = (((v >> 16) & 255) * a + ((bg >> 16) & 255) * (255 - a)) / 255;
                    unsigned g = (((v >> 8) & 255) * a + ((bg >> 8) & 255) * (255 - a)) / 255;
                    unsigned b = ((v & 255) * a + (bg & 255) * (255 - a)) / 255;
                    drow[x] = 0xff000000u | (r << 16) | (g << 8) | b;
                }
            }
        }
    }
    /* 4. popups/toast/snap, active popup last */
    for (ShellWin *s = swins; s; s = s->next)
        if (s->visible && s != popup && s->type != SW_TASKBAR && s->type != SW_DESKICON)
            blit_canvas(sc, &s->cv, s->x, s->y);
    if (popup && popup->visible) blit_canvas(sc, &popup->cv, popup->x, popup->y);
    /* 5. cursor sprite */
    cursor_stamp();
    /* buffers copied: release them + fire frame callbacks */
    for (Client *c = clients; c; c = c->next)
        if (c->mapped) wlserver_ack_paint(wl, c->wl);
    for (WlWin *w = wlserver_wins(wl); w; w = wlwin_next(w))
        if (w->popup && w->mapped && w->px) wlserver_ack_paint(wl, w);
}

static void present(void) {
    if (needs_composite) composite();
    if (use_gpu) card_gpu_frame(card, 0, 0, sw, sh, (unsigned)now_ms());
    card_flip(card);
    needs_composite = 0;
}

/* ---------------- wayland client callbacks ---------------- */
static void cb_mapped(void *ud, WlWin *w) { (void)ud; manage(w); }
static void cb_unmapped(void *ud, WlWin *w) {
    (void)ud;
    Client *c = client_of_wl(w);
    if (c) unmanage(c, 1);
    else needs_composite = 1;                      /* popup gone */
}
static void cb_title(void *ud, WlWin *w) {
    (void)ud;
    Client *c = client_of_wl(w);
    if (c) { get_title_class(c); draw_frame_title(c); taskbar_dirty(); }
}
static void cb_resized(void *ud, WlWin *w, int bw, int bh) {
    (void)ud;
    if (!client_of_wl(w)) { needs_composite = 1; return; }
    client_resized(w, bw, bh);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest();
    signal(SIGCHLD, on_chld);
    signal(SIGPIPE, SIG_IGN);
    lg = fopen(LOGPATH, "a");
    setvbuf(stdout, NULL, _IOLBF, 0);

    card = card_open();
    if (!card) { fprintf(stderr, "loseshell: no KMS device\n"); return 1; }
    if (card_modeset(card, 0, 0) != 0) { fprintf(stderr, "loseshell: modeset failed\n"); return 1; }
    sw = card_fbwidth(card);
    sh = card_fbheight(card);
    use_gpu = card_egl_init(card) == 0 && card_gpu_ok(card);
    logmsg("display: %dx%d backend=%s driver=%s gpu=%s", sw, sh,
           card_backend(card), card_driver(card), use_gpu ? card_gpu_renderer(card) : "none");

    input_set_screen(sw, sh);
    int ifds[16];
    int ninputs = input_open(ifds, 16);
    logmsg("input: %d evdev device(s)", ninputs);

    wl = wlserver_create("losedoze0");
    if (!wl) { fprintf(stderr, "loseshell: wayland server failed\n"); return 1; }
    struct wlserver_cbs cb = { .mapped = cb_mapped, .unmapped = cb_unmapped,
                               .title = cb_title, .resized = cb_resized };
    wlserver_set_cbs(wl, &cb);
    setenv("XDG_RUNTIME_DIR", "/run", 1);
    setenv("WAYLAND_DISPLAY", wlserver_socket_path(wl), 1);

    init_masks();
    set_theme();
    if (!load_fonts())
        logmsg("WARNING: Liberation Sans missing; text rendering disabled");
    cfg_load();
    set_theme();
    make_cursors();
    set_root_cursor();
    wallpaper_set();

    ShellWin *tb = sw_get(SW_TASKBAR, 0);
    sw_resize(tb, sw, TB_H);
    sw_show(tb, 0, sh - TB_H);
    for (int i = 0; i < 3; i++) {
        ShellWin *ic = sw_get(SW_DESKICON, i);
        sw_resize(ic, 100, 110);
        sw_show(ic, 12, 12 + i * 118);
    }

    apps_load();
    {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        cal_m = tm.tm_mon;
        cal_y = tm.tm_year + 1900;
        last_minute = t / 60;
    }
    notify_open();
    needs_composite = 1;
    present();

    /* smoke-test markers on the console */
    {
        int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
        if (fd >= 0) {
            dprintf(fd, "EGL: %s\n", use_gpu ? card_gpu_renderer(card) : "none");
            ssize_t w = write(fd, "LOSEDOZE READY\n", 15);
            (void)w;
            close(fd);
        }
    }
    logmsg("loseshell up: %dx%d dark=%d gpu=%d", sw, sh, dark_mode, use_gpu);

    unsigned prev_btns = 0;
    while (!quit_flag) {
        struct pollfd pfd[20];
        int map[20];
        int nf = 0;
        for (int i = 0; i < ninputs && nf < 17; i++) {
            map[nf] = -1 - i;              /* negative: input device index */
            pfd[nf].fd = ifds[i];
            pfd[nf].events = POLLIN;
            nf++;
        }
        map[nf] = -100;                    /* wayland server */
        pfd[nf].fd = wlserver_fd(wl);
        pfd[nf].events = POLLIN;
        nf++;
        if (fifo_fd >= 0) {
            map[nf] = -200;                /* notification fifo */
            pfd[nf].fd = fifo_fd;
            pfd[nf].events = POLLIN;
            nf++;
        }
        int any_mapped = 0;
        for (Client *c = clients; c; c = c->next)
            if (c->mapped && !c->min) any_mapped = 1;
        int to = any_mapped ? 16 : 1000;
        unsigned long n = now_ms();
        if (toast_until) {
            if (n >= toast_until) {
                toast_until = 0;
                sw_hide(sw_get(SW_TOAST, 0));
            } else {
                int left = (int)(toast_until - n) + 1;
                if (left < to) to = left;
            }
        }
        int r = poll(pfd, (nfds_t)nf, to);
        if (r < 0) {
            if (errno == EINTR) continue;
            logmsg("poll: %s", strerror(errno));
            break;
        }
        int cursor_moved = 0;
        for (int i = 0; i < nf; i++) {
            if (!(pfd[i].revents & (POLLIN | POLLHUP))) continue;
            if (map[i] == -100) { wlserver_pump(wl); continue; }
            if (map[i] == -200) { notify_read(); continue; }
            int fd = ifds[-1 - map[i]];
            KeyInput keys[32];
            int nk = 0;
            MouseInput m;
            memset(&m, 0, sizeof m);
            if (input_read(fd, keys, &nk, 32, &m) != 0) continue;
            for (int k = 0; k < nk; k++) {
                unsigned mods = input_mods();
                KeyEvent ke = { .down = keys[k].pressed != 0, .code = keys[k].code,
                                .mods = mods, .ch = input_key_char(keys[k].code, mods & 1) };
                on_key(&ke);
            }
            if (m.changed) {
                int nx = mouse_x, ny = mouse_y;
                if (m.has_abs) { nx = m.abs_x; ny = m.abs_y; }
                else { nx += m.dx; ny += m.dy; }
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx >= sw) nx = sw - 1;
                if (ny >= sh) ny = sh - 1;
                cursor_set_pos(nx, ny);
                cursor_moved = 1;
                on_motion(nx, ny);
            }
            if (m.dw) wlserver_pointer_axis(wl, m.dw);
            /* button transitions (device bit0 left, bit1 right, bit2 middle) */
            static const int bmap[3] = { 1, 3, 2 };   /* shell button numbers */
            unsigned diff = (prev_btns ^ (unsigned)m.buttons) & 7u;
            for (int b = 0; b < 3; b++)
                if (diff & (1u << b)) {
                    if (m.buttons & (1u << b)) on_press(bmap[b], mouse_x, mouse_y);
                    else on_release(bmap[b], mouse_x, mouse_y);
                }
            prev_btns = (unsigned)m.buttons & 7u;
        }
        time_t t = time(NULL);
        if (t / 60 != last_minute) {
            last_minute = t / 60;
            taskbar_dirty();
            if (popup && popup->type == SW_CAL) sw_draw(popup);
        }
        wlserver_pump(wl);   /* flush pending every iteration (no POLLOUT wiring) */
        if (needs_composite) present();
        else if (cursor_moved) { cursor_stamp(); card_flip(card); }
        else if (any_mapped) card_flip(card);   /* vblank pacing for clients */
    }

    logmsg("loseshell exiting");
    card_close(card);
    if (lg) fclose(lg);
    return 0;
}



