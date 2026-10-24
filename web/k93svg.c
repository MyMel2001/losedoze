/* k93svg.c - clean-room SVG rasterizer + PNG encoder.
 *
 * Written from scratch against the PUBLISHED W3C SVG 1.1 and PNG format
 * specifications (documentation only - no renderer/encoder code was
 * consulted or copied). Pipeline inside the browser:
 *
 *   svg text -> parse (mini XML walk) -> flatten to float segments ->
 *   4x4-supersampled nonzero/evenodd scanline coverage -> src-over blend
 *   into an RGBA canvas -> PNG bytes (own encoder) -> caller's decoder.
 *
 * Ceilings (upgrade paths marked inline):
 *   - fills: url(#gradient) paints are skipped (shape stays unfilled);
 *     solid colors (#rgb/#rrggbb/rgb()/named) only.
 *   - stroke: joins are round-approximated (miter/bevel differ at sharp
 *     corners), stroke-dasharray ignored, non-uniform transforms use the
 *     x-axis scale for stroke width.
 *   - no <text> glyph rendering, no <image> nesting, no <use> refs,
 *     no CSS <style> blocks, no nested <svg> viewports (treated as <g>).
 *   - group opacity is approximated by multiplying into children
 *     (differs from spec only where sibling shapes overlap).
 *   - rect rx/ry corners use a fixed 8 segments per quarter arc. */
#include "k93svg.h"
#include "k93html.h"          /* k93css_color: shared named/hex colors */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXPT   100000        /* flattened outline points per document */
#define MAXSG   200000        /* fill/stroke segments per shape */
#define MAXAT   32            /* attributes per element */
#define ATBUF   (64 * 1024)   /* attribute value store per element */

#define PI 3.14159265358979323846

/* ------------------------------------------------------------------ grow */

static int grow(void **base, int *cap, int need, size_t el)
{
    if (*cap >= need) return 0;
    int nc = *cap ? *cap : 256;
    while (nc < need) nc *= 2;
    void *p = realloc(*base, (size_t)nc * el);
    if (!p) return -1;
    *base = p;
    *cap = nc;
    return 0;
}

/* --------------------------------------------------------------- matrices */
/* affine: x' = a*x + c*y + e ; y' = b*x + d*y + f
 * (same layout as the font engine; SVG matrix(a b c d e f) maps 1:1) */
typedef struct { float a, b, c, d, e, f; } M2;
typedef struct { float x, y; } V;
typedef struct { float x0, y0, x1, y1; } SG;

static const M2 M_ID = { 1, 0, 0, 1, 0, 0 };

static M2 m_mul(const M2 *P, const M2 *C)          /* P after C */
{
    M2 t;
    t.a = P->a * C->a + P->c * C->b;
    t.c = P->a * C->c + P->c * C->d;
    t.b = P->b * C->a + P->d * C->b;
    t.d = P->b * C->c + P->d * C->d;
    t.e = P->a * C->e + P->c * C->f + P->e;
    t.f = P->b * C->e + P->d * C->f + P->f;
    return t;
}

static void m_apply(const M2 *M, double x, double y, float *ox, float *oy)
{
    *ox = (float)(M->a * x + M->c * y + M->e);
    *oy = (float)(M->b * x + M->d * y + M->f);
}

static float m_scale(const M2 *M)        /* x-axis image length */
{
    float s = M->a * M->a + M->b * M->b;
    return s > 1e-12f ? (float)sqrt(s) : 1.0f;
}

/* transform="translate(..) scale(..) rotate(..) skewX(..) skewY(..) matrix(..)" */
static M2 parse_transform(const char *s)
{
    M2 ctm = M_ID;
    if (!s) return ctm;
    while (*s) {
        while (*s && (isspace((unsigned char)*s) || *s == ';')) s++;
        char name[16];
        int ni = 0;
        while (isalpha((unsigned char)*s) && ni < 15) name[ni++] = *s++;
        name[ni] = 0;
        while (*s && isspace((unsigned char)*s)) s++;
        if (*s != '(') break;
        s++;
        double v[6];
        int nv = 0;
        while (nv < 6) {
            while (*s && (isspace((unsigned char)*s) || *s == ',')) s++;
            char *e;
            double d = strtod(s, &e);
            if (e == s) break;
            v[nv++] = d;
            s = e;
        }
        while (*s && *s != ')') s++;
        if (*s == ')') s++;
        M2 t = M_ID;
        if (!strcasecmp(name, "translate") && nv >= 1) {
            t.e = (float)v[0];
            t.f = nv >= 2 ? (float)v[1] : 0;
        } else if (!strcasecmp(name, "scale") && nv >= 1) {
            t.a = (float)v[0];
            t.d = nv >= 2 ? (float)v[1] : (float)v[0];
        } else if (!strcasecmp(name, "rotate") && nv >= 1) {
            double r = v[0] * PI / 180.0;
            double cs = cos(r), sn = sin(r);
            t.a = (float)cs; t.b = (float)sn;
            t.c = (float)-sn; t.d = (float)cs;
            if (nv >= 3) {                     /* rotate about (cx,cy) */
                M2 to = M_ID, back = M_ID;
                to.e = (float)v[1]; to.f = (float)v[2];
                back.e = (float)-v[1]; back.f = (float)-v[2];
                t = m_mul(&to, &t);
                t = m_mul(&t, &back);
            }
        } else if (!strcasecmp(name, "matrix") && nv >= 6) {
            t.a = (float)v[0]; t.b = (float)v[1]; t.c = (float)v[2];
            t.d = (float)v[3]; t.e = (float)v[4]; t.f = (float)v[5];
        } else if (!strcasecmp(name, "skewx") && nv >= 1) {
            t.c = (float)tan(v[0] * PI / 180.0);
        } else if (!strcasecmp(name, "skewy") && nv >= 1) {
            t.b = (float)tan(v[0] * PI / 180.0);
        }
        ctm = m_mul(&ctm, &t);
    }
    return ctm;
}

/* ---------------------------------------------------- canvas + geometry */

static uint8_t *cv;                            /* RGBA8 canvas */
static int CW, CH;
static V  *pt;                                 /* flattened points (px) */
static int npt, capp;
static int *sub_s, *sub_n, *sub_cl;            /* subpath ranges */
static int nsub, caps_s, caps_n, caps_cl;
static SG *sg;                                 /* current shape segments */
static int nsg, capsg;
static uint16_t *acc;                          /* coverage accumulator */
static int capacc;
typedef struct { float x; int dir; } CROSS;
static CROSS *cross;
static int capcross;

static void sub_begin(void)
{
    if (nsub >= MAXAT * 8 + 64) return;        /* per-element cap; ample */
    if (grow((void **)&sub_s, &caps_s, nsub + 1, sizeof *sub_s) ||
        grow((void **)&sub_n, &caps_n, nsub + 1, sizeof *sub_n) ||
        grow((void **)&sub_cl, &caps_cl, nsub + 1, sizeof *sub_cl))
        return;
    sub_s[nsub++] = npt;
    sub_n[nsub - 1] = 0;
    sub_cl[nsub - 1] = 0;
}

