/* k93font.c - clean-room TrueType font loader + glyph rasterizer.
 *
 * Written from scratch against the PUBLISHED TrueType/OpenType format
 * documentation (Apple "The TrueType Reference Manual", Microsoft
 * "OpenType specification" - format descriptions only, no font-rendering
 * code was consulted or copied). Scope:
 *   sfnt container, head/maxp/hhea/hmtx, loca+glyf outlines
 *   (simple + composite), cmap formats 4 and 12, kern format 0.
 * Rendering: outlines are flattened (adaptive de Casteljau on the
 * quadratics) and filled with a 4x4-supersampled nonzero-winding scanline
 * - unhinted by design; the supersampling is the smoothing.
 *
 * Ceilings (upgrade paths noted inline):
 *   - CFF/OTF outlines rejected at load ("OTTO"): add a charstring
 *     interpreter if a CFF-only font ever matters.
 *   - No hinting (`prep`/`fpgm`/glyph instructions are skipped).
 *   - Composites with point-matching arguments (legacy) are skipped;
 *     XY-offset/scale composites are fully supported.
 *   - kern: one horizontal format-0 subtable (MS layout; Apple's
 *     version-1 layout skipped).
 *   - cmap: formats 4 and 12 only.
 *   - <= 32 family/weight slots, <= 64K outline points per glyph. */
#include "k93font.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* local floor/ceil on float: keeps the library libm-free (static links) */
static int ifl(float v) { int i = (int)v; return (v < 0 && (float)i != v) ? i - 1 : i; }
static int ice(float v) { int i = (int)v; return (v > 0 && (float)i != v) ? i + 1 : i; }

#define FONTDIR "/usr/share/fonts/k93"
#define MAXF    32

/* ---------------------------------------------------------------- basics */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static int16_t  rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

