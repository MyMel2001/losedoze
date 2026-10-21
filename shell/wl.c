/* wl.c — hand-rolled Wayland + xdg-shell wire server for loseshell.
 *
 * No libwayland-server: raw unix-socket wire protocol, object registry,
 * event marshalling, shm buffers, xkb_v1 keymap fd. Original code from the
 * public protocol definitions. Covers what win/wlclient.c (Win32 subsystem) needs:
 * wl_display, wl_registry, wl_compositor(+surface,region), wl_shm(+pool,
 * buffer), wl_seat(+keyboard,+pointer), wl_output, wl_callback,
 * xdg_wm_base(+surface,+toplevel,+popup,+positioner),
 * wl_subcompositor(+subsurface), wl_data_device_manager (skeleton —
 * clipboard transfer is a documented ceiling).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "lost.h"
#include "keymap.h"

#define LOGF "/var/log/losedoze.log"
static FILE *wlg;

static void wlog(const char *fmt, ...) {
    va_list ap;
    if (!wlg) { wlg = fopen(LOGF, "a"); if (!wlg) wlg = fopen("/tmp/losedoze-wl.log", "a"); }
    va_start(ap, fmt);
    if (wlg) { va_list a2; va_copy(a2, ap); vfprintf(wlg, fmt, ap); fputc('\n', wlg); fflush(wlg); va_end(a2); }
    va_end(ap);
}

/* ---------------- basics ---------------- */
typedef struct Cl Cl;
typedef struct Obj Obj;
typedef struct Surf Surf;
typedef struct Buf Buf;
typedef struct Pool Pool;
typedef struct Wi Wi;

typedef struct Cur { const uint8_t *p, *end; Cl *cl; } Cur;
typedef struct ObjVt {
    const char *name;
    void (*req)(Cl *cl, Obj *o, uint32_t op, Cur *cur);
} ObjVt;

struct Obj { uint32_t id; const ObjVt *vt; void *d; };

struct Cl {
    int fd;
    uint8_t rbuf[131072]; size_t rlen;
    Obj **objs; int objs_cap;
    int *fds; int nfd;
    uint8_t *wbuf; size_t wlen, wcap;
    size_t wr_pos;
    Obj *kb_surf, *ptr_surf;
    unsigned last_mods;
    Cl *next;
};

struct Pool { uint8_t *map; size_t len; int fd; int refs; };
struct Buf { Pool *pool; int off, w, h, stride, format; uint32_t *px; uint32_t obj_id; int released;
              void *dmap; size_t dlen; };   /* dmap: dma-buf mapping (not pool memory) */
typedef struct Fc { uint32_t id; struct Fc *next; } Fc;

struct Surf {
    Obj *o; Cl *cl;
    Buf *pend_buf; int pend_set; Buf *cur_buf;
    Fc *pend_fc, *cur_fc;
    int mapped;
    Surf *parent;
    Wi *win;
};

typedef struct XdgS {
    Surf *s;
    uint32_t ser_pend;
    int is_popup;
    int cfg_sent;              /* initial configure delivered */
    int ax, ay, aw, ah, pw, ph, anchor, gravity, adj, ox, oy;
} XdgS;

struct Wi {
    WlWin pub;
    Surf *s;
    XdgS *xdg;
    Obj *toplevel;
    Obj *xdg_obj;
    Cl *owner;
    Wi *next;
};

typedef struct wlserver {
    int lfd;
    char sock[96];
    Cl *cls;
    struct wlserver_cbs cb;
    Wi *wins;                   /* head = topmost */
    Wi *kb_focus;
    Surf *ptr_surf;
    uint32_t serial;
} wlserver;