static void spush(const M2 *M, double x, double y)   /* user units -> px */
{
    if (nsub < 1 || npt >= MAXPT) return;
    if (grow((void **)&pt, &capp, npt + 1, sizeof *pt)) return;
    m_apply(M, x, y, &pt[npt].x, &pt[npt].y);
    npt++;
    sub_n[nsub - 1]++;
}

static void subs_reset(void) { nsub = npt = 0; }

/* adaptive de Casteljau flatten into the current subpath (px space) */
static void bez_quad(float x0, float y0, float x1, float y1,
                     float x2, float y2, int d)
{
    float dx = x2 - x0, dy = y2 - y0;
    float dd = dx * dx + dy * dy;
    float e = (x1 - x0) * dy - (y1 - y0) * dx;
    float dist2 = dd > 1e-6f ? (e * e) / dd
                             : ((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    if (dist2 <= 0.0625f || d >= 12) {          /* <= 0.25 px off chord */
        if (nsub > 0) {
            if (grow((void **)&pt, &capp, npt + 1, sizeof *pt) == 0) {
                pt[npt].x = x2; pt[npt].y = y2;
                npt++; sub_n[nsub - 1]++;
            }
        }
        return;
    }
    float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
    float nx = (x1 + x2) * 0.5f, ny = (y1 + y2) * 0.5f;
    float cx = (mx + nx) * 0.5f, cy = (my + ny) * 0.5f;
    bez_quad(x0, y0, mx, my, cx, cy, d + 1);
    bez_quad(cx, cy, nx, ny, x2, y2, d + 1);
}

static void bez_cubic(float x0, float y0, float x1, float y1,
                      float x2, float y2, float x3, float y3, int d)
{
    float dx = x3 - x0, dy = y3 - y0;
    float dd = dx * dx + dy * dy;
    float flat2 = 0;
    for (int k = 1; k <= 2; k++) {              /* both controls vs chord */
        float cx_ = k == 1 ? x1 : x2, cy_ = k == 1 ? y1 : y2;
        float e = (cx_ - x0) * dy - (cy_ - y0) * dx;
        float v = dd > 1e-6f ? (e * e) / dd
                             : ((cx_ - x0) * (cx_ - x0) + (cy_ - y0) * (cy_ - y0));
        if (v > flat2) flat2 = v;
    }
    if (flat2 <= 0.0625f || d >= 12) {
        if (nsub > 0) {
            if (grow((void **)&pt, &capp, npt + 1, sizeof *pt) == 0) {
                pt[npt].x = x3; pt[npt].y = y3;
                npt++; sub_n[nsub - 1]++;
            }
        }
        return;
    }
    float x01 = (x0 + x1) * .5f, y01 = (y0 + y1) * .5f;
    float x12 = (x1 + x2) * .5f, y12 = (y1 + y2) * .5f;
    float x23 = (x2 + x3) * .5f, y23 = (y2 + y3) * .5f;
    float x012 = (x01 + x12) * .5f, y012 = (y01 + y12) * .5f;
    float x123 = (x12 + x23) * .5f, y123 = (y12 + y23) * .5f;
    float xc = (x012 + x123) * .5f, yc = (y012 + y123) * .5f;
    bez_cubic(x0, y0, x01, y01, x012, y012, xc, yc, d + 1);
    bez_cubic(xc, yc, x123, y123, x23, y23, x3, y3, d + 1);
}

/* ------------------------------------------------- coverage scanline fill */

static void sg_add(float x0, float y0, float x1, float y1)
{
    if (x0 == x1 && y0 == y1) return;
    if (nsg >= MAXSG) return;
    if (grow((void **)&sg, &capsg, nsg + 1, sizeof *sg)) return;
    sg[nsg].x0 = x0; sg[nsg].y0 = y0;
    sg[nsg].x1 = x1; sg[nsg].y1 = y1;
    nsg++;
}

static float poly_area(const V *p, int n)
{
    float a = 0;
    for (int i = 0; i < n; i++) {
        const V *q = p + i, *r = p + (i + 1) % n;
        a += q->x * r->y - r->x * q->y;
    }
    return a;
}

/* stroke polygon points with consistent winding (nonzero unions must not
 * cancel; discs are generated counter-clockwise-in-shoelace like the rect
 * builders, quads get normalized to match) */
static void poly_add(const V *p, int n, int normalize)
{
    if (n < 3) return;
    if (normalize && poly_area(p, n) < 0) {
        for (int i = 0; i < n / 2; i++) {
            V t = p[i];
            ((V *)p)[i] = p[n - 1 - i];
            ((V *)p)[n - 1 - i] = t;
        }
    }
    for (int i = 0; i < n; i++) {
        const V *q = p + i, *r = p + (i + 1) % n;
        sg_add(q->x, q->y, r->x, r->y);
    }
}

static void disc_add(float cx, float cy, float r)
{
    V d[24];
    for (int i = 0; i < 24; i++) {
        double th = 2.0 * PI * i / 24.0;
        d[i].x = cx + r * (float)cos(th);
        d[i].y = cy + r * (float)sin(th);
    }
    poly_add(d, 24, 1);
}

/* rasterize the current segment list into the canvas with src-over.
 * evenodd: inside = crossing parity; else nonzero winding. */
static void fill_blend(uint32_t rgb, int alpha, int evenodd)
{
    if (nsg < 1 || alpha <= 0 || !cv) return;
    if (grow((void **)&acc, &capacc, CW * CH, sizeof *acc)) return;
    if (grow((void **)&cross, &capcross, nsg, sizeof *cross)) return;
    memset(acc, 0, (size_t)CW * CH * sizeof *acc);

    /* subrows only over the shape's vertical bbox */
    float miny = 1e9f, maxy = -1e9f, minx = 1e9f, maxx = -1e9f;
    for (int i = 0; i < nsg; i++) {
        if (sg[i].y0 < miny) miny = sg[i].y0;
        if (sg[i].y1 < miny) miny = sg[i].y1;
        if (sg[i].y0 > maxy) maxy = sg[i].y0;
        if (sg[i].y1 > maxy) maxy = sg[i].y1;
        if (sg[i].x0 < minx) minx = sg[i].x0;
        if (sg[i].x1 < minx) minx = sg[i].x1;
        if (sg[i].x0 > maxx) maxx = sg[i].x0;
        if (sg[i].x1 > maxx) maxx = sg[i].x1;
    }
    int sy0 = (int)(miny * 4.0f) - 4; if (sy0 < 0) sy0 = 0;
    int sy1 = (int)(maxy * 4.0f) + 4; if (sy1 > 4 * CH - 1) sy1 = 4 * CH - 1;
    int ix0 = (int)floor(minx); if (ix0 < 0) ix0 = 0;
    int ix1 = (int)ceil(maxx); if (ix1 > CW) ix1 = CW;

    for (int sy = sy0; sy <= sy1; sy++) {
        float y = ((float)sy + 0.5f) * 0.25f;
        int ncross = 0;
        for (int i = 0; i < nsg; i++) {
            float ya = sg[i].y0, yb = sg[i].y1;
            if (ya == yb) continue;
            if (y < (ya < yb ? ya : yb) || y >= (ya < yb ? yb : ya)) continue;
            float x = sg[i].x0 + (y - ya) * (sg[i].x1 - sg[i].x0) / (yb - ya);
            cross[ncross].x = x;
            cross[ncross].dir = yb > ya ? 1 : -1;
            ncross++;
        }
        for (int i = 1; i < ncross; i++) {        /* insertion sort by x */
            CROSS v = cross[i];
            int j = i - 1;
            while (j >= 0 && cross[j].x > v.x) { cross[j + 1] = cross[j]; j--; }
            cross[j + 1] = v;
        }
        int wind = 0, row = (sy >> 2) * CW;
        for (int i = 0; i + 1 < ncross; i++) {
            wind += cross[i].dir;
            int inside = evenodd ? (wind & 1) : (wind != 0);
            if (!inside) continue;
            float xa = cross[i].x, xb = cross[i + 1].x;
            if (xa >= (float)ix1 || xb <= (float)ix0) continue;
            if (xa < (float)ix0) xa = (float)ix0;
            if (xb > (float)ix1) xb = (float)ix1;
            int px0 = (int)floorf(xa);
            int px1 = (int)floorf(xb - 1e-6f);
            if (px1 > CW - 1) px1 = CW - 1;
            for (int px = px0; px <= px1; px++) {
                float l = xa > (float)px ? xa : (float)px;
                float r2 = xb < (float)px + 1 ? xb : (float)px + 1;
                if (r2 <= l) continue;
                int add = (int)((r2 - l) * 64.0f + 0.5f);   /* 1/64 px units */
                if (add > 0) {
                    unsigned v2 = (unsigned)acc[row + px] + (unsigned)add;
                    acc[row + px] = (uint16_t)(v2 > 256 ? 256 : v2);
                }
            }
        }
    }

    /* src-over blend of the coverage mask, straight alpha */
    for (int i = 0; i < CW * CH; i++) {
        unsigned a256 = acc[i];
        if (!a256) continue;
        int a = (int)((a256 * (unsigned)alpha) / 256);
        if (a <= 0) continue;
        uint8_t *d = cv + (size_t)i * 4;
        int da = d[3];
        int na = a + da * (255 - a) / 255;
        int sr = (rgb >> 16) & 255, sgn = (rgb >> 8) & 255, sb = rgb & 255;
        if (na <= 0) continue;
        int r = (sr * a + (d[0] * da * (255 - a)) / 255 + na / 2) / na;
        int g = (sgn * a + (d[1] * da * (255 - a)) / 255 + na / 2) / na;
        int b = (sb * a + (d[2] * da * (255 - a)) / 255 + na / 2) / na;
        d[0] = (uint8_t)(r > 255 ? 255 : r);
        d[1] = (uint8_t)(g > 255 ? 255 : g);
        d[2] = (uint8_t)(b > 255 ? 255 : b);
        d[3] = (uint8_t)(na > 255 ? 255 : na);
    }
    nsg = 0;
}

/* ------------------------------------------------------- paint + shapes */

typedef struct {
    uint32_t fill_rgb, stroke_rgb;
    float fill_a, stroke_a, opacity, sw;
    int do_fill, do_stroke, evenodd;       /* evenodd: fill-rule=evenodd */
    int cap;                               /* 0 butt 1 round 2 square */
} PAIN;

static PAIN pain_default(void)
{
    PAIN p;
    memset(&p, 0, sizeof p);
    p.fill_rgb = 0x000000;
    p.fill_a = p.stroke_a = p.opacity = 1.0f;
    p.sw = 1.0f;
    p.do_fill = 1;
    return p;
}

static double num_attr(const char *v)      /* length; px/% suffix ignored */
{
    return v ? strtod(v, NULL) : 0.0;
}

/* paint value: rgb()/rgba(), #hex/named (shared k93css_color), none,
 * transparent, currentColor (=black default), url(#..) skipped. Returns 1
 * and sets rgb+alpha, or 0 = "no paint". */
static int parse_paint(const char *v, uint32_t *rgb, float *alpha)
{
    char buf[64];
    while (*v && isspace((unsigned char)*v)) v++;
    size_t L = strlen(v);
    while (L > 0 && isspace((unsigned char)v[L - 1])) L--;
    if (L == 0 || L >= sizeof buf) return 0;
    memcpy(buf, v, L);
    buf[L] = 0;
    if (!strcasecmp(buf, "none") || !strcasecmp(buf, "transparent")) return 0;
    if (!strncasecmp(buf, "url(", 4)) return 0;    /* gradients: ceiling */
    if (!strcasecmp(buf, "currentcolor")) {
        *rgb = 0;
        *alpha = 1.0f;
        return 1;
    }
    if (!strncasecmp(buf, "rgb", 3) && strchr(buf, '(')) {
        double vals[4] = { 0, 0, 0, 1 };
        int n = 0;
        const char *s = strchr(buf, '(') + 1;
        while (n < 4 && *s && *s != ')') {
            while (*s && (isspace((unsigned char)*s) || *s == ',')) s++;
            char *e;
            double d = strtod(s, &e);
            if (e == s) break;
            vals[n++] = d;
            s = e;
        }
        if (n < 3) return 0;
        int r = (int)vals[0], g = (int)vals[1], b = (int)vals[2];
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (g < 0) g = 0;
        if (g > 255) g = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        float a = 1.0f;
        if (n == 4) {
            a = (float)vals[3];
            if (a > 1.0f) a = 1.0f;               /* tolerate 0..255 */
            if (a < 0.0f) a = 0.0f;
        }
        *rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        *alpha = a;
        return 1;
    }
    if (k93css_color(buf, rgb)) {                 /* hex + named colors */
        *alpha = 1.0f;
        return 1;
    }
    return 0;
}

static void set_prop(PAIN *p, const char *k, const char *v)
{
    uint32_t c;
    float f;
    double d;
    if (!v || !*v) return;
    if (!strcasecmp(k, "fill")) {
        if (parse_paint(v, &c, &f)) {
            p->fill_rgb = c; p->fill_a = f; p->do_fill = 1;
        } else p->do_fill = 0;
    } else if (!strcasecmp(k, "stroke")) {
        if (parse_paint(v, &c, &f)) {
            p->stroke_rgb = c; p->stroke_a = f; p->do_stroke = 1;
        } else p->do_stroke = 0;
    } else if (!strcasecmp(k, "stroke-width")) {
        d = num_attr(v);
        if (d > 0) p->sw = (float)d;
    } else if (!strcasecmp(k, "stroke-linecap")) {
        if (!strcasecmp(v, "round")) p->cap = 1;
        else if (!strcasecmp(v, "square")) p->cap = 2;
        else p->cap = 0;
    } else if (!strcasecmp(k, "fill-rule")) {
        p->evenodd = !strcasecmp(v, "evenodd");
    } else if (!strcasecmp(k, "fill-opacity")) {
        d = num_attr(v);
        p->fill_a = (float)(d < 0 ? 0 : d > 1 ? 1 : d);   /* replaces paint
                                                             alpha (ceiling) */
    } else if (!strcasecmp(k, "stroke-opacity")) {
        d = num_attr(v);
        p->stroke_a = (float)(d < 0 ? 0 : d > 1 ? 1 : d);
    } else if (!strcasecmp(k, "opacity")) {
        d = num_attr(v);
        p->opacity = (float)(d < 0 ? 0 : d > 1 ? 1 : d);
    }
}

/* inline style="" attribute: same prop:value pairs, applied after the
 * element's presentation attributes (CSS precedence) */
static void apply_style(PAIN *p, const char *style)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", style);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ";", &save); tok;
         tok = strtok_r(NULL, ";", &save)) {
        char *col = strchr(tok, ':');
        if (!col) continue;
        *col = 0;
        char *k = tok, *v = col + 1;
        while (*k && isspace((unsigned char)*k)) k++;
        while (*v && isspace((unsigned char)*v)) v++;
        char *ke = k + strlen(k);
        while (ke > k && isspace((unsigned char)ke[-1])) *--ke = 0;
        set_prop(p, k, v);
    }
}

