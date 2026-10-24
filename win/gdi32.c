/* gdi32.c — homemade GDI for the losedoze Win32 subsystem.
 * Draws into ARGB window buffers behind HDCs (W32Canvas). Text via
 * stb_truetype + Liberation Sans (OFL), same rasterizer as loseshell.
 * Original code. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define GDI_MAGIC 0x33444947u

static W32GdiObj stocks[] = {
    { GDI_MAGIC, GDI_BRUSH, W32RGB(255, 255, 255), 0, 0 }, /* WHITE_BRUSH 0 */
    { GDI_MAGIC, GDI_BRUSH, W32RGB(192, 192, 192), 0, 0 }, /* LTGRAY 1 */
    { GDI_MAGIC, GDI_BRUSH, W32RGB(128, 128, 128), 0, 0 }, /* GRAY 2 */
    { GDI_MAGIC, GDI_BRUSH, W32RGB(64, 64, 64), 0, 0 },    /* DKGRAY 3 */
    { GDI_MAGIC, GDI_BRUSH, W32RGB(0, 0, 0), 0, 0 },       /* BLACK_BRUSH 4 */
    { GDI_MAGIC, GDI_BRUSH, 0, 0, 0 },                     /* NULL_BRUSH 5 */
    { GDI_MAGIC, GDI_PEN, W32RGB(255, 255, 255), PS_SOLID, 1 },
    { GDI_MAGIC, GDI_PEN, W32RGB(0, 0, 0), PS_SOLID, 1 },
};

W32GdiObj *gdi_stock(int idx) {
  if (idx < 0 || idx >= (int)(sizeof stocks / sizeof stocks[0])) return NULL;
  return &stocks[idx];
}

static W32GdiObj *obj_of(HGDIOBJ h) {
  W32GdiObj *o = (W32GdiObj *)h;
  return o && o->magic == GDI_MAGIC ? o : NULL;
}

static W32Canvas *cv(HDC dc) { return (W32Canvas *)dc; }

/* COLORREF 0x00bbggrr -> pixel 0xffRRGGBB */
static W32 cr_px(W32 c) {
  return 0xff000000u | ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255);
}

static void put(W32Canvas *c, int x, int y, W32 px) {
  if ((unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h &&
      (unsigned)x >= (unsigned)c->clip.left && (unsigned)y >= (unsigned)c->clip.top &&
      (unsigned)x < (unsigned)c->clip.right && (unsigned)y < (unsigned)c->clip.bottom)
    c->px[(size_t)y * c->stride + x] = px;
}

/* ---------------- font (stb_truetype, Liberation Sans) ---------------- */
static stbtt_fontinfo font;
static unsigned char *font_buf;
static int font_ok, font_tried;
static float font_scale(W32Canvas *c) { return stbtt_ScaleForPixelHeight(&font, c && c->font_px > 0 ? c->font_px : 16.0f); }

/* SetFontSize: managed Font.Size -> GDI text rasterization height (px). */
BOOL SetFontSize(HDC dc, float px) {
  W32Canvas *c = cv(dc);
  if (!c) return W32_FALSE;
  c->font_px = px > 0 ? px : 16.0f;
  return W32_TRUE;
}

static int font_load(void) {
  if (font_tried) return font_ok;
  font_tried = 1;
  const char *paths[] = {
      "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
      "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
      NULL };
  for (int i = 0; paths[i]; i++) {
    FILE *f = fopen(paths[i], "rb");
    if (!f) continue;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 1024) { fclose(f); continue; }
    unsigned char *buf = malloc((size_t)n);
    if (!buf) { fclose(f); continue; }
    if (fread(buf, 1, (size_t)n, f) == (size_t)n &&
        stbtt_InitFont(&font, buf, stbtt_GetFontOffsetForIndex(buf, 0))) {
      fclose(f);
      font_buf = buf;
      font_ok = 1;
      return 1;
    }
    free(buf);
    fclose(f);
  }
  return 0;
}

