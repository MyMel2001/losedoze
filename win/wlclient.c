/* wlclient.c — hand-rolled Wayland client for the homemade Win32 subsystem.
 * Connects to loseshell's compositor (wl.c): registry, compositor, shm double
 * buffers, xdg-shell toplevels, seat keyboard/pointer. Raw wire protocol, no
 * libwayland-client. Original code from the public protocol definitions. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

#include "host.h"

/* object types (client-side registry of ids we know) */
enum { T_NONE, T_DISPLAY, T_REGISTRY, T_COMPOSITOR, T_SURFACE, T_SHM, T_POOL,
       T_BUFFER, T_SEAT, T_POINTER, T_KEYBOARD, T_WM_BASE, T_XDG_SURFACE,
       T_XDG_TOPLEVEL, T_CALLBACK };

#define MAXOBJ 4096
static W32 obj_type[MAXOBJ];
static W32 next_id = 2;
static int sfd = -1;
static struct WlcCbs cbs;

static uint8_t rbuf[131072]; static size_t rlen;
static int rfds[32]; static int nrfds;
static uint8_t *wbuf; static size_t wlen, wcap;

typedef struct WlcBuf { W32 id; int w, h, stride; W32 *px; int released; } WlcBuf;
struct WlcWin {
  W32 surf, xdg_s, toplevel;
  WlcBuf bufs[2];
  int pool_id;
  void *pool_map;
  size_t pool_len;
  int w, h;
  int configured, got_configure, alive;
  struct WlcWin *next;
};
static WlcWin *wins;

/* ---------------- wire write ---------------- */
static void put32(size_t off, W32 v) { memcpy(wbuf + off, &v, 4); }
static W32 need(size_t n) {
  if (wlen + n > wcap) {
    size_t nc = wcap ? wcap * 2 : 8192;
    while (nc < wlen + n) nc *= 2;
    uint8_t *nb = realloc(wbuf, nc);
    if (!nb) exit(1);
    wbuf = nb; wcap = nc;
  }
  return (W32)wlen;
}
/* request builder: header is back-patched with the final size on the next
 * req_begin / flush (args stream out via w_u32/w_str). */