uint32_t k93_utf8_next(const char *s, int *i)
{
    const unsigned char *u = (const unsigned char *)s;
    int j = *i;
    unsigned c = u[j];
    if (c < 0x80) { *i = j + 1; return c; }
    int n = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
    else { *i = j + 1; return c; }                  /* stray byte: raw */
    for (int k = 1; k <= n; k++) {
        unsigned cc = u[j + k];
        if ((cc & 0xC0) != 0x80) { *i = j + 1; return c; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *i = j + n + 1;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { *i = j + 1; return c; }
    return cp;
}

/* ------------------------------------------------------- font container */

typedef struct {
    char name[48];                 /* family name (weight suffix stripped) */
    char path[200];
    int  bold;                     /* family id of -Bold sibling or -1 */
    uint8_t *data;
    size_t len;
    const uint8_t *glyf, *loca, *hhea, *hmtx;
    size_t glyf_len;
    const uint8_t *sub;            /* chosen cmap subtable */
    size_t sub_len;
    int  sub_fmt;                  /* 4 or 12 */
    const uint8_t *kpair;          /* kern format-0 pair array */
    int  npair;
    int  upem, nglyphs, loca_long, nhm, asc, desc;
    int  loaded;                   /* 0 not yet, 1 ok, -1 failed */
} Font;

static Font F[MAXF];
static int  nF, def_id = -1, cur = -1;

static int tag_is(const uint8_t *rec, const char *t)
{
    return rec[0] == t[0] && rec[1] == t[1] && rec[2] == t[2] && rec[3] == t[3];
}

static const uint8_t *table(const Font *f, const char *t, size_t *out_len)
{
    if (f->len < 12) return NULL;
    int nt = rd16(f->data + 4);
    if (nt < 1 || nt > 64 || 12 + (size_t)nt * 16 > f->len) return NULL;
    for (int i = 0; i < nt; i++) {
        const uint8_t *rec = f->data + 12 + (size_t)i * 16;
        if (!tag_is(rec, t)) continue;
        uint32_t off = rd32(rec + 8), ln = rd32(rec + 12);
        if ((size_t)off + ln > f->len || ln == 0) return NULL;
        *out_len = ln;
        return f->data + off;
    }
    return NULL;
}

static int choose_cmap(Font *f)
{
    size_t n;
    const uint8_t *cm = table(f, "cmap", &n);
    if (!cm || n < 4) return -1;
    int ntab = rd16(cm + 2);
    if (ntab < 1 || 4 + (size_t)ntab * 8 > n) return -1;
    int best = -1, bestscore = -1;
    for (int i = 0; i < ntab; i++) {
        const uint8_t *rec = cm + 4 + (size_t)i * 8;
        int plat = rd16(rec), enc = rd16(rec + 2);
        uint32_t off = rd32(rec + 4);
        if ((size_t)off + 4 > n) continue;
        int fmt = rd16(cm + off);
        int score = -1;
        if (fmt == 12 && plat == 3 && enc == 10) score = 100;
        else if (fmt == 12 && plat == 0) score = 90;
        else if (fmt == 12) score = 80;
        else if (fmt == 4 && plat == 3 && enc == 1) score = 60;
        else if (fmt == 4 && plat == 0) score = 55;
        else if (fmt == 4) score = 40;
        if (score > bestscore) { bestscore = score; best = i; }
    }
    if (best < 0) return -1;
    uint32_t off = rd32(cm + 4 + (size_t)best * 8 + 4);
    const uint8_t *st = cm + off;
    size_t avail = n - off;
    int fmt = rd16(st);
    if (fmt == 12) {
        if (avail < 16) return -1;
        size_t ln = rd32(st + 4);
        if (ln < 16 || ln > avail || 16 + (size_t)rd32(st + 12) * 12 > ln)
            return -1;
    } else if (fmt == 4) {
        if (avail < 16) return -1;
        size_t ln = rd16(st + 2);
        if (ln < 16 || ln > avail) return -1;
    } else {
        return -1;
    }
    f->sub = st;
    f->sub_len = fmt == 12 ? rd32(st + 4) : rd16(st + 2);
    f->sub_fmt = fmt;
    return 0;
}

static int font_parse(Font *f)
{
    if (f->len < 12) return -1;
    uint32_t ver = rd32(f->data);
    if (ver != 0x00010000 && ver != 0x74727565) return -1;  /* not TrueType */
    size_t n;
    const uint8_t *head = table(f, "head", &n);
    if (!head || n < 54) return -1;
    f->upem = rd16(head + 18);
    if (f->upem < 16 || f->upem > 16384) return -1;
    f->loca_long = rds16(head + 50) == 1;

    const uint8_t *maxp = table(f, "maxp", &n);
    if (!maxp || n < 6) return -1;
    f->nglyphs = rd16(maxp + 4);
    if (f->nglyphs < 1) return -1;

    f->hhea = table(f, "hhea", &n);
    if (!f->hhea || n < 36) return -1;
    f->asc = rds16(f->hhea + 4);
    f->desc = rds16(f->hhea + 6);
    f->nhm = rd16(f->hhea + 34);
    if (f->asc <= f->desc || f->nhm < 1) {             /* sane fallback */
        f->asc = (int)(f->upem * 0.8);
        f->desc = -(int)(f->upem * 0.2);
        if (f->asc <= f->desc) return -1;
    }

    f->hmtx = table(f, "hmtx", &n);
    if (!f->hmtx) return -1;
    size_t hneed = (size_t)f->nhm * 4;
    if (hneed > n) return -1;
    if (f->nglyphs > f->nhm && (size_t)(f->nglyphs - f->nhm) * 2 + hneed > n)
        return -1;

    size_t lneed = (size_t)(f->nglyphs + 1) * (f->loca_long ? 4 : 2);
    f->loca = table(f, "loca", &n);
    if (!f->loca || n < lneed) return -1;
    size_t gn;
    f->glyf = table(f, "glyf", &gn);
    if (!f->glyf || gn < 2) return -1;
    f->glyf_len = gn;
    if (choose_cmap(f) != 0) return -1;

    /* kern: MS layout (version u16 == 0); accept format 0, horizontal,
     * non-cross-stream, both coverage byte orders (spec readers differ). */
    const uint8_t *kern = table(f, "kern", &n);
    f->kpair = NULL;
    f->npair = 0;
    if (kern && n >= 4 && rd16(kern) == 0) {
        int ntab = rd16(kern + 2);
        size_t off = 4;
        for (int i = 0; i < ntab && off + 14 <= n; i++) {
            const uint8_t *st = kern + off;
            int ln = rd16(st + 2), cov = rd16(st + 4);
            if (ln < 14 || off + (size_t)ln > n) break;
            int fmt = (cov >> 8) ? (cov >> 8) : (cov & 0xFF);
            int horiz = (cov & 0x0101) != 0;
            int cross = (cov & 0x0202) != 0;
            if (fmt == 0 && horiz && !cross) {
                int np = rd16(st + 6);
                if (np >= 0 && 14 + (size_t)np * 6 <= (size_t)ln) {
                    f->kpair = st + 14;
                    f->npair = np;
                    break;
                }
            }
            off += (size_t)ln;
        }
    }
    return 0;
}

static int ensure(int id)
{
    if (id < 0 || id >= nF) return 0;
    Font *f = &F[id];
    if (f->loaded == 1) return 1;
    if (f->loaded < 0) return 0;
    FILE *fp = fopen(f->path, "rb");
    if (!fp) { f->loaded = -1; return 0; }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); f->loaded = -1; return 0; }
    long sz = ftell(fp);
    if (sz < 64 || sz > 32 * 1024 * 1024) {
        fclose(fp); f->loaded = -1; return 0;
    }
    rewind(fp);
    uint8_t *d = malloc((size_t)sz);
    if (!d) { fclose(fp); return 0; }
    if (fread(d, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp); free(d); f->loaded = -1; return 0;
    }
    fclose(fp);
    f->data = d;
    f->len = (size_t)sz;
    if (font_parse(f) != 0) { f->loaded = -1; return 0; }
    f->loaded = 1;
    return 1;
}

/* --------------------------------------------------------- cmap + metrics */

