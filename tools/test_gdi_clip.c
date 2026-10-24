/* test_gdi_clip.c — host-side check for the Win32 GDI additions that fixed the
 * WinForms ceilings: DC clipping (SelectClipRect/IntersectClipRect/GetClipBox/
 * SaveDC/RestoreDC, honoured by fill_rect/put/textout) and scalable text
 * (SetFontSize -> stb_truetype pixel height -> GetTextExtentPoint32W).
 *
 * Builds a W32Canvas in a heap buffer and drives it through the HDC cast, the
 * same way user32 hands a canvas to BeginPaint.
 *
 *   gcc -O2 -Wall -DW32_ABI_SYSV -o /tmp/test_gdi_clip \
 *       tools/test_gdi_clip.c win/gdi32.c -Iwin -Ibuild -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#define W 100
#define H 100

static int fails, checks;

static void expect(const char *what, int cond) {
  checks++;
  if (cond) {
    printf("ok    %s\n", what);
  } else {
    printf("FAIL  %s\n", what);
    fails++;
  }
}

static W32Canvas *canvas_new(void) {
  static W32 px[W * H];
  memset(px, 0, sizeof px);
  W32Canvas *c = calloc(1, sizeof *c);
  c->px = px;
  c->w = W;
  c->h = H;
  c->stride = W;
  c->textcol = W32RGB(0, 0, 0);
  c->bkcol = W32RGB(255, 255, 255);
  c->bkmode = OPAQUE;
  c->font_px = 16.0f;
  c->clip.left = 0;
  c->clip.top = 0;
  c->clip.right = W;
  c->clip.bottom = H;
  c->clip_stack[0] = c->clip;
  return c;
}

static W32 px_at(W32Canvas *c, int x, int y) { return c->px[(size_t)y * c->stride + x]; }

static const W32 BLUE = 0xff0a141e;  /* W32RGB(10, 20, 30) -> ARGB */
static const W32 GREEN = 0xff28323c; /* W32RGB(40, 50, 60) */

/* symbols the library pulls in transitively when built standalone */
int lstrlenW(LPCWSTR s) { int n = 0; if (!s) return 0; while (s[n]) n++; return n; }
int gl32_swap(W32Canvas *c) { (void)c; return 0; }

int main(void) {
  W32Canvas *c = canvas_new();
  HDC dc = (HDC)c;
  HBRUSH blue = CreateSolidBrush(W32RGB(10, 20, 30));
  HBRUSH green = CreateSolidBrush(W32RGB(40, 50, 60));

  /* full-canvas fill, no clip restriction */
  FillRect(dc, &(RECT){0, 0, W, H}, blue);
  expect("fill whole canvas", px_at(c, 0, 0) == BLUE && px_at(c, W - 1, H - 1) == BLUE);
  expect("fill leaves no stray row", px_at(c, 50, 0) == BLUE && px_at(c, 0, 50) == BLUE);

  /* clip rect: the second fill must only touch the clip region */
  FillRect(dc, &(RECT){0, 0, W, H}, green);
  expect("unclipped fill repainted everything", px_at(c, 0, 0) == GREEN && px_at(c, 50, 50) == GREEN);
  SelectClipRect(dc, &(RECT){20, 20, 30, 30});
  FillRect(dc, &(RECT){0, 0, W, H}, blue);
  expect("clip keeps pixels outside the rect", px_at(c, 0, 0) == GREEN && px_at(c, 99, 99) == GREEN);
  expect("clip repaints the rect itself", px_at(c, 25, 25) == BLUE);
  expect("clip right/bottom are exclusive", px_at(c, 30, 25) == GREEN && px_at(c, 25, 30) == GREEN);
  expect("clip rejects the region below the rect", px_at(c, 25, 31) == GREEN);

  /* intersect + query */
  int s1 = SaveDC(dc);
  expect("SaveDC returns the saved-state token", s1 == 1);
  IntersectClipRect(dc, 25, 25, 60, 60);
  RECT box = {0, 0, 0, 0};
  GetClipBox(dc, &box);
  expect("IntersectClipRect narrows to the overlap",
         box.left == 25 && box.top == 25 && box.right == 30 && box.bottom == 30);
  SelectClipRect(dc, &(RECT){40, 40, 50, 50});
  FillRect(dc, &(RECT){0, 0, W, H}, green);
  expect("intersected clip bounds the fill", px_at(c, 27, 27) == BLUE && px_at(c, 45, 45) == GREEN);

  /* restore the saved state */
  expect("RestoreDC succeeds", RestoreDC(dc, s1) == W32_TRUE);
  GetClipBox(dc, &box);
  expect("RestoreDC brings the saved clip back",
         box.left == 20 && box.top == 20 && box.right == 30 && box.bottom == 30);
  SelectClipRect(dc, &(RECT){-5, -5, 1000, 1000});
  GetClipBox(dc, &box);
  expect("SelectClipRect clamps to the canvas",
         box.left == 0 && box.top == 0 && box.right == W && box.bottom == H);

  /* save/restore stack is 8 deep (states 0..8) */
  RestoreDC(dc, 0);
  SelectClipRect(dc, &(RECT){0, 0, W, H});
  int toks[9];
  for (int i = 0; i < 9; i++) toks[i] = SaveDC(dc);
  expect("SaveDC tokens advance then fail", toks[0] == 1 && toks[7] == 8 && toks[8] == -1);
  expect("RestoreDC pops back", RestoreDC(dc, toks[7]) == W32_TRUE && RestoreDC(dc, 0) == W32_TRUE);
  SelectClipRect(dc, &(RECT){0, 0, W, H});
  FillRect(dc, &(RECT){0, 0, W, H}, blue);
  expect("restored base state still fills the canvas",
         px_at(c, 0, 0) == BLUE && px_at(c, W - 1, H - 1) == BLUE);

  /* scalable text: Font.Size -> SetFontSize -> rasterizer pixel height */
  SIZE s_small = {0, 0}, s_big = {0, 0};
  static const W16 txt[] = {'H', 'e', 'l', 'l', 'o', 0};
  GetTextExtentPoint32W(dc, txt, -1, &s_small);
  expect("extent reports a sane default size", s_small.cx > 0 && s_small.cy > 0);
  if (s_small.cx != 8 * 5 || s_small.cy != 16) { /* font loaded (not the fallback) */
    SetFontSize(dc, 32.0f);
    GetTextExtentPoint32W(dc, txt, -1, &s_big);
    expect("SetFontSize(32) scales the text up", s_big.cx > s_small.cx && s_big.cy > s_small.cy);
  } else {
    printf("skip  (Liberation Sans not found on this host; size scaling untested)\n");
  }

  printf("\n%d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
