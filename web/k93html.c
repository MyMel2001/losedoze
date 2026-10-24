/* k93html.c - clean-room HTML parser + CSS subset for Safari.
 * Tag/attr names lowercase; text nodes tagged "#text"; comments dropped.
 * Stack-based tokenizer: void elements, raw-text elements (script/style/
 * textarea/title), implied end tags (li/td/tr/p/...), entity decoding. */
#define _GNU_SOURCE
#include "k93html.h"
#include "k93font.h"
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------------------------------------------------------- entities */

struct ent { const char *n; const char *v; };
/* mapped to real Unicode (UTF-8) - the TTF engine renders them; the 8x16
 * bitmap fallback shows notdef for these (ceiling) */
static const struct ent ents[] = {
    { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" },
    { "apos", "'" }, { "nbsp", " " },
    { "copy", "\xC2\xA9" }, { "reg", "\xC2\xAE" }, { "trade", "\xE2\x84\xA2" },
    { "hellip", "\xE2\x80\xA6" }, { "mdash", "\xE2\x80\x94" },
    { "ndash", "\xE2\x80\x93" }, { "rsquo", "\xE2\x80\x99" },
    { "lsquo", "\xE2\x80\x98" }, { "ldquo", "\xE2\x80\x9C" },
    { "rdquo", "\xE2\x80\x9D" }, { "bull", "\xE2\x80\xA2" },
    { "middot", "\xC2\xB7" }, { "deg", "\xC2\xB0" },
    { "plusmn", "\xC2\xB1" }, { "times", "\xC2\xD7" },
    { "frac12", "\xC2\xBD" }, { "laquo", "\xC2\xAB" },
    { "raquo", "\xC2\xBB" }, { "cent", "\xC2\xA2" }, { "pound", "\xC2\xA3" },
    { "yen", "\xC2\xA5" }, { "sect", "\xC2\xA7" }, { "para", "\xC2\xB6" },
    { "dagger", "\xE2\x80\xA0" }, { "permil", "\xE2\x80\xB0" },
    { "euro", "\xE2\x82\xAC" }, { "dollar", "$" },
    { "agrave", "\xC3\x80" }, { "aacute", "\xC3\xA1" },
    { "atilde", "\xC3\x83" }, { "auml", "\xC3\xA4" },
    { "aring", "\xC3\xA5" }, { "ccedil", "\xC3\xA7" },
    { "egrave", "\xC3\x88" }, { "eacute", "\xC3\xA9" },
    { "igrave", "\xC3\x8C" }, { "iacute", "\xC3\xAD" },
    { "iuml", "\xC3\xAF" }, { "ntilde", "\xC3\xB1" },
    { "ograve", "\xC3\x92" }, { "oacute", "\xC3\xB3" },
    { "otilde", "\xC3\x95" }, { "ouml", "\xC3\xB6" },
    { "ugrave", "\xC3\xB9" }, { "uacute", "\xC3\xBA" },
    { "uuml", "\xC3\xBC" }, { "yacute", "\xC3\xBD" },
    { "szlig", "\xC3\x9F" },
    { NULL, NULL }
};

void k93html_entity_decode(char *s)
{
    size_t o = 0;
    for (size_t i = 0; s[i]; ) {
        if (s[i] != '&') { s[o++] = s[i++]; continue; }
        char *semi = strchr(s + i, ';');
        if (!semi || semi - (s + i) > 10) { s[o++] = s[i++]; continue; }
        char name[12];
        size_t nl = (size_t)(semi - (s + i + 1));
        if (nl == 0 || nl >= sizeof name) { s[o++] = s[i++]; continue; }
        memcpy(name, s + i + 1, nl);
        name[nl] = 0;
        const char *val = NULL;
        char numbuf[8];
        if (name[0] == '#') {
            long code = (name[1] == 'x' || name[1] == 'X')
                        ? strtol(name + 2, NULL, 16)
                        : strtol(name + 1, NULL, 10);
            if (code > 0 && code < 0x110000) {   /* UTF-8 encode (TTF era) */
                if (code < 0x80) {
                    numbuf[0] = (char)code; numbuf[1] = 0;
                } else if (code < 0x800) {
                    numbuf[0] = (char)(0xC0 | (code >> 6));
                    numbuf[1] = (char)(0x80 | (code & 63));
                    numbuf[2] = 0;
                } else if (code < 0x10000) {
                    numbuf[0] = (char)(0xE0 | (code >> 12));
                    numbuf[1] = (char)(0x80 | ((code >> 6) & 63));
                    numbuf[2] = (char)(0x80 | (code & 63));
                    numbuf[3] = 0;
                } else {
                    numbuf[0] = (char)(0xF0 | (code >> 18));
                    numbuf[1] = (char)(0x80 | ((code >> 12) & 63));
                    numbuf[2] = (char)(0x80 | ((code >> 6) & 63));
                    numbuf[3] = (char)(0x80 | (code & 63));
                    numbuf[4] = 0;
                }
                val = numbuf;
            }
        } else {
            for (int e = 0; ents[e].n; e++)
                if (!strcmp(ents[e].n, name)) { val = ents[e].v; break; }
        }
        if (!val) { s[o++] = s[i++]; continue; }
        size_t vl = strlen(val);
        memmove(s + o + vl, semi + 1, strlen(semi + 1) + 1);
        memcpy(s + o, val, vl);
        o += vl;
        i = o;                          /* resume after inserted value */
    }
    s[o] = 0;
}

/* ------------------------------------------------------------- nodes */

k93node *k93html_elem(const char *tag)
{
    k93node *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->tag = k93_strdup(tag);
    for (char *p = n->tag; *p; p++) *p = (char)tolower((unsigned char)*p);
    return n;
}

k93node *k93html_textnode(const char *s)
{
    k93node *n = k93html_elem("#text");
    if (n) n->text = k93_strdup(s ? s : "");
    return n;
}

void k93html_unlink(k93node *n)
{
    if (!n || !n->parent) return;
    if (n->prev) n->prev->next = n->next;
    else n->parent->first = n->next;
    if (n->next) n->next->prev = n->prev;
    else n->parent->last = n->prev;
    n->parent = n->prev = n->next = NULL;
}

void k93html_append(k93node *p, k93node *c)
{
    if (!p || !c) return;
    k93html_unlink(c);
    c->parent = p;
    c->prev = p->last;
    c->next = NULL;
    if (p->last) p->last->next = c;
    else p->first = c;
    p->last = c;
}

void k93html_insert_before(k93node *ref, k93node *n)
{
    if (!ref || !n || !ref->parent) return;
    k93html_unlink(n);
    n->parent = ref->parent;
    n->next = ref;
    n->prev = ref->prev;
    if (ref->prev) ref->prev->next = n;
    else ref->parent->first = n;
    ref->prev = n;
}

static void attr_free(k93node *n)
{
    for (int i = 0; i < n->nattr; i++) { free(n->ak[i]); free(n->av[i]); }
    free(n->ak); free(n->av);
    n->ak = n->av = NULL;
    n->nattr = 0;
}

void k93html_free(k93node *n)
{
    if (!n) return;
    k93node *c = n->first;
    while (c) { k93node *nx = c->next; k93html_free(c); c = nx; }
    free(n->tag);
    free(n->text);
    attr_free(n);
    free(n);
}

const char *k93html_attr(const k93node *n, const char *k)
{
    if (!n) return NULL;
    for (int i = 0; i < n->nattr; i++)
        if (!strcmp(n->ak[i], k)) return n->av[i];
    return NULL;
}

void k93html_set_attr(k93node *n, const char *k, const char *v)
{
    if (!n || !k) return;
    for (int i = 0; i < n->nattr; i++)
        if (!strcmp(n->ak[i], k)) {
            free(n->av[i]);
            n->av[i] = k93_strdup(v ? v : "");
            return;
        }
    n->ak = realloc(n->ak, sizeof(char *) * (size_t)(n->nattr + 1));
    n->av = realloc(n->av, sizeof(char *) * (size_t)(n->nattr + 1));
    n->ak[n->nattr] = k93_strdup(k);
    for (char *p = n->ak[n->nattr]; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    n->av[n->nattr] = k93_strdup(v ? v : "");
    n->nattr++;
}

k93node *k93html_clone(const k93node *n)
{
    if (!n) return NULL;
    k93node *c = k93html_elem(n->tag);
    if (!c) return NULL;
    if (n->text) c->text = k93_strdup(n->text);
    for (int i = 0; i < n->nattr; i++)
        k93html_set_attr(c, n->ak[i], n->av[i]);
    for (k93node *k = n->first; k; k = k->next)
        k93html_append(c, k93html_clone(k));
    return c;
}

void k93html_text_content(const k93node *n, char *buf, size_t cap)
{
    if (!buf || cap == 0) return;
    buf[0] = 0;
    if (!n) return;
    if (n->text) {
        size_t tl = strlen(n->text);
        if (tl > cap - 1) tl = cap - 1;
        memcpy(buf, n->text, tl);
        buf[tl] = 0;
        return;
    }
    for (k93node *c = n->first; c; c = c->next) {
        size_t used = strlen(buf);
        if (used + 1 >= cap) break;
        k93html_text_content(c, buf + used, cap - used);
    }
}

static void esc_append(char **out, size_t *o, size_t cap, const char *s)
{
    for (; *s && *o + 1 < cap; s++) {
        char c = *s;
        const char *rep = c == '&' ? "&amp;" : c == '<' ? "&lt;"
                        : c == '>' ? "&gt;" : c == '"' ? "&quot;" : NULL;
        if (rep) {
            size_t rl = strlen(rep);
            if (*o + rl >= cap) break;
            memcpy(*out + *o, rep, rl);
            *o += rl;
        } else {
            (*out)[(*o)++] = c;
        }
    }
    (*out)[*o] = 0;
}

static void serialize(const k93node *n, char **out, size_t *o, size_t cap)
{
    if (!strcmp(n->tag, "#text")) { esc_append(out, o, cap, n->text ? n->text : ""); return; }
    if (n->tag[0] == '#') return;
    size_t need = strlen(n->tag) + 8;
    if (*o + need >= cap) return;
    *o += (size_t)sprintf(*out + *o, "<%s", n->tag);
    for (int i = 0; i < n->nattr; i++) {
        if (*o + strlen(n->ak[i]) + strlen(n->av[i]) + 8 >= cap) break;
        *o += (size_t)sprintf(*out + *o, " %s=\"", n->ak[i]);
        esc_append(out, o, cap, n->av[i]);
        if (*o + 2 >= cap) break;
        (*out)[(*o)++] = '"'; (*out)[*o] = 0;
    }
    if (!n->first) {
        if (n->text) {                  /* rawtext element (script/style/..) */
            if (*o + 3 < cap) { (*out)[(*o)++] = '>'; (*out)[*o] = 0; }
            esc_append(out, o, cap, n->text);
        }
        if (*o + strlen(n->tag) + 5 < cap)
            *o += (size_t)sprintf(*out + *o, "</%s>", n->tag);
        return;
    }
    if (*o + 2 < cap) { (*out)[(*o)++] = '>'; (*out)[*o] = 0; }
    for (k93node *c = n->first; c; c = c->next) serialize(c, out, o, cap);
    if (*o + strlen(n->tag) + 5 < cap)
        *o += (size_t)sprintf(*out + *o, "</%s>", n->tag);
}

char *k93html_inner_html(const k93node *n)
{
    if (!n) return k93_strdup("");
    size_t cap = 64;
    for (k93node *c = n->first; c; c = c->next) cap += 64;
    for (;;) {
        char *out = k93_xmalloc(cap);
        size_t o = 0;
        out[0] = 0;
        for (k93node *c = n->first; c; c = c->next) serialize(c, &out, &o, cap);
        size_t used = strlen(out);
        if (used + 64 < cap || cap > (1u << 22)) return out;
        free(out);
        cap *= 2;
    }
}

/* ----------------------------------------------------------- parser */

static int is_void(const char *t)
{
    static const char *v[] = { "br", "img", "meta", "link", "input", "hr",
        "area", "base", "col", "embed", "source", "track", "wbr", "param",
        "keygen", "menuitem", NULL };
    for (int i = 0; v[i]; i++) if (!strcmp(t, v[i])) return 1;
    return 0;
}

static int is_rawtext(const char *t)
{
    return !strcmp(t, "script") || !strcmp(t, "style")
        || !strcmp(t, "textarea") || !strcmp(t, "title");
}

static void autoclose(k93node **stack, int *sp, const char *tag)
{
    k93node *top = *sp > 0 ? stack[*sp - 1] : NULL;
    if (!top) return;
    const char *t = top->tag;
    if (!strcmp(tag, "li") && !strcmp(t, "li")) (*sp)--;
    else if ((!strcmp(tag, "td") || !strcmp(tag, "th")) &&
             (!strcmp(t, "td") || !strcmp(t, "th"))) (*sp)--;
    else if (!strcmp(tag, "tr") && !strcmp(t, "tr")) (*sp)--;
    else if ((!strcmp(tag, "dd") || !strcmp(tag, "dt")) &&
             (!strcmp(t, "dd") || !strcmp(t, "dt"))) (*sp)--;
    else if (!strcmp(tag, "option") && !strcmp(t, "option")) (*sp)--;
    else if (!strcmp(t, "p")) {
        static const char *blocks[] = { "div", "p", "h1", "h2", "h3", "h4",
            "h5", "h6", "li", "table", "ul", "ol", "section", "article",
            "header", "footer", "blockquote", NULL };
        for (int i = 0; blocks[i]; i++)
            if (!strcmp(tag, blocks[i])) { (*sp)--; return; }
    }
}

k93node *k93html_parse(const char *html)
{
    k93node *root = k93html_elem("document");
    k93node *stack[256];
    int sp = 0;
    stack[sp++] = root;
    const char *p = html ? html : "";
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) p += 3;
    char tbuf[4096];
    tbuf[0] = 0;                       /* used = strlen(tbuf) below trusts this */
    while (*p) {
        if (*p == '<') {
            if (!strncmp(p, "<!--", 4)) {
                const char *e = strstr(p + 4, "-->");
                p = e ? e + 3 : p + strlen(p);
                continue;
            }
            if (p[1] == '!' || p[1] == '?') {
                const char *e = strchr(p, '>');
                p = e ? e + 1 : p + strlen(p);
                continue;
            }
            if (p[1] == '/') {
                const char *q = p + 2;
                char name[32];
                size_t i = 0;
                while (*q && (isalnum((unsigned char)*q) || *q == '-' ||
                              *q == ':' || *q == '_') && i < sizeof name - 1)
                    name[i++] = (char)tolower((unsigned char)*q++);
                name[i] = 0;
                const char *e = strchr(q, '>');
                p = e ? e + 1 : q;
                if (i == 0) continue;
                for (int s = sp - 1; s > 0; s--) {
                    if (!strcmp(stack[s]->tag, name)) { sp = s; break; }
                }
                continue;
            }
            /* start tag */
            const char *q = p + 1;
            char name[32];
            size_t i = 0;
            while (*q && (isalnum((unsigned char)*q) || *q == '-' ||
                          *q == ':' || *q == '_') && i < sizeof name - 1)
                name[i++] = (char)tolower((unsigned char)*q++);
            name[i] = 0;
            if (i == 0) {              /* lone '<' */
                size_t tl = strlen(tbuf);
                if (tl + 1 < sizeof tbuf) {
                    tbuf[tl] = '<'; tbuf[tl + 1] = 0;
                }
                p++;
                continue;
            }
            if (tbuf[0]) {             /* pending text */
                k93html_entity_decode(tbuf);
                k93html_append(stack[sp - 1], k93html_textnode(tbuf));
                tbuf[0] = 0;
            }
            k93node *el = k93html_elem(name);
            if (is_rawtext(name)) {
                /* skip attributes up to '>' */
                const char *gt = strchr(q, '>');
                if (!gt) gt = q + strlen(q);
                const char *body = gt + 1;
                char close[40];
                snprintf(close, sizeof close, "</%s", name);
                const char *e = NULL;
                for (const char *s = body; (s = strchr(s, '<')); s++) {
                    if (!strncasecmp(s, close, strlen(close))) { e = s; break; }
                }
                size_t bl = e ? (size_t)(e - body) : strlen(body);
                if (bl >= sizeof tbuf) bl = sizeof tbuf - 1;
                memcpy(tbuf, body, bl);
                tbuf[bl] = 0;
                if (!strcmp(name, "textarea") || !strcmp(name, "title"))
                    k93html_entity_decode(tbuf);
                el->text = k93_strdup(tbuf);
                tbuf[0] = 0;
                p = e ? strchr(e, '>') : body + bl;
                if (!p) p = body + bl; else p++;
                k93html_append(stack[sp - 1], el);
                continue;
            }
            /* attributes */
            for (;;) {
                while (*q && isspace((unsigned char)*q)) q++;
                if (*q == '/' && q[1] == '>') { q += 2; break; }
                if (*q == '>') { q++; break; }
                if (!*q) break;
                char an[48]; size_t ai = 0;
                while (*q && (isalnum((unsigned char)*q) || *q == '-' ||
                              *q == ':' || *q == '_') && ai < sizeof an - 1)
                    an[ai++] = (char)tolower((unsigned char)*q++);
                an[ai] = 0;
                if (ai == 0) { q++; continue; }
                while (*q && isspace((unsigned char)*q)) q++;
                char av[1024]; size_t vi = 0;
                if (*q == '=' ) {
                    q++;
                    while (*q && isspace((unsigned char)*q)) q++;
                    char quote = 0;
                    if (*q == '"' || *q == '\'') quote = *q++;
                    while (*q && vi + 1 < sizeof av) {
                        if (quote) { if (*q == quote) { q++; break; } }
                        else if (isspace((unsigned char)*q) || *q == '>') break;
                        av[vi++] = *q++;
                    }
                }
                av[vi] = 0;
                k93html_entity_decode(av);
                k93html_set_attr(el, an, av);
            }
            autoclose(stack, &sp, name);     /* implied end tags */
            k93html_append(stack[sp - 1], el);
            int selfclosed = (q > p + 1 && q[-2] == '/' && q[-1] == '>');
            p = q;
            if (!is_void(name) && !selfclosed) {
                if (sp < (int)(sizeof stack / sizeof stack[0]))
                    stack[sp++] = el;
            }
            continue;
        }
        /* text */
        const char *e = strchr(p, '<');
        size_t tl = e ? (size_t)(e - p) : strlen(p);
        size_t used = strlen(tbuf);
        if (used + tl >= sizeof tbuf) {         /* flush before overflow */
            k93html_entity_decode(tbuf);
            k93html_append(stack[sp - 1], k93html_textnode(tbuf));
            tbuf[0] = 0;
            used = 0;
        }
        if (tl > sizeof tbuf - 1 - used) tl = sizeof tbuf - 1 - used;
        memcpy(tbuf + used, p, tl);
        tbuf[used + tl] = 0;
        p += e ? (size_t)(e - p) : tl;
        if (e) {
            k93html_entity_decode(tbuf);
            k93html_append(stack[sp - 1], k93html_textnode(tbuf));
            tbuf[0] = 0;
        }
    }
    if (tbuf[0]) {
        k93html_entity_decode(tbuf);
        k93html_append(stack[sp - 1], k93html_textnode(tbuf));
    }
    autoclose(stack, &sp, "");         /* nothing */
    return root;
}

void k93html_set_inner_html(k93node *n, const char *html)
{
    if (!n) return;
    k93node *frag = k93html_parse(html ? html : "");
    for (k93node *c = n->first; c; c = c->next) c->parent = NULL;
    n->first = n->last = NULL;
    k93node *c = frag ? frag->first : NULL;
    while (c) {
        k93node *nx = c->next;
        k93html_append(n, c);
        c = nx;
    }
    k93html_free(frag);
}

/* ---------------------------------------------------------- query */

/* [attr], [attr=v], [attr^=v], [attr$=v], [attr*=v], [attr~=v], [attr|=v] */
static int attr_match(const k93node *n, const char *expr)
{
    char name[48]; size_t ni = 0;
    const char *p = expr;
    while (*p && *p != '=' && *p != '^' && *p != '$' && *p != '*' &&
           *p != '~' && *p != '|' && ni < sizeof name - 1)
        name[ni++] = *p++;
    while (ni > 0 && name[ni - 1] == ' ') ni--;
    name[ni] = 0;
    while (*p == ' ') p++;
    if (!*p) return k93html_attr(n, name) != NULL;   /* bare presence */
    char op[3] = { 0, 0, 0 };
    if (p[1] == '=') { op[0] = *p; op[1] = '='; p += 2; }
    else if (*p == '=') { op[0] = '='; p++; }
    else return 0;
    while (*p == ' ') p++;
    char want[64]; size_t wi = 0;
    while (*p && *p != ']' && wi < sizeof want - 1) want[wi++] = *p++;
    while (wi > 0 && want[wi - 1] == ' ') wi--;
    want[wi] = 0;
    if (wi >= 2 && (want[0] == '"' || want[0] == '\'') &&
        want[wi - 1] == want[0]) {
        memmove(want, want + 1, wi - 2);
        wi -= 2;
        want[wi] = 0;
    }
    const char *v = k93html_attr(n, name);
    if (!v) return 0;
    size_t vl = strlen(v), wl = strlen(want);
    if (!strcmp(op, "=")) return vl == wl && !strncmp(v, want, wl);
    if (!strcmp(op, "^=")) return wl <= vl && !strncmp(v, want, wl);
    if (!strcmp(op, "$=")) return wl <= vl && !strcmp(v + vl - wl, want);
    if (!strcmp(op, "*=")) return wl == 0 || strstr(v, want) != NULL;
    if (!strcmp(op, "~=")) {                        /* word in list */
        const char *c = v;
        while (*c) {
            while (*c == ' ' || *c == '\t') c++;
            size_t cl = 0;
            while (c[cl] && c[cl] != ' ' && c[cl] != '\t') cl++;
            if (cl == wl && !strncmp(c, want, wl)) return 1;
            c += cl;
        }
        return 0;
    }
    if (!strcmp(op, "|="))                          /* exact or prefix- */
        return (vl == wl && !strncmp(v, want, wl)) ||
               (wl < vl && !strncmp(v, want, wl) && v[wl] == '-');
    return 0;
}

static int match_sel(const k93node *n, const char *sel);   /* :not uses it */

/* index of n among element siblings, 1-based; 0 if not found */
static int elem_index(const k93node *n)
{
    if (!n || !n->parent) return 0;
    int idx = 0;
    for (const k93node *k = n->parent->first; k; k = k->next) {
        if (k->tag[0] != '#') idx++;
        if (k == n) return idx;
    }
    return 0;
}

/* "an+b" microsyntax: 3 | 2n | 2n+1 | -n+3 | odd | even */
static int nth_match(const k93node *n, const char *arg)
{
    int idx = elem_index(n);
    if (idx <= 0) return 0;
    while (*arg == ' ') arg++;
    if (!strcasecmp(arg, "odd")) return idx % 2 == 1;
    if (!strcasecmp(arg, "even")) return idx % 2 == 0;
    char buf[32];
    k93_strlcpy(buf, arg, sizeof buf);
    for (char *q = buf; *q; q++) if (*q == ' ') *q = 0;
    char *p2 = buf, *end = NULL;
    long a = 0, b = 0;
    if (*p2 == 'n') a = 1;
    else {
        a = strtol(p2, &end, 10);
        if (end == p2) return 0;
        p2 = end;
    }
    if (*p2 == 'n') {
        p2++;
        if (*p2 == '+' || *p2 == '-') b = strtol(p2, &end, 10);
    } else {
        b = a;                            /* plain "N": index must equal N */
        a = 0;
    }
    if (a == 0) return idx == b;
    long diff = idx - b;
    return diff % a == 0 && diff / a >= 0;
}

/* part := tag? ( "." cls | "#id" | "[attr...]" | ":pseudo" )* */
static int match_part(const k93node *n, const char *part)
{
    if (!n || n->tag[0] == '#') return 0;
    const char *p = part;
    char tag[32]; size_t i = 0;
    while (*p && *p != '.' && *p != '#' && *p != '[' && *p != ':' &&
           i < sizeof tag - 1)
        tag[i++] = *p++;
    tag[i] = 0;
    if (i && strcmp(tag, "*") && strcmp(n->tag, tag)) return 0;
    while (*p) {
        if (*p == '[') {                           /* attribute selector */
            char expr[128]; size_t ei = 0;
            p++;
            while (*p && *p != ']' && ei < sizeof expr - 1) expr[ei++] = *p++;
            if (*p == ']') p++;
            expr[ei] = 0;
            if (!attr_match(n, expr)) return 0;
            continue;
        }
        if (*p == ':') {                           /* pseudo-class */
            p++;
            if (*p == ':') p++;                    /* pseudo-element */
            char name[32]; size_t ni = 0;
            while (*p && *p != '(' && *p != '.' && *p != '#' && *p != '[' &&
                   *p != ':' && ni < sizeof name - 1)
                name[ni++] = *p++;
            name[ni] = 0;
            char arg[64]; size_t ai = 0;
            arg[0] = 0;
            if (*p == '(') {
                p++;
                int depth = 1;
                while (*p && ai < sizeof arg - 1) {
                    if (*p == '(') depth++;
                    else if (*p == ')') {
                        if (--depth == 0) { p++; break; }
                    }
                    arg[ai++] = *p++;
                }
                arg[ai] = 0;
            }
            if (!strcmp(name, "first-child")) {
                const k93node *k = n->prev;
                while (k && k->tag[0] == '#') k = k->prev;
                if (k) return 0;
            } else if (!strcmp(name, "last-child")) {
                const k93node *k = n->next;
                while (k && k->tag[0] == '#') k = k->next;
                if (k) return 0;
            } else if (!strcmp(name, "only-child")) {
                if (n->prev || n->next) return 0;
            } else if (!strcmp(name, "nth-child")) {
                if (!nth_match(n, arg)) return 0;
            } else if (!strcmp(name, "nth-last-child")) {
                if (!nth_match(n, arg)) return 0;   /* ceiling: counted forward */
            } else if (!strcmp(name, "not")) {
                if (match_sel(n, arg)) return 0;
            } else if (!strcmp(name, "root")) {
                if (!n->parent || strcmp(n->parent->tag, "document")) return 0;
            } else if (!strcmp(name, "empty")) {
                if (n->first) return 0;
                if (n->text && *n->text && strcmp(n->tag, "#text")) return 0;
            } else if (!strcmp(name, "link")) {
                const char *h = k93html_attr(n, "href");
                if (!h || !*h) return 0;
            } else if (!strcmp(name, "enabled") || !strcmp(name, "disabled")) {
                int dis = k93html_attr(n, "disabled") != NULL;
                if ((strcmp(name, "enabled") == 0) == dis) return 0;
            } else {
                /* :hover/:focus/:visited/:active/:checked: no interaction
                 * state in the cascade (ceiling) - never match */
                return 0;
            }
            continue;
        }
        char kind = *p++;
        char val[64]; size_t vi = 0;
        while (*p && *p != '.' && *p != '#' && *p != '[' &&
               vi < sizeof val - 1)
            val[vi++] = *p++;
        val[vi] = 0;
        if (!vi) return 0;
        if (kind == '#') {
            const char *id = k93html_attr(n, "id");
            if (!id || strcmp(id, val)) return 0;
        } else {
            const char *cls = k93html_attr(n, "class");
            if (!cls) return 0;
            int hit = 0;
            const char *c = cls;
            while (*c) {
                while (*c == ' ' || *c == '\t' || *c == '\n') c++;
                size_t cl = 0;
                while (c[cl] && c[cl] != ' ' && c[cl] != '\t' && c[cl] != '\n')
                    cl++;
                if (cl == strlen(val) && !strncmp(c, val, cl)) { hit = 1; break; }
                c += cl;
            }
            if (!hit) return 0;
        }
    }
    return 1;
}

/* sel := part (comb part)* with comb in {descendant, '>', '+', '~'};
 * comb j links part j to part j+1 (j+1 is closer to n; walk right to left) */
static int match_sel(const k93node *n, const char *sel)
{
    struct { char part[96]; char comb; } seg[8];
    int nseg = 0;
    const char *p = sel;
    while (*p && nseg < 8) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',') p++;
        if (!*p) break;
        size_t i = 0;
        int depth = 0;
        while (*p && i < 95) {
            char c = *p;
            if (depth > 0) {               /* keep everything inside (...) */
                if (c == '(') depth++;
                else if (c == ')') depth--;
                seg[nseg].part[i++] = *p++;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == ',' ||
                c == '>' || c == '+' || c == '~')
                break;
            if (c == '(') depth++;
            seg[nseg].part[i++] = *p++;
        }
        seg[nseg].part[i] = 0;
        if (!i) break;
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        seg[nseg].comb = ' ';
        if (*p == '>' || *p == '+' || *p == '~') seg[nseg].comb = *p++;
        nseg++;
    }
    if (!nseg) return 0;
    if (!match_part(n, seg[nseg - 1].part)) return 0;
    const k93node *cur = n;
    for (int j = nseg - 2; j >= 0; j--) {
        const k93node *a;
        if (seg[j].comb == ' ') {
            a = cur->parent;
            while (a && !match_part(a, seg[j].part)) a = a->parent;
        } else if (seg[j].comb == '>') {
            a = cur->parent;
            if (a && !match_part(a, seg[j].part)) a = NULL;
        } else if (seg[j].comb == '+') {
            a = cur->prev;
            if (a && !match_part(a, seg[j].part)) a = NULL;
        } else {                                    /* '~' general sibling */
            a = cur->prev;
            while (a && !match_part(a, seg[j].part)) a = a->prev;
        }
        if (!a) return 0;
        cur = a;
    }
    return 1;
}