/* ------------------------------------------------------------ xml mini */

typedef struct { char k[32]; const char *v; } AT;
static AT at[MAXAT];
static char atbuf[ATBUF];
static int atbuf_used;

static const char *at_get(int na, const char *name)
{
    for (int i = 0; i < na; i++)
        if (!strcasecmp(at[i].k, name)) return at[i].v;
    return NULL;
}

/* copy src into dst decoding the basic XML entities + numeric refs */
static void ent_copy(char *dst, size_t cap, const char *src, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < cap; i++) {
        if (src[i] != '&') { dst[o++] = src[i]; continue; }
        size_t j = i + 1;
        while (j < n && src[j] != ';' && j - i < 12) j++;
        if (j >= n || src[j] != ';') { dst[o++] = src[i]; continue; }
        char e[12];
        size_t el = j - i - 1;
        memcpy(e, src + i + 1, el);
        e[el] = 0;
        uint32_t cp = 0;
        int ok = 0;
        if (!strcmp(e, "amp")) { cp = '&'; ok = 1; }
        else if (!strcmp(e, "lt")) { cp = '<'; ok = 1; }
        else if (!strcmp(e, "gt")) { cp = '>'; ok = 1; }
        else if (!strcmp(e, "quot")) { cp = '"'; ok = 1; }
        else if (!strcmp(e, "apos")) { cp = '\''; ok = 1; }
        else if (e[0] == '#') {
            char *end;
            long v = (e[1] == 'x' || e[1] == 'X')
                         ? strtol(e + 2, &end, 16)
                         : strtol(e + 1, &end, 10);
            if (end != e + 1 && v > 0 && v < 0x110000) {
                cp = (uint32_t)v;
                ok = 1;
            }
        }
        if (!ok) { dst[o++] = src[i]; continue; }
        if (cp < 0x80) dst[o++] = (char)cp;
        else if (cp < 0x800) {
            dst[o++] = (char)(0xC0 | (cp >> 6));
            dst[o++] = (char)(0x80 | (cp & 63));
        } else {
            dst[o++] = (char)(0xE0 | (cp >> 12));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 63));
            dst[o++] = (char)(0x80 | (cp & 63));
        }
        i = j;
    }
    dst[o] = 0;
}