static uint32_t now_ms_t(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ---------------- arg parsing ---------------- */
static uint32_t a_u(Cur *c) { uint32_t v = 0; if (c->p + 4 <= c->end) { memcpy(&v, c->p, 4); c->p += 4; } return v; }
static int32_t a_i(Cur *c) { return (int32_t)a_u(c); }
static const char *a_s(Cur *c, char *buf, size_t cap) {
    uint32_t n = a_u(c);
    if (!n || c->p + n > c->end) { buf[0] = 0; return buf; }
    size_t k = n - 1 < cap - 1 ? n - 1 : cap - 1;
    memcpy(buf, c->p, k); buf[k] = 0;
    c->p += (n + 3) & ~3u;
    return buf;
}
static int a_fd(Cur *c);

static Obj *a_o(Cur *c, const ObjVt *vt) {
    uint32_t id = a_u(c);
    if (!id) return NULL;
    Cl *cl = c->cl;
    if ((int)id >= cl->objs_cap || !cl->objs[id]) return NULL;
    Obj *o = cl->objs[id];
    if (vt && o->vt != vt) return NULL;
    return o;
}

/* ---------------- event marshalling ---------------- */
typedef struct Bf { uint8_t d[1024]; uint32_t n; } Bf;
static void bu32(Bf *b, uint32_t v) { if (b->n + 4 <= sizeof b->d) { memcpy(b->d + b->n, &v, 4); b->n += 4; } }
static void bi32(Bf *b, int32_t v) { bu32(b, (uint32_t)v); }
static void bfix(Bf *b, int32_t v) { bu32(b, (uint32_t)v); }
static void bobj(Bf *b, uint32_t id) { bu32(b, id); }
static void bstr(Bf *b, const char *s) {
    uint32_t n = s ? (uint32_t)strlen(s) + 1 : 0;
    bu32(b, n);
    if (n && b->n + n <= sizeof b->d) { memcpy(b->d + b->n, s, n); b->n += (n + 3) & ~3u; }
}
static void barr(Bf *b, const void *data, uint32_t n) {
    bu32(b, n);
    if (b->n + n <= sizeof b->d) { memcpy(b->d + b->n, data, n); b->n += (n + 3) & ~3u; }
}

static void cl_flush(Cl *cl);   /* fwd */

static void send_ev(Cl *cl, uint32_t obj, uint32_t op, Bf *b) {
    size_t total = 8 + b->n;
    if (cl->wlen + total > cl->wcap) {
        size_t nc = cl->wcap ? cl->wcap * 2 : 8192;
        while (nc < cl->wlen + total) nc *= 2;
        uint8_t *nb = realloc(cl->wbuf, nc);
        if (!nb) return;
        cl->wbuf = nb; cl->wcap = nc;
    }
    uint32_t h0 = obj, h1 = ((uint32_t)total << 16) | op;
    memcpy(cl->wbuf + cl->wlen, &h0, 4);
    memcpy(cl->wbuf + cl->wlen + 4, &h1, 4);
    if (b->n) memcpy(cl->wbuf + cl->wlen + 8, b->d, b->n);
    cl->wlen += total;
}
#define EV(cl, obj, op, code) do { Bf _b = { .n = 0 }; { code } send_ev((cl), (obj), (op), &_b); } while (0)

static void ev_display_delete_id(Cl *cl, uint32_t id) {
    EV(cl, 1, 1, { bu32(&_b, id); });
}
static void ev_display_error(Cl *cl, uint32_t obj, int32_t code, const char *msg) {
    EV(cl, 1, 0, { bu32(&_b, obj); bi32(&_b, code); bstr(&_b, msg); });
}

/* one event carrying an fd via SCM_RIGHTS (fd args occupy NO stream bytes;
 * wl_keyboard.keymap(format u, fd -, size u) = 8-byte payload + cmsg) */
static void send_ev_fd(Cl *cl, uint32_t obj, uint32_t op, int fd, uint32_t w1, uint32_t w2) {
    uint32_t payload = 8;
    uint32_t h0 = obj, h1 = ((8 + payload) << 16) | op;
    uint8_t pkt[8 + 8];
    memcpy(pkt, &h0, 4);
    memcpy(pkt + 4, &h1, 4);
    memcpy(pkt + 8, &w1, 4);
    memcpy(pkt + 12, &w2, 4);
    struct msghdr mh = { 0 };
    struct iovec iov = { .iov_base = pkt, .iov_len = sizeof pkt };
    char cbuf[CMSG_SPACE(sizeof(int))];
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    if (sendmsg(cl->fd, &mh, MSG_NOSIGNAL) < 0)
        wlog("wl: sendmsg fd failed: %s", strerror(errno));
}

/* ---------------- object registry ---------------- */
static Obj *obj_new(Cl *cl, uint32_t id, const ObjVt *vt, void *d) {
    if (!id) return NULL;
    if ((int)id >= cl->objs_cap) {                  /* grow (clients create many ids) */
        int nc = cl->objs_cap;
        while ((int)id >= nc) nc *= 2;
        Obj **nt = realloc(cl->objs, (size_t)nc * sizeof *nt);
        if (!nt) return NULL;
        memset(nt + cl->objs_cap, 0, (size_t)(nc - cl->objs_cap) * sizeof *nt);
        cl->objs = nt; cl->objs_cap = nc;
    }
    if (cl->objs[id]) return NULL;
    Obj *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->id = id; o->vt = vt; o->d = d;
    cl->objs[id] = o;
    return o;
}
static void obj_free(Cl *cl, Obj *o) {
    if (!o) return;
    uint32_t id = o->id;
    cl->objs[id] = NULL;
    free(o);
    ev_display_delete_id(cl, id);
}

static const ObjVt vt_display, vt_registry, vt_compositor, vt_surface, vt_region,
    vt_shm, vt_pool, vt_buffer, vt_seat, vt_pointer, vt_keyboard, vt_output,
    vt_wm_base, vt_xdg_surface, vt_xdg_toplevel, vt_xdg_positioner, vt_xdg_popup,
    vt_subcompositor, vt_subsurface, vt_ddm, vt_data_offer, vt_data_device,
    vt_callback, vt_dmabuf, vt_dmabuf_params;

static const ObjVt *iface_by_name(const char *n) {
    static const ObjVt *all[] = { &vt_compositor, &vt_surface, &vt_region, &vt_shm,
        &vt_pool, &vt_buffer, &vt_seat, &vt_pointer, &vt_keyboard, &vt_output,
        &vt_wm_base, &vt_xdg_surface, &vt_xdg_toplevel, &vt_xdg_positioner,
        &vt_xdg_popup, &vt_subcompositor, &vt_subsurface, &vt_ddm,
        &vt_data_offer, &vt_data_device, &vt_callback, &vt_dmabuf,
        &vt_dmabuf_params, NULL };
    for (int i = 0; all[i]; i++)
        if (!strcmp(all[i]->name, n)) return all[i];
    return NULL;
}

/* ---------------- pools / buffers ---------------- */
static void pool_unref(Pool *p) {
    if (!p) return;
    if (--p->refs > 0) return;
    if (p->map && p->map != MAP_FAILED) munmap(p->map, p->len);
    if (p->fd >= 0) close(p->fd);
    free(p);
}

/* ---------------- window stack ---------------- */
static void win_to_top(Wi *w) {
    wlserver *s = (wlserver *)w->pub.srv;
    Wi **pp = &s->wins;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp == w) *pp = w->next;
    w->next = s->wins;
    s->wins = w;
}

/* ---------------- surface commit ---------------- */
static void xdg_send_configure(Wi *w, uint32_t cw, uint32_t ch, int activated);
static void surf_commit(Surf *sf, wlserver *s) {
    if (sf->pend_set) {
        if (sf->pend_buf && sf->pend_buf != sf->cur_buf) sf->pend_buf->released = 0;
        sf->cur_buf = sf->pend_buf;
        sf->pend_set = 0;
        sf->pend_buf = NULL;
    }
    if (sf->pend_fc) {                    /* pending frame cbs -> ready (done at ack) */
        Fc **pp = &sf->pend_fc;
        while (*pp) pp = &(*pp)->next;
        *pp = sf->cur_fc;
        sf->cur_fc = sf->pend_fc;
        sf->pend_fc = NULL;
    }
    Wi *w = sf->win;
    wlog("wl: surf_commit sf=%p win=%p xdg=%p cur_buf=%p %dx%d pend_set=%d", (void*)sf,
         (void*)w, w ? (void*)w->xdg : NULL, (void*)sf->cur_buf,
         sf->cur_buf ? sf->cur_buf->w : 0, sf->cur_buf ? sf->cur_buf->h : 0, sf->pend_set);
    if (w && w->xdg && !w->xdg->cfg_sent) {      /* initial configure on first commit */
        w->xdg->cfg_sent = 1;
        if (w->toplevel) xdg_send_configure(w, 0, 0, 0);   /* 0 = client picks size */
    }
    if (sf->cur_buf) {
        int was = sf->mapped;
        sf->mapped = 1;
        if (w) {
            w->pub.bw = sf->cur_buf->w;
            w->pub.bh = sf->cur_buf->h;
            w->pub.stride4 = sf->cur_buf->stride / 4;
            w->pub.alpha = sf->cur_buf->format == 1;   /* WL_SHM_FORMAT_ARGB8888 */
            w->pub.px = sf->cur_buf->px;
            w->pub.mapped = 1;
            if (!was) {
                win_to_top(w);
                if (s->cb.mapped) s->cb.mapped(s->cb.ud, &w->pub);
            }
            if (s->cb.resized) s->cb.resized(s->cb.ud, &w->pub, w->pub.bw, w->pub.bh);
        }
    } else if (sf->mapped) {
        sf->mapped = 0;
        if (w) {
            w->pub.mapped = 0;
            w->pub.px = NULL;
            if (s->cb.unmapped) s->cb.unmapped(s->cb.ud, &w->pub);
        }
    }
}