static void walk_collect(k93node *n, const char *sel, k93node **out,
                         int max, int *count)
{
    for (k93node *c = n->first; c; c = c->next) {
        if (match_sel(c, sel)) {
            if (*count < max) out[*count] = c;
            (*count)++;
        }
        walk_collect(c, sel, out, max, count);
    }
}

int k93html_query_all(k93node *root, const char *sel, k93node **out, int max)
{
    int count = 0;
    if (!root || !sel) return 0;
    char buf[512];
    k93_strlcpy(buf, sel, sizeof buf);
    char *save = NULL;
    for (char *one = strtok_r(buf, ",", &save); one;
         one = strtok_r(NULL, ",", &save))
        walk_collect(root, one, out, max, &count);
    return count > max ? max : count;
}

k93node *k93html_query(k93node *root, const char *sel)
{
    k93node *out[1];
    return k93html_query_all(root, sel, out, 1) ? out[0] : NULL;
}

int k93html_matches(const k93node *n, const char *sel)
{
    if (!n || !sel) return 0;
    char buf[512];
    k93_strlcpy(buf, sel, sizeof buf);
    char *save = NULL;
    for (char *one = strtok_r(buf, ",", &save); one;
         one = strtok_r(NULL, ",", &save)) {
        while (*one == ' ' || *one == '\t') one++;
        if (match_sel(n, one)) return 1;
    }
    return 0;
}