/* parse attributes from *pp (just after the tag name) until past '>'.
 * Returns count; sets *selfclose when the tag ended with "/>". */
static int parse_attrs(const char **pp, const char *end, int *selfclose)
{
    const char *p = *pp;
    int na = 0;
    *selfclose = 0;
    for (;;) {
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) break;
        if (*p == '>') { p++; break; }
        if (*p == '/') { *selfclose = 1; p++; continue; }
        if (!isalpha((unsigned char)*p)) { p++; continue; }
        int idx = na < MAXAT ? na : -1;
        int ki = 0;
        if (idx >= 0) at[idx].k[0] = 0;
        while (p < end && (isalnum((unsigned char)*p) || *p == '-' ||
                           *p == '_' || *p == ':' || *p == '.')) {
            if (idx >= 0 && ki < 31)
                at[idx].k[ki++] = (char)tolower((unsigned char)*p);
            p++;
        }
        if (idx >= 0) at[idx].k[ki] = 0;
        while (p < end && isspace((unsigned char)*p)) p++;
        const char *val = NULL;
        size_t vl = 0;
        if (p < end && *p == '=') {
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p < end && (*p == '"' || *p == '\'')) {
                char q = *p++;
                val = p;
                while (p < end && *p != q) p++;
                vl = (size_t)(p - val);
                if (p < end) p++;
            } else {
                val = p;
                while (p < end && !isspace((unsigned char)*p) && *p != '>')
                    p++;
                vl = (size_t)(p - val);
            }
        }
        if (idx >= 0) {
            if (val && atbuf_used + vl + 8 < ATBUF) {
                ent_copy(atbuf + atbuf_used, (size_t)ATBUF - atbuf_used,
                         val, vl);
                at[idx].v = atbuf + atbuf_used;
                atbuf_used += (int)strlen(at[idx].v) + 1;
            } else {
                at[idx].v = "";
            }
            na++;
        }
    }
    *pp = p;
    return na;
}

/* ------------------------------------------------------------- shapes */

static void shp_round_corner(const M2 *M, double cx, double cy,
                             double rx, double ry, double th0, double th1)
{
    for (int i = 1; i <= 8; i++) {
        double th = th0 + (th1 - th0) * i / 8.0;
        spush(M, cx + rx * cos(th), cy + ry * sin(th));
    }
}

static void shp_rect(const M2 *M, double x, double y, double w, double h,
                     double rx, double ry)
{
    if (w <= 0 || h <= 0) return;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx == 0 && ry > 0) rx = ry;
    if (ry == 0) ry = rx;
    if (rx > w / 2) rx = w / 2;
    if (ry > h / 2) ry = h / 2;
    sub_begin();
    if (nsub < 1) return;
    if (rx <= 0 || ry <= 0) {
        spush(M, x, y);
        spush(M, x + w, y);
        spush(M, x + w, y + h);
        spush(M, x, y + h);
        sub_cl[nsub - 1] = 1;
        return;
    }
    spush(M, x + rx, y);
    spush(M, x + w - rx, y);
    shp_round_corner(M, x + w - rx, y + ry, rx, ry, -PI / 2, 0);
    spush(M, x + w, y + h - ry);
    shp_round_corner(M, x + w - rx, y + h - ry, rx, ry, 0, PI / 2);
    spush(M, x + rx, y + h);
    shp_round_corner(M, x + rx, y + h - ry, rx, ry, PI / 2, PI);
    spush(M, x, y + ry);
    shp_round_corner(M, x + rx, y + ry, rx, ry, PI, 1.5 * PI);
    sub_cl[nsub - 1] = 1;
}