static int cmap4(const uint8_t *st, size_t n, uint32_t cp)
{
    if (cp > 0xFFFF || n < 16) return -1;
    int seg2 = rd16(st + 6);
    if (seg2 < 2 || (seg2 & 1) || 14 + (size_t)seg2 * 4 + 2 > n) return -1;
    int segs = seg2 / 2;
    const uint8_t *ends = st + 14;
    const uint8_t *starts = ends + seg2 + 2;
    const uint8_t *deltas = starts + seg2;
    const uint8_t *ros = deltas + seg2;
    for (int i = 0; i < segs; i++) {
        uint32_t end = rd16(ends + 2 * i);
        if (cp > end) continue;
        uint32_t start = rd16(starts + 2 * i);
        if (cp < start) return -1;
        int ro = rd16(ros + 2 * i);
        if (ro) {
            size_t addr = (size_t)(ros + 2 * i - st) + (size_t)ro +
                          2 * (cp - start);
            if (addr + 2 > n) return -1;
            int g = rd16(st + addr);
            if (!g) return -1;
            return g;
        }
        return (int)((cp + (int16_t)rd16(deltas + 2 * i)) & 0xFFFF);
    }
    return -1;
}

static int cmap12(const uint8_t *st, size_t n, uint32_t cp)
{
    if (n < 16) return -1;
    uint32_t ng = rd32(st + 12);
    if (16 + (size_t)ng * 12 > n) return -1;
    uint32_t lo = 0, hi = ng;
    const uint8_t *grp = st + 16;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *g = grp + (size_t)mid * 12;
        uint32_t a = rd32(g), b = rd32(g + 4);
        if (cp < a) hi = mid;
        else if (cp > b) lo = mid + 1;
        else {
            uint32_t v = rd32(g + 8) + (cp - a);
            return v <= 0xFFFF ? (int)v : -1;
        }
    }
    return -1;
}

static int cmap_lookup(const Font *f, uint32_t cp)
{
    int g = f->sub_fmt == 12 ? cmap12(f->sub, f->sub_len, cp)
                             : cmap4(f->sub, f->sub_len, cp);
    if (g < 0 || g >= f->nglyphs) return -1;
    return g;
}

static int kern_px(const Font *f, int gl, int gr, float sc)
{
    if (!f->kpair || f->npair < 1 || gl < 0 || gr < 0) return 0;
    uint32_t key = ((uint32_t)gl << 16) | (uint32_t)gr;
    int lo = 0, hi = f->npair - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *e = f->kpair + (size_t)mid * 6;
        uint32_t k = ((uint32_t)rd16(e) << 16) | rd16(e + 2);
        if (k < key) lo = mid + 1;
        else if (k > key) hi = mid - 1;
        else {
            float v = (float)rds16(e + 4) * sc;
            return (int)(v + (v >= 0 ? 0.5f : -0.5f));
        }
    }
    return 0;
}

static int hmtx_adv(const Font *f, int gid)
{
    if (gid < 0) gid = 0;
    if (gid >= f->nhm) gid = f->nhm - 1;      /* trailing glyphs repeat */
    return rd16(f->hmtx + (size_t)gid * 4);
}

/* -------------------------------------------------- outline accumulation */
/* Points are collected in FONT UNITS with an affine applied at push time,
 * so composite recursion just composes affines. */

typedef struct { float x, y; } PT;
typedef struct { float x0, y0, x1, y1; } SEG;

static PT      *pt;
static uint8_t *pon;
static int      npt, cappt, capon;
static int     *ct_start, nct, capct;
static SEG     *sg;
static int      nsg, capsg;
typedef struct { float x; int dir; } CROSS;
static uint16_t *acc;
static int      capacc;
static CROSS   *cross;
static int      capcross;

static int grow(void **base, int *cap, int need, size_t el)
{
    if (*cap >= need) return 0;
    int nc = *cap ? *cap : 128;
    while (nc < need) nc *= 2;
    void *p = realloc(*base, (size_t)nc * el);
    if (!p) return -1;
    *base = p;
    *cap = nc;
    return 0;
}

/* affine: x' = a*x + c*y + e ; y' = b*x + d*y + f */
typedef struct { float a, b, c, d, e, f; } AFF;
static const AFF IDENTITY = { 1, 0, 0, 1, 0, 0 };
static AFF aff_compose(const AFF *P, const AFF *C)
{
    AFF t;
    t.a = P->a * C->a + P->c * C->b;
    t.c = P->a * C->c + P->c * C->d;
    t.b = P->b * C->a + P->d * C->b;
    t.d = P->b * C->c + P->d * C->d;
    t.e = P->a * C->e + P->c * C->f + P->e;
    t.f = P->b * C->e + P->d * C->f + P->f;
    return t;
}

static int ppush(const AFF *M, float x, float y, int iso)
{
    if (grow((void **)&pt, &cappt, npt + 1, sizeof *pt)) return -1;
    if (grow((void **)&pon, &capon, npt + 1, 1)) return -1;
    pt[npt].x = M->a * x + M->c * y + M->e;
    pt[npt].y = M->b * x + M->d * y + M->f;
    pon[npt] = (uint8_t)iso;
    npt++;
    return 0;
}

static int ctbeg(void)
{
    if (grow((void **)&ct_start, &capct, nct + 1, sizeof *ct_start)) return -1;
    ct_start[nct++] = npt;
    return 0;
}