k93node *k93html_get_id(k93node *root, const char *id)
{
    if (!root) return NULL;
    for (k93node *c = root->first; c; c = c->next) {
        const char *v = k93html_attr(c, "id");
        if (v && !strcmp(v, id)) return c;
        k93node *f = k93html_get_id(c, id);
        if (f) return f;
    }
    return NULL;
}

/* ------------------------------------------------------------- CSS */

struct k93css { char **sel, **prop, **val; int n, cap; };

k93css *k93css_new(void) { return calloc(1, sizeof(k93css)); }

void k93css_free(k93css *c)
{
    if (!c) return;
    for (int i = 0; i < c->n; i++) {
        free(c->sel[i]); free(c->prop[i]); free(c->val[i]);
    }
    free(c->sel); free(c->prop); free(c->val);
    free(c);
}

static void css_push(k93css *c, const char *sel, const char *prop,
                     const char *val)
{
    if (c->n >= c->cap) {
        c->cap = c->cap ? c->cap * 2 : 64;
        c->sel = realloc(c->sel, sizeof(char *) * (size_t)c->cap);
        c->prop = realloc(c->prop, sizeof(char *) * (size_t)c->cap);
        c->val = realloc(c->val, sizeof(char *) * (size_t)c->cap);
    }
    c->sel[c->n] = k93_strdup(sel);
    c->prop[c->n] = k93_strdup(prop);
    c->val[c->n] = k93_strdup(val);
    c->n++;
}