/* ---------------- keyboard focus ---------------- */
static void kb_focus_set(wlserver *s, Wi *w) {
    if (s->kb_focus == w) return;
    if (s->kb_focus && s->kb_focus->s && s->kb_focus->s->cl && s->kb_focus->s->o) {
        Cl *cl = s->kb_focus->s->cl;
        EV(cl, s->kb_focus->s->o->id, 2, { bu32(&_b, ++s->serial); bobj(&_b, s->kb_focus->s->o->id); });
        cl_flush(cl);
    }
    s->kb_focus = w;
    if (w && w->s && w->s->cl && w->s->o) {
        Cl *cl = w->s->cl;
        EV(cl, w->s->o->id, 1, {
            bu32(&_b, ++s->serial);
            bobj(&_b, w->s->o->id);
            barr(&_b, NULL, 0);
        });
        EV(cl, w->s->o->id, 4, {
            bu32(&_b, ++s->serial);
            bu32(&_b, cl->last_mods);
            bu32(&_b, 0); bu32(&_b, 0); bu32(&_b, 0);
        });
        cl_flush(cl);
    }
}

/* ---------------- xdg configure ---------------- */
static void xdg_send_configure(Wi *w, uint32_t cw, uint32_t ch, int activated) {
    if (!w || !w->xdg) return;
    Cl *cl = w->s->cl;
    wlserver *s = (wlserver *)w->pub.srv;
    /* NB: pub.cw/ch = size the compositor *requested* (see wlwin_configure).
     * The size sent here must not overwrite it: a toplevel that never asked
     * for a specific size keeps its own, and clients resize to whatever we
     * send. Sending a bogus 1x1 would shrink the client to a pixel. */
    w->xdg->ser_pend = ++s->serial;
    if (w->toplevel) {
        uint32_t st[1]; uint32_t nst = 0;
        if (activated) st[nst++] = 8;               /* XDG_TOPLEVEL_STATE_ACTIVATED */
        Bf b = { .n = 0 };
        bu32(&b, cw); bu32(&b, ch);
        barr(&b, st, nst * 4);
        send_ev(cl, w->toplevel->id, 0, &b);        /* xdg_toplevel.configure */
    } else {
        EV(cl, w->xdg_obj->id, 0, {                 /* xdg_popup.configure */
            bi32(&_b, 0); bi32(&_b, 0); bu32(&_b, cw); bu32(&_b, ch);
        });
    }
    EV(cl, w->xdg_obj->id, 0, { bu32(&_b, w->xdg->ser_pend); }); /* xdg_surface.configure */
    cl_flush(cl);
}

/* ---------------- registry globals ---------------- */
static const struct { uint32_t name; const char *iface; uint32_t ver; } globals[] = {
    { 1, "wl_compositor", 4 },
    { 2, "wl_shm", 1 },
    { 3, "wl_seat", 5 },
    { 4, "wl_output", 2 },
    { 5, "xdg_wm_base", 6 },
    { 6, "wl_data_device_manager", 3 },
    { 7, "wl_subcompositor", 1 },
    { 8, "zwp_linux_dmabuf_v1", 1 },
};

/* ---------------- request handlers ---------------- */
static void req_display(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    (void)o;
    if (op == 0) {                                  /* sync(callback) */
        uint32_t id = a_u(cur);
        obj_new(cl, id, &vt_callback, NULL);
        EV(cl, id, 0, { bu32(&_b, 0); });           /* done(0) */
        obj_free(cl, cl->objs[id]);
        cl_flush(cl);
    } else if (op == 1) {                           /* get_registry */
        uint32_t id = a_u(cur);
        obj_new(cl, id, &vt_registry, NULL);
        for (size_t i = 0; i < sizeof globals / sizeof globals[0]; i++)
            EV(cl, id, 0, { bu32(&_b, globals[i].name); bstr(&_b, globals[i].iface); bu32(&_b, globals[i].ver); });
        cl_flush(cl);
    }
}

static void bind_seat(Cl *cl, Obj *o);
static void bind_output(Cl *cl, Obj *o);

static void req_registry(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    if (op != 0) return;                            /* bind(name, interface, version, id) */
    uint32_t name = a_u(cur);
    char ifn[64];
    a_s(cur, ifn, sizeof ifn);
    a_u(cur);                                       /* version */
    uint32_t id = a_u(cur);
    const ObjVt *vt = iface_by_name(ifn);
    if (!vt) { ev_display_error(cl, o->id, 0, "unknown interface"); return; }
    Obj *no = obj_new(cl, id, vt, cl->objs[1] ? cl->objs[1]->d : NULL);
    if (!no) return;
    if (!strcmp(ifn, "wl_seat")) bind_seat(cl, no);
    else if (!strcmp(ifn, "wl_output")) bind_output(cl, no);
    else if (!strcmp(ifn, "zwp_linux_dmabuf_v1"))
        EV(cl, id, 0, { bu32(&_b, 0x34325258u); });  /* format: XRGB8888 */
    else if (!strcmp(ifn, "xdg_wm_base"))
        EV(cl, id, 0, { bu32(&_b, ++((wlserver *)no->d)->serial); });  /* ping */
    (void)name;
    cl_flush(cl);
}

static void req_compositor(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    (void)o;
    if (op == 0) {                                  /* create_surface */
        uint32_t id = a_u(cur);
        Surf *sf = calloc(1, sizeof *sf);
        if (!sf) return;
        sf->cl = cl;
        sf->o = obj_new(cl, id, &vt_surface, sf);
        if (!sf->o) free(sf);
    } else if (op == 1) {                           /* create_region */
        uint32_t id = a_u(cur);
        obj_new(cl, id, &vt_region, NULL);
    }
}