static void shp_ellipse(const M2 *M, double cx, double cy, double rx, double ry)
{
    if (rx <= 0 || ry <= 0) return;
    sub_begin();
    if (nsub < 1) return;
    int n = 48;
    for (int i = 0; i < n; i++) {
        double th = 2.0 * PI * i / n;
        spush(M, cx + rx * cos(th), cy + ry * sin(th));
    }
    sub_cl[nsub - 1] = 1;
}

static void shp_points(const M2 *M, const char *nums, int closed)
{
    const char *s = nums;
    int before = nsub;
    sub_begin();
    if (nsub <= before) return;
    int count = 0;
    for (;;) {
        char *e;
        double vx = strtod(s, &e);
        if (e == s) break;
        s = e;
        while (*s == ',' || isspace((unsigned char)*s)) s++;
        double vy = strtod(s, &e);
        if (e == s) break;
        s = e;
        spush(M, vx, vy);
        count++;
    }
    if (count < 2) { nsub--; return; }
    if (closed) sub_cl[nsub - 1] = 1;
}

/* --------------------------------------------------------------- stroke */

/* Build stroke geometry for one subpath: one quad per segment (extended
 * for square caps) + round discs at joins/caps. All polys share the same
 * winding so the nonzero union has no self-cancelled holes. */
static void stroke_sub(int s, int n, int closed, const PAIN *pn, float hw)
{
    if (n < 2 || hw < 0.05f) return;
    float ext_start = pn->cap == 2 ? hw : 0.0f;
    float ext_end = pn->cap == 2 ? hw : 0.0f;
    int segs = closed ? n : n - 1;
    for (int i = 0; i < segs; i++) {
        V a = pt[s + i], b = pt[s + (i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = (float)sqrt((double)dx * dx + (double)dy * dy);
        if (len < 1e-6f) continue;
        float ux = dx / len, uy = dy / len;
        float nx = -uy * hw, ny = ux * hw;
        float ex0 = 0, ey0 = 0, ex1 = 0, ey1 = 0;
        if (!closed && i == 0) { ex0 = -ux * ext_start; ey0 = -uy * ext_start; }
        if (!closed && i == segs - 1) { ex1 = ux * ext_end; ey1 = uy * ext_end; }
        V q[4];
        q[0].x = a.x + nx + ex0; q[0].y = a.y + ny + ey0;
        q[1].x = b.x + nx + ex1; q[1].y = b.y + ny + ey1;
        q[2].x = b.x - nx + ex1; q[2].y = b.y - ny + ey1;
        q[3].x = a.x - nx + ex0; q[3].y = a.y - ny + ey0;
        poly_add(q, 4, 1);
    }
    int joins = closed ? n : n - 1;
    for (int i = 0; i < joins; i++)           /* round joins at vertices */
        disc_add(pt[s + i].x, pt[s + i].y, hw);
    if (!closed && pn->cap == 1) {            /* round caps */
        disc_add(pt[s].x, pt[s].y, hw);
        disc_add(pt[s + n - 1].x, pt[s + n - 1].y, hw);
    }
}

/* render accumulated subpaths of one element with the resolved paint */
static void shape_emit(const M2 *M, const PAIN *pn)
{
    if (pn->do_fill) {
        for (int i = 0; i < nsub; i++) {      /* fill: all subpaths closed */
            int s = sub_s[i], n = sub_n[i];
            for (int k = 0; k < n; k++) {
                const V *a = pt + s + k, *b = pt + s + (k + 1) % n;
                sg_add(a->x, a->y, b->x, b->y);
            }
        }
        fill_blend(pn->fill_rgb,
                   (int)(pn->fill_a * pn->opacity * 255.0f + 0.5f),
                   pn->evenodd);
    }
    if (pn->do_stroke) {
        float hw = 0.5f * pn->sw * m_scale(M);
        for (int i = 0; i < nsub; i++)
            stroke_sub(sub_s[i], sub_n[i], sub_cl[i], pn, hw);
        fill_blend(pn->stroke_rgb,
                   (int)(pn->stroke_a * pn->opacity * 255.0f + 0.5f), 0);
    }
    subs_reset();
}

/* ---------------------------------------------------------- path data */

/* SVG F.6.5 endpoint -> center arc conversion; samples the elliptical arc
 * in USER space (non-uniform transforms then map each sample exactly). */
static void arc_path(const M2 *M, double x0, double y0, double rx, double ry,
                     double phi, int laf, int sf, double x1, double y1)
{
    if (x0 == x1 && y0 == y1) return;
    if (rx <= 0 || ry <= 0) { spush(M, x1, y1); return; }
    double cs = cos(phi), sn = sin(phi);
    double dx2 = (x0 - x1) / 2.0, dy2 = (y0 - y1) / 2.0;
    double x1p = cs * dx2 + sn * dy2;
    double y1p = -sn * dx2 + cs * dy2;
    double lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lam > 1) { double r = sqrt(lam); rx *= r; ry *= r; }
    double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double co = den > 0 ? sqrt(num > 0 ? num / den : 0) : 0;
    if (laf == sf) co = -co;
    double cxp = co * rx * y1p / ry;
    double cyp = -co * ry * x1p / rx;
    double cx = cs * cxp - sn * cyp + (x0 + x1) / 2.0;
    double cy = sn * cxp + cs * cyp + (y0 + y1) / 2.0;
    double th0 = atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
    double th1 = atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx);
    double dth = th1 - th0;
    if (!sf && dth > 0) dth -= 2.0 * PI;
    if (sf && dth < 0) dth += 2.0 * PI;
    int steps = (int)(fabs(dth) / 0.26) + 1;      /* <= ~15 deg per step */
    if (steps > 128) steps = 128;
    for (int i = 1; i <= steps; i++) {
        double th = th0 + dth * i / steps;
        spush(M, cx + rx * cos(phi) * cos(th) - ry * sin(phi) * sin(th),
                  cy + rx * sin(phi) * cos(th) + ry * cos(phi) * sin(th));
    }
}

/* d="..." path parser: M L H V C S Q T A Z (+relative), implicit repeats,
 * subpath close tracking. Points land in the current subpath. */