/* parse one simple glyph's points and push them as contours */
static int load_simple(const Font *f, const uint8_t *g, size_t glen,
                       const AFF *M)
{
    if (glen < 12) return -1;
    int nc = rds16(g);
    if (nc <= 0) return -1;
    if (10 + (size_t)nc * 2 + 2 > glen) return -1;
    int n = rd16(g + 10 + (size_t)(nc - 1) * 2) + 1;
    if (n < 1 || n > 60000) return -1;

    size_t ip = 10 + (size_t)nc * 2;
    int ilen = rd16(g + ip);
    ip += 2 + (size_t)ilen;
    if (ip > glen) return -1;

    uint8_t *fl = malloc((size_t)n);
    if (!fl) return -1;
    int i = 0;
    while (i < n) {
        if (ip >= glen) { free(fl); return -1; }
        uint8_t v = g[ip++];
        fl[i++] = v;
        if (v & 0x08) {                       /* REPEAT */
            if (ip >= glen) { free(fl); return -1; }
            int r = g[ip++];
            while (r-- > 0 && i < n) fl[i++] = v;
        }
    }

    float *xs = malloc((size_t)n * sizeof *xs);
    float *ys = malloc((size_t)n * sizeof *ys);
    if (!xs || !ys) { free(fl); free(xs); free(ys); return -1; }

    float x = 0;
    for (int k = 0; k < n; k++) {
        uint8_t v = fl[k];
        if (v & 0x02) {                       /* x short */
            if (ip >= glen) goto bad;
            float d = (float)g[ip++];
            x += (v & 0x10) ? d : -d;
        } else if (!(v & 0x10)) {             /* x int16 */
            if (ip + 2 > glen) goto bad;
            x += (float)rds16(g + ip);
            ip += 2;
        }                                      /* else: x unchanged */
        xs[k] = x;
    }
    float y = 0;
    for (int k = 0; k < n; k++) {
        uint8_t v = fl[k];
        if (v & 0x04) {                       /* y short */
            if (ip >= glen) goto bad;
            float d = (float)g[ip++];
            y += (v & 0x20) ? d : -d;
        } else if (!(v & 0x20)) {
            if (ip + 2 > glen) goto bad;
            y += (float)rds16(g + ip);
            ip += 2;
        }
        ys[k] = y;
    }

    /* push each contour (endPts boundaries) */
    int start = 0;
    for (int c = 0; c < nc; c++) {
        int end = rd16(g + 10 + (size_t)c * 2);
        if (end >= n || end < start) goto bad;
        if (ctbeg() < 0) goto bad;
        for (int k = start; k <= end; k++)
            if (ppush(M, xs[k], ys[k], fl[k] & 1) < 0) goto bad;
        start = end + 1;
    }
    free(fl); free(xs); free(ys);
    return 0;
bad:
    free(fl); free(xs); free(ys);
    return -1;
}

/* load one glyph's points (font units, affine-applied) into pt/pon/ct_start */
static void load_glyph(const Font *f, int gid, const AFF *M, int depth)
{
    if (gid < 0 || gid >= f->nglyphs || depth > 8) return;
    size_t off, next;
    if (f->loca_long) {
        const uint8_t *L = f->loca;
        off = rd32(L + (size_t)gid * 4);
        next = rd32(L + (size_t)gid * 4 + 4);
    } else {
        const uint8_t *L = f->loca;
        off = (size_t)rd16(L + (size_t)gid * 2) * 2;
        next = (size_t)rd16(L + (size_t)gid * 2 + 2) * 2;
    }
    if (next <= off || next > f->glyf_len) return;
    if (off + 2 > next) return;
    size_t glen = next - off;
    const uint8_t *g = f->glyf + off;
    int nc = rds16(g);

    if (nc > 0) {
        load_simple(f, g, glen, M);
        return;
    }
    if (nc == 0) return;

    /* composite */
    size_t ip = 10;
    for (;;) {
        if (ip + 4 > glen) return;
        int flags = rd16(g + ip);
        int cgid = rd16(g + ip + 2);
        ip += 4;
        float dx = 0, dy = 0;
        if (flags & 0x0001) {                 /* ARG_1_AND_2_ARE_WORDS */
            if (ip + 4 > glen) return;
            int16_t a1 = rds16(g + ip), a2 = rds16(g + ip + 2);
            ip += 4;
            if (flags & 0x0002) { dx = (float)a1; dy = (float)a2; }
            /* point-matching args: skipped (legacy; ceiling above) */
        } else {
            if (ip + 2 > glen) return;
            int8_t a1 = (int8_t)g[ip], a2 = (int8_t)g[ip + 1];
            ip += 2;
            if (flags & 0x0002) { dx = (float)a1; dy = (float)a2; }
        }
        float sx = 1, sy = 1, hi = 0, hj = 0;
        if (flags & 0x0080) {                 /* 2x2: four F2Dot14 */
            if (ip + 8 > glen) return;
            sx = (float)rds16(g + ip) / 16384.0f;
            hj = (float)rds16(g + ip + 2) / 16384.0f;
            hi = (float)rds16(g + ip + 4) / 16384.0f;
            sy = (float)rds16(g + ip + 6) / 16384.0f;
            ip += 8;
        } else if (flags & 0x0040) {          /* x/y scale: two F2Dot14 */
            if (ip + 4 > glen) return;
            sx = (float)rds16(g + ip) / 16384.0f;
            sy = (float)rds16(g + ip + 2) / 16384.0f;
            ip += 4;
        } else if (flags & 0x0008) {          /* scale: two F2Dot14 */
            if (ip + 4 > glen) return;
            sx = (float)rds16(g + ip) / 16384.0f;
            sy = (float)rds16(g + ip + 2) / 16384.0f;
            ip += 4;
        }
        AFF C = { sx, hi, hj, sy, dx, dy };
        AFF T = aff_compose(M, &C);
        load_glyph(f, cgid, &T, depth + 1);
        if (!(flags & 0x0020)) break;        /* MORE_COMPONENTS */
    }
}