static size_t hpos;
static W32 hop;
static int hpend;
static int wlc_trace;
static void req_begin(W32 obj, W32 op, size_t args) {
  if (hpend) {
    put32(hpos + 4, (W32)((wlen - hpos) << 16) | hop);
    if (wlc_trace)
      fprintf(stderr, "wlclient: # obj=%u op=%u size=%u\n",
              *(W32 *)(wbuf + hpos), hop, (W32)(wlen - hpos));
  }
  if (wlc_trace) fprintf(stderr, "wlclient: -> obj=%u op=%u\n", obj, op);
  hpos = need(8 + args);
  put32(hpos, obj);
  put32(hpos + 4, (W32)((8 + args) << 16) | op);
  wlen = hpos + 8;
  hop = op;
  hpend = 1;
}
static void req_finish(void) {
  if (hpend) {
    put32(hpos + 4, (W32)((wlen - hpos) << 16) | hop);
    hpend = 0;
    if (wlc_trace)
      fprintf(stderr, "wlclient: # obj=%u op=%u size=%u\n",
              *(W32 *)(wbuf + hpos), hop, (W32)(wlen - hpos));
  }
}
static void w_u32(W32 v) { W32 o = need(4); put32(o, v); wlen = o + 4; }
static void w_i32(W32 v) { w_u32(v); }
static void w_str(const char *s) {
  W32 n = s ? (W32)strlen(s) + 1 : 0;
  W32 o = need(4 + ((n + 3) & ~3u));
  put32(o, n);
  if (n) memcpy(wbuf + o + 4, s, n);
  for (W32 i = 0; i < ((n + 3) & ~3u) - n; i++) wbuf[o + 4 + n + i] = 0;
  wlen = o + 4 + ((n + 3) & ~3u);
}
static void flush_w(void) {
  req_finish();
  if (wlc_trace) {
    fprintf(stderr, "wlclient: >>> %zu bytes:", wlen);
    for (size_t i = 0; i < wlen; i++)
      fprintf(stderr, " %02x", wbuf[i]);
    fprintf(stderr, "\n");
  }
  size_t off = 0;
  while (off < wlen) {
    ssize_t n = send(sfd, wbuf + off, wlen - off, MSG_NOSIGNAL);
    if (n > 0) { off += (size_t)n; continue; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    break;
  }
  if (off) { memmove(wbuf, wbuf + off, wlen - off); wlen -= off; }
}
/* request carrying an fd (wl_shm.create_pool) */
static void req_fd(W32 obj, W32 op, W32 a1, W32 a2, int fd) {
  flush_w();
  uint8_t pkt[16];                 /* fd args occupy no stream bytes */
  W32 h0 = obj, h1 = (16u << 16) | op;
  memcpy(pkt, &h0, 4); memcpy(pkt + 4, &h1, 4);
  memcpy(pkt + 8, &a1, 4); memcpy(pkt + 12, &a2, 4);
  struct msghdr mh = { 0 };
  struct iovec iov = { .iov_base = pkt, .iov_len = 16 };
  char cbuf[CMSG_SPACE(sizeof(int))];
  mh.msg_iov = &iov; mh.msg_iovlen = 1;
  mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
  struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
  cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm), &fd, sizeof(int));
  if (wlc_trace) {
    fprintf(stderr, "wlclient: >>> fd-msg16:");
    for (int i = 0; i < 16; i++) fprintf(stderr, " %02x", pkt[i]);
    fprintf(stderr, " fd=%d\n", fd);
  }
  ssize_t sent = sendmsg(sfd, &mh, MSG_NOSIGNAL);
  if (sent != 16)
    fprintf(stderr, "wlclient: sendmsg fd: ret=%zd err=%s\n", sent, strerror(errno));
}

/* ---------------- arg readers ---------------- */
typedef struct { const uint8_t *p, *end; } Rd;
static W32 r_u32(Rd *r) { W32 v = 0; if (r->p + 4 <= r->end) { memcpy(&v, r->p, 4); r->p += 4; } return v; }
static const char *r_str(Rd *r, char *buf, size_t cap) {
  W32 n = r_u32(r);
  if (!n || r->p + n > r->end) { buf[0] = 0; return buf; }
  size_t k = n - 1 < cap - 1 ? n - 1 : cap - 1;
  memcpy(buf, r->p, k); buf[k] = 0;
  r->p += (n + 3) & ~3u;
  return buf;
}
static int r_fd(void) {
  if (nrfds <= 0) return -1;
  int fd = rfds[0];
  memmove(rfds, rfds + 1, (size_t)(nrfds - 1) * sizeof(int));
  nrfds--;
  return fd;
}
static void set_type(W32 id, W32 t) { if (id < MAXOBJ) obj_type[id] = t; }
static W32 get_type(W32 id) { return id < MAXOBJ ? obj_type[id] : T_NONE; }

/* ---------------- window/buffer ops ---------------- */
static W32 comp_id, shm_id, seat_id, wmbase_id;

static void buf_free(WlcBuf *b) {
  if (!b->id) return;
  req_begin(b->id, 0, 0);          /* wl_buffer.destroy */
  b->id = 0; b->px = NULL;
}

