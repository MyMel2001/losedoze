/* k93font.h - clean-room TrueType font loading + glyph rasterization.
 *
 * Written from scratch against the published TrueType reference manual
 * (Apple "The TrueType Reference Manual" / MS "OpenType spec" - format
 * documentation, not code). Parses the sfnt tables a GUI needs
 * (head/maxp/cmap/loca/glyf/hhea/hmtx/kern) and rasterizes glyphs with a
 * 4x4-supersampled nonzero-winding scanline filler.
 *
 * Fallback: when no TTF is available (text console, missing files) the
 * caller keeps using the embedded 8x16 bitmap font; k93font_ready()==0
 * signals that. */
#ifndef K93FONT_H
#define K93FONT_H

#include <stdint.h>

int k93font_init(void);       /* scan font dir; 0 = at least the system font */
void k93font_shutdown(void);
int k93font_ready(void);      /* current font loaded? */

int k93font_count(void);      /* selectable families */
int k93font_id(const char *family);  /* alias/family name -> id, -1 unknown */
int k93font_default_id(void); /* system default family id */
const char *k93font_name(int id);
int k93font_select(int id);   /* choose family for subsequent calls, 0 ok */
int k93font_bold_id(int id);  /* bold sibling family id or -1 */

/* metrics for a text cell of 16*scale px (layout uses 16*scale everywhere,
 * so swapping the scalable font in does not move any box) */
int k93font_px(int scale);      /* glyph raster height box, = 16*scale */
int k93font_baseline(int scale);/* px from cell top to baseline */
int k93font_width(int scale, const char *utf8); /* advance sum + kerning */

/* Rasterize one codepoint at height k93font_px(scale) (or the bold
 * variant when bold!=0 and one exists). Returns a malloc'd alpha-coverage
 * bitmap w*h (0..255), or NULL when the font has no bitmap (empty glyph
 * such as space, or no font). *adv = pen advance in px including kerning
 * against prev (prev=0 = line start), always set. */
uint8_t *k93font_glyph(uint32_t prev, uint32_t cp, int scale, int bold,
                       int *w, int *h, int *xoff, int *yoff, float *adv);
void k93font_glyph_free(uint8_t *p);

/* UTF-8 decode helper (shared with k93ui/html): returns cp and advances *i. */
uint32_t k93_utf8_next(const char *s, int *i);

#endif