/* ------------------------------------------------------- outline -> fill */

/* adaptive de Casteljau flatten of a quadratic into the segment list */
static void quad_seg(float x0, float y0, float x1, float y1,
                     float x2, float y2, int d)
{
    float dx = x2 - x0, dy = y2 - y0;
    float dd = dx * dx + dy * dy;
    float e = (x1 - x0) * dy - (y1 - y0) * dx;
    float dist2 = dd > 1e-6f ? (e * e) / dd
                             : ((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    if (dist2 <= 0.0625f || d >= 12) {          /* <= 0.25 px off chord */
        if (x0 != x2 || y0 != y2) {
            if (grow((void **)&sg, &capsg, nsg + 1, sizeof *sg) == 0) {
                sg[nsg].x0 = x0; sg[nsg].y0 = y0;
                sg[nsg].x1 = x2; sg[nsg].y1 = y2;
                nsg++;
            }
        }
        return;
    }
    float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
    float nx = (x1 + x2) * 0.5f, ny = (y1 + y2) * 0.5f;
    float cx = (mx + nx) * 0.5f, cy = (my + ny) * 0.5f;
    quad_seg(x0, y0, mx, my, cx, cy, d + 1);
    quad_seg(cx, cy, nx, ny, x2, y2, d + 1);
}

static void line_to(float x0, float y0, float x1, float y1)
{
    if (x0 == x1 && y0 == y1) return;
    if (grow((void **)&sg, &capsg, nsg + 1, sizeof *sg)) return;
    sg[nsg].x0 = x0; sg[nsg].y0 = y0;
    sg[nsg].x1 = x1; sg[nsg].y1 = y1;
    nsg++;
}

/* Convert accumulated contours (font units) to px segments.
 * X = x*sc ; Y = -y*sc (y axis flipped; coordinates are baseline-relative,
 * pen-origin at x=0). */
static void flatten(float sc)
{
    nsg = 0;
    for (int c = 0; c < nct; c++) {
        int s = ct_start[c];
        int e = (c + 1 < nct) ? ct_start[c + 1] : npt;
        int n = e - s;
        if (n < 2) continue;
        float *X = malloc((size_t)n * sizeof *X);
        float *Y = malloc((size_t)n * sizeof *Y);
        uint8_t *on = malloc((size_t)n);
        if (!X || !Y || !on) { free(X); free(Y); free(on); continue; }
        for (int k = 0; k < n; k++) {
            X[k] = pt[s + k].x * sc;
            Y[k] = -pt[s + k].y * sc;
            on[k] = pon[s + k];
        }

        int fi = -1;
        for (int k = 0; k < n; k++) if (on[k]) { fi = k; break; }
        float sx, sy;                            /* contour start point */
        int i0;
        if (fi < 0) {                            /* all-off-curve contour */
            sx = (X[n - 1] + X[0]) * 0.5f;
            sy = (Y[n - 1] + Y[0]) * 0.5f;
            i0 = 0;
        } else {
            sx = X[fi]; sy = Y[fi];
            i0 = (fi + 1) % n;
        }
        float cx = sx, cy = sy;                  /* current point */
        float kx = 0, ky = 0;                    /* pending control */
        int have = 0;
        int i = i0;
        for (int step = 0; step < n; step++) {
            if (on[i]) {
                if (have) { quad_seg(cx, cy, kx, ky, X[i], Y[i], 0); have = 0; }
                else line_to(cx, cy, X[i], Y[i]);
                cx = X[i]; cy = Y[i];
            } else {
                if (have) {                     /* implied on-curve mid */
                    float mx = (kx + X[i]) * 0.5f, my = (ky + Y[i]) * 0.5f;
                    quad_seg(cx, cy, kx, ky, mx, my, 0);
                    cx = mx; cy = my;
                }
                kx = X[i]; ky = Y[i];
                have = 1;
            }
            i = (i + 1) % n;
        }
        if (have) quad_seg(cx, cy, kx, ky, sx, sy, 0);
        else line_to(cx, cy, sx, sy);
        free(X); free(Y); free(on);
    }
}

/* 4x4 supersampled nonzero-winding scanline fill -> tight alpha bitmap.
 * Returns malloc'd w*h coverage or NULL (nothing to draw). */
static uint8_t *rasterize(int *ow, int *oh, int *ox, int *oy)
{
    if (nsg < 1) return NULL;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < nsg; i++) {
        if (sg[i].x0 < minx) minx = sg[i].x0;
        if (sg[i].y0 < miny) miny = sg[i].y0;
        if (sg[i].x1 < minx) minx = sg[i].x1;
        if (sg[i].y1 < miny) miny = sg[i].y1;
        if (sg[i].x0 > maxx) maxx = sg[i].x0;
        if (sg[i].y0 > maxy) maxy = sg[i].y0;
        if (sg[i].x1 > maxx) maxx = sg[i].x1;
        if (sg[i].y1 > maxy) maxy = sg[i].y1;
    }
    int x0 = ifl(minx) - 1, y0 = ifl(miny) - 1;
    int x1 = ice(maxx) + 1, y1 = ice(maxy) + 1;
    if (x0 < -4096) x0 = -4096;                  /* runaway outline guard */
    if (y0 < -4096) y0 = -4096;
    if (x1 > 4096) x1 = 4096;
    if (y1 > 4096) y1 = 4096;
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return NULL;
    if (grow((void **)&acc, &capacc, w * h, sizeof *acc)) return NULL;
    memset(acc, 0, (size_t)w * h * sizeof *acc);
    if (grow((void **)&cross, &capcross, nsg, sizeof *cross)) return NULL;

    for (int sy = 0; sy < 4 * h; sy++) {
        float y = (float)y0 + ((float)sy + 0.5f) * 0.25f;
        int nc = 0;
        for (int i = 0; i < nsg; i++) {
            float ya = sg[i].y0, yb = sg[i].y1;
            if (ya == yb) continue;
            if (y < (ya < yb ? ya : yb) || y >= (ya < yb ? yb : ya)) continue;
            float x = sg[i].x0 + (y - ya) * (sg[i].x1 - sg[i].x0) / (yb - ya);
            cross[nc].x = x;
            cross[nc].dir = yb > ya ? 1 : -1;
            nc++;
        }
        for (int i = 1; i < nc; i++) {            /* insertion sort by x */
            CROSS v = cross[i];
            int j = i - 1;
            while (j >= 0 && cross[j].x > v.x) { cross[j + 1] = cross[j]; j--; }
            cross[j + 1] = v;
        }
        int wind = 0;
        for (int i = 0; i + 1 < nc; i++) {
            wind += cross[i].dir;
            if (!wind) continue;
            float xa = cross[i].x, xb = cross[i + 1].x;
            if (xb <= 0) continue;
            int px0 = ifl(xa);
            if (px0 < 0) px0 = 0;
            int px1 = ifl(xb);
            if (px1 > w - 1) px1 = w - 1;
            if (xa < 0) xa = 0;
            if (xb > (float)w) xb = (float)w;
            int row = (sy >> 2) * w;
            for (int px = px0; px <= px1; px++) {
                float l = xa > (float)px ? xa : (float)px;
                float r = xb < (float)px + 1 ? xb : (float)px + 1;
                if (r <= l) continue;
                int add = (int)((r - l) * 64.0f + 0.5f);   /* 64 = 1/64 px */
                if (add > 0) {
                    unsigned v = (unsigned)acc[row + px] + (unsigned)add;
                    acc[row + px] = (uint16_t)(v > 256 ? 256 : v);
                }
            }
        }
    }

    /* trim empty rows/cols for a tight bitmap */
    int tr = 0, br = h - 1, lc = 0, rc = w - 1;
    while (tr <= br) {
        int any = 0;
        for (int x = 0; x < w; x++) if (acc[tr * w + x]) { any = 1; break; }
        if (any) break;
        tr++;
    }
    while (br >= tr) {
        int any = 0;
        for (int x = 0; x < w; x++) if (acc[br * w + x]) { any = 1; break; }
        if (any) break;
        br--;
    }
    while (lc <= rc) {
        int any = 0;
        for (int y = tr; y <= br; y++) if (acc[y * w + lc]) { any = 1; break; }
        if (any) break;
        lc++;
    }
    while (rc >= lc) {
        int any = 0;
        for (int y = tr; y <= br; y++) if (acc[y * w + rc]) { any = 1; break; }
        if (any) break;
        rc--;
    }
    if (tr > br || lc > rc) return NULL;
    int tw = rc - lc + 1, th = br - tr + 1;
    uint8_t *out = malloc((size_t)tw * th);
    if (!out) return NULL;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            unsigned v = acc[(tr + y) * w + (lc + x)];
            out[y * tw + x] = (uint8_t)(v >= 256 ? 255 : (v * 255) / 256);
        }
    *ow = tw; *oh = th;
    *ox = x0 + lc; *oy = y0 + tr;
    return out;
}