/* parse "prop: val; ..." (one rule body) for a selector list */
static void css_block(k93css *c, const char *sels, const char *body, size_t len)
{
    char buf[2048];
    if (len >= sizeof buf) len = sizeof buf - 1;
    memcpy(buf, body, len);
    buf[len] = 0;
    /* selector list: split on ',' at top level of sels string */
    char s[512];
    k93_strlcpy(s, sels, sizeof s);
    char *sv[8]; int nsv = 0;
    char *save = NULL;
    for (char *tok = strtok_r(s, ",", &save); tok && nsv < 8;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t' || *tok == '\n') tok++;
        char *e = tok + strlen(tok);
        while (e > tok && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n')) *--e = 0;
        if (*tok) sv[nsv++] = tok;
    }
    char *d = buf;
    while (*d) {
        while (*d == ' ' || *d == '\t' || *d == '\n' || *d == ';') d++;
        char *pe = d;
        while (*pe && *pe != ':' && *pe != ';' && *pe != '}') pe++;
        if (*pe != ':') break;
        char prop[48];
        size_t pl = (size_t)(pe - d);
        if (pl >= sizeof prop) pl = sizeof prop - 1;
        memcpy(prop, d, pl);
        prop[pl] = 0;
        char *vs = pe + 1;
        char *ve = vs;
        while (*ve && *ve != ';') ve++;
        char val[256];
        size_t vl = (size_t)(ve - vs);
        if (vl >= sizeof val) vl = sizeof val - 1;
        memcpy(val, vs, vl);
        val[vl] = 0;
        char *ve2 = val + strlen(val);
        while (ve2 > val && (ve2[-1] == ' ' || ve2[-1] == '\t' ||
                             ve2[-1] == '\n')) *--ve2 = 0;
        char *vs2 = val;
        while (*vs2 == ' ' || *vs2 == '\t' || *vs2 == '\n') vs2++;
        if (*prop && *vs2)
            for (int i = 0; i < nsv; i++) css_push(c, sv[i], prop, vs2);
        d = *ve ? ve + 1 : ve;
    }
}

void k93css_add(k93css *c, const char *text)
{
    if (!c || !text) return;
    const char *p = text;
    char sels[512];
    while (*p) {
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            p = e ? e + 2 : p + strlen(p);
            continue;
        }
        if (*p == '@') {                 /* @media etc: skip whole block */
            const char *b = strchr(p, '{');
            const char *semi = strchr(p, ';');
            if (!b || (semi && semi < b)) {
                p = semi ? semi + 1 : p + strlen(p);
                continue;
            }
            int depth = 0;
            for (p = b; *p; p++) {
                if (*p == '{') depth++;
                else if (*p == '}' && --depth == 0) { p++; break; }
            }
            continue;
        }
        if (*p == '}') { p++; continue; }
        /* selector up to '{' */
        const char *b = strchr(p, '{');
        if (!b) break;
        size_t sl = (size_t)(b - p);
        if (sl >= sizeof sels) sl = sizeof sels - 1;
        memcpy(sels, p, sl);
        sels[sl] = 0;
        int depth = 1;
        const char *body = b + 1;
        const char *e = body;
        while (*e && depth) {
            if (*e == '{') depth++;
            else if (*e == '}') depth--;
            if (depth) e++;
        }
        css_block(c, sels, body, (size_t)(e - body));
        p = *e ? e + 1 : e;
    }
}