void wlc_win_surface(WlcWin *w, int cw, int ch) {
  if (cw < 1) cw = 1;
  if (ch < 1) ch = 1;
  if (!w->configured) return;      /* wait for toplevel configure first */
  if (w->bufs[0].id && w->w == cw && w->h == ch) return;
  /* drop old buffers + pool (server frees them; ponytail: no pool reuse) */
  for (int i = 0; i < 2; i++) buf_free(&w->bufs[i]);
  if (w->pool_id) { req_begin(w->pool_id, 0, 0); w->pool_id = 0; } /* pool.destroy */
  if (w->pool_map && w->pool_map != MAP_FAILED) {
    munmap(w->pool_map, w->pool_len);
    w->pool_map = NULL;
  }
  w->w = cw; w->h = ch;
  size_t stride = (size_t)cw * 4, total = stride * (size_t)ch * 2;
  int pfd = memfd_create("w32-buf", 0);
  if (pfd < 0) return;
  if (ftruncate(pfd, (off_t)total) < 0) { close(pfd); return; }
  W32 *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, pfd, 0);
  if (map == MAP_FAILED) { close(pfd); return; }
  w->pool_map = map;
  w->pool_len = total;
  w->pool_id = next_id++;
  set_type(w->pool_id, T_POOL);
  if (wlc_trace)
    fprintf(stderr, "wlclient: pool cw=%d ch=%d stride=%zu total=%zu\n",
            cw, ch, stride, total);
  req_fd(shm_id, 0 /*wl_shm.create_pool(new, fd, size)*/, w->pool_id, (W32)total,
         pfd);
  close(pfd);
  if (getenv("WLC_POOLONLY")) { w->configured = 2; flush_w(); return; }
  for (int i = 0; i < 2; i++) {
    WlcBuf *b = &w->bufs[i];
    b->id = next_id++;
    b->w = cw; b->h = ch; b->stride = (int)stride;
    b->px = map + (stride * (size_t)i) / 4;
    set_type(b->id, T_BUFFER);
    req_begin(w->pool_id, 1, 20);   /* pool.create_buffer */
    w_u32(b->id);
    w_i32((W32)(stride * (size_t)i));
    w_i32(cw); w_i32(ch); w_i32((W32)stride);
    w_u32(0);                       /* WL_SHM_FORMAT_XRGB8888 (opaque) */
    b->released = 1;
  }
  w->configured = 1;
  flush_w();
}

W32 *wlc_win_pixels(WlcWin *w, int *stride4) {
  for (int i = 0; i < 2; i++)
    if (w->bufs[i].id && w->bufs[i].released) {
      if (stride4) *stride4 = w->bufs[i].stride / 4;
      return w->bufs[i].px;
    }
  return NULL;
}

void wlc_win_commit(WlcWin *w) {
  if (!w->got_configure) {
    /* xdg-shell: first commit must precede any buffer attach; wait for the
     * compositor's initial configure + our ack, then attach on next paint. */
    req_begin(w->surf, 6, 0);
    flush_w();
    return;
  }
  if (!w->bufs[0].id && w->w > 0) /* pool once configure gave us a size */
    wlc_win_surface(w, w->w, w->h);
  WlcBuf *b = NULL;
  for (int i = 0; i < 2; i++)
    if (w->bufs[i].id && w->bufs[i].px && w->bufs[i].released) { b = &w->bufs[i]; break; }
  if (!b) return;
  b->released = 0;
  if (wlc_trace)
    fprintf(stderr, "wlclient: attach buf=%u surf=%u %dx%d\n",
            b->id, w->surf, w->w, w->h);
  req_begin(w->surf, 1, 8);         /* wl_surface.attach(buffer, 0, 0) */
  w_u32(b->id); w_i32(0); w_i32(0);
  req_begin(w->surf, 2, 16);        /* damage(0,0,w,h) */
  w_i32(0); w_i32(0); w_i32(w->w); w_i32(w->h);
  req_begin(w->surf, 6, 0);         /* commit */
  flush_w();
}

int wlc_win_buf_busy(WlcWin *w) {
  if (!w->bufs[0].id) return 0;
  if (!w->bufs[0].released) return 1;
  if (w->bufs[1].id && !w->bufs[1].released) return 1;
  return 0;
}