/* ---------------------------------------------------- family file scan */

static int str_ieq_tail(const char *s, const char *tail)
{
    size_t ls = strlen(s), lt = strlen(tail);
    if (ls < lt) return 0;
    const char *p = s + ls - lt;
    for (size_t i = 0; i < lt; i++)
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)tail[i]))
            return 0;
    return 1;
}

/* strip ".ttf" + a weight suffix -> family base name; kind:
 * 0 = regular/none, 1 = bold, 2 = other (italic etc.) */
static void family_base(const char *path, char *out, size_t n, int *kind)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    snprintf(out, n, "%s", b);
    size_t L = strlen(out);
    if (L > 4 && str_ieq_tail(out, ".ttf")) out[L -= 4] = 0;
    *kind = 0;
    static const struct { const char *sfx; int kind; } w[] = {
        { "-bolditalic", 2 }, { "-boldoblique", 2 }, { "-italic", 2 },
        { "-oblique", 2 }, { "-regular", 0 }, { "-bold", 1 },
    };
    for (size_t i = 0; i < sizeof w / sizeof w[0]; i++) {
        if (str_ieq_tail(out, w[i].sfx)) {
            out[strlen(out) - strlen(w[i].sfx)] = 0;
            *kind = w[i].kind;
            break;
        }
    }
}