static void req_surface(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    Surf *sf = (Surf *)o->d;
    wlserver *s = (wlserver *)(cl->objs[1] ? cl->objs[1]->d : NULL);
    switch (op) {
    case 0:                                         /* destroy */
        if (sf->win) {
            Wi *w = sf->win;
            if (s && s->cb.unmapped && w->pub.mapped) s->cb.unmapped(s->cb.ud, &w->pub);
            w->pub.mapped = 0;
            w->pub.px = NULL;
            w->s = NULL;                            /* Wi kept; freed at disconnect */
            sf->win = NULL;
        }
        while (sf->pend_fc) { Fc *f = sf->pend_fc; sf->pend_fc = f->next; free(f); }
        while (sf->cur_fc) { Fc *f = sf->cur_fc; sf->cur_fc = f->next; free(f); }
        obj_free(cl, o);
        free(sf);
        break;
    case 1: {                                       /* attach(buffer, x, y) */
        Obj *bo = a_o(cur, &vt_buffer);
        sf->pend_buf = bo ? (Buf *)bo->d : NULL;
        sf->pend_set = 1;
        break;
    }
    case 2: case 9: case 10:                        /* damage / damage_buffer / offset */
        break;
    case 3: {                                       /* frame(callback) */
        uint32_t id = a_u(cur);
        Fc *f = calloc(1, sizeof *f);
        if (!f) break;
        f->id = id;
        if (obj_new(cl, id, &vt_callback, f)) {
            f->next = sf->pend_fc;
            sf->pend_fc = f;
        } else free(f);
        break;
    }
    case 4: case 5:                                 /* set_*_region */
        a_o(cur, NULL);
        break;
    case 6:                                         /* commit */
        surf_commit(sf, s);
        cl_flush(cl);
        break;
    case 7: case 8:                                 /* buffer transform/scale */
        a_i(cur);
        break;
    }
}

static void req_region(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    if (op == 0) { obj_free(cl, o); return; }
    (void)cur;                                      /* add/subtract: no-op */
}

static void req_shm(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    (void)o;
    if (op != 0) return;                            /* create_pool(id, fd, size):
                                                       stream: id, size; fd via cmsg */
    uint32_t id = a_u(cur);
    int32_t size = a_i(cur);
    int fd = a_fd(cur);
    if (fd < 0 || size <= 0) return;
    Pool *p = calloc(1, sizeof *p);
    if (!p) { close(fd); return; }
    p->fd = fd;
    p->len = (size_t)size;
    p->map = mmap(NULL, p->len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p->map == MAP_FAILED) { close(fd); free(p); return; }
    p->refs = 1;
    obj_new(cl, id, &vt_pool, p);
}

static void req_pool(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    Pool *p = (Pool *)o->d;
    switch (op) {
    case 0:                                         /* destroy (protocol op0) */
        obj_free(cl, o);
        pool_unref(p);
        break;
    case 1: {                                       /* create_buffer (op1) */
        uint32_t id = a_u(cur);
        int32_t off = a_i(cur), w = a_i(cur), h = a_i(cur), stride = a_i(cur);
        uint32_t fmt = a_u(cur);
        if (off < 0 || w <= 0 || h <= 0 || stride <= 0 ||
            (size_t)off + (size_t)h * (size_t)stride > p->len) return;
        Buf *b = calloc(1, sizeof *b);
        if (!b) return;
        b->pool = p; p->refs++;
        b->off = off; b->w = w; b->h = h; b->stride = stride; b->format = (int)fmt;
        b->px = (uint32_t *)(p->map + off);
        b->obj_id = id;
        b->released = 1;
        obj_new(cl, id, &vt_buffer, b);
        break;
    }
    case 2: {                                       /* resize */
        int32_t size = a_i(cur);
        if (size <= 0 || (size_t)size <= p->len) break;
        void *nm = mremap(p->map, p->len, (size_t)size, MREMAP_MAYMOVE);
        if (nm != MAP_FAILED) { p->map = nm; p->len = (size_t)size; }
        break;
    }
    }
}


/* ---------------- dmabuf (zwp_linux_dmabuf_v1) ----------------
 * Client-side GPU buffers: the client allocates a dma-buf (gbm bo + PRIME
 * export) and hands us the fd; we import it and composite it like an shm
 * buffer.  Deviation from upstream v1: params.create carries the wl_buffer
 * new_id (client allocated) instead of a server allocated id, because our
 * object table is a flat array indexed by id and cannot hold the 0xff000000
 * server id range.
 */
typedef struct DmabufParams {
    int fd;                     /* dma-buf fd from params.add */
    int32_t plane, offset, stride;
} DmabufParams;

static void req_dmabuf(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    if (op == 0) {                                  /* create_params(new_id) */
        uint32_t id = a_u(cur);
        DmabufParams *p = calloc(1, sizeof *p);
        if (!p) return;
        p->fd = -1;
        if (!obj_new(cl, id, &vt_dmabuf_params, p)) free(p);
    } else if (op == 1) {                           /* destroy */
        obj_free(cl, o);
    }
}

static void req_dmabuf_params(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    DmabufParams *p = (DmabufParams *)o->d;
    wlserver *s = (wlserver *)(cl->objs[1] ? cl->objs[1]->d : NULL);
    if (op == 0) {                                  /* destroy */
        obj_free(cl, o);
        if (p->fd >= 0) close(p->fd);
        free(p);
    } else if (op == 1) {                           /* add(fd, plane_idx, offset, stride) */
        int fd = a_fd(cur);
        int32_t plane = a_i(cur), off = a_i(cur), stride = a_i(cur);
        if (fd >= 0 && plane == 0 && stride > 0) {
            if (p->fd >= 0) close(p->fd);
            p->fd = fd; p->plane = plane; p->offset = off; p->stride = stride;
        } else if (fd >= 0) close(fd);
    } else if (op == 2) {                           /* create(new_id buffer, width, height) */
        uint32_t id = a_u(cur);
        int32_t w = a_i(cur), h = a_i(cur);
        Obj *ex = id < (uint32_t)cl->objs_cap ? cl->objs[id] : NULL;
        /* re-creating an existing buffer id refreshes it in place: the client
         * re-exports the same bo every frame (needed on copy-on-export drivers,
         * harmless on zero-copy ones) */
        Buf *b = (ex && ex->vt == &vt_buffer) ? (Buf *)ex->d : NULL;
        int fresh = 0;
        if (!b) {
            b = calloc(1, sizeof *b);
            if (b && obj_new(cl, id, &vt_buffer, b)) { fresh = 1; b->obj_id = id; }
            else { free(b); b = NULL; }
        }
        void *px = NULL;
        int iw = (int)w, ih = (int)h;                /* client-declared size */
        uint32_t len = p->stride > 0 && h > 0 ? (uint32_t)p->stride * (uint32_t)h : 0;
        int ok = b && p->fd >= 0 && w > 0 && h > 0 && s && s->cb.dmabuf_import &&
                 s->cb.dmabuf_import(s->cb.ud, p->fd, 0x34325258u /*XRGB8888*/,
                                      (uint32_t)p->stride, len, &px, &iw, &ih) == 0;
        if (!ok) {
            EV(cl, o->id, 1, { });                  /* failed */
            cl_flush(cl);
            obj_free(cl, o);
            if (p->fd >= 0) close(p->fd);
            free(p);
            return;
        }
        if (!fresh && b->dmap) { munmap(b->dmap, b->dlen); b->dmap = NULL; b->dlen = 0; }
        b->pool = NULL; b->off = 0; b->w = iw; b->h = ih; b->stride = p->stride;
        b->format = 1;                             /* XRGB8888: opaque */
        b->px = (uint32_t *)px; b->released = 1;
        b->dmap = px; b->dlen = len;
        EV(cl, o->id, 0, { bu32(&_b, id); });        /* created(buffer) */
        cl_flush(cl);
        obj_free(cl, o);
        if (p->fd >= 0) close(p->fd);
        free(p);
    }
}