static void path_add(const M2 *M, const char *d)
{
    double cx = 0, cy = 0;                     /* current point (user) */
    double sx = 0, sy = 0;                     /* subpath start */
    double qx = 0, qy = 0, fx = 0, fy = 0;     /* last controls (user) */
    int last_type = 0;                         /* 1 quad 2 cubic */
    char cmd = 0;
    int have = 0, started = 0, reseed = 0;
    const char *s = d;

    while (*s) {
        while (*s && (isspace((unsigned char)*s) || *s == ',')) s++;
        if (!*s) break;
        if (isalpha((unsigned char)*s)) { cmd = *s++; have = 1; }
        else if (!have) break;
        int rel = islower((unsigned char)cmd);
        char C = (char)toupper((unsigned char)cmd);
        double v[8];
        int nv;
        switch (C) {
        case 'Z': nv = 0; break;
        case 'H': case 'V': nv = 1; break;
        case 'M': case 'L': case 'T': nv = 2; break;
        case 'S': case 'Q': nv = 4; break;
        case 'C': nv = 6; break;
        case 'A': nv = 7; break;
        default: have = 0; continue;           /* unknown command letter */
        }
        for (int i = 0; i < nv; i++) {
            char *e;
            while (*s && (isspace((unsigned char)*s) || *s == ',')) s++;
            v[i] = strtod(s, &e);
            if (e == s) { have = 0; goto iter_done; }
            s = e;
        }
        if (C == 'Z') {
            if (started && nsub > 0) sub_cl[nsub - 1] = 1;
            cx = sx; cy = sy;
            last_type = 0;
            reseed = started;
            have = 0;                          /* next drawing cmd needs a letter */
            continue;
        }
        if (C != 'M' && reseed && started) {   /* new subpath after closepath */
            sub_begin();
            if (nsub > 0) spush(M, sx, sy);
            reseed = 0;
        }
        switch (C) {
        case 'M': {
            double x = v[0] + (rel ? cx : 0), y = v[1] + (rel ? cy : 0);
            sub_begin();
            spush(M, x, y);
            cx = sx = x; cy = sy = y;
            started = 1;
            reseed = 0;
            last_type = 0;
            cmd = rel ? 'l' : 'L';             /* extra pairs = lineto */
            break;
        }
        case 'L': case 'T': {
            double x = v[0] + (rel ? cx : 0), y = v[1] + (rel ? cy : 0);
            if (C == 'L') {
                spush(M, x, y);
                last_type = 0;
            } else {                            /* smooth quadratic */
                double px = cx, py = cy;
                if (last_type == 1) { px = 2 * cx - qx; py = 2 * cy - qy; }
                qx = px; qy = py;
                float ax, ay, bx, by, ex, ey;
                m_apply(M, cx, cy, &ax, &ay);
                m_apply(M, qx, qy, &bx, &by);
                m_apply(M, x, y, &ex, &ey);
                bez_quad(ax, ay, bx, by, ex, ey, 0);
                last_type = 1;
            }
            cx = x; cy = y;
            break;
        }
        case 'H': {
            double x = v[0] + (rel ? cx : 0);
            spush(M, x, cy);
            cx = x;
            last_type = 0;
            break;
        }
        case 'V': {
            double y = v[0] + (rel ? cy : 0);
            spush(M, cx, y);
            cy = y;
            last_type = 0;
            break;
        }
        case 'C': {
            double x1 = v[0] + (rel ? cx : 0), y1 = v[1] + (rel ? cy : 0);
            double x2 = v[2] + (rel ? cx : 0), y2 = v[3] + (rel ? cy : 0);
            double x = v[4] + (rel ? cx : 0), y = v[5] + (rel ? cy : 0);
            float ax, ay, b1x, b1y, b2x, b2y, ex, ey;
            m_apply(M, cx, cy, &ax, &ay);
            m_apply(M, x1, y1, &b1x, &b1y);
            m_apply(M, x2, y2, &b2x, &b2y);
            m_apply(M, x, y, &ex, &ey);
            bez_cubic(ax, ay, b1x, b1y, b2x, b2y, ex, ey, 0);
            fx = x2; fy = y2;
            cx = x; cy = y;
            last_type = 2;
            break;
        }
        case 'S': {
            double x1 = cx, y1 = cy;
            if (last_type == 2) { x1 = 2 * cx - fx; y1 = 2 * cy - fy; }
            double x2 = v[0] + (rel ? cx : 0), y2 = v[1] + (rel ? cy : 0);
            double x = v[2] + (rel ? cx : 0), y = v[3] + (rel ? cy : 0);
            float ax, ay, b1x, b1y, b2x, b2y, ex, ey;
            m_apply(M, cx, cy, &ax, &ay);
            m_apply(M, x1, y1, &b1x, &b1y);
            m_apply(M, x2, y2, &b2x, &b2y);
            m_apply(M, x, y, &ex, &ey);
            bez_cubic(ax, ay, b1x, b1y, b2x, b2y, ex, ey, 0);
            fx = x2; fy = y2;
            cx = x; cy = y;
            last_type = 2;
            break;
        }
        case 'Q': {
            double x1 = v[0] + (rel ? cx : 0), y1 = v[1] + (rel ? cy : 0);
            double x = v[2] + (rel ? cx : 0), y = v[3] + (rel ? cy : 0);
            float ax, ay, b1x, b1y, ex, ey;
            m_apply(M, cx, cy, &ax, &ay);
            m_apply(M, x1, y1, &b1x, &b1y);
            m_apply(M, x, y, &ex, &ey);
            bez_quad(ax, ay, b1x, b1y, ex, ey, 0);
            qx = x1; qy = y1;
            cx = x; cy = y;
            last_type = 1;
            break;
        }
        case 'A': {
            double rx = fabs(v[0]), ry = fabs(v[1]);
            double phi = v[2] * PI / 180.0;
            int laf = v[3] != 0.0, sf = v[4] != 0.0;
            double x = v[5] + (rel ? cx : 0), y = v[6] + (rel ? cy : 0);
            arc_path(M, cx, cy, rx, ry, phi, laf, sf, x, y);
            cx = x; cy = y;
            last_type = 0;
            break;
        }
        default:
            break;
        }
iter_done:;
    }
}

/* ------------------------------------------------------------- driver */

static int looks_skip(const char *name)
{
    static const char *s[] = { "defs", "clippath", "mask", "marker",
                               "symbol", "filter", "pattern", NULL };
    for (int i = 0; s[i]; i++) if (!strcasecmp(name, s[i])) return 1;
    return 0;
}

static float attr_len(int na, const char *name)
{
    const char *v = at_get(na, name);
    return v ? (float)num_attr(v) : 0.0f;
}

/* resolve element paint: base (inherited) + transform + presentation
 * attributes + style="" (CSS precedence) */
static void el_paint(int na, const M2 *ctm, const PAIN *base,
                     M2 *out_m, PAIN *out_p)
{
    const char *tr = at_get(na, "transform");
    *out_m = *ctm;
    if (tr) {
        M2 t = parse_transform(tr);
        *out_m = m_mul(ctm, &t);
    }
    *out_p = *base;
    static const char *props[] = {
        "fill", "stroke", "stroke-width", "stroke-linecap", "fill-rule",
        "fill-opacity", "stroke-opacity", "opacity", NULL
    };
    for (int i = 0; props[i]; i++) {
        const char *v = at_get(na, props[i]);
        if (v) set_prop(out_p, props[i], v);
    }
    const char *st = at_get(na, "style");
    if (st) apply_style(out_p, st);
}