/* wait for registry globals (compositor/shm/wm_base) before first requests */
static int wait_bound(int timeout_ms) {
  while ((!comp_id || !wmbase_id || !shm_id) && timeout_ms > 0) {
    struct pollfd p = { .fd = sfd, .events = POLLIN };
    poll(&p, 1, 50);
    timeout_ms -= 50;
    wlc_flush();
    wlc_dispatch();
  }
  return comp_id && wmbase_id && shm_id;
}

WlcWin *wlc_toplevel(const char *title, const char *appid) {
  if (!wait_bound(3000)) {
    fprintf(stderr, "wlclient: compositor globals missing\n");
    return NULL;
  }
  WlcWin *w = calloc(1, sizeof *w);
  if (!w) return NULL;
  w->alive = 1;
  w->surf = next_id++;
  set_type(w->surf, T_SURFACE);
  req_begin(comp_id, 0, 4);         /* compositor.create_surface */
  w_u32(w->surf);
  w->xdg_s = next_id++;
  set_type(w->xdg_s, T_XDG_SURFACE);
  req_begin(wmbase_id, 2, 8);       /* get_xdg_surface(new, surface) */
  w_u32(w->xdg_s); w_u32(w->surf);
  w->toplevel = next_id++;
  set_type(w->toplevel, T_XDG_TOPLEVEL);
  req_begin(w->xdg_s, 1, 4);        /* get_toplevel */
  w_u32(w->toplevel);
  req_begin(w->toplevel, 2, 0);     /* set_title */
  w_str(title);
  req_begin(w->toplevel, 3, 0);     /* set_app_id */
  w_str(appid);
  w->next = wins; wins = w;
  flush_w();
  return w;
}

void wlc_win_destroy(WlcWin *w) { w->alive = 0; }
int wlc_win_alive(WlcWin *w) { return w && w->alive; }
int wlc_win_configured(WlcWin *w) { return w->configured; }

/* ---------------- event parsing ---------------- */
static unsigned kb_mods;
static int ptr_x, ptr_y;

static void on_keymap(Rd *r) {
  W32 fmt = r_u32(r);
  int fd = r_fd();
  W32 len = r_u32(r);
  (void)fmt; (void)len;
  if (fd >= 0) close(fd);           /* server xkb string unused: raw evdev codes */
}

static void kbd_key(W32 serial, W32 time, W32 code, W32 st) {
  (void)serial; (void)time;
  if (cbs.key) cbs.key(cbs.ud, (int)code, (int)st, kb_mods);
}

/* pending toplevel id for the configure currently being parsed */
static W32 cur_obj;

static void handle_registry(W32 op, Rd *r) {
  if (op == 0) {                    /* global(name, interface, version) */
    W32 name = r_u32(r);
    char ifn[64];
    r_str(r, ifn, sizeof ifn);
    W32 ver = r_u32(r);
    if (!strcmp(ifn, "wl_compositor")) {
      W32 id = next_id++;
      set_type(id, T_COMPOSITOR);
      comp_id = id;
      req_begin(2 /*registry*/, 0, 0);
      w_u32(name); w_str(ifn); w_u32(ver < 4 ? ver : 4); w_u32(id);
    } else if (!strcmp(ifn, "wl_shm")) {
      W32 id = next_id++;
      set_type(id, T_SHM);
      shm_id = id;
      req_begin(2, 0, 0);
      w_u32(name); w_str(ifn); w_u32(ver < 6 ? ver : 6); w_u32(id);
    } else if (!strcmp(ifn, "wl_seat")) {
      W32 id = next_id++;
      set_type(id, T_SEAT);
      seat_id = id;
      req_begin(2, 0, 0);
      w_u32(name); w_str(ifn); w_u32(ver < 5 ? ver : 5); w_u32(id);
      W32 pk = next_id++, pp = next_id++;
      set_type(pk, T_KEYBOARD);
      set_type(pp, T_POINTER);
      req_begin(id, 1, 4); w_u32(pk);   /* get_keyboard */
      req_begin(id, 0, 4); w_u32(pp);   /* get_pointer */
    } else if (!strcmp(ifn, "xdg_wm_base")) {
      W32 id = next_id++;
      set_type(id, T_WM_BASE);
      wmbase_id = id;
      req_begin(2, 0, 0);
      w_u32(name); w_str(ifn); w_u32(1); w_u32(id);
    }
    flush_w();
  }
}