void gdi_textout(W32Canvas *c, int x, int y, const W16 *s, int n) {
  if (!c || !c->px || n <= 0) return;
  if (!font_load()) return;
  float scale = font_scale(c);
  int asc, desc, gap;
  stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
  int base = y + (int)(asc * scale);
  int cx = x;
  W32 fgpx = cr_px(c->textcol);
  W32 bkpx = cr_px(c->bkcol);
  for (int i = 0; i < n; i++) {
    unsigned cp = s[i];
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < n && s[i + 1] >= 0xdc00 &&
        s[i + 1] <= 0xdfff) {
      cp = 0x10000 + ((cp - 0xd800) << 10) + (s[i + 1] - 0xdc00);
      i++;
    }
    int w = 0, h = 0, xo, yo;
    unsigned char *bm =
        stbtt_GetCodepointBitmap(&font, 0, scale, (int)cp, &w, &h, &xo, &yo);
    if (bm && c->bkmode == OPAQUE) { /* opaque bg first */
      int gx = cx + xo, gy = base + yo;
      for (int yy = -1; yy <= h; yy++)
        for (int xx = -1; xx <= w; xx++)
          put(c, gx + xx, gy + yy, bkpx);
    }
    if (bm) {
      int gx = cx + xo, gy = base + yo;
      for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
          unsigned a = bm[(size_t)yy * w + xx];
          if (!a) continue;
          int X = gx + xx, Y = gy + yy;
          if ((unsigned)X >= (unsigned)c->w || (unsigned)Y >= (unsigned)c->h) continue;
          if ((unsigned)X < (unsigned)c->clip.left || (unsigned)Y < (unsigned)c->clip.top ||
              (unsigned)X >= (unsigned)c->clip.right || (unsigned)Y >= (unsigned)c->clip.bottom)
            continue;
          W32 dst = c->px[(size_t)Y * c->stride + X];
          if (a == 255) { c->px[(size_t)Y * c->stride + X] = fgpx; continue; }
          unsigned r = ((fgpx >> 16 & 255) * a + (dst >> 16 & 255) * (255 - a)) / 255;
          unsigned g = ((fgpx >> 8 & 255) * a + (dst >> 8 & 255) * (255 - a)) / 255;
          unsigned b = ((fgpx & 255) * a + (dst & 255) * (255 - a)) / 255;
          c->px[(size_t)Y * c->stride + X] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
      free(bm);
    }
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&font, (int)cp, &adv, &lsb);
    cx += (int)(adv * scale);
  }
}

/* text metrics: advance width + line height via stb_truetype (same font,
 * same 16 px scale the rasterizer uses) */
BOOL GetTextExtentPoint32W(HDC dc, LPCWSTR s, int n, SIZE *out) {
  W32Canvas *c = cv(dc);
  if (!out) return W32_FALSE;
  if (!s) return W32_FALSE;
  if (n < 0) n = lstrlenW(s);
  if (n <= 0) { out->cx = 0; out->cy = 0; return W32_TRUE; }
  if (!font_load()) { out->cx = 8 * n; out->cy = 16; return W32_TRUE; }
  float scale = font_scale(c);
  int asc, desc, gap;
  stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
  int cx = 0;
  for (int i = 0; i < n; i++) {
    unsigned cp = s[i];
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < n && s[i + 1] >= 0xdc00 &&
        s[i + 1] <= 0xdfff) {
      cp = 0x10000 + ((cp - 0xd800) << 10) + (s[i + 1] - 0xdc00);
      i++;
    }
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&font, (int)cp, &adv, &lsb);
    cx += (int)(adv * scale);
  }
  out->cx = cx;
  out->cy = (int)((asc - desc) * scale);
  return W32_TRUE;
}

/* ---------------- text out ---------------- */
BOOL TextOutW(HDC dc, int x, int y, LPCWSTR s, int n) {
  W32Canvas *c = cv(dc);
  if (!c || !s) return W32_FALSE;
  if (n < 0) n = lstrlenW(s);
  gdi_textout(c, x, y, s, n);
  return W32_TRUE;
}
BOOL TextOutA(HDC dc, int x, int y, LPCSTR s, int n) {
  W16 w[512];
  if (n < 0) n = (int)strlen(s);
  if (n > 511) n = 511;
  for (int i = 0; i < n; i++) w[i] = (W8)s[i];
  return TextOutW(dc, x, y, w, n);
}

