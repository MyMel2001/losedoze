/* k93html.h - clean-room HTML tree parser + CSS subset (Safari engine).
 * Tag/attr names lowercase; text node tag = "#text"; comments dropped.
 * Entity decoding (named basics + numeric) happens at parse time.
 * CSS subset: tag / .class / #id / compound / descendant / child / sibling
 * combinators, attr operators, pseudo-classes (:first-child, :nth-child,
 * :not, :root); properties: color (hex/rgb/hsl/named), background-color,
 * font-*, text-*, display, visibility, margin/padding, border, width,
 * line-height, opacity, list-style-type. */
#ifndef K93HTML_H
#define K93HTML_H
#include <stddef.h>
#include <stdint.h>

typedef struct k93node k93node;
struct k93node {
    char *tag;                        /* lowercase, or "#text" */
    char *text;                       /* #text content (entities decoded) */
    int nattr;
    char **ak, **av;                  /* lowercase keys, decoded values */
    k93node *parent, *first, *last, *next, *prev;
};

k93node *k93html_parse(const char *html);
void     k93html_free(k93node *n);
k93node *k93html_elem(const char *tag);
k93node *k93html_textnode(const char *s);
void     k93html_append(k93node *p, k93node *c);
void     k93html_insert_before(k93node *ref, k93node *n);
void     k93html_unlink(k93node *n);
k93node *k93html_clone(const k93node *n);
const char *k93html_attr(const k93node *n, const char *k);
void     k93html_set_attr(k93node *n, const char *k, const char *v);
k93node *k93html_get_id(k93node *root, const char *id);
k93node *k93html_query(k93node *root, const char *sel);        /* first match */
int      k93html_query_all(k93node *root, const char *sel,
                           k93node **out, int max);
void     k93html_text_content(const k93node *n, char *buf, size_t cap);
char    *k93html_inner_html(const k93node *n);                 /* malloc'd */
void     k93html_set_inner_html(k93node *n, const char *html);
void     k93html_entity_decode(char *s);

/* CSS subset */
typedef struct k93css k93css;
k93css *k93css_new(void);
void    k93css_free(k93css *c);
void    k93css_add(k93css *c, const char *text);

enum {
    K93CSS_FG = 1, K93CSS_BG = 2, K93CSS_BOLD = 4, K93CSS_CENTER = 8,
    K93CSS_HIDDEN = 16, K93CSS_SCALE = 32, K93CSS_INDENT = 64,
    K93CSS_UNDERLINE = 128, K93CSS_FONT = 256, K93CSS_XFORM = 512,
    K93CSS_OPACITY = 1024, K93CSS_WIDTH = 2048, K93CSS_LH = 4096,
    K93CSS_BORDER = 8192, K93CSS_LSTYLE = 16384, K93CSS_STRIKE = 32768,
    K93CSS_GRAD = 65536
};
/* list-style-type values */
enum { K93L_DISC, K93L_CIRCLE, K93L_SQUARE, K93L_DECIMAL, K93L_LALPHA,
       K93L_UALPHA, K93L_LROMAN, K93L_UROMAN, K93L_NONE };
typedef struct {
    uint32_t fg, bg;
    uint32_t gbot;                    /* gradient bottom (with K93CSS_GRAD) */
    int bold, center, hidden, scale, indent;
    int underline;                    /* text-decoration: underline */
    int strike;                       /* text-decoration: line-through */
    int font;                         /* k93font id from font-family, -1 = none */
    int xform;                        /* text-transform: 0 none 1 upper 2 lower */
    int opacity;                      /* percent, default 100 */
    int width;                        /* css width px, 0 = auto */
    int lh;                           /* line-height px, 0 = default */
    int bordw;                        /* border width px, 0 = none */
    uint32_t bordc;                   /* border color */
    int lstyle;                       /* K93L_* */
    unsigned flags;                   /* K93CSS_* of set properties */
} k93cstyle;

int  k93css_color(const char *v, uint32_t *out); /* hex/rgb/hsl/140 names */
void k93css_apply(const k93css *c, const k93node *n, k93cstyle *st);
int  k93html_matches(const k93node *n, const char *sel);  /* selector list */

#endif