static void req_buffer(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    (void)cur;
    if (op != 0) return;
    Buf *b = (Buf *)o->d;
    obj_free(cl, o);
    pool_unref(b->pool);
    if (b->dmap) munmap(b->dmap, b->dlen);
    free(b);
}

static void bind_seat(Cl *cl, Obj *o) {
    EV(cl, o->id, 0, { bu32(&_b, 0x3); });          /* capabilities: pointer|keyboard */
    EV(cl, o->id, 1, { bstr(&_b, "losedoze-seat"); });
    cl_flush(cl);
}
static void bind_output(Cl *cl, Obj *o) {
    EV(cl, o->id, 0, {                              /* geometry */
        bi32(&_b, 0); bi32(&_b, 0); bi32(&_b, 0); bi32(&_b, 0);
        bstr(&_b, "losedoze"); bstr(&_b, "lost-panel");
        bi32(&_b, 0); bi32(&_b, 0); bi32(&_b, 1);
    });
    EV(cl, o->id, 1, { bu32(&_b, 3); bi32(&_b, 1024); bi32(&_b, 768); bi32(&_b, 60000); });
    EV(cl, o->id, 2, { });                          /* done */
    EV(cl, o->id, 3, { bi32(&_b, 1); });            /* scale */
    cl_flush(cl);
}

static void req_seat(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    if (op == 2) { obj_free(cl, o); return; }       /* release */
    uint32_t id = a_u(cur);
    if (op == 0) {                                  /* get_pointer */
        obj_new(cl, id, &vt_pointer, NULL);
    } else if (op == 1) {                           /* get_keyboard */
        obj_new(cl, id, &vt_keyboard, NULL);
        size_t klen = strlen(lost_xkb_keymap) + 1;
        int mfd = memfd_create("lost-keymap", 0);
        if (mfd >= 0) {
            if (write(mfd, lost_xkb_keymap, klen) == (ssize_t)klen) {
                lseek(mfd, 0, SEEK_SET);              /* readers may read() not mmap() */
                send_ev_fd(cl, id, 0, mfd, 1 /*XKB_V1*/, (uint32_t)klen);
            }
            close(mfd);
        }
        EV(cl, id, 5, { bi32(&_b, 25); bi32(&_b, 400); }); /* repeat_info */
    }
    cl_flush(cl);
}

static void req_pointer(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    (void)cur;
    if (op == 1) obj_free(cl, o);                   /* release; set_cursor ignored */
}
static void req_keyboard(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    (void)cur;
    if (op == 0) obj_free(cl, o);
}
static void req_output(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    (void)cur;
    if (op == 0) obj_free(cl, o);
}

static Wi *win_new(wlserver *s, Surf *sf, XdgS *xdg, Cl *cl) {
    Wi *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->pub.srv = s;
    w->pub.popup = xdg->is_popup;
    w->s = sf; w->xdg = xdg; w->owner = cl;
    sf->win = w;
    w->next = s->wins;
    s->wins = w;
    return w;
}

static void req_wm_base(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    (void)cur;
    switch (op) {
    case 0: obj_free(cl, o); break;                 /* destroy */
    case 1: {                                       /* create_positioner */
        uint32_t id = a_u(cur);
        XdgS *x = calloc(1, sizeof *x);
        if (x) obj_new(cl, id, &vt_xdg_positioner, x);
        break;
    }
    case 2: {                                       /* get_xdg_surface(new, surface) */
        uint32_t id = a_u(cur);
        Obj *so = a_o(cur, &vt_surface);
        if (!so) break;
        XdgS *x = calloc(1, sizeof *x);
        if (!x) break;
        x->s = (Surf *)so->d;
        obj_new(cl, id, &vt_xdg_surface, x);
        break;
    }
    case 3: break;                                  /* pong */
    }
}

static void req_xdg_surface(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    wlserver *s = (wlserver *)(cl->objs[1] ? cl->objs[1]->d : NULL);
    XdgS *x = (XdgS *)o->d;
    switch (op) {
    case 0:                                         /* destroy */
        obj_free(cl, o);
        break;
    case 1: {                                       /* get_toplevel(new) */
        uint32_t id = a_u(cur);
        if (x->s->win) break;
        x->is_popup = 0;
        Wi *w = win_new(s, x->s, x, cl);
        if (!w) break;
        w->xdg_obj = o;
        w->toplevel = obj_new(cl, id, &vt_xdg_toplevel, w);
        EV(cl, id, 3, { bu32(&_b, 0); });           /* wm_capabilities: empty array */
        cl_flush(cl);
        break;
    }
    case 2: {                                       /* get_popup(new, parent, positioner) */
        uint32_t id = a_u(cur);
        Obj *po = a_o(cur, &vt_xdg_surface);
        Obj *pro = a_o(cur, &vt_xdg_positioner);
        if (x->s->win) break;
        x->is_popup = 1;
        if (pro && pro->d) {
            XdgS *px = (XdgS *)pro->d;
            x->ax = px->ax; x->ay = px->ay; x->aw = px->aw; x->ah = px->ah;
            x->pw = px->pw; x->ph = px->ph;
        }
        Wi *w = win_new(s, x->s, x, cl);
        if (!w) break;
        w->xdg_obj = o;
        obj_new(cl, id, &vt_xdg_popup, w);
        if (po && ((XdgS *)po->d)->s && ((XdgS *)po->d)->s->win) {
            Wi *pw = ((XdgS *)po->d)->s->win;
            w->pub.x = pw->pub.x + x->ax;
            w->pub.y = pw->pub.y + x->ay;
        }
        x->cfg_sent = 1;
        xdg_send_configure(w, (uint32_t)(x->pw > 0 ? x->pw : 100),
                           (uint32_t)(x->ph > 0 ? x->ph : 100), 0);
        break;
    }
    case 3:                                        /* set_window_geometry */
        a_i(cur); a_i(cur); a_i(cur); a_i(cur);
        break;
    case 4: {                                      /* ack_configure(serial) */
        uint32_t ser = a_u(cur);
        if (x->ser_pend && ser != x->ser_pend)
            wlog("wl: ack serial %u != pending %u", ser, x->ser_pend);
        break;
    }
    }
}