/* ---------------- fills / shapes ---------------- */
static void fill_rect(W32Canvas *c, int x, int y, int w, int h, W32 pxcol) {
  int x2 = x + w, y2 = y + h;
  if (x < c->clip.left) x = c->clip.left;
  if (y < c->clip.top) y = c->clip.top;
  if (x2 > c->clip.right) x2 = c->clip.right;
  if (y2 > c->clip.bottom) y2 = c->clip.bottom;
  if (x < 0) { x2 += -x; x = 0; }
  if (y < 0) { y2 += -y; y = 0; }
  if (x2 > c->w) x2 = c->w;
  if (y2 > c->h) y2 = c->h;
  int w2 = x2 - x, h2 = y2 - y;
  if (w2 <= 0 || h2 <= 0) return;
  for (int j = 0; j < h2; j++) {
    W32 *row = c->px + (size_t)(y + j) * c->stride + x;
    for (int i = 0; i < w2; i++) row[i] = pxcol;
  }
}

int FillRect(HDC dc, const RECT *r, HBRUSH br) {
  W32Canvas *c = cv(dc);
  W32GdiObj *b = obj_of(br);
  if (!c || !r || !b) return 0;
  if (b == gdi_stock(NULL_BRUSH)) return 1;   /* NULL brush = no fill */
  fill_rect(c, r->left, r->top, r->right - r->left, r->bottom - r->top,
            cr_px(b->color));
  return 1;
}

BOOL Rectangle(HDC dc, int l, int t, int r, int b) {
  W32Canvas *c = cv(dc);
  if (!c) return W32_FALSE;
  if (c->brush && c->brush != gdi_stock(NULL_BRUSH))
    fill_rect(c, l + 1, t + 1, r - l - 1, b - t - 1, cr_px(c->brush->color));
  W32 pen = c->pen ? cr_px(c->pen->color) : cr_px(W32RGB(0, 0, 0));
  fill_rect(c, l, t, r - l, 1, pen);
  fill_rect(c, l, b - 1, r - l, 1, pen);
  fill_rect(c, l, t, 1, b - t, pen);
  fill_rect(c, r - 1, t, 1, b - t, pen);
  return W32_TRUE;
}

/* pen position: stored in the canvas' spare field (low 32 bits, x|y<<16) */
static void pen_pos(W32Canvas *c, int *x, int *y) {
  W64 v = (W64)c->font;
  int px_ = (int)(v & 0xffff), py_ = (int)((v >> 16) & 0xffff);
  if (px_ & 0x8000) px_ -= 0x10000;
  if (py_ & 0x8000) py_ -= 0x10000;
  *x = px_; *y = py_;
}
static void pen_set(W32Canvas *c, int x, int y) {
  c->font = (W32GdiObj *)(W64)(((W32)x & 0xffff) | (((W32)y & 0xffff) << 16));
}

BOOL MoveToEx(HDC dc, int x, int y, POINT *old) {
  W32Canvas *c = cv(dc);
  if (!c) return W32_FALSE;
  if (old) pen_pos(c, &old->x, &old->y);
  pen_set(c, x, y);
  return W32_TRUE;
}

BOOL LineTo(HDC dc, int x2, int y2) {
  W32Canvas *c = cv(dc);
  if (!c) return W32_FALSE;
  int x1, y1;
  pen_pos(c, &x1, &y1);
  W32 col = c->pen ? cr_px(c->pen->color) : cr_px(W32RGB(0, 0, 0));
  int dx = abs(x2 - x1), dy = abs(y2 - y1);
  int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
  int e = dx - dy;
  for (;;) {
    put(c, x1, y1, col);
    if (x1 == x2 && y1 == y2) break;
    int e2 = 2 * e;
    if (e2 > -dy) { e -= dy; x1 += sx; }
    if (e2 < dx) { e += dx; y1 += sy; }
  }
  pen_set(c, x2, y2);
  return W32_TRUE;
}

/* ---------------- attributes ---------------- */
COLORREF SetTextColor(HDC dc, COLORREF col) {
  W32Canvas *c = cv(dc);
  if (!c) return 0;
  W32 old = c->textcol;
  c->textcol = col;
  return old;
}
COLORREF SetBkColor(HDC dc, COLORREF col) {
  W32Canvas *c = cv(dc);
  if (!c) return 0;
  W32 old = c->bkcol;
  c->bkcol = col;
  return old;
}
int SetBkMode(HDC dc, int m) {
  W32Canvas *c = cv(dc);
  if (!c) return 0;
  int old = c->bkmode;
  c->bkmode = m;
  return old;
}