/* split "a, b , c" style args inside rgb()/hsl() - commas OR spaces */
static int func_args(const char *v, char toks[][40], int max)
{
    const char *o = strchr(v, '(');
    if (!o) return 0;
    const char *e = strchr(o, ')');
    char buf[160];
    size_t len = e ? (size_t)(e - o - 1) : strlen(o + 1);
    if (len >= sizeof buf) len = sizeof buf - 1;
    memcpy(buf, o + 1, len);
    buf[len] = 0;
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, ", \t", &save); t && n < max;
         t = strtok_r(NULL, ", \t", &save))
        k93_strlcpy(toks[n++], t, 40);
    return n;
}

/* h,s,l (s/l already 0..1) -> 0xRRGGBB; hand-rolled, no libm */
static uint32_t hsl2rgb(double h, double s, double l)
{
    while (h >= 360.0) h -= 360.0;
    while (h < 0.0) h += 360.0;
    double c = (1.0 - (l >= 0.5 ? 2.0 * l - 1.0 : 1.0 - 2.0 * l)) * s;
    double hp = h / 60.0;
    int sector = (int)hp;
    double frac = hp - sector;
    double x = c * ((sector & 1) ? 1.0 - frac : frac);   /* even sectors rise */
    double r = 0, g = 0, b = 0;
    switch (sector) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    double m = l - c / 2.0;
    int R = (int)((r + m) * 255.0 + 0.5), G = (int)((g + m) * 255.0 + 0.5),
        B = (int)((b + m) * 255.0 + 0.5);
    if (R < 0) R = 0;
    if (R > 255) R = 255;
    if (G < 0) G = 0;
    if (G > 255) G = 255;
    if (B < 0) B = 0;
    if (B > 255) B = 255;
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

/* all 148 CSS Color Module named colors (public spec data) */
static const struct { const char *n; uint32_t c; } cnames[] = {
    { "aliceblue", 0xF0F8FF }, { "antiquewhite", 0xFAEBD7 },
    { "aqua", 0x00FFFF }, { "aquamarine", 0x7FFFD4 }, { "azure", 0xF0FFFF },
    { "beige", 0xF5F5DC }, { "bisque", 0xFFE4C4 }, { "black", 0x000000 },
    { "blanchedalmond", 0xFFEBCD }, { "blue", 0x0000FF },
    { "blueviolet", 0x8A2BE2 }, { "brown", 0xA52A2A }, { "burlywood", 0xDEB887 },
    { "cadetblue", 0x5F9EA0 }, { "chartreuse", 0x7FFF00 },
    { "chocolate", 0xD2691E }, { "coral", 0xFF7F50 },
    { "cornflowerblue", 0x6495ED }, { "cornsilk", 0xFFF8DC },
    { "crimson", 0xDC143C }, { "cyan", 0x00FFFF }, { "darkblue", 0x00008B },
    { "darkcyan", 0x008B8B }, { "darkgoldenrod", 0xB8860B },
    { "darkgray", 0xA9A9A9 }, { "darkgreen", 0x006400 }, { "darkgrey", 0xA9A9A9 },
    { "darkkhaki", 0xBDB76B }, { "darkmagenta", 0x8B008B },
    { "darkolivegreen", 0x556B2F }, { "darkorange", 0xFF8C00 },
    { "darkorchid", 0x9932CC }, { "darkred", 0x8B0000 }, { "darksalmon", 0xE9967A },
    { "darkseagreen", 0x8FBC8F }, { "darkslateblue", 0x483D8B },
    { "darkslategray", 0x2F4F4F }, { "darkslategrey", 0x2F4F4F },
    { "darkturquoise", 0x00CED1 }, { "darkviolet", 0x9400D3 },
    { "deeppink", 0xFF1493 }, { "deepskyblue", 0x00BFFF }, { "dimgray", 0x696969 },
    { "dimgrey", 0x696969 }, { "dodgerblue", 0x1E90FF }, { "firebrick", 0xB22222 },
    { "floralwhite", 0xFFFAF0 }, { "forestgreen", 0x228B22 },
    { "fuchsia", 0xFF00FF }, { "gainsboro", 0xDCDCDC }, { "ghostwhite", 0xF8F8FF },
    { "gold", 0xFFD700 }, { "goldenrod", 0xDAA520 }, { "gray", 0x808080 },
    { "grey", 0x808080 }, { "green", 0x008000 }, { "greenyellow", 0xADFF2F },
    { "honeydew", 0xF0FFF0 }, { "hotpink", 0xFF69B4 }, { "indianred", 0xCD5C5C },
    { "indigo", 0x4B0082 }, { "ivory", 0xFFFFF0 }, { "khaki", 0xF0E68C },
    { "lavender", 0xE6E6FA }, { "lavenderblush", 0xFFF0F5 },
    { "lawngreen", 0x7CFC00 }, { "lemonchiffon", 0xFFFACD },
    { "lightblue", 0xADD8E6 }, { "lightcoral", 0xF08080 }, { "lightcyan", 0xE0FFFF },
    { "lightgoldenrodyellow", 0xFAFAD2 }, { "lightgray", 0xD3D3D3 },
    { "lightgreen", 0x90EE90 }, { "lightgrey", 0xD3D3D3 }, { "lightpink", 0xFFB6C1 },
    { "lightsalmon", 0xFFA07A }, { "lightseagreen", 0x20B2AA },
    { "lightskyblue", 0x87CEFA }, { "lightslategray", 0x778899 },
    { "lightslategrey", 0x778899 }, { "lightsteelblue", 0xB0C4DE },
    { "lightyellow", 0xFFFFE0 }, { "lime", 0x00FF00 }, { "limegreen", 0x32CD32 },
    { "linen", 0xFAF0E6 }, { "magenta", 0xFF00FF }, { "maroon", 0x800000 },
    { "mediumaquamarine", 0x66CDAA }, { "mediumblue", 0x0000CD },
    { "mediumorchid", 0xBA55D3 }, { "mediumpurple", 0x9370DB },
    { "mediumseagreen", 0x3CB371 }, { "mediumslateblue", 0x7B68EE },
    { "mediumspringgreen", 0x00FA9A }, { "mediumturquoise", 0x48D1CC },
    { "mediumvioletred", 0xC71585 }, { "midnightblue", 0x191970 },
    { "mintcream", 0xF5FFFA }, { "mistyrose", 0xFFE4E1 }, { "moccasin", 0xFFE4B5 },
    { "navajowhite", 0xFFDEAD }, { "navy", 0x000080 }, { "oldlace", 0xFDF5E6 },
    { "olive", 0x808000 }, { "olivedrab", 0x6B8E23 }, { "orange", 0xFFA500 },
    { "orangered", 0xFF4500 }, { "orchid", 0xDA70D6 },
    { "palegoldenrod", 0xEEE8AA }, { "palegreen", 0x98FB98 },
    { "paleturquoise", 0xAFEEEE }, { "palevioletred", 0xDB7093 },
    { "papayawhip", 0xFFEFD5 }, { "peachpuff", 0xFFDAB9 }, { "peru", 0xCD853F },
    { "pink", 0xFFC0CB }, { "plum", 0xDDA0DD }, { "powderblue", 0xB0E0E6 },
    { "purple", 0x800080 }, { "rebeccapurple", 0x663399 }, { "red", 0xFF0000 },
    { "rosybrown", 0xBC8F8F }, { "royalblue", 0x4169E1 }, { "saddlebrown", 0x8B4513 },
    { "salmon", 0xFA8072 }, { "sandybrown", 0xF4A460 }, { "seagreen", 0x2E8B57 },
    { "seashell", 0xFFF5EE }, { "sienna", 0xA0522D }, { "silver", 0xC0C0C0 },
    { "skyblue", 0x87CEEB }, { "slateblue", 0x6A5ACD }, { "slategray", 0x708090 },
    { "slategrey", 0x708090 }, { "snow", 0xFFFAFA }, { "springgreen", 0x00FF7F },
    { "steelblue", 0x4682B4 }, { "tan", 0xD2B48C }, { "teal", 0x008080 },
    { "thistle", 0xD8BFD8 }, { "tomato", 0xFF6347 }, { "turquoise", 0x40E0D0 },
    { "violet", 0xEE82EE }, { "wheat", 0xF5DEB3 }, { "white", 0xFFFFFF },
    { "whitesmoke", 0xF5F5F5 }, { "yellow", 0xFFFF00 }, { "yellowgreen", 0x9ACD32 },
    { "transparent", 0 }, { NULL, 0 }
};

int k93css_color(const char *v, uint32_t *out)
{
    if (!v || !*v) return 0;
    while (*v == ' ' || *v == '\t') v++;
    if (v[0] == '#') {
        char hex[8];
        size_t hl = strspn(v + 1, "0123456789abcdefABCDEF");
        if (hl == 3) {
            for (int i = 0; i < 3; i++) {
                char h[3] = { v[1 + i], v[1 + i], 0 };
                hex[i * 2] = h[0]; hex[i * 2 + 1] = h[0];
            }
            hex[6] = 0;
            *out = (uint32_t)strtoul(hex, NULL, 16);
            return 1;
        }
        if (hl == 6) {
            memcpy(hex, v + 1, 6);
            hex[6] = 0;
            *out = (uint32_t)strtoul(hex, NULL, 16);
            return 1;
        }
        return 0;
    }
    if (!strncasecmp(v, "rgb", 3) && (v[3] == '(' || v[3] == 'a')) {
        if (v[3] == 'a' && v[4] != '(') return 0;
        char t[4][40];
        int n = func_args(v, t, 4);
        if (n < 3) return 0;
        int ch[3];
        for (int i = 0; i < 3; i++) {
            double d = strtod(t[i], NULL);
            if (strchr(t[i], '%')) d = d * 255.0 / 100.0;
            if (d < 0) d = 0;
            if (d > 255) d = 255;
            ch[i] = (int)(d + 0.5);
        }
        *out = ((uint32_t)ch[0] << 16) | ((uint32_t)ch[1] << 8) |
               (uint32_t)ch[2];
        return 1;
    }
    if (!strncasecmp(v, "hsl", 3) && (v[3] == '(' || v[3] == 'a')) {
        if (v[3] == 'a' && v[4] != '(') return 0;
        char t[4][40];
        int n = func_args(v, t, 4);
        if (n < 3) return 0;
        double h = strtod(t[0], NULL);
        double s = strtod(t[1], NULL);
        double l = strtod(t[2], NULL);
        if (strchr(t[1], '%')) s /= 100.0;
        if (strchr(t[2], '%')) l /= 100.0;
        if (s > 1.0) s /= 100.0;
        if (l > 1.0) l /= 100.0;
        *out = hsl2rgb(h, s, l);
        return 1;
    }
    for (int i = 0; cnames[i].n; i++)
        if (!strcasecmp(cnames[i].n, v)) {
            if (!cnames[i].c && strcasecmp(v, "transparent")) return 0;
            *out = cnames[i].c;
            return 1;
        }
    return 0;
}

/* linear-gradient([to <dir>,] c1 [, pos] [, c2 [, pos]]) -> top/bottom color.
 * Only the two endpoint colors are kept (linear vertical gradient). */
static int linear_gradient(const char *v, uint32_t *top, uint32_t *bot)
{
    const char *p = strcasestr(v, "linear-gradient(");
    if (!p) return 0;
    p += strlen("linear-gradient(");
    char args[256];
    size_t o = 0;
    while (*p && *p != ')' && o < sizeof args - 1) args[o++] = *p++;
    args[o] = 0;
    char *save = NULL, *tok;
    uint32_t cols[2];
    int nc = 0;
    for (tok = strtok_r(args, ",", &save); tok && nc < 2;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        if (!strncasecmp(tok, "to ", 3)) continue;      /* direction */
        uint32_t c;
        if (k93css_color(tok, &c)) { cols[nc++] = c; continue; }
        char lead[64];
        size_t li = 0;
        while (*tok && *tok != ' ' && li < sizeof lead - 1) lead[li++] = *tok++;
        lead[li] = 0;
        if (li && k93css_color(lead, &c)) cols[nc++] = c;  /* color stop */
    }
    if (nc < 1) return 0;
    *top = cols[0];
    *bot = nc > 1 ? cols[1] : cols[0];
    return 1;
}

static void apply_decl(k93cstyle *st, const char *prop, const char *val)
{
    uint32_t col;
    uint32_t gt, gb;
    if (!strcasecmp(prop, "color")) {
        if (k93css_color(val, &col)) { st->fg = col; st->flags |= K93CSS_FG; }
    } else if (!strcasecmp(prop, "background-color")) {
        if (linear_gradient(val, &gt, &gb)) {
            st->bg = gt; st->gbot = gb;
            st->flags |= K93CSS_BG | K93CSS_GRAD;
        } else if (strcasecmp(val, "transparent") && k93css_color(val, &col)) {
            st->bg = col; st->flags |= K93CSS_BG;
        }
    } else if (!strcasecmp(prop, "background")) {
        if (linear_gradient(val, &gt, &gb)) {
            st->bg = gt; st->gbot = gb;
            st->flags |= K93CSS_BG | K93CSS_GRAD;
        } else {
        /* shorthand: first color token wins; url()/gradient() tokens and
         * position/size keywords skipped */
        const char *q = val;
        while (*q) {
            while (*q == ' ' || *q == ',') q++;
            char tok[64]; size_t ti = 0;
            while (*q && *q != ' ' && *q != ',' && ti < sizeof tok - 1)
                tok[ti++] = *q++;
            tok[ti] = 0;
            if (!ti || strchr(tok, '(')) continue;
            if (k93css_color(tok, &col) && strcasecmp(tok, "transparent")) {
                st->bg = col; st->flags |= K93CSS_BG;
                break;
            }
        }
        }
    } else if (!strcasecmp(prop, "font-weight")) {
        if (!strcasecmp(val, "bold") || !strcasecmp(val, "bolder") ||
            atoi(val) >= 600) { st->bold = 1; st->flags |= K93CSS_BOLD; }
        else if (!strcasecmp(val, "normal") || atoi(val) > 0) {
            st->bold = 0; st->flags |= K93CSS_BOLD;
        }
    } else if (!strcasecmp(prop, "font-size")) {
        int px = 0;
        if (strchr(val, '%') || (strstr(val, "em") && !strstr(val, "px")))
            px = (int)(strtod(val, NULL) * 16);    /* em/% of 16px base */
        else if (isdigit((unsigned char)val[0]) || val[0] == '-' ||
                 val[0] == '.')
            px = atoi(val);
        else if (!strcasecmp(val, "small")) px = 14;
        else if (!strcasecmp(val, "medium")) px = 16;
        else if (!strcasecmp(val, "large")) px = 18;
        else if (!strcasecmp(val, "x-large")) px = 24;
        else if (!strcasecmp(val, "xx-large")) px = 32;
        if (px >= 22) { st->scale = 2; st->flags |= K93CSS_SCALE; }
        else if (px > 0) { st->scale = 1; st->flags |= K93CSS_SCALE; }
    } else if (!strcasecmp(prop, "font")) {
        /* shorthand: [style] [weight] size[/lh] family */
        const char *q = val;
        const char *fam = NULL;
        while (*q) {
            while (*q == ' ' || *q == ',') q++;
            char tok[64]; size_t ti = 0;
            while (*q && *q != ' ' && ti < sizeof tok - 1) tok[ti++] = *q++;
            tok[ti] = 0;
            if (!ti) continue;
            if (!strcasecmp(tok, "bold") || !strcasecmp(tok, "bolder")) {
                st->bold = 1; st->flags |= K93CSS_BOLD;
            } else if (strchr(tok, '/')) {          /* size/lh, family next */
                fam = q;
                apply_decl(st, "font-size", tok);
                break;
            } else if (isdigit((unsigned char)tok[0])) {
                apply_decl(st, "font-size", tok);
                fam = q;
                break;
            }
        }
        if (fam && *fam) {
            char fambuf[64];
            size_t fi = 0;
            while (*fam && *fam != ',' && *fam != '/' && fi < sizeof fambuf - 1)
                fambuf[fi++] = *fam++;
            while (fi > 0 && (fambuf[fi - 1] == ' ' || fambuf[fi - 1] == ' '))
                fi--;
            fambuf[fi] = 0;
            if (fi) apply_decl(st, "font-family", fambuf);
        }
    } else if (!strcasecmp(prop, "text-align")) {
        if (!strcasecmp(val, "center")) {
            st->center = 1; st->flags |= K93CSS_CENTER;
        } else if (!strcasecmp(val, "left") || !strcasecmp(val, "right") ||
                   !strcasecmp(val, "justify")) {
            st->center = 0; st->flags |= K93CSS_CENTER;
        }
    } else if (!strcasecmp(prop, "display")) {
        if (!strcasecmp(val, "none")) {
            st->hidden = 1; st->flags |= K93CSS_HIDDEN;
        }
    } else if (!strcasecmp(prop, "visibility")) {
        if (!strcasecmp(val, "hidden") || !strcasecmp(val, "collapse")) {
            st->hidden = 1; st->flags |= K93CSS_HIDDEN;
        }
    } else if (!strcasecmp(prop, "margin-left") ||
               !strcasecmp(prop, "padding-left") ||
               !strcasecmp(prop, "text-indent")) {
        int px = atoi(val);
        if (px > 0) { st->indent += px; st->flags |= K93CSS_INDENT; }
    } else if (!strcasecmp(prop, "margin") || !strcasecmp(prop, "padding")) {
        /* shorthand: left component = 4th of 4, 2nd of 2, else the 1st */
        char toks[4][32]; int nt = 0;
        const char *q = val;
        while (*q && nt < 4) {
            while (*q == ' ') q++;
            size_t ti = 0;
            while (*q && *q != ' ' && ti < 31) toks[nt][ti++] = *q++;
            toks[nt][ti] = 0;
            if (ti) nt++;
        }
        const char *lv = nt == 4 ? toks[3] : nt == 2 ? toks[1]
                        : nt ? toks[0] : "";
        int px = atoi(lv);
        if (px > 0) { st->indent += px; st->flags |= K93CSS_INDENT; }
    } else if (!strcasecmp(prop, "text-decoration")) {
        st->underline = strstr(val, "underline") != NULL;
        st->strike = strstr(val, "line-through") != NULL;
        st->flags |= K93CSS_UNDERLINE | K93CSS_STRIKE;
    } else if (!strcasecmp(prop, "text-transform")) {
        if (!strcasecmp(val, "uppercase") || !strcasecmp(val, "capitalize"))
            st->xform = 1;
        else if (!strcasecmp(val, "lowercase"))
            st->xform = 2;
        else
            st->xform = 0;
        st->flags |= K93CSS_XFORM;
    } else if (!strcasecmp(prop, "line-height")) {
        if (!strcasecmp(val, "normal")) {
            st->lh = 0; st->flags |= K93CSS_LH;
        } else if (strchr(val, 'p')) {
            int px = atoi(val);
            if (px > 0) { st->lh = px; st->flags |= K93CSS_LH; }
        } else {
            double d = strtod(val, NULL);          /* unitless factor */
            if (d > 0) {
                st->lh = (int)(d * 16.0 + 0.5);
                st->flags |= K93CSS_LH;
            }
        }
    } else if (!strcasecmp(prop, "opacity")) {
        double d = strtod(val, NULL);
        if (d < 0) d = 0;
        int pct = d <= 1.0 ? (int)(d * 100.0 + 0.5) : (int)(d + 0.5);
        if (pct > 100) pct = 100;
        st->opacity = pct;
        st->flags |= K93CSS_OPACITY;
    } else if (!strcasecmp(prop, "width")) {
        int px = atoi(val);                        /* px only; % ceiling */
        if (px > 0) { st->width = px; st->flags |= K93CSS_WIDTH; }
    } else if (!strcasecmp(prop, "list-style") ||
               !strcasecmp(prop, "list-style-type")) {
        const char *q = val;
        while (*q) {
            while (*q == ' ' || *q == ',') q++;
            char tok[48]; size_t ti = 0;
            while (*q && *q != ' ' && *q != ',' && ti < sizeof tok - 1)
                tok[ti++] = *q++;
            tok[ti] = 0;
            int ls = -1;
            if (!strcasecmp(tok, "disc")) ls = K93L_DISC;
            else if (!strcasecmp(tok, "circle")) ls = K93L_CIRCLE;
            else if (!strcasecmp(tok, "square")) ls = K93L_SQUARE;
            else if (!strcasecmp(tok, "decimal")) ls = K93L_DECIMAL;
            else if (!strcasecmp(tok, "lower-alpha") ||
                     !strcasecmp(tok, "lower-latin")) ls = K93L_LALPHA;
            else if (!strcasecmp(tok, "upper-alpha") ||
                     !strcasecmp(tok, "upper-latin")) ls = K93L_UALPHA;
            else if (!strcasecmp(tok, "lower-roman")) ls = K93L_LROMAN;
            else if (!strcasecmp(tok, "upper-roman")) ls = K93L_UROMAN;
            else if (!strcasecmp(tok, "none")) ls = K93L_NONE;
            if (ls >= 0) {
                st->lstyle = ls;
                st->flags |= K93CSS_LSTYLE;
            }
        }
    } else if (!strncasecmp(prop, "border", 6) &&
               strcasecmp(prop, "border-radius") &&
               strcasecmp(prop, "border-collapse") &&
               strcasecmp(prop, "border-spacing") &&
               strcasecmp(prop, "border-image")) {
        if (!strcasecmp(prop, "border-width")) {
            int px = atoi(val);
            if (px > 0) { st->bordw = px; st->flags |= K93CSS_BORDER; }
        } else if (!strcasecmp(prop, "border-color")) {
            if (k93css_color(val, &col)) {
                st->bordc = col;
                st->flags |= K93CSS_BORDER;
            }
        } else if (!strcasecmp(prop, "border-style")) {
            if (!strcasecmp(val, "none") || !strcasecmp(val, "hidden")) {
                st->bordw = 0;
                st->flags |= K93CSS_BORDER;
            } else if (!st->bordw) {
                st->bordw = 1;
                st->flags |= K93CSS_BORDER;
            }
        } else {
            /* border / border-<side> shorthand: width style color tokens;
             * sides collapse into one box (layout is line-based) */
            const char *q = val;
            int got = 0;
            while (*q) {
                while (*q == ' ' || *q == ',') q++;
                char tok[48]; size_t ti = 0;
                while (*q && *q != ' ' && *q != ',' && ti < sizeof tok - 1)
                    tok[ti++] = *q++;
                tok[ti] = 0;
                if (!ti) continue;
                if (!strcasecmp(tok, "none") || !strcasecmp(tok, "hidden")) {
                    st->bordw = 0; got = 1;
                } else if (!strcasecmp(tok, "thin")) { st->bordw = 1; got = 1; }
                else if (!strcasecmp(tok, "medium")) { st->bordw = 2; got = 1; }
                else if (!strcasecmp(tok, "thick")) { st->bordw = 3; got = 1; }
                else if (isdigit((unsigned char)tok[0])) {
                    int px = atoi(tok);
                    if (px > 0) { st->bordw = px; got = 1; }
                } else if (k93css_color(tok, &col)) {
                    if (strcasecmp(tok, "transparent")) st->bordc = col;
                    got = 1;
                } else if (!strcasecmp(tok, "solid") ||
                           !strcasecmp(tok, "dashed") ||
                           !strcasecmp(tok, "dotted") ||
                           !strcasecmp(tok, "double") ||
                           !strcasecmp(tok, "groove") ||
                           !strcasecmp(tok, "ridge") ||
                           !strcasecmp(tok, "inset") ||
                           !strcasecmp(tok, "outset")) {
                    if (st->bordw == 0) st->bordw = 1;
                    got = 1;
                }
            }
            if (got) st->flags |= K93CSS_BORDER;
        }
    } else if (!strcasecmp(prop, "font-family")) {
        /* first family name, quotes stripped; unknown -> system default */
        char fam[48]; size_t fi = 0;
        const char *q = val;
        while (*q && *q != ',' && fi < sizeof fam - 1) fam[fi++] = *q++;
        while (fi > 0 && (fam[fi - 1] == ' ' || fam[fi - 1] == '\t')) fi--;
        fam[fi] = 0;
        if (fi >= 2 && (fam[0] == '"' || fam[0] == '\'') &&
            fam[fi - 1] == fam[0]) {
            memmove(fam, fam + 1, fi - 2);
            fi -= 2;
            fam[fi] = 0;
        }
        st->font = fi ? k93font_id(fam) : -1;
        st->flags |= K93CSS_FONT;
    } else if (!strcasecmp(prop, "font-style")) {
        /* italic/oblique: no italic faces rasterized (ceiling) - upright */
    }
    /* unhandled props (float, position, overflow, box-shadow, cursor,
     * transition, transform, ...) fall through: ignored by design */
}

/* rough specificity: ids*100 + (classes+attrs+pseudos)*10 + tags; later wins
 * ties */
static int sel_spec(const char *s)
{
    int spec = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '#') spec += 100;
        else if (*p == '.' || *p == '[' || *p == ':') spec += 10;
        else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                 *p == '*') {
            spec += 1;
            while (p[1] && ((p[1] >= 'a' && p[1] <= 'z') ||
                            (p[1] >= 'A' && p[1] <= 'Z') ||
                            (p[1] >= '0' && p[1] <= '9') || p[1] == '-' ||
                            p[1] == '*' || p[1] == '_' || p[1] == '['))
                p++;
        }
    }
    return spec;
}