static void req_xdg_toplevel(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    Wi *w = (Wi *)o->d;
    wlserver *s = (wlserver *)w->pub.srv;
    switch (op) {
    case 0:                                         /* destroy */
        if (s->kb_focus == w) kb_focus_set(s, NULL);
        if (s->cb.unmapped && w->pub.mapped) s->cb.unmapped(s->cb.ud, &w->pub);
        w->pub.mapped = 0;
        w->pub.px = NULL;
        obj_free(cl, o);
        break;
    case 2:                                         /* set_title */
        a_s(cur, w->pub.title, sizeof w->pub.title);
        if (s->cb.title) s->cb.title(s->cb.ud, &w->pub);
        break;
    case 3:                                         /* set_app_id */
        a_s(cur, w->pub.appid, sizeof w->pub.appid);
        if (s->cb.title) s->cb.title(s->cb.ud, &w->pub);
        break;
    case 5: case 6: case 4:                         /* move/resize/show_menu: ceiling */
        break;
    case 7: case 8:                                 /* set_max/min_size */
        a_i(cur); a_i(cur);
        break;
    case 11:                                        /* set_fullscreen */
        a_o(cur, &vt_output);
        break;
    case 9: case 10: case 12: case 13:              /* max/full/min hints: ceiling */
        break;
    }
}

static void req_xdg_positioner(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    XdgS *x = (XdgS *)o->d;
    switch (op) {
    case 0: obj_free(cl, o); free(x); break;
    case 1: x->pw = a_i(cur); x->ph = a_i(cur); break;
    case 2:
        x->ax = a_i(cur); x->ay = a_i(cur); x->aw = a_i(cur); x->ah = a_i(cur);
        break;
    case 3: x->anchor = (int)a_u(cur); break;
    case 4: x->gravity = (int)a_u(cur); break;
    case 5: x->adj = (int)a_u(cur); break;
    case 6: x->ox = a_i(cur); x->oy = a_i(cur); break;
    case 7: a_u(cur); break;
    case 8: a_i(cur); a_i(cur); break;
    }
}

static void req_xdg_popup(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    (void)cur;
    Wi *w = (Wi *)o->d;
    wlserver *s = (wlserver *)w->pub.srv;
    if (op == 0) {                                  /* destroy */
        if (s->cb.unmapped && w->pub.mapped) s->cb.unmapped(s->cb.ud, &w->pub);
        w->pub.mapped = 0;
        w->pub.px = NULL;
        obj_free(cl, o);
    }                                               /* grab: ignored */
}

static void req_subcompositor(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    if (op == 0) { obj_free(cl, o); return; }
    if (op == 1) {                                  /* get_subsurface(new, surface, parent) */
        uint32_t id = a_u(cur);
        Obj *s1 = a_o(cur, &vt_surface);
        Obj *s2 = a_o(cur, &vt_surface);
        if (s1 && s2) ((Surf *)s1->d)->parent = (Surf *)s2->d;
        obj_new(cl, id, &vt_subsurface, NULL);
    }
}
static void req_subsurface(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    if (op == 0) { obj_free(cl, o); return; }
    (void)cur;                                      /* geometry: ceiling */
}

static void req_ddm(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    if (op == 0) { obj_free(cl, o); return; }
    if (op == 1) {                                  /* create_data_offer */
        uint32_t id = a_u(cur);
        obj_new(cl, id, &vt_data_offer, NULL);
    } else if (op == 4) {                           /* get_data_device */
        uint32_t id = a_u(cur);
        obj_new(cl, id, &vt_data_device, NULL);
    } else {
        a_o(cur, &vt_seat);
        a_o(cur, &vt_data_offer);
    }
}
static void req_data_offer(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    if (op == 0) { obj_free(cl, o); return; }
    if (op == 1) { char m[64]; a_s(cur, m, sizeof m); (void)a_fd(cur); } /* receive: ceiling */
}
static void req_data_device(Cl *cl, Obj *o, uint32_t op, Cur *cur) {
    if (op == 3) { obj_free(cl, o); return; }
    (void)cur;
}

static const ObjVt vt_display = { "wl_display", req_display };
static const ObjVt vt_registry = { "wl_registry", req_registry };
static const ObjVt vt_callback = { "wl_callback", NULL };
static const ObjVt vt_compositor = { "wl_compositor", req_compositor };
static const ObjVt vt_surface = { "wl_surface", req_surface };
static const ObjVt vt_region = { "wl_region", req_region };
static const ObjVt vt_shm = { "wl_shm", req_shm };
static const ObjVt vt_pool = { "wl_shm_pool", req_pool };
static const ObjVt vt_buffer = { "wl_buffer", req_buffer };
static const ObjVt vt_seat = { "wl_seat", req_seat };
static const ObjVt vt_pointer = { "wl_pointer", req_pointer };
static const ObjVt vt_keyboard = { "wl_keyboard", req_keyboard };
static const ObjVt vt_output = { "wl_output", req_output };
static const ObjVt vt_wm_base = { "xdg_wm_base", req_wm_base };
static const ObjVt vt_xdg_surface = { "xdg_surface", req_xdg_surface };
static const ObjVt vt_xdg_toplevel = { "xdg_toplevel", req_xdg_toplevel };
static const ObjVt vt_xdg_positioner = { "xdg_positioner", req_xdg_positioner };
static const ObjVt vt_xdg_popup = { "xdg_popup", req_xdg_popup };
static const ObjVt vt_subcompositor = { "wl_subcompositor", req_subcompositor };
static const ObjVt vt_subsurface = { "wl_subsurface", req_subsurface };
static const ObjVt vt_ddm = { "wl_data_device_manager", req_ddm };
static const ObjVt vt_data_offer = { "wl_data_offer", req_data_offer };
static const ObjVt vt_data_device = { "wl_data_device", req_data_device };
static const ObjVt vt_dmabuf = { "zwp_linux_dmabuf_v1", req_dmabuf };
static const ObjVt vt_dmabuf_params = { "zwp_linux_buffer_params_v1", req_dmabuf_params };

/* ---------------- fd plumbing ---------------- */
static int a_fd(Cur *c) {
    Cl *cl = c->cl;
    if (cl->nfd <= 0) return -1;
    int fd = cl->fds[0];
    memmove(cl->fds, cl->fds + 1, (size_t)(cl->nfd - 1) * sizeof(int));
    cl->nfd--;
    return fd;
}