uint8_t *k93svg_to_rgba(const char *svg, size_t len, int *out_w, int *out_h)
{
    if (!svg || len < 5 || len > 8u * 1024 * 1024 || !out_w || !out_h)
        return NULL;
    const char *p = svg, *end = svg + len;

    /* locate the root <svg ...> tag */
    const char *lt = NULL;
    for (const char *q = p; q + 4 < end; q++) {
        if (q[0] == '<' && tolower((unsigned char)q[1]) == 's' &&
            tolower((unsigned char)q[2]) == 'v' &&
            tolower((unsigned char)q[3]) == 'g' &&
            !isalnum((unsigned char)q[4])) {
            lt = q;
            break;
        }
    }
    if (!lt) return NULL;
    p = lt + 4;
    atbuf_used = 0;
    int selfclose = 0;
    int na = parse_attrs(&p, end, &selfclose);
    if (selfclose) return NULL;                  /* nothing to draw */

    double vb[4] = { 0, 0, 0, 0 };
    int has_vb = 0;
    const char *vbs = at_get(na, "viewBox");
    if (!vbs) vbs = at_get(na, "viewbox");       /* html parser lowercases */
    if (vbs) {
        const char *s = vbs;
        int i = 0;
        while (i < 4) {
            char *e;
            double d = strtod(s, &e);
            if (e == s) break;
            vb[i++] = d;
            s = e;
        }
        has_vb = i == 4 && vb[2] > 0 && vb[3] > 0;
    }
    double W = attr_len(na, "width"), H = attr_len(na, "height");
    if (W <= 0) W = has_vb ? vb[2] : 300.0;      /* SVG defaults */
    if (H <= 0) H = has_vb ? vb[3] : 150.0;
    CW = (int)(W + 0.5);
    CH = (int)(H + 0.5);
    if (CW < 1) CW = 1;
    if (CH < 1) CH = 1;
    if (CW > 2048) CW = 2048;
    if (CH > 2048) CH = 2048;

    M2 ctm = M_ID;
    if (has_vb) {                                /* preserveAspectRatio=
                                                  * xMidYMid meet */
        double s = CW / vb[2] < CH / vb[3] ? CW / vb[2] : CH / vb[3];
        ctm.a = ctm.d = (float)s;
        ctm.e = (float)((CW - vb[2] * s) / 2.0 - vb[0] * s);
        ctm.f = (float)((CH - vb[3] * s) / 2.0 - vb[1] * s);
    }
    PAIN pain = pain_default();
    el_paint(na, &ctm, &pain, &ctm, &pain);      /* root transform/paint */

    cv = calloc((size_t)CW * (size_t)CH, 4);
    if (!cv) return NULL;

    M2 stack[8];
    PAIN pstack[8];
    int sp = 0, skipd = 0;
    while (p < end) {
        const char *q = memchr(p, '<', (size_t)(end - p));
        if (!q) break;
        p = q + 1;
        if (p >= end) break;
        if (*p == '?') {                         /* xml declaration */
            while (p + 1 < end && !(p[0] == '?' && p[1] == '>')) p++;
            p += 2;
            continue;
        }
        if (*p == '!') {                         /* comment / doctype */
            if (p + 2 < end && p[1] == '-' && p[2] == '-') {
                p += 3;
                while (p + 2 < end &&
                       !(p[0] == '-' && p[1] == '-' && p[2] == '>')) p++;
                p += 3;
            } else {
                while (p < end && *p != '>') p++;
                p++;
            }
            continue;
        }
        if (*p == '/') {                         /* close tag */
            while (p < end && *p != '>') p++;
            if (p < end) p++;
            if (skipd) { skipd--; continue; }
            if (sp > 0) { sp--; ctm = stack[sp]; pain = pstack[sp]; }
            continue;
        }
        if (!isalpha((unsigned char)*p)) { p++; continue; }
        char name[32];
        int ni = 0;
        while (p < end && (isalnum((unsigned char)*p) || *p == '-' ||
                           *p == '_')) {
            if (ni < 31) name[ni++] = (char)tolower((unsigned char)*p);
            p++;
        }
        name[ni] = 0;
        atbuf_used = 0;
        na = parse_attrs(&p, end, &selfclose);
        if (skipd) { if (!selfclose) skipd++; continue; }
        if (looks_skip(name)) { if (!selfclose) skipd = 1; continue; }
        if (!strcmp(name, "g") || !strcmp(name, "svg")) {
            if (selfclose) continue;             /* nested svg = plain g */
            if (sp < 8) { stack[sp] = ctm; pstack[sp] = pain; sp++; }
            el_paint(na, &ctm, &pain, &ctm, &pain);
            continue;
        }
        /* leaf shapes: build subpaths, fill + stroke, composite */
        {
            M2 m;
            PAIN pn;
            el_paint(na, &ctm, &pain, &m, &pn);
            subs_reset();
            if (!strcmp(name, "rect")) {
                shp_rect(&m, num_attr(at_get(na, "x")),
                         num_attr(at_get(na, "y")),
                         num_attr(at_get(na, "width")),
                         num_attr(at_get(na, "height")),
                         num_attr(at_get(na, "rx")),
                         num_attr(at_get(na, "ry")));
                shape_emit(&m, &pn);
            } else if (!strcmp(name, "circle")) {
                double r = num_attr(at_get(na, "r"));
                shp_ellipse(&m, num_attr(at_get(na, "cx")),
                            num_attr(at_get(na, "cy")), r, r);
                shape_emit(&m, &pn);
            } else if (!strcmp(name, "ellipse")) {
                shp_ellipse(&m, num_attr(at_get(na, "cx")),
                            num_attr(at_get(na, "cy")),
                            num_attr(at_get(na, "rx")),
                            num_attr(at_get(na, "ry")));
                shape_emit(&m, &pn);
            } else if (!strcmp(name, "line")) {
                sub_begin();
                spush(&m, num_attr(at_get(na, "x1")),
                      num_attr(at_get(na, "y1")));
                spush(&m, num_attr(at_get(na, "x2")),
                      num_attr(at_get(na, "y2")));
                shape_emit(&m, &pn);
            } else if (!strcmp(name, "polyline")) {
                const char *pts = at_get(na, "points");
                if (pts) shp_points(&m, pts, 0);
                shape_emit(&m, &pn);
            } else if (!strcmp(name, "polygon")) {
                const char *pts = at_get(na, "points");
                if (pts) shp_points(&m, pts, 1);
                shape_emit(&m, &pn);
            } else if (!strcmp(name, "path")) {
                const char *dd = at_get(na, "d");
                if (dd) path_add(&m, dd);
                shape_emit(&m, &pn);
            }
            /* text/image/use/etc: not painted (ceiling) */
        }
    }
    *out_w = CW;
    *out_h = CH;
    return cv;
}

uint8_t *k93svg_to_png(const char *svg, size_t len, size_t *out_len,
                       int *out_w, int *out_h)
{
    int w = 0, h = 0;
    uint8_t *rgba = k93svg_to_rgba(svg, len, &w, &h);
    if (!rgba) return NULL;
    uint8_t *png = k93png_encode(rgba, w, h, out_len);
    free(rgba);
    if (!png) return NULL;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return png;
}

/* ----------------------------------------------------------- png codec */

static uint32_t crc_tab[256];
static int crc_ready;