void k93css_apply(const k93css *c, const k93node *n, k93cstyle *st)
{
    if (!n) return;
    if (c && c->n) {
        int ord[512], spec[512], m = 0;     /* ceiling: 512 matching decls */
        for (int i = 0; i < c->n && m < 512; i++)
            if (match_sel(n, c->sel[i])) {
                ord[m] = i;
                spec[m] = sel_spec(c->sel[i]);
                m++;
            }
        for (int i = 1; i < m; i++) {       /* stable sort: spec, then order */
            int o = ord[i], s = spec[i], j = i - 1;
            while (j >= 0 && (spec[j] > s || (spec[j] == s && ord[j] > o))) {
                ord[j + 1] = ord[j];
                spec[j + 1] = spec[j];
                j--;
            }
            ord[j + 1] = o;
            spec[j + 1] = s;
        }
        for (int i = 0; i < m; i++)
            apply_decl(st, c->prop[ord[i]], c->val[ord[i]]);
    }
    /* inline style attribute wins (applied last) */
    const char *sa = k93html_attr(n, "style");
    if (sa) {
        char buf[512];
        k93_strlcpy(buf, sa, sizeof buf);
        char *d = buf;
        while (*d) {
            while (*d == ' ' || *d == '\t') d++;
            char *pe = d;
            while (*pe && *pe != ':' && *pe != ';') pe++;
            if (*pe != ':') break;
            char prop[48];
            size_t pl = (size_t)(pe - d);
            if (pl >= sizeof prop) pl = sizeof prop - 1;
            memcpy(prop, d, pl);
            prop[pl] = 0;
            char *vs = pe + 1;
            char *ve = vs;
            while (*ve && *ve != ';') ve++;
            *ve = 0;
            apply_decl(st, prop, vs);
            d = *ve ? ve + 1 : ve;
        }
    }
}