static void cl_flush(Cl *cl) {
    while (cl->wlen > cl->wr_pos) {
        ssize_t n = send(cl->fd, cl->wbuf + cl->wr_pos, cl->wlen - cl->wr_pos, MSG_NOSIGNAL);
        if (n > 0) { cl->wr_pos += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        return;                                     /* broken; pump will drop */
    }
    cl->wlen = 0; cl->wr_pos = 0;
}

static void cl_free(wlserver *s, Cl *cl) {
    wlog("wl: client gone (fd %d)", cl->fd);
    for (int i = 0; i < cl->objs_cap; i++) {
        Obj *o = cl->objs[i];
        if (!o) continue;
        if (o->vt == &vt_surface) {
            Surf *sf = (Surf *)o->d;
            if (sf->win) {
                if (s->cb.unmapped && sf->win->pub.mapped)
                    s->cb.unmapped(s->cb.ud, &sf->win->pub);
                sf->win->pub.mapped = 0;
                sf->win = NULL;
            }
        }
        free(o);
        cl->objs[i] = NULL;
    }
    /* free Wi owned by this client */
    Wi **pp = &s->wins;
    while (*pp) {
        Wi *w = *pp;
        if (w->owner == cl) {
            if (s->kb_focus == w) s->kb_focus = NULL;
            if (s->ptr_surf && s->ptr_surf->cl == cl) s->ptr_surf = NULL;
            *pp = w->next;
            free(w);
        } else pp = &w->next;
    }
    for (int i = 0; i < cl->nfd; i++) close(cl->fds[i]);
    close(cl->fd);
    free(cl->objs); free(cl->fds); free(cl->wbuf);
    free(cl);
}

/* ---------------- wire read ---------------- */
static int cl_read(wlserver *s, Cl *cl) {
    for (;;) {
        struct msghdr mh = { 0 };
        struct iovec iov = { .iov_base = cl->rbuf + cl->rlen, .iov_len = sizeof cl->rbuf - cl->rlen };
        char cbuf[CMSG_SPACE(16 * sizeof(int))];
        mh.msg_iov = &iov; mh.msg_iovlen = 1;
        mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
        ssize_t n = recvmsg(cl->fd, &mh, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return 0;                               /* dead */
        }
        if (n == 0) return 0;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
            int nf = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            if (nf <= 0) continue;
            int *nfds = realloc(cl->fds, (size_t)(cl->nfd + nf) * sizeof(int));
            if (!nfds) break;
            cl->fds = nfds;
            memcpy(cl->fds + cl->nfd, CMSG_DATA(cm), (size_t)nf * sizeof(int));
            cl->nfd += nf;
        }
        cl->rlen += (size_t)n;
        if (cl->rlen >= sizeof cl->rbuf) break;
    }
    /* parse complete messages */
    size_t off = 0;
    while (off + 8 <= cl->rlen) {
        uint32_t h0, h1;
        memcpy(&h0, cl->rbuf + off, 4);
        memcpy(&h1, cl->rbuf + off + 4, 4);
        uint32_t obj = h0, size = h1 >> 16, op = h1 & 0xffff;
        if (size < 8 || size > 65535) { wlog("wl: bad message size %u from fd %d", size, cl->fd); return 0; }
        if (off + size > cl->rlen) break;
        Obj *o = (obj && (int)obj < cl->objs_cap) ? cl->objs[obj] : NULL;
        if (!o) {
            if (obj == 1) {
                static Obj disp;
                disp.id = 1; disp.vt = &vt_display; disp.d = s;
                o = &disp;
            } else {
                wlog("wl: request to unknown object %u (op %u) from fd %d", obj, op, cl->fd);
                return 0;
            }
        }
        Cur cur = { .p = cl->rbuf + off + 8, .end = cl->rbuf + off + size, .cl = cl };
        if (o->vt && o->vt->req) o->vt->req(cl, o, op, &cur);
        off += size;
    }
    if (off) { memmove(cl->rbuf, cl->rbuf + off, cl->rlen - off); cl->rlen -= off; }
    return 1;
}

/* ---------------- server API ---------------- */
wlserver *wlserver_create(const char *sockname) {
    wlserver *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (sockname && sockname[0] == '/')
        snprintf(s->sock, sizeof s->sock, "%s", sockname);
    else
        snprintf(s->sock, sizeof s->sock, "/run/%s", sockname ? sockname : "losedoze0");
    unlink(s->sock);
    s->lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s->lfd < 0) { free(s); return NULL; }
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", s->sock);
    if (bind(s->lfd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(s->lfd, 8) < 0) {
        close(s->lfd); free(s); return NULL;
    }
    chmod(s->sock, 0666);
    wlog("wl: listening on %s", s->sock);
    return s;
}

int wlserver_fd(wlserver *s) { return s->lfd; }
const char *wlserver_socket_path(wlserver *s) { return strrchr(s->sock, '/') + 1; }
void wlserver_set_cbs(wlserver *s, const struct wlserver_cbs *cb) { s->cb = *cb; }

void wlserver_pump(wlserver *s) {
    for (;;) {                                      /* accept */
        int fd = accept4(s->lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) break;
        wlog("wl: ACCEPT fd");
        Cl *cl = calloc(1, sizeof *cl);
        if (!cl) { close(fd); continue; }
        cl->fd = fd;
        cl->objs_cap = 8192;
        cl->objs = calloc((size_t)cl->objs_cap, sizeof *cl->objs);
        obj_new(cl, 1, &vt_display, s);
        cl->next = s->cls;
        s->cls = cl;
        wlog("wl: client connected (fd %d)", fd);
    }
    Cl *prev = NULL, *cl = s->cls;                  /* read + flush, unlink dead */
    while (cl) {
        Cl *next = cl->next;
        if (!cl_read(s, cl)) {
            if (prev) prev->next = next; else s->cls = next;
            cl_free(s, cl);
        } else prev = cl;
        cl = next;
    }
}

void wlserver_destroy(wlserver *s) {
    while (s->cls) { Cl *c = s->cls; s->cls = c->next; cl_free(s, c); }
    while (s->wins) { Wi *w = s->wins; s->wins = w->next; free(w); }
    if (s->lfd >= 0) close(s->lfd);
    unlink(s->sock);
    free(s);
}

WlWin *wlserver_wins(wlserver *s) { return s->wins ? &s->wins->pub : NULL; }
WlWin *wlwin_next(WlWin *w) { Wi *i = (Wi *)w; return i->next ? &i->next->pub : NULL; }
void wlwin_size(WlWin *w, int *bw, int *bh) { if (bw) *bw = w->bw; if (bh) *bh = w->bh; }
const char *wlwin_title(WlWin *w) { return w->title; }
const char *wlwin_appid(WlWin *w) { return w->appid; }
const u32 *wlwin_pixels(WlWin *w, int *stride4, int *bw, int *bh) {
    if (stride4) *stride4 = w->stride4;
    if (bw) *bw = w->bw;
    if (bh) *bh = w->bh;
    return w->mapped ? w->px : NULL;
}