static void dispatch(W32 obj, W32 op, Rd *r) {
  W32 t = get_type(obj);
  switch (t) {
  case T_DISPLAY:
    if (wlc_trace && op == 0) {
      size_t eoff = (size_t)(r->p - rbuf) - 8;
      fprintf(stderr, "wlclient: err-hdr bytes:");
      for (size_t i = 0; i < 8 && eoff + i < rlen; i++)
        fprintf(stderr, " %02x", rbuf[eoff + i]);
      fprintf(stderr, "\n");
    }
    if (op == 1) {                   /* delete_id(id) */
      fprintf(stderr, "wlclient: delete_id %u\n", r_u32(r));
      break;
    }
    if (op == 0) {                   /* error(object_id, code, message) */
      W32 objid = r_u32(r), code = r_u32(r);
      char msg[256];
      r_str(r, msg, sizeof msg);
      fprintf(stderr, "wlclient: DISPLAY ERROR obj=%u code=%u: %s\n",
              objid, code, msg);
    }
    break;                           /* delete_id: ignore */
  case T_REGISTRY:
    handle_registry(op, r);
    break;
  case T_WM_BASE:
    if (op == 0) {                   /* ping(serial) */
      req_begin(obj, 3, 4);
      w_u32(r_u32(r));
      flush_w();
    }
    break;
  case T_SEAT:
    break;                           /* capabilities/name ignored */
  case T_KEYBOARD:
    if (op == 0) on_keymap(r);       /* keymap(format, fd, size) */
    else if (op == 3) {              /* key(serial, time, key, state) */
      W32 serial = r_u32(r); r_u32(r);
      W32 code = r_u32(r), st = r_u32(r);
      kbd_key(serial, 0, code, st);
    } else if (op == 1) {            /* enter: serial, surface, keys[] */
      r_u32(r); r_u32(r);
      W32 n = r_u32(r);
      r->p += ((n + 3) & ~3u);
    } else if (op == 4) {            /* modifiers(serial, depressed, latched, locked, group) */
      r_u32(r);
      kb_mods = r_u32(r);
    }
    break;
  case T_POINTER:
    if (op == 2) {                   /* motion(time, sx, sy) */
      r_u32(r);
      ptr_x = (int)(r_u32(r) >> 8);
      ptr_y = (int)(r_u32(r) >> 8);
      if (cbs.motion) cbs.motion(cbs.ud, ptr_x, ptr_y);
    } else if (op == 3) {            /* button(serial, time, button, state) */
      r_u32(r); r_u32(r);
      W32 btn = r_u32(r), st = r_u32(r);
      int b = btn == 0x110 ? 1 : btn == 0x111 ? 2 : btn == 0x112 ? 3 : 0;
      if (b && cbs.button) cbs.button(cbs.ud, b, (int)st, ptr_x, ptr_y);
    }
    break;
  case T_XDG_SURFACE:
    if (op == 0) {                   /* configure(serial) */
      W32 serial = r_u32(r);
      req_begin(obj, 4, 4);          /* ack_configure */
      w_u32(serial);
      flush_w();
      for (WlcWin *w = wins; w; w = w->next)
        if (w->xdg_s == obj) w->got_configure = 1;
    }
    break;
  case T_XDG_TOPLEVEL:
    if (op == 0) {                   /* configure(w, h, states[]) */
      W32 cw = r_u32(r), ch = r_u32(r), nst = r_u32(r);
      r->p += ((nst + 3) & ~3u);
      for (WlcWin *w = wins; w; w = w->next)
        if (w->toplevel == cur_obj) {
          w->configured = 1;
          if (cbs.configure) cbs.configure(cbs.ud, w, (int)cw, (int)ch);
          break;
        }
    } else if (op == 1) {            /* close */
      for (WlcWin *w = wins; w; w = w->next)
        if (w->toplevel == cur_obj && cbs.close_req) cbs.close_req(cbs.ud, w);
    }
    break;
  case T_BUFFER:
    if (op == 0)                    /* release */
      for (WlcWin *w = wins; w; w = w->next)
        for (int i = 0; i < 2; i++)
          if (w->bufs[i].id == cur_obj) {
            w->bufs[i].released = 1;
            if (cbs.buf_release) cbs.buf_release(cbs.ud, w);
          }
    break;
  default:
    break;
  }
}