HBRUSH CreateSolidBrush(COLORREF col) {
  W32GdiObj *o = calloc(1, sizeof *o);
  if (!o) return NULL;
  o->magic = GDI_MAGIC; o->kind = GDI_BRUSH; o->color = col;
  return (HBRUSH)o;
}
HPEN CreatePen(int style, int width, COLORREF col) {
  W32GdiObj *o = calloc(1, sizeof *o);
  if (!o) return NULL;
  o->magic = GDI_MAGIC; o->kind = GDI_PEN; o->style = style;
  o->width = width ? width : 1; o->color = col;
  return (HPEN)o;
}
BOOL DeleteObject(HGDIOBJ h) {
  W32GdiObj *o = obj_of(h);
  if (!o) return W32_FALSE;
  for (size_t i = 0; i < sizeof stocks / sizeof stocks[0]; i++)
    if (o == &stocks[i]) return W32_TRUE; /* stocks are immortal */
  o->magic = 0;
  free(o);
  return W32_TRUE;
}
HGDIOBJ SelectObject(HDC dc, HGDIOBJ h) {
  W32Canvas *c = cv(dc);
  W32GdiObj *o = obj_of(h);
  if (!c || !o) return NULL;
  HGDIOBJ old = NULL;
  if (o->kind == GDI_BRUSH) { old = (HGDIOBJ)c->brush; c->brush = o; }
  else if (o->kind == GDI_PEN) { old = (HGDIOBJ)c->pen; c->pen = o; }
  else if (o->kind == GDI_FONT) { old = (HGDIOBJ)c->font; c->font = o; }
  return old;
}
HGDIOBJ GetStockObject(int idx) { return (HGDIOBJ)gdi_stock(idx); }

/* ---------------- clip region (DC state) ----------------
 * Owner-drawn children must not bleed outside their bounds, so the DC keeps a
 * clip rect that every primitive honours. SaveDC/RestoreDC stack it (8 deep). */
/* clip rect that every primitive honours. SaveDC/RestoreDC stack it (8 deep).
 * clip_stack holds the states 0..clip_sp, the current clip IS clip_stack[clip_sp]. */
int SaveDC(HDC dc) {
  W32Canvas *c = cv(dc);
  if (!c || c->clip_sp >= 8) return -1;
  c->clip_stack[c->clip_sp + 1] = c->clip;
  c->clip_sp++;
  return c->clip_sp;
}
BOOL RestoreDC(HDC dc, int saved) {
  W32Canvas *c = cv(dc);
  if (!c || saved < 0 || saved > c->clip_sp) return W32_FALSE;
  c->clip_sp = saved;
  c->clip = c->clip_stack[saved];
  return W32_TRUE;
}
BOOL SelectClipRect(HDC dc, const RECT *r) {
  W32Canvas *c = cv(dc);
  if (!c || !r) return W32_FALSE;
  c->clip.left = r->left; c->clip.top = r->top;
  c->clip.right = r->right; c->clip.bottom = r->bottom;
  if (c->clip.left < 0) c->clip.left = 0;
  if (c->clip.top < 0) c->clip.top = 0;
  if (c->clip.right > c->w) c->clip.right = c->w;
  if (c->clip.bottom > c->h) c->clip.bottom = c->h;
  return W32_TRUE;
}
int IntersectClipRect(HDC dc, int l, int t, int r, int b) {
  W32Canvas *c = cv(dc);
  if (!c) return 0;
  if (l > c->clip.left) c->clip.left = l;
  if (t > c->clip.top) c->clip.top = t;
  if (r < c->clip.right) c->clip.right = r;
  if (b < c->clip.bottom) c->clip.bottom = b;
  if (c->clip.left < 0) c->clip.left = 0;
  if (c->clip.top < 0) c->clip.top = 0;
  if (c->clip.right > c->w) c->clip.right = c->w;
  if (c->clip.bottom > c->h) c->clip.bottom = c->h;
  if (c->clip.right < c->clip.left) c->clip.right = c->clip.left;
  if (c->clip.bottom < c->clip.top) c->clip.bottom = c->clip.top;
  return 0;
}
int GetClipBox(HDC dc, RECT *rc) {
  W32Canvas *c = cv(dc);
  if (!c || !rc) return 0;
  rc->left = c->clip.left; rc->top = c->clip.top;
  rc->right = c->clip.right; rc->bottom = c->clip.bottom;
  return 1;
}

/* SwapBuffers: present the current opengl32 context into the DC's canvas. */
BOOL SwapBuffers(HDC dc) { return gl32_swap(cv(dc)) ? W32_TRUE : W32_FALSE; }