static void crc_init(void)
{
    if (crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
    crc_ready = 1;
}

static uint32_t crc_run(uint32_t c, const uint8_t *d, size_t n)
{
    crc_init();
    for (size_t i = 0; i < n; i++)
        c = crc_tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}

uint32_t k93png_crc32(const void *data, size_t len)
{
    return crc_run(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

static uint32_t adler32_run(uint32_t a, const uint8_t *d, size_t n)
{
    uint32_t s1 = a & 0xFFFF, s2 = (a >> 16) & 0xFFFF;
    while (n > 0) {
        size_t k = n > 5552 ? 5552 : n;           /* mod block: no overflow */
        n -= k;
        while (k--) { s1 += *d++; s2 += s1; }
        s1 %= 65521;
        s2 %= 65521;
    }
    return (s2 << 16) | s1;
}

static uint8_t *put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return p + 4;
}

static uint32_t get32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint8_t *png_chunk(uint8_t *p, const char *type,
                          const uint8_t *data, size_t n)
{
    p = put32be(p, (uint32_t)n);
    memcpy(p, type, 4);
    uint32_t c = crc_run(0xFFFFFFFFu, (const uint8_t *)type, 4);
    c = crc_run(c, data, n);
    if (n) memcpy(p + 4, data, n);
    put32be(p + 4 + n, c ^ 0xFFFFFFFFu);
    return p + 8 + n;                        /* p already +4 past chunk start */
}

uint8_t *k93png_encode(const uint8_t *rgba, int w, int h, size_t *out_len)
{
    if (!rgba || !out_len || w < 1 || h < 1 || w > 16384 || h > 16384)
        return NULL;
    size_t row = (size_t)w * 4 + 1;
    size_t raw_len = row * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    if (!raw) return NULL;
    for (int y = 0; y < h; y++) {
        raw[row * y] = 0;                        /* filter: none */
        memcpy(raw + row * y + 1, rgba + (size_t)y * (size_t)w * 4,
               (size_t)w * 4);
    }
    /* zlib stream: 2 byte header, stored deflate blocks (max 64K-1 each),
     * then the adler32 of the uncompressed data - valid per RFC 1950/1951 */
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t zcap = 2 + raw_len + 5 * nblocks + 4;
    uint8_t *z = malloc(zcap);
    if (!z) { free(raw); return NULL; }
    z[0] = 0x78;
    z[1] = 0x01;
    size_t o = 2, left = raw_len, off = 0;
    while (left > 0) {
        size_t take = left > 65535 ? 65535 : left;
        z[o++] = (uint8_t)(take == left ? 1 : 0);   /* BFINAL, BTYPE=00 */
        z[o++] = (uint8_t)(take & 0xFF);
        z[o++] = (uint8_t)(take >> 8);
        z[o++] = (uint8_t)(~take & 0xFF);
        z[o++] = (uint8_t)((~take >> 8) & 0xFF);
        memcpy(z + o, raw + off, take);
        o += take;
        off += take;
        left -= take;
    }
    put32be(z + o, adler32_run(1, raw, raw_len));
    o += 4;
    free(raw);

    uint8_t ihdr[13];
    uint8_t *q = ihdr;
    q = put32be(q, (uint32_t)w);
    q = put32be(q, (uint32_t)h);
    *q++ = 8;                                    /* bit depth */
    *q++ = 6;                                    /* color type: RGBA */
    *q++ = 0;
    *q++ = 0;                                    /* compression, filter */
    *q++ = 0;                                    /* interlace */

    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t *out = malloc(8 + 12 + 13 + 12 + o + 12);
    if (!out) { free(z); return NULL; }
    uint8_t *p = out;
    memcpy(p, sig, 8);
    p += 8;
    p = png_chunk(p, "IHDR", ihdr, 13);
    p = png_chunk(p, "IDAT", z, o);
    p = png_chunk(p, "IEND", NULL, 0);
    free(z);
    *out_len = (size_t)(p - out);
    return out;
}

uint8_t *k93png_decode_stored(const uint8_t *png, size_t len,
                              int *out_w, int *out_h)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (!png || len < 8 + 37 || !out_w || !out_h || memcmp(png, sig, 8))
        return NULL;
    int w = 0, h = 0;
    uint8_t *z = NULL;
    size_t zlen = 0, pos = 8;
    while (pos + 12 <= len) {
        uint32_t cl = get32be(png + pos);
        const uint8_t *type = png + pos + 4;
        const uint8_t *data = png + pos + 8;
        if (pos + 12 + (size_t)cl > len) { free(z); return NULL; }
        uint32_t want = get32be(png + pos + 8 + cl);
        uint32_t got = crc_run(0xFFFFFFFFu, type, 4 + cl) ^ 0xFFFFFFFFu;
        if (want != got) { free(z); return NULL; }
        if (!memcmp(type, "IHDR", 4) && cl == 13) {
            w = (int)get32be(data);
            h = (int)get32be(data + 4);
            if (w < 1 || h < 1 || w > 16384 || h > 16384) { free(z); return NULL; }
            if (data[8] != 8 || data[9] != 6 || data[12] != 0) {
                free(z);
                return NULL;
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *nz = realloc(z, zlen + cl);  /* concat spec-split IDATs */
            if (!nz) { free(z); return NULL; }
            z = nz;
            memcpy(z + zlen, data, cl);
            zlen += cl;
        }
        pos += 12 + (size_t)cl;
    }
    if (!z || w < 1 || h < 1 || zlen < 8) { free(z); return NULL; }
    if ((z[0] & 0x0F) != 8 || ((unsigned)z[0] * 256 + z[1]) % 31 != 0) {
        free(z);
        return NULL;
    }
    size_t raw_len = (size_t)h * ((size_t)w * 4 + 1);
    uint8_t *raw = malloc(raw_len);
    if (!raw) { free(z); return NULL; }
    size_t ro = 0, zo = 2;
    for (;;) {
        if (zo + 5 > zlen) goto bad;
        uint8_t hdr = z[zo++];
        if ((hdr >> 1) & 3) goto bad;             /* stored blocks only */
        size_t bl = (size_t)z[zo] | ((size_t)z[zo + 1] << 8);
        size_t nl = (size_t)z[zo + 2] | ((size_t)z[zo + 3] << 8);
        zo += 4;
        if ((bl ^ 0xFFFF) != nl || zo + bl > zlen || ro + bl > raw_len)
            goto bad;
        memcpy(raw + ro, z + zo, bl);
        ro += bl;
        zo += bl;
        if (hdr & 1) break;
    }
    if (zo + 4 > zlen || ro != raw_len) goto bad;
    if (get32be(z + zo) != adler32_run(1, raw, raw_len)) goto bad;
    uint8_t *out = malloc((size_t)w * (size_t)h * 4);
    if (!out) goto bad;
    for (int y = 0; y < h; y++) {
        size_t rs = (size_t)h > 0 ? (size_t)y * ((size_t)w * 4 + 1) : 0;
        if (raw[rs] != 0) {                       /* encoder uses filter 0 */
            free(out);
            goto bad;
        }
        memcpy(out + (size_t)y * (size_t)w * 4, raw + rs + 1,
               (size_t)w * 4);
    }
    free(raw);
    free(z);
    *out_w = w;
    *out_h = h;
    return out;
bad:
    free(raw);
    free(z);
    return NULL;
}