void wlc_dispatch(void) {
  for (;;) {
    struct msghdr mh = { 0 };
    struct iovec iov = { .iov_base = rbuf + rlen, .iov_len = sizeof rbuf - rlen };
    char cbuf[CMSG_SPACE(16 * sizeof(int))];
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
    ssize_t n = recvmsg(sfd, &mh, MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      break;                         /* dead */
    }
    if (n == 0) break;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
      if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
      int nf = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      for (int i = 0; i < nf && nrfds < 32; i++)
        rfds[nrfds++] = ((int *)CMSG_DATA(cm))[i];
    }
    rlen += (size_t)n;
    if (rlen >= sizeof rbuf) break;
  }
  size_t off = 0;
  while (off + 8 <= rlen) {
    W32 h0, h1;
    memcpy(&h0, rbuf + off, 4);
    memcpy(&h1, rbuf + off + 4, 4);
    W32 obj = h0, size = h1 >> 16, op = h1 & 0xffff;
    if (size < 8 || size > 65535) { rlen = 0; return; }
    if (off + size > rlen) break;
    Rd r = { .p = rbuf + off + 8, .end = rbuf + off + size };
    if (wlc_trace)
      fprintf(stderr, "wlclient: <- obj=%u op=%u size=%u\n", obj, op, size);
    cur_obj = obj;
    dispatch(obj, op, &r);
    off += size;
  }
  if (off) { memmove(rbuf, rbuf + off, rlen - off); rlen -= off; }
}

/* ---------------- connect ---------------- */
static void registry_ready(void) {
  req_begin(1 /*display*/, 1, 4);    /* get_registry */
  W32 id = next_id++;
  set_type(id, T_REGISTRY);
  w_u32(id);
  flush_w();
}

int wlc_connect(void) {
  const char *disp = getenv("WAYLAND_DISPLAY");
  const char *rtd = getenv("XDG_RUNTIME_DIR");
  char path[512];
  if (!disp) disp = "losedoze0";
  if (disp[0] == '/') snprintf(path, sizeof path, "%s", disp);
  else snprintf(path, sizeof path, "%s/%s", rtd ? rtd : "/run", disp);
  sfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sfd < 0) return -1;
  struct sockaddr_un sa = { .sun_family = AF_UNIX };
  snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
  if (connect(sfd, (struct sockaddr *)&sa, sizeof sa) < 0) {
    fprintf(stderr, "wlclient: connect %s: %s\n", path, strerror(errno));
    close(sfd); sfd = -1;
    return -1;
  }
  set_type(1, T_DISPLAY);
  wlc_trace = getenv("WLC_TRACE") != NULL;
  registry_ready();
  return 0;
}

int wlc_fd(void) { return sfd; }
int wlc_have_display(void) { return sfd >= 0; }
void wlc_set_cbs(const struct WlcCbs *cb) { cbs = *cb; }
void wlc_flush(void) { flush_w(); }