void wlwin_configure(WlWin *w, int cw, int ch) {
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    w->cw = cw; w->ch = ch;              /* requested size sticks */
    xdg_send_configure((Wi *)w, (uint32_t)cw, (uint32_t)ch, w->activated);
}
void wlwin_set_pos(WlWin *w, int x, int y) { w->x = x; w->y = y; }
void wlwin_raise(WlWin *w) { win_to_top((Wi *)w); }
void wlwin_set_activated(WlWin *w, int on) {
    Wi *i = (Wi *)w;
    wlserver *s = (wlserver *)w->srv;
    w->activated = on;
    /* activate configure: keep the size the client already uses */
    uint32_t cw = w->cw > 0 ? (uint32_t)w->cw : (w->bw > 0 ? (uint32_t)w->bw : 1);
    uint32_t ch = w->ch > 0 ? (uint32_t)w->ch : (w->bh > 0 ? (uint32_t)w->bh : 1);
    xdg_send_configure(i, cw, ch, on);
    kb_focus_set(s, on ? i : NULL);
}
void wlwin_close(WlWin *w) {
    Wi *i = (Wi *)w;
    if (i->toplevel) {
        EV(i->s->cl, i->toplevel->id, 0, { });       /* xdg_toplevel.close */
        cl_flush(i->s->cl);
    }
}

void wlserver_ack_paint(wlserver *s, WlWin *win) {
    Wi *w = (Wi *)win;
    Surf *sf = w->s;
    if (!sf) return;
    if (sf->cur_buf && !sf->cur_buf->released) {
        EV(sf->cl, sf->cur_buf->obj_id, 0, { });     /* wl_buffer.release */
        sf->cur_buf->released = 1;
    }
    while (sf->cur_fc) {                            /* frame.done for painted frames */
        Fc *f = sf->cur_fc;
        sf->cur_fc = f->next;
        Cl *cl = sf->cl;
        if (cl && (int)f->id < cl->objs_cap && cl->objs[f->id]) {
            EV(cl, f->id, 0, { bu32(&_b, ++s->serial); });
            obj_free(cl, cl->objs[f->id]);
        }
        free(f);
    }
    cl_flush(sf->cl);
}

WlWin *wlserver_pick(wlserver *s, int sx, int sy, int *lx, int *ly) {
    for (Wi *w = s->wins; w; w = w->next) {
        if (!w->pub.mapped || !w->s || !w->s->cur_buf) continue;
        if (sx >= w->pub.x && sy >= w->pub.y &&
            sx < w->pub.x + w->pub.bw && sy < w->pub.y + w->pub.bh) {
            if (lx) *lx = sx - w->pub.x;
            if (ly) *ly = sy - w->pub.y;
            return &w->pub;
        }
    }
    return NULL;
}

static void ptr_focus_set(wlserver *s, Surf *sf, int lx, int ly) {
    if (s->ptr_surf == sf) return;
    if (s->ptr_surf && s->ptr_surf->cl && s->ptr_surf->o) {
        Cl *cl = s->ptr_surf->cl;
        EV(cl, s->ptr_surf->o->id, 1, { bu32(&_b, ++s->serial); bobj(&_b, s->ptr_surf->o->id); });
        EV(cl, s->ptr_surf->o->id, 5, { });
        cl_flush(cl);
    }
    s->ptr_surf = sf;
    if (sf && sf->cl && sf->o) {
        Cl *cl = sf->cl;
        EV(cl, sf->o->id, 0, {
            bu32(&_b, ++s->serial);
            bobj(&_b, sf->o->id);
            bfix(&_b, lx << 8); bfix(&_b, ly << 8);
        });
        EV(cl, sf->o->id, 5, { });
        cl_flush(cl);
    }
}

void wlserver_pointer_motion(wlserver *s, int sx, int sy) {
    int lx = 0, ly = 0;
    WlWin *w = wlserver_pick(s, sx, sy, &lx, &ly);
    Surf *sf = w ? ((Wi *)w)->s : NULL;
    if (!sf) { ptr_focus_set(s, NULL, 0, 0); return; }
    if (sf != s->ptr_surf) ptr_focus_set(s, sf, lx, ly);
    Cl *cl = sf->cl;
    EV(cl, sf->o->id, 2, { bu32(&_b, now_ms_t()); bfix(&_b, lx << 8); bfix(&_b, ly << 8); });
    EV(cl, sf->o->id, 5, { });
    cl_flush(cl);
}

void wlserver_pointer_button(wlserver *s, int button, int pressed) {
    Surf *sf = s->ptr_surf;
    static const uint32_t codes[4] = { 0, 0x110, 0x112, 0x111 };
    if (!sf || button < 1 || button > 3) return;
    Cl *cl = sf->cl;
    EV(cl, sf->o->id, 3, {
        bu32(&_b, ++s->serial); bu32(&_b, now_ms_t());
        bu32(&_b, codes[button]); bu32(&_b, pressed ? 1 : 0);
    });
    EV(cl, sf->o->id, 5, { });
    cl_flush(cl);
}

void wlserver_pointer_axis(wlserver *s, int dir) {
    Surf *sf = s->ptr_surf;
    if (!sf) return;
    Cl *cl = sf->cl;
    EV(cl, sf->o->id, 4, { bu32(&_b, now_ms_t()); bu32(&_b, 0); bfix(&_b, dir > 0 ? -(10 << 8) : (10 << 8)); });
    EV(cl, sf->o->id, 5, { });
    cl_flush(cl);
}

void wlserver_key(wlserver *s, int code, int pressed, unsigned mods) {
    Wi *w = s->kb_focus;
    if (!w || !w->s || !w->s->cl || !w->s->o) return;
    if (code < 0 || code > 767) return;
    Cl *cl = w->s->cl;
    EV(cl, w->s->o->id, 3, {
        bu32(&_b, ++s->serial); bu32(&_b, now_ms_t());
        bu32(&_b, (uint32_t)code); bu32(&_b, pressed ? 1 : 0);
    });
    if (mods != cl->last_mods) {
        cl->last_mods = mods;
        EV(cl, w->s->o->id, 4, {
            bu32(&_b, ++s->serial); bu32(&_b, mods);
            bu32(&_b, 0); bu32(&_b, 0); bu32(&_b, 0);
        });
    }
    cl_flush(cl);
}