static void norm(const char *s, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 1 < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c)) out[j++] = (char)tolower(c);
    }
    out[j] = 0;
}

static int cmp_path(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* scan one directory for *.ttf files into paths[]; returns new count */
static int scan_one_dir(const char *dir, char **paths, int np, int maxp)
{
    DIR *d = opendir(dir);
    if (!d) return np;
    struct dirent *de;
    size_t dl = strlen(dir);
    while ((de = readdir(d)) != NULL && np < maxp) {
        size_t l = strlen(de->d_name);
        if (l < 5 || !str_ieq_tail(de->d_name, ".ttf")) continue;
        char *p = malloc(dl + l + 2);
        if (!p) break;
        snprintf(p, dl + l + 2, "%s/%s", dir, de->d_name);
        paths[np++] = p;
    }
    closedir(d);
    return np;
}

/* Families always come from FONTDIR (/usr/share/fonts/k93); K93_FONTDIR
 * adds colon-separated EXTRA dirs (test hook, multi-dir hosts). */
static void scan_fonts(void)
{
    char *paths[MAXF * 4];
    int np = 0;
    nF = 0;
    def_id = -1;
    cur = -1;
    const char *env = getenv("K93_FONTDIR");
    if (env && *env) {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", env);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ":", &save); tok;
             tok = strtok_r(NULL, ":", &save))
            np = scan_one_dir(tok, paths, np, MAXF * 4);
    }
    np = scan_one_dir(FONTDIR, paths, np, MAXF * 4);
    if (np == 0) return;
    if (np > 1) qsort(paths, (size_t)np, sizeof paths[0], cmp_path);

    /* pass 1: regular/plain files become families */
    for (int i = 0; i < np && nF < MAXF; i++) {
        char base[48];
        int kind;
        family_base(paths[i], base, sizeof base, &kind);
        if (kind != 0) continue;
        int dup = 0;
        for (int k = 0; k < nF; k++)
            if (strcmp(F[k].name, base) == 0) { dup = 1; break; }
        if (dup) continue;
        snprintf(F[nF].name, sizeof F[nF].name, "%s", base);
        snprintf(F[nF].path, sizeof F[nF].path, "%s", paths[i]);
        F[nF].bold = -1;
        F[nF].loaded = 0;
        nF++;
    }
    /* pass 2: -Bold files attach to a matching family */
    for (int i = 0; i < np; i++) {
        char base[48];
        int kind;
        family_base(paths[i], base, sizeof base, &kind);
        if (kind != 1) continue;
        for (int k = 0; k < nF; k++)
            if (strcmp(F[k].name, base) == 0) {
                if (F[k].bold < 0 && nF < MAXF) {  /* one bold slot max */
                    F[k].bold = nF;
                    snprintf(F[nF].name, sizeof F[nF].name, "%s", base);
                    snprintf(F[nF].path, sizeof F[nF].path, "%s", paths[i]);
                    F[nF].bold = -1;
                    F[nF].loaded = 0;
                    nF++;
                }
                break;
            }
    }
    /* pass 3: oddball dirs with only italic/bold files: first file */
    if (nF == 0 && np > 0 && nF < MAXF) {
        char base[48];
        int kind;
        family_base(paths[0], base, sizeof base, &kind);
        snprintf(F[0].name, sizeof F[0].name, "%s", base);
        snprintf(F[0].path, sizeof F[0].path, "%s", paths[0]);
        F[0].bold = -1;
        F[0].loaded = 0;
        nF = 1;
    }
    for (int i = 0; i < np; i++) free(paths[i]);

    /* default family: prefer a "sans" (non-mono, non-serif) face, else 0 */
    def_id = nF ? 0 : -1;
    for (int i = 0; i < nF; i++) {
        char nn[64];
        norm(F[i].name, nn, sizeof nn);
        if (strstr(nn, "sans") && !strstr(nn, "mono")) { def_id = i; break; }
    }
    cur = def_id;
}

/* ---------------------------------------------------------- public API */

int k93font_init(void)
{
    scan_fonts();
    if (cur >= 0) ensure(cur);
    return nF > 0 ? 0 : -1;
}

void k93font_shutdown(void)
{
    for (int i = 0; i < MAXF; i++) {
        free(F[i].data);
        F[i].data = NULL;
        F[i].loaded = 0;
    }
    nF = 0;
    cur = def_id = -1;
}

int k93font_ready(void) { return cur >= 0 && F[cur].loaded == 1; }
int k93font_count(void) { return nF; }
int k93font_default_id(void) { return def_id; }

const char *k93font_name(int id)
{
    return (id >= 0 && id < nF) ? F[id].name : "";
}

int k93font_bold_id(int id)
{
    return (id >= 0 && id < nF) ? F[id].bold : -1;
}

int k93font_id(const char *family)
{
    char q[64];
    if (!family || !*family) return def_id;
    norm(family, q, sizeof q);
    if (!strcmp(q, "system") || !strcmp(q, "default") || !strcmp(q, "ui"))
        return def_id;
    if (!strcmp(q, "sans") || !strcmp(q, "sansserif")) {
        for (int i = 0; i < nF; i++) {
            char nn[64];
            norm(F[i].name, nn, sizeof nn);
            if (strstr(nn, "sans")) return i;
        }
        return def_id;
    }
    if (!strcmp(q, "serif")) {
        for (int i = 0; i < nF; i++) {
            char nn[64];
            norm(F[i].name, nn, sizeof nn);
            if (strstr(nn, "serif") && !strstr(nn, "sans")) return i;
        }
        return def_id;
    }
    if (!strcmp(q, "mono") || !strcmp(q, "monospace") || !strcmp(q, "fixed") ||
        !strcmp(q, "courier")) {
        for (int i = 0; i < nF; i++) {
            char nn[64];
            norm(F[i].name, nn, sizeof nn);
            if (strstr(nn, "mono") || strstr(nn, "courier")) return i;
        }
        return def_id;
    }
    for (int i = 0; i < nF; i++) {
        char nn[64];
        norm(F[i].name, nn, sizeof nn);
        if (!strcmp(nn, q)) return i;
    }
    for (int i = 0; i < nF; i++)                       /* substring match */
        if (strstr(F[i].name, family) || strstr(family, F[i].name))
            return i;
    return -1;
}

int k93font_select(int id)
{
    if (id < 0 || id >= nF) return -1;
    if (!ensure(id)) return -1;
    cur = id;
    return 0;
}

static float scale_of(const Font *f, int scale)
{
    int px = 16 * (scale < 1 ? 1 : scale);
    return (float)px / (float)(f->asc - f->desc);
}

int k93font_px(int scale) { return 16 * (scale < 1 ? 1 : scale); }

int k93font_baseline(int scale)
{
    if (!k93font_ready()) return 16 * scale;
    const Font *f = &F[cur];
    float sc = scale_of(f, scale);
    return (int)((float)f->asc * sc + 0.5f);
}

int k93font_width(int scale, const char *s)
{
    if (!k93font_ready() || !s) {
        /* legacy fallback: 8x16 cell per byte, matches the bitmap font */
        return (int)strlen(s) * 8 * (scale < 1 ? 1 : scale);
    }
    const Font *f = &F[cur];
    float sc = scale_of(f, scale);
    float pen = 0;
    uint32_t prev = 0;
    int i = 0;
    while (s[i]) {
        uint32_t cp = k93_utf8_next(s, &i);
        int g = cmap_lookup(f, cp);
        if (g < 0) g = 0;
        int pg = prev ? cmap_lookup(f, prev) : -1;
        pen += kern_px(f, pg, g, sc);
        pen += (float)hmtx_adv(f, g) * sc;
        prev = cp;
    }
    return (int)(pen + 0.5f);
}

uint8_t *k93font_glyph(uint32_t prev, uint32_t cp, int scale, int bold,
                       int *w, int *h, int *xoff, int *yoff, float *adv)
{
    *w = *h = *xoff = *yoff = 0;
    *adv = 0;
    if (!k93font_ready()) return NULL;
    Font *f = &F[cur];
    if (bold && f->bold >= 0 && ensure(f->bold)) f = &F[f->bold];

    int miss = 0;
    int g = cmap_lookup(f, cp);
    if (g < 0) { g = 0; miss = 1; }
    int pg = prev ? cmap_lookup(f, prev) : -1;

    float sc = scale_of(f, scale);
    *adv = kern_px(f, pg, g, sc) + (float)hmtx_adv(f, g) * sc;

    npt = nct = 0;
    AFF M = IDENTITY;
    load_glyph(f, g, &M, 0);
    if (miss && npt == 0) {                 /* empty .notdef -> try '?' */
        int q = cmap_lookup(f, '?');
        if (q > 0) { npt = nct = 0; load_glyph(f, q, &M, 0); }
    }
    if (npt < 2 || nct < 1) return NULL;    /* blank glyph (space etc.) */

    flatten(sc);
    int bw = 0, bh = 0, bx = 0, by = 0;
    uint8_t *out = rasterize(&bw, &bh, &bx, &by);
    if (!out) return NULL;
    *w = bw; *h = bh;
    *xoff = bx; *yoff = by;
    return out;
}

void k93font_glyph_free(uint8_t *p) { free(p); }
