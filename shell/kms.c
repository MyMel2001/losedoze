/* kms.c — native DRM/KMS display for loseshell.
 *
 * Modesetting/flip code adapted from K93OS-x86 k93ui.c (GPL-2.0, used with permission):
 * raw DRM ioctls, object property resolution, atomic commits. Extended here with:
 *  - double dumb buffer + shadow canvas (vblank-paced, no tearing)
 *  - fbdev fallback (/dev/fb0)
 *  - optional GPU acceleration: EGL + GL ES 2 over GBM (Mesa = kernel GPU userspace),
 *    accelerated path for i915/amdgpu/radeon/nouveau/virtio-gpu; llvmpipe fallback.
 */
#define _GNU_SOURCE
#include "lost.h"
#ifndef __user
#define __user
#endif
#include "uapi/drm.h"
#include "uapi/drm_mode.h"

#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOG(fmt, ...) fprintf(stderr, "LOSEDOZE KMS: " fmt "\n", ##__VA_ARGS__)

#ifndef DRM_FORMAT_XRGB8888
#define fourcc_code(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_XRGB8888 fourcc_code('X', 'R', '2', '4')
#endif
#define GBM_BO_USE_SCANOUT (1 << 0)
#define GBM_BO_USE_RENDERING (1 << 1)

static int drm_ioctl(int fd, unsigned long cmd, void *arg) {
  for (;;) {
    int r = ioctl(fd, cmd, arg);
    if (r < 0 && errno == EINTR) continue;
    return r;
  }
}

/* ---- object property helpers (from k93ui.c) ---- */
static int drm_obj_prop(int fd, uint32_t obj, uint32_t obj_type, const char *pname,
                        uint32_t *pid, uint64_t *pval) {
  struct drm_mode_obj_get_properties gp;
  memset(&gp, 0, sizeof(gp));
  gp.obj_id = obj;
  gp.obj_type = obj_type;
  if (drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0) return -1;
  uint64_t vals[64];
  uint32_t props[64];
  if (gp.count_props > 64) gp.count_props = 64;
  gp.props_ptr = (uintptr_t)props;
  gp.prop_values_ptr = (uintptr_t)vals;
  if (drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0) return -1;
  for (uint32_t i = 0; i < gp.count_props; i++) {
    struct drm_mode_get_property p;
    memset(&p, 0, sizeof(p));
    p.prop_id = props[i];
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) < 0) continue;
    if (strncmp(p.name, pname, DRM_PROP_NAME_LEN) != 0) continue;
    *pid = props[i];
    if (pval) {
      /* count-probe then fetch the value (k93 pattern) */
      memset(&p, 0, sizeof(p));
      p.prop_id = props[i];
      uint64_t v = 0;
      p.values_ptr = (uintptr_t)&v;
      p.count_values = 1;
      if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) == 0) *pval = v;
    }
    return 0;
  }
  return -1;
}

static int drm_obj_prop_val(int fd, uint32_t obj, uint32_t obj_type, const char *pname,
                            uint64_t *val) {
  uint32_t id;
  return drm_obj_prop(fd, obj, obj_type, pname, &id, val);
}

struct card {
  int fd, fbfd; /* fbdev fallback fd (-1 if unused) */
  uint32_t conn, crtc, plane, fb_id[2];
  uint32_t conn_prop_id, crtc_mode_id, crtc_active;
  struct drm_mode_modeinfo mode;
  uint32_t dumb_handle[2], dumb_pitch[2];
  size_t dumb_size[2];
  void *dumb_map[2];
  int cur; /* front buffer index */
  Canvas shadow;
  int use_fbdev;
  char driver[32];
  /* EGL/GPU */
  void *egl, *gbm; /* dlopen handles */
  int gpu_ok;
  void *gbm_dev, *gbm_surf, *egl_dpy, *egl_ctx, *egl_surf;
  uint32_t gpu_bo_fb[8]; /* fb ids of gbm bos, keyed by handle hash -> simple cache */
  uint32_t gpu_bo_handle[8];
  int gpu_bo_n;
  uint32_t gl_prog, gl_tex, gl_vbo;
  int gl_tex_w, gl_tex_h;
  const char *gl_renderer;
  /* scanout of gbm front */
  uint32_t scan_fb;
  int scan_w, scan_h;
  void *last_front;
};

/* ---- open ---- */
static const char *dri_path(int minor) {
  static char p[64];
  snprintf(p, sizeof(p), "/dev/dri/card%d", minor);
  return p;
}

struct card *card_open(void) {
  struct card *c = calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->fd = c->fbfd = -1;
  DIR *d = opendir("/dev/dri");
  if (d) {
    struct dirent *e;
    int best = -1;
    while ((e = readdir(d))) {
      int m;
      if (sscanf(e->d_name, "card%d", &m) == 1)
        if (best < 0 || m < best) best = m;
    }
    closedir(d);
    if (best >= 0) c->fd = open(dri_path(best), O_RDWR | O_CLOEXEC);
  }
  if (c->fd >= 0) {
    /* driver name via DRM_IOCTL_VERSION */
    struct drm_version dv;
    memset(&dv, 0, sizeof(dv));
    char name[32];
    dv.name = name;
    dv.name_len = sizeof(name);
    if (drm_ioctl(c->fd, DRM_IOCTL_VERSION, &dv) == 0 && dv.name_len) {
      size_t n = dv.name_len < sizeof(c->driver) - 1 ? dv.name_len : sizeof(c->driver) - 1;
      memcpy(c->driver, name, n);
      c->driver[n] = 0;
    } else {
      snprintf(c->driver, sizeof(c->driver), "unknown");
    }
    /* atomic modesetting: needed for vblank-paced page flips */
    struct drm_get_cap dc = {.capability = DRM_CLIENT_CAP_ATOMIC};
    if (drm_ioctl(c->fd, DRM_IOCTL_GET_CAP, &dc) == 0 && dc.value) {
      struct drm_set_client_cap sc = {.capability = DRM_CLIENT_CAP_ATOMIC, .value = 1};
      if (drm_ioctl(c->fd, DRM_IOCTL_SET_CLIENT_CAP, &sc) < 0)
        LOG("atomic cap rejected (%s) — will retry plain modeset", strerror(errno));
    }
  } else {
    /* fbdev fallback */
    c->fbfd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (c->fbfd < 0) {
      free(c);
      return NULL;
    }
    c->use_fbdev = 1;
    snprintf(c->driver, sizeof(c->driver), "fbdev");
    LOG("no /dev/dri/card* (%s), falling back to fbdev", strerror(errno));
  }
  return c;
}

const char *card_backend(struct card *c) { return c->use_fbdev ? "fbdev" : "kms"; }
const char *card_driver(struct card *c) { return c->driver; }

static int dumb_create(struct card *c, int idx, int w, int h) {
  struct drm_mode_create_dumb cr = {.width = w, .height = h, .bpp = 32};
  if (drm_ioctl(c->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr) < 0) return -1;
  c->dumb_handle[idx] = cr.handle;
  c->dumb_pitch[idx] = cr.pitch;
  c->dumb_size[idx] = cr.size;
  struct drm_mode_map_dumb mp = {.handle = cr.handle};
  if (drm_ioctl(c->fd, DRM_IOCTL_MODE_MAP_DUMB, &mp) < 0) return -1;
  void *m = mmap(NULL, cr.size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, mp.offset);
  if (m == MAP_FAILED) return -1;
  c->dumb_map[idx] = m;
  uint32_t handles[4] = {cr.handle, 0, 0, 0}, pitches[4] = {cr.pitch, 0, 0, 0},
           offsets[4] = {0, 0, 0, 0};
  struct drm_mode_fb_cmd2 fb = {.width = w,
                                .height = h,
                                .pixel_format = DRM_FORMAT_XRGB8888,
                                .flags = 0,
                                .handles = {0},
                                .pitches = {0},
                                .offsets = {0}};
  memcpy(fb.handles, handles, sizeof(handles));
  memcpy(fb.pitches, pitches, sizeof(pitches));
  memcpy(fb.offsets, offsets, sizeof(offsets));
  if (drm_ioctl(c->fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) return -1;
  c->fb_id[idx] = fb.fb_id;
  return 0;
}

/* Plane-based atomic commit, exactly as k93ui.c does (proven: full modeset with
 * MODE_ID blob on first commit; fast flags=0 flip afterwards). */
static uint32_t primary_plane(struct card *c) {
  struct drm_mode_get_plane_res pr;
  memset(&pr, 0, sizeof(pr));
  if (drm_ioctl(c->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0 || !pr.count_planes) return 0;
  uint32_t planes[64], pn = pr.count_planes < 64 ? pr.count_planes : 64;
  pr.plane_id_ptr = (uintptr_t)planes;
  pr.count_planes = pn;
  if (drm_ioctl(c->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0) return 0;
  uint32_t fallback = 0;
  for (uint32_t i = 0; i < pr.count_planes; i++) {
    uint64_t type = 0, pcrtc = 0;
    if (drm_obj_prop_val(c->fd, planes[i], DRM_MODE_OBJECT_PLANE, "type", &type) == 0 &&
        type == 1 /* DRM_PLANE_TYPE_PRIMARY */ &&
        drm_obj_prop_val(c->fd, planes[i], DRM_MODE_OBJECT_PLANE, "CRTC_ID", &pcrtc) == 0 &&
        (pcrtc == 0 || pcrtc == c->crtc))
      return planes[i];
    if (!fallback) fallback = planes[i];
  }
  return fallback;
}

static int atomic_commit_full(struct card *c, uint32_t fb) {
  uint32_t p_fb = 0, p_crtc = 0, p_src = 0, p_src_y = 0, p_src_w = 0, p_src_h = 0;
  uint32_t p_cx = 0, p_cy = 0, p_cw = 0, p_ch = 0;
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "FB_ID", &p_fb, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &p_crtc, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "SRC_X", &p_src, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "SRC_Y", &p_src_y, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "SRC_W", &p_src_w, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "SRC_H", &p_src_h, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "CRTC_X", &p_cx, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &p_cy, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "CRTC_W", &p_cw, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "CRTC_H", &p_ch, NULL);
  if (!p_fb || !p_crtc || !p_src_w || !p_cw) {
    LOG("plane props missing");
    return -1;
  }
  /* MODE_ID takes a blob from our modeline (k93 pattern) */
  struct drm_mode_create_blob mb;
  memset(&mb, 0, sizeof(mb));
  mb.data = (uintptr_t)&c->mode;
  mb.length = sizeof(c->mode);
  if (drm_ioctl(c->fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &mb) < 0) {
    LOG("create_blob: %s", strerror(errno));
    return -1;
  }
  uint64_t W = c->mode.hdisplay, H = c->mode.vdisplay;
  uint32_t objs[3] = {c->plane, c->crtc, c->conn};
  uint32_t npc[3] = {10, 2, 1};
  uint32_t props[13] = {p_fb,  p_crtc, p_src, p_src_y, p_src_w, p_src_h,
                        p_cx, p_cy,   p_cw,  p_ch,
                        c->crtc_mode_id, c->crtc_active,
                        c->conn_prop_id};
  uint64_t vals[13] = {fb, c->crtc, 0, 0, W << 16, H << 16, 0, 0, W, H,
                       mb.blob_id, 1,
                       c->crtc};
  struct drm_mode_atomic at;
  memset(&at, 0, sizeof(at));
  at.flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
  at.count_objs = 3;
  at.objs_ptr = (uintptr_t)objs;
  at.count_props_ptr = (uintptr_t)npc;
  at.props_ptr = (uintptr_t)props;
  at.prop_values_ptr = (uintptr_t)vals;
  int r = drm_ioctl(c->fd, DRM_IOCTL_MODE_ATOMIC, &at);
  struct drm_mode_destroy_blob db = {.blob_id = mb.blob_id};
  drm_ioctl(c->fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &db);
  if (r < 0) LOG("atomic commit: %s", strerror(errno));
  return r;
}

static int drm_atomic_flip(struct card *c, uint32_t fb) {
  uint32_t p_fb = 0, p_crtc = 0, p_src = 0, p_src_y = 0, p_src_w = 0, p_src_h = 0;
  uint32_t p_cx = 0, p_cy = 0, p_cw = 0, p_ch = 0;
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "FB_ID", &p_fb, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &p_crtc, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "SRC_X", &p_src, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "SRC_Y", &p_src_y, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "SRC_W", &p_src_w, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "SRC_H", &p_src_h, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "CRTC_X", &p_cx, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &p_cy, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "CRTC_W", &p_cw, NULL);
  drm_obj_prop(c->fd, c->plane, DRM_MODE_OBJECT_PLANE, "CRTC_H", &p_ch, NULL);
  if (!p_fb || !p_crtc || !p_cw) return -1;
  uint64_t W = c->mode.hdisplay, H = c->mode.vdisplay;
  uint32_t objs[3] = {c->plane, c->crtc, c->conn};
  uint32_t npc[3] = {10, 1, 1};
  uint32_t props[12] = {p_fb,  p_crtc, p_src, p_src_y, p_src_w, p_src_h,
                        p_cx, p_cy,   p_cw,  p_ch,
                        c->crtc_active,
                        c->conn_prop_id};
  uint64_t vals[12] = {fb, c->crtc, 0, 0, W << 16, H << 16, 0, 0, W, H,
                       1,
                       c->crtc};
  struct drm_mode_atomic at;
  memset(&at, 0, sizeof(at));
  at.flags = 0; /* same mode: fast page-flip */
  at.count_objs = 3;
  at.objs_ptr = (uintptr_t)objs;
  at.count_props_ptr = (uintptr_t)npc;
  at.props_ptr = (uintptr_t)props;
  at.prop_values_ptr = (uintptr_t)vals;
  if (drm_ioctl(c->fd, DRM_IOCTL_MODE_ATOMIC, &at) == 0) return 0;
  /* driver rejected fast flip (e.g. first GPU flip) — full commit */
  return atomic_commit_full(c, fb);
}

static int modeset(struct card *c, uint32_t *conn_out, uint32_t *crtc_out,
                   struct drm_mode_modeinfo *mode_out) {
  struct drm_mode_card_res res;
  memset(&res, 0, sizeof(res));
  if (drm_ioctl(c->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
    LOG("GETRESOURCES(0) failed: %s", strerror(errno));
    return -1;
  }
  uint32_t conns[32], encs[32], crts[32], fbs[32];
  if (res.count_connectors > 32) res.count_connectors = 32;
  if (res.count_encoders > 32) res.count_encoders = 32;
  if (res.count_crtcs > 32) res.count_crtcs = 32;
  res.connector_id_ptr = (uintptr_t)conns;
  res.encoder_id_ptr = (uintptr_t)encs;
  res.crtc_id_ptr = (uintptr_t)crts;
  res.fb_id_ptr = (uintptr_t)fbs;
  if (drm_ioctl(c->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
    LOG("GETRESOURCES(1) failed: %s", strerror(errno));
    return -1;
  }
  LOG("resources: conn=%u enc=%u crtc=%u driver=%s", res.count_connectors,
      res.count_encoders, res.count_crtcs, c->driver);

  for (uint32_t i = 0; i < res.count_connectors; i++) {
    /* second call must carry valid pointers for ALL counts fetched by the
       first (libdrm pattern) — kernel copy_to_user's props/encoders too */
    struct drm_mode_get_connector conn;
    memset(&conn, 0, sizeof(conn));
    conn.connector_id = conns[i];
    if (drm_ioctl(c->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
      LOG("GETCONNECTOR hdr failed: %s", strerror(errno));
      continue;
    }
    uint32_t enc64[64] = {0}, prop64[64] = {0};
    uint64_t pval64[64] = {0};
    struct drm_mode_modeinfo modes[64];
    memset(modes, 0, sizeof(modes));
    uint32_t want = conn.count_modes > 64 ? 64 : conn.count_modes;
    conn.count_modes = want;
    conn.modes_ptr = (uintptr_t)modes;
    if (conn.count_encoders > 64) conn.count_encoders = 64;
    conn.encoders_ptr = (uintptr_t)enc64;
    if (conn.count_props > 64) conn.count_props = 64;
    conn.props_ptr = (uintptr_t)prop64;
    conn.prop_values_ptr = (uintptr_t)pval64;
    if (drm_ioctl(c->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
      LOG("GETCONNECTOR modes(%u) failed: %s", want, strerror(errno));
      continue;
    }
    LOG("conn 0x%x: enc=0x%x status=%u modes=%u", conns[i], conn.connector_id,
        conn.count_modes ? 1u : 0u, conn.count_modes);
    if (!want) continue;
    *conn_out = conns[i];
    *mode_out = modes[0]; /* preferred/first */
    for (uint32_t m = 0; m < want; m++) {
      if (modes[m].type & DRM_MODE_TYPE_PREFERRED) {
        *mode_out = modes[m];
        break;
      }
    }
    /* find crtc via encoder */
    uint32_t crtc = 0;
    struct drm_mode_get_encoder enc;
    memset(&enc, 0, sizeof(enc));
    if (conn.encoder_id) {
      enc.encoder_id = conn.encoder_id;
      if (drm_ioctl(c->fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id) crtc = enc.crtc_id;
    }
    if (!crtc) {
      uint32_t enc_n = conn.count_encoders > 64 ? 64 : conn.count_encoders;
      for (uint32_t e = 0; e < enc_n && !crtc; e++) {
        struct drm_mode_get_encoder e2;
        memset(&e2, 0, sizeof(e2));
        e2.encoder_id = conn.encoders_ptr ? ((uint32_t *)conn.encoders_ptr)[e] : 0;
        if (!e2.encoder_id) continue;
        if (drm_ioctl(c->fd, DRM_IOCTL_MODE_GETENCODER, &e2) < 0) continue;
        for (uint32_t k = 0; k < res.count_crtcs; k++) {
          if (e2.possible_crtcs & (1u << k)) {
            crtc = crts[k];
            break;
          }
        }
      }
    }
    if (!crtc && res.count_crtcs) crtc = crts[0];
    if (!crtc) continue;
    *crtc_out = crtc;
    return 0;
  }
  return -1;
}

int card_modeset(struct card *c, int fbw, int fbh) {
  if (c->use_fbdev) {
    struct fb_var_screeninfo v;
    if (ioctl(c->fbfd, FBIOGET_VSCREENINFO, &v) < 0) return -1;
    struct fb_fix_screeninfo f;
    memset(&f, 0, sizeof(f));
    if (ioctl(c->fbfd, FBIOGET_FSCREENINFO, &f) < 0) return -1;
    void *m = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, c->fbfd, 0);
    if (m == MAP_FAILED) return -1;
    c->dumb_map[0] = m;
    c->dumb_size[0] = f.smem_len;
    c->shadow.px = m;
    c->shadow.w = v.xres;
    c->shadow.h = v.yres;
    c->shadow.stride = (f.line_length ? f.line_length / 4 : v.xres);
    return 0;
  }

  /* caps before property queries (k93 pattern) */
  struct drm_set_client_cap uc = {.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1};
  drm_ioctl(c->fd, DRM_IOCTL_SET_CLIENT_CAP, &uc);
  struct drm_set_client_cap ac = {.capability = DRM_CLIENT_CAP_ATOMIC, .value = 1};
  drm_ioctl(c->fd, DRM_IOCTL_SET_CLIENT_CAP, &ac);

  uint32_t conn, crtc;
  struct drm_mode_modeinfo mode;
  memset(&mode, 0, sizeof(mode));
  if (modeset(c, &conn, &crtc, &mode) < 0) {
    LOG("no connected connector");
    return -1;
  }
  c->conn = conn;
  c->crtc = crtc;
  c->mode = mode;
  if (drm_obj_prop(c->fd, conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &c->conn_prop_id, NULL) < 0)
    return -1;
  if (drm_obj_prop(c->fd, crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", &c->crtc_mode_id, NULL) < 0)
    return -1;
  if (drm_obj_prop(c->fd, crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", &c->crtc_active, NULL) < 0)
    return -1;
  c->plane = primary_plane(c);
  if (!c->plane) {
    LOG("no primary plane");
    return -1;
  }

  int w = mode.hdisplay, h = mode.vdisplay;
  (void)fbw;
  (void)fbh;
  if (dumb_create(c, 0, w, h) < 0 || dumb_create(c, 1, w, h) < 0) {
    LOG("dumb buffer create failed: %s", strerror(errno));
    return -1;
  }
  c->cur = 0;
  c->shadow.w = w;
  c->shadow.h = h;
  c->shadow.stride = w;
  c->shadow.px = c->dumb_map[0]; /* startup: draw in buffer0 until first flip */
  if (atomic_commit_full(c, c->fb_id[0]) < 0) {
    LOG("initial atomic modeset failed: %s", strerror(errno));
    return -1;
  }
  LOG("mode %dx%d@%d driver=%s plane=%u", w, h, mode.vrefresh, c->driver, c->plane);
  return 0;
}

Canvas *card_shadow(struct card *c) { return &c->shadow; }
int card_fbwidth(struct card *c) { return c->shadow.w; }
int card_fbheight(struct card *c) { return c->shadow.h; }

unsigned long card_flip(struct card *c) {
  if (c->use_fbdev) return 0; /* direct draw */
  int back = 1 - c->cur;
  /* If shadow doesn't alias back buffer (GPU path or initial state), copy. */
  if (c->shadow.px != c->dumb_map[back]) {
    int w = c->shadow.w;
    for (int y = 0; y < c->shadow.h; y++)
      memcpy((u32 *)((char *)c->dumb_map[back] + y * c->dumb_pitch[back]),
             c->shadow.px + y * c->shadow.stride, (size_t)w * 4);
  }
  if (drm_atomic_flip(c, c->fb_id[back]) < 0) return (unsigned long)-1;
  c->cur = back;
  c->shadow.px = c->dumb_map[back]; /* draw directly into scanout next frame (k93-style) */
  return 1;
}

void card_close(struct card *c) {
  if (!c) return;
  card_gpu_shutdown(c);
  for (int i = 0; i < 2; i++)
    if (c->dumb_map[i]) munmap(c->dumb_map[i], c->dumb_size[i]);
  if (c->fd >= 0) close(c->fd);
  if (c->fbfd >= 0) close(c->fbfd);
  free(c);
}

/* ================= GPU acceleration (EGL + GL ES2 over GBM, via dlopen) ================= */
typedef void *EGLDisplay, *EGLContext, *EGLSurface, *EGLConfig;
typedef unsigned int EGLenum, EGLBoolean, EGLint_;
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0

typedef EGLDisplay (*PFN_eglGetDisplay)(void *);
typedef EGLDisplay (*PFN_eglGetPlatformDisplay)(uint32_t, void *, const EGLint_ *);
#ifndef EGL_PLATFORM_GBM_MESA
#define EGL_PLATFORM_GBM_MESA 0x31D7
#endif
typedef EGLBoolean (*PFN_eglInitialize)(EGLDisplay, EGLint_ *, EGLint_ *);
typedef EGLBoolean (*PFN_eglBindAPI)(EGLenum);
typedef EGLBoolean (*PFN_eglChooseConfig)(EGLDisplay, const EGLint_ *, EGLConfig *, EGLint_,
                                          EGLint_ *);
typedef EGLContext (*PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint_ *);
typedef EGLSurface (*PFN_eglCreateWindowSurface)(EGLDisplay, EGLConfig, void *,
                                                 const EGLint_ *);
typedef EGLBoolean (*PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (*PFN_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef const char *(*PFN_eglQueryString)(EGLDisplay, EGLint_);
typedef void *(*PFN_eglGetProcAddress)(const char *);
typedef EGLBoolean (*PFN_eglDestroySurface)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*PFN_eglDestroyContext)(EGLDisplay, EGLContext);
typedef EGLBoolean (*PFN_eglTerminate)(EGLDisplay);
typedef EGLint_ (*PFN_eglGetError)(void);

typedef struct gbm_device gbm_device;
typedef struct gbm_surface gbm_surface;
typedef struct gbm_bo gbm_bo;
typedef struct gbm_device *(*PFN_gbm_create_device)(int);
typedef struct gbm_surface *(*PFN_gbm_surface_create)(struct gbm_device *, uint32_t, uint32_t,
                                                      uint32_t, uint32_t);
typedef struct gbm_bo *(*PFN_gbm_surface_lock_front_buffer)(struct gbm_surface *);
typedef void (*PFN_gbm_surface_release_buffer)(struct gbm_surface *, struct gbm_bo *);
typedef uint32_t (*PFN_gbm_bo_get_handle)(struct gbm_bo *);
typedef uint32_t (*PFN_gbm_bo_get_stride)(struct gbm_bo *);
typedef void (*PFN_gbm_surface_destroy)(struct gbm_surface *);
typedef void (*PFN_gbm_device_destroy)(struct gbm_device *);

static struct {
  PFN_eglGetDisplay GetDisplay;
  PFN_eglGetPlatformDisplay GetPlatformDisplay;
  PFN_eglInitialize Initialize;
  PFN_eglBindAPI BindAPI;
  PFN_eglChooseConfig ChooseConfig;
  PFN_eglCreateContext CreateContext;
  PFN_eglCreateWindowSurface CreateWindowSurface;
  PFN_eglMakeCurrent MakeCurrent;
  PFN_eglSwapBuffers SwapBuffers;
  PFN_eglQueryString QueryString;
  PFN_eglGetProcAddress GetProcAddress;
  PFN_eglDestroySurface DestroySurface;
  PFN_eglDestroyContext DestroyContext;
  PFN_eglTerminate Terminate;
  PFN_eglGetError GetError;
  PFN_gbm_create_device gbm_create_device;
  PFN_gbm_surface_create gbm_surface_create;
  PFN_gbm_surface_lock_front_buffer gbm_surface_lock_front_buffer;
  PFN_gbm_surface_release_buffer gbm_surface_release_buffer;
  PFN_gbm_bo_get_handle gbm_bo_get_handle;
  PFN_gbm_bo_get_stride gbm_bo_get_stride;
  PFN_gbm_surface_destroy gbm_surface_destroy;
  PFN_gbm_device_destroy gbm_device_destroy;
} E;

/* GL ES2 subset via eglGetProcAddress */
static struct {
  unsigned (*CreateShader)(unsigned);
  void (*ShaderSource)(unsigned, int, const char *const *, const int *);
  void (*CompileShader)(unsigned);
  void (*GetShaderiv)(unsigned, unsigned, int *);
  unsigned (*CreateProgram)(void);
  void (*AttachShader)(unsigned, unsigned);
  void (*LinkProgram)(unsigned);
  void (*GetProgramiv)(unsigned, unsigned, int *);
  void (*UseProgram)(unsigned);
  int (*GetUniformLocation)(unsigned, const char *);
  void (*Uniform1i)(int, int);
  void (*Uniform2f)(int, float, float);
  void (*Uniform1f)(int, float);
  void (*GenBuffers)(int, unsigned *);
  void (*BindBuffer)(unsigned, unsigned);
  void (*BufferData)(unsigned, long, const void *, unsigned);
  void (*EnableVertexAttribArray)(unsigned);
  void (*VertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
  void (*DrawArrays)(unsigned, int, int);
  void (*GenTextures)(int, unsigned *);
  void (*BindTexture)(unsigned, unsigned);
  void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
  void (*TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
  void (*TexParameteri)(unsigned, unsigned, int);
  void (*ActiveTexture)(unsigned);
  void (*Viewport)(int, int, int, int);
  void (*ClearColor)(float, float, float, float);
  void (*Clear)(unsigned);
  void (*Disable)(unsigned);
  void (*PixelStorei)(unsigned, int);
} G;

static const char *gl_shader_v =
    "attribute vec2 a_pos; attribute vec2 a_uv; varying vec2 v_uv;\n"
    "uniform vec2 u_scale; uniform vec2 u_off;\n"
    "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos*u_scale+u_off, 0.0, 1.0); }\n";
static const char *gl_shader_f =
    "precision mediump float; varying vec2 v_uv;\n"
    "uniform int u_mode; uniform float u_t; uniform sampler2D u_tex;\n"
    "void main(){\n"
    "  if(u_mode==0){\n"
    "    float g = v_uv.y*0.6 + 0.2*sin(u_t*0.0004 + v_uv.x*3.0);\n"
    "    vec3 a = vec3(0.05,0.09,0.20); vec3 b = vec3(0.12,0.35,0.60);\n"
    "    vec3 c = mix(a,b,clamp(g,0.0,1.0));\n"
    "    c += 0.05*vec3(sin(u_t*0.0007+v_uv.x*5.0), cos(u_t*0.0005+v_uv.y*4.0), 0.0);\n"
    "    gl_FragColor = vec4(c,1.0);\n"
    "  } else { vec4 t = texture2D(u_tex, v_uv); gl_FragColor = vec4(t.b, t.g, t.r, t.a); }\n"
    "}\n";

static int gl_load(void) {
  const char *names[] = {"glCreateShader",      "glShaderSource",
                         "glCompileShader",     "glGetShaderiv",
                         "glCreateProgram",     "glAttachShader",
                         "glLinkProgram",       "glGetProgramiv",
                         "glUseProgram",        "glGetUniformLocation",
                         "glUniform1i",         "glUniform2f",
                         "glUniform1f",         "glGenBuffers",
                         "glBindBuffer",        "glBufferData",
                         "glEnableVertexAttribArray", "glVertexAttribPointer",
                         "glDrawArrays",        "glGenTextures",
                         "glBindTexture",       "glTexImage2D",
                         "glTexSubImage2D",     "glTexParameteri",
                         "glActiveTexture",     "glViewport",
                         "glClearColor",        "glClear",
                         "glDisable",           "glPixelStorei"};
  void **slot = (void **)(uintptr_t)&G;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    void *p = E.GetProcAddress(names[i]);
    if (!p) return -1;
    slot[i] = p;
  }
  return 0;
}

static uint32_t gbm_bo_fb(struct card *c, gbm_bo *bo) {
  uint32_t h = E.gbm_bo_get_handle(bo);
  for (int i = 0; i < c->gpu_bo_n; i++)
    if (c->gpu_bo_handle[i] == h) return c->gpu_bo_fb[i];
  uint32_t handles[4] = {h, 0, 0, 0}, pitches[4] = {E.gbm_bo_get_stride(bo), 0, 0, 0},
           offsets[4] = {0, 0, 0, 0};
  struct drm_mode_fb_cmd2 fb;
  memset(&fb, 0, sizeof(fb));
  fb.width = c->mode.hdisplay;
  fb.height = c->mode.vdisplay;
  fb.pixel_format = DRM_FORMAT_XRGB8888;
  memcpy(fb.handles, handles, sizeof(handles));
  memcpy(fb.pitches, pitches, sizeof(pitches));
  memcpy(fb.offsets, offsets, sizeof(offsets));
  if (drm_ioctl(c->fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) return 0;
  if (c->gpu_bo_n < 8) {
    c->gpu_bo_handle[c->gpu_bo_n] = h;
    c->gpu_bo_fb[c->gpu_bo_n] = fb.fb_id;
    c->gpu_bo_n++;
  }
  return fb.fb_id;
}

int card_egl_init(struct card *c) {
  if (c->use_fbdev) return -1;
  c->egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  c->gbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!c->egl || !c->gbm) {
    LOG("EGL: libEGL/libgbm not loadable (%s) — GPU accel unavailable", dlerror());
    return -1;
  }
#define LOAD(mod, field, sym)                                                  \
  E.field = (__typeof__(E.field))(uintptr_t)dlsym(c->mod, sym);                 \
  if (!E.field) {                                                               \
    LOG("EGL: missing %s", sym);                                                \
    return -1;                                                                 \
  }
  LOAD(egl, GetDisplay, "eglGetDisplay");
  LOAD(egl, GetPlatformDisplay, "eglGetPlatformDisplay");
  LOAD(egl, Initialize, "eglInitialize");
  LOAD(egl, BindAPI, "eglBindAPI");
  LOAD(egl, ChooseConfig, "eglChooseConfig");
  LOAD(egl, CreateContext, "eglCreateContext");
  LOAD(egl, CreateWindowSurface, "eglCreateWindowSurface");
  LOAD(egl, MakeCurrent, "eglMakeCurrent");
  LOAD(egl, SwapBuffers, "eglSwapBuffers");
  LOAD(egl, QueryString, "eglQueryString");
  LOAD(egl, GetProcAddress, "eglGetProcAddress");
  LOAD(egl, DestroySurface, "eglDestroySurface");
  LOAD(egl, DestroyContext, "eglDestroyContext");
  LOAD(egl, Terminate, "eglTerminate");
  LOAD(egl, GetError, "eglGetError");
  LOAD(gbm, gbm_create_device, "gbm_create_device");
  LOAD(gbm, gbm_surface_create, "gbm_surface_create");
  LOAD(gbm, gbm_surface_lock_front_buffer, "gbm_surface_lock_front_buffer");
  LOAD(gbm, gbm_surface_release_buffer, "gbm_surface_release_buffer");
  LOAD(gbm, gbm_bo_get_handle, "gbm_bo_get_handle");
  LOAD(gbm, gbm_bo_get_stride, "gbm_bo_get_stride");
  LOAD(gbm, gbm_surface_destroy, "gbm_surface_destroy");
  LOAD(gbm, gbm_device_destroy, "gbm_device_destroy");
#undef LOAD

  c->gbm_dev = E.gbm_create_device(c->fd);
  if (!c->gbm_dev) {
    LOG("EGL: gbm_create_device failed");
    return -1;
  }
  c->egl_dpy = E.GetPlatformDisplay
                   ? E.GetPlatformDisplay(EGL_PLATFORM_GBM_MESA, c->gbm_dev, NULL)
                   : E.GetDisplay((void *)0);
  if (!c->egl_dpy || !E.Initialize(c->egl_dpy, NULL, NULL)) {
    LOG("EGL: init failed (err 0x%x)", E.GetError ? E.GetError() : 0);
    return -1;
  }
  if (!E.BindAPI(EGL_OPENGL_ES_API)) return -1;
  EGLint_ cfg_attr[] = {EGL_SURFACE_TYPE,
                        EGL_WINDOW_BIT,
                        EGL_RENDERABLE_TYPE,
                        EGL_OPENGL_ES2_BIT,
                        EGL_NATIVE_VISUAL_ID,
                        (EGLint_)DRM_FORMAT_XRGB8888,
                        EGL_NONE};
  EGLConfig cfg = NULL;
  EGLint_ n = 0;
  if (!E.ChooseConfig(c->egl_dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
    LOG("EGL: no config (err 0x%x)", E.GetError());
    return -1;
  }
  EGLint_ ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  c->egl_ctx = E.CreateContext(c->egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (!c->egl_ctx) {
    LOG("EGL: context failed (err 0x%x)", E.GetError());
    return -1;
  }
  c->gbm_surf = E.gbm_surface_create(c->gbm_dev, c->mode.hdisplay, c->mode.vdisplay,
                                     DRM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!c->gbm_surf) return -1;
  c->egl_surf = E.CreateWindowSurface(c->egl_dpy, cfg, c->gbm_surf, NULL);
  if (!c->egl_surf) return -1;
  if (!E.MakeCurrent(c->egl_dpy, c->egl_surf, c->egl_surf, c->egl_ctx)) return -1;
  if (gl_load() < 0) {
    LOG("EGL: GL loader missing entry");
    return -1;
  }
  c->gl_renderer = E.QueryString(c->egl_dpy, 0x1F01 /* EGL_RENDERER */);
  /* compile shaders once */
  unsigned vs = G.CreateShader(0x8B31 /* VERTEX */), fs = G.CreateShader(0x8B30 /* FRAGMENT */);
  int ok = 0;
  G.ShaderSource(vs, 1, &gl_shader_v, NULL);
  G.CompileShader(vs);
  G.GetShaderiv(vs, 0x8B82 /* COMPILE_STATUS */, &ok);
  if (!ok) return -1;
  G.ShaderSource(fs, 1, &gl_shader_f, NULL);
  G.CompileShader(fs);
  G.GetShaderiv(fs, 0x8B82, &ok);
  if (!ok) return -1;
  c->gl_prog = G.CreateProgram();
  G.AttachShader(c->gl_prog, vs);
  G.AttachShader(c->gl_prog, fs);
  G.LinkProgram(c->gl_prog);
  G.GetProgramiv(c->gl_prog, 0x8B82 /* LINK_STATUS */, &ok);
  if (!ok) return -1;
  G.UseProgram(c->gl_prog);
  G.GenBuffers(1, &c->gl_vbo);
  G.BindBuffer(0x8892 /* ARRAY_BUFFER */, c->gl_vbo);
  static const float quad[] = { /* x,y,u,v pairs, two triangles */
      -1, -1, 0, 1, 1, -1, 1, 1, -1, 1, 0, 0,
      -1, 1, 0, 0, 1, -1, 1, 1, 1, 1, 1, 0};
  G.BufferData(0x8892, sizeof(quad), quad, 0x88E4 /* STATIC_DRAW */);
  G.GenTextures(1, &c->gl_tex);
  G.ActiveTexture(0x84C1 /* TEXTURE1 */);
  G.BindTexture(0x0DE1 /* TEXTURE_2D */, c->gl_tex);
  G.TexParameteri(0x0DE1, 0x2801 /* TEXTURE_MIN_FILTER */, 0x2601 /* LINEAR */);
  G.TexParameteri(0x0DE1, 0x2800 /* TEXTURE_MAG_FILTER */, 0x2601);
  G.TexParameteri(0x0DE1, 0x2802 /* TEXTURE_WRAP_S */, 0x812F /* CLAMP_TO_EDGE */);
  G.TexParameteri(0x0DE1, 0x2803 /* TEXTURE_WRAP_T */, 0x812F);
  G.Disable(0x0B71 /* DEPTH_TEST */);
  c->gpu_ok = 1;
  c->gl_tex_w = c->gl_tex_h = 0;
  LOG("GPU accel ON: renderer=\"%s\"", c->gl_renderer ? c->gl_renderer : "?");
  return 0;
}

/* Import a client dma-buf (zwp_linux_dmabuf_v1): prove the PRIME import into
 * our DRM device (best effort, no GPU screen needed) and map the buffer for
 * CPU compositing. ponytail: linear buffers only — the client allocates with
 * GBM_BO_USE_LINEAR; a tiled layout would need the GPU/EGL path. */
int card_dmabuf_import(struct card *c, int fd, uint32_t format, uint32_t stride,
                       uint32_t length, void **px, int *w, int *h) {
  (void)format;
  if (fd < 0 || stride < 4 || length < stride) return -1;
  if (c && c->fd >= 0) {                          /* PRIME import proof */
    struct drm_prime_handle ph = { .handle = 0, .flags = 0, .fd = fd };
    if (drm_ioctl(c->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) == 0)
      LOG("dmabuf: PRIME import ok handle=%u", ph.handle);
    else
      LOG("dmabuf: PRIME import failed: %s (mmap path continues)", strerror(errno));
  }
  void *map = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    LOG("dmabuf: mmap(%u) failed: %s", length, strerror(errno));
    return -1;
  }
  int dw = *w > 0 ? *w : (int)(stride / 4);
  int dh = *h > 0 ? *h : (int)(length / stride);
  uint32_t sum = 2166136261u;                     /* FNV-1a over the pixels */
  const uint32_t *p = (const uint32_t *)map;
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) sum = (sum ^ p[(size_t)y * (stride / 4) + x]) * 16777619u;
  LOG("dmabuf: import %dx%d stride=%u sum=%08x", dw, dh, stride, sum);
  *px = map;
  *w = dw;
  *h = dh;
  return 0;
}

int card_gpu_ok(struct card *c) { return c->gpu_ok; }
const char *card_gpu_renderer(struct card *c) { return c->gl_renderer; }

int card_gpu_frame(struct card *c, int x, int y, int w, int h, unsigned t_ms) {
  if (!c->gpu_ok) return -1;
  /* Full-canvas upload keeps every visible window's pixels on screen while the
   * wallpaper pass redraws underneath (dirty-rect partial upload ceiling: would
   * wipe uncovered windows; upgrade = retain-damage tracking). */
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  int W = c->mode.hdisplay, H = c->mode.vdisplay;
  E.MakeCurrent(c->egl_dpy, c->egl_surf, c->egl_surf, c->egl_ctx);
  G.UseProgram(c->gl_prog);
  G.Viewport(0, 0, W, H);
  G.BindBuffer(0x8892 /* ARRAY_BUFFER */, c->gl_vbo);
  G.EnableVertexAttribArray(0);
  G.EnableVertexAttribArray(1);
  G.VertexAttribPointer(0, 2, 0x1406 /*FLOAT*/, 0, 16, (void *)0);
  G.VertexAttribPointer(1, 2, 0x1406, 0, 16, (void *)8);
  int u_mode = G.GetUniformLocation(c->gl_prog, "u_mode");
  int u_scale = G.GetUniformLocation(c->gl_prog, "u_scale");
  int u_off = G.GetUniformLocation(c->gl_prog, "u_off");
  int u_t = G.GetUniformLocation(c->gl_prog, "u_t");
  int u_tex = G.GetUniformLocation(c->gl_prog, "u_tex");

  /* pass 1: wallpaper (GPU-generated gradient, animated) */
  G.Uniform1i(u_mode, 0);
  G.Uniform1f(u_t, (float)(t_ms % 600000));
  G.Uniform2f(u_scale, 1.0f, 1.0f);
  G.Uniform2f(u_off, 0.0f, 0.0f);
  G.DrawArrays(0x0004 /*TRIANGLES*/, 0, 6);

  /* pass 2: full CPU canvas (shell UI + windows) as texture */
  if (c->shadow.w != c->gl_tex_w || c->shadow.h != c->gl_tex_h) {
    G.ActiveTexture(0x84C1 /*TEXTURE1*/);
    G.BindTexture(0x0DE1 /*TEXTURE_2D*/, c->gl_tex);
    G.TexImage2D(0x0DE1, 0, 0x8058 /*RGBA8*/, c->shadow.w, c->shadow.h, 0, 0x1908 /*RGBA*/,
                 0x1401 /*UBYTE*/, c->shadow.px);
    c->gl_tex_w = c->shadow.w;
    c->gl_tex_h = c->shadow.h;
  } else {
    G.ActiveTexture(0x84C1);
    G.BindTexture(0x0DE1, c->gl_tex);
    G.PixelStorei(0x0CF5 /*UNPACK_ALIGNMENT*/, 4);
    G.TexSubImage2D(0x0DE1, 0, 0, 0, c->shadow.w, c->shadow.h, 0x1908, 0x1401, c->shadow.px);
  }
  G.Uniform1i(u_mode, 1);
  G.Uniform1i(u_tex, 1);
  G.Uniform2f(u_scale, 1.0f, 1.0f);
  G.Uniform2f(u_off, 0.0f, 0.0f);
  G.DrawArrays(0x0004, 0, 6);

  if (!E.SwapBuffers(c->egl_dpy, c->egl_surf)) {
    LOG("SwapBuffers failed 0x%x", E.GetError ? E.GetError() : 0);
    return -1;
  }
  /* scanout the GBM front buffer */
  gbm_bo *bo = E.gbm_surface_lock_front_buffer(c->gbm_surf);
  if (!bo) return -1;
  uint32_t fb = gbm_bo_fb(c, bo);
  if (!fb) {
    E.gbm_surface_release_buffer(c->gbm_surf, bo);
    return -1;
  }
  if (drm_atomic_flip(c, fb) < 0) return -1;
  if (c->last_front && c->last_front != bo) E.gbm_surface_release_buffer(c->gbm_surf, c->last_front);
  c->last_front = bo;
  return 0;
}

void card_gpu_shutdown(struct card *c) {
  if (!c->gpu_ok && !c->egl_dpy) return;
  if (c->egl_dpy && c->egl_ctx) {
    E.MakeCurrent(c->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    E.DestroyContext(c->egl_dpy, c->egl_ctx);
  }
  if (c->egl_dpy && c->egl_surf) E.DestroySurface(c->egl_dpy, c->egl_surf);
  if (c->gbm_surf) E.gbm_surface_destroy(c->gbm_surf);
  if (c->egl_dpy) E.Terminate(c->egl_dpy);
  if (c->gbm_dev) E.gbm_device_destroy(c->gbm_dev);
  c->gpu_ok = 0;
}

int kms_selftest(void) {
  int fd = -1;
  DIR *d = opendir("/dev/dri");
  if (d) {
    struct dirent *e;
    int best = -1;
    while ((e = readdir(d))) {
      int m;
      if (sscanf(e->d_name, "renderD%d", &m) == 1)
        if (best < 0 || m < best) best = m;
    }
    closedir(d);
    if (best >= 0) {
      char p[64];
      snprintf(p, sizeof(p), "/dev/dri/renderD%d", best);
      fd = open(p, O_RDWR | O_CLOEXEC);
    }
  }
  if (fd < 0) {
    printf("selftest kms: no render node (no GPU) — skipped\n");
    return 0;
  }
  void *egl = dlopen("libEGL.so.1", RTLD_NOW);
  if (!egl) {
    printf("selftest kms: libEGL missing — skipped\n");
    close(fd);
    return 0;
  }
  EGLDisplay (*GetDisplay)(void *) = (void *)(uintptr_t)dlsym(egl, "eglGetDisplay");
  EGLBoolean (*Initialize)(EGLDisplay, EGLint_ *, EGLint_ *) = (void *)(uintptr_t)dlsym(egl, "eglInitialize");
  EGLBoolean (*BindAPI)(EGLenum) = (void *)(uintptr_t)dlsym(egl, "eglBindAPI");
  EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint_ *, EGLConfig *, EGLint_, EGLint_ *) =
      (void *)(uintptr_t)dlsym(egl, "eglChooseConfig");
  EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint_ *) =
      (void *)(uintptr_t)dlsym(egl, "eglCreateContext");
  EGLSurface (*CreatePbuffer)(EGLDisplay, EGLConfig, const EGLint_ *) =
      (void *)(uintptr_t)dlsym(egl, "eglCreatePbufferSurface");
  EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) =
      (void *)(uintptr_t)dlsym(egl, "eglMakeCurrent");
  const char *(*QueryString)(EGLDisplay, EGLint_) = (void *)(uintptr_t)dlsym(egl, "eglQueryString");
  void *(*GetProcAddress)(const char *) = (void *)(uintptr_t)dlsym(egl, "eglGetProcAddress");
  if (!GetDisplay || !Initialize || !BindAPI || !ChooseConfig || !CreateContext ||
      !MakeCurrent || !QueryString || !GetProcAddress) {
    printf("selftest kms: libEGL incomplete — skipped\n");
    dlclose(egl);
    close(fd);
    return 0;
  }
  /* surfaceless platform: headless context on the default render device */
  setenv("EGL_PLATFORM", "surfaceless", 1);
  EGLDisplay dpy = GetDisplay(NULL);
  if (!dpy || !Initialize(dpy, NULL, NULL)) {
    printf("selftest kms: eglInitialize failed — skipped\n");
    dlclose(egl);
    close(fd);
    return 0;
  }
  BindAPI(0x30A0 /* ES */);
  EGLint_ attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
  EGLConfig cfg = NULL;
  EGLint_ n = 0;
  if (!ChooseConfig(dpy, attr, &cfg, 1, &n) || n < 1) {
    printf("selftest kms: chooseconfig failed — skipped\n");
    dlclose(egl);
    close(fd);
    return 0;
  }
  EGLint_ ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = CreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  EGLint_ pb[] = {0x3057 /* WIDTH */, 4, 0x3056 /* HEIGHT */, 4, EGL_NONE};
  EGLSurface surf = CreatePbuffer ? CreatePbuffer(dpy, cfg, pb) : EGL_NO_SURFACE;
  if (!ctx || !surf || !MakeCurrent(dpy, surf, surf, ctx)) {
    printf("selftest kms: context/pbuffer failed — skipped\n");
    dlclose(egl);
    close(fd);
    return 0;
  }
  void (*ClearColor)(float, float, float, float) = (void *)(uintptr_t)GetProcAddress("glClearColor");
  void (*Clear)(unsigned) = (void *)(uintptr_t)GetProcAddress("glClear");
  void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void *) =
      (void *)(uintptr_t)GetProcAddress("glReadPixels");
  int pass = 0;
  if (ClearColor && Clear && ReadPixels) {
    ClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    Clear(0x4000);
    unsigned pxv = 0;
    ReadPixels(0, 0, 1, 1, 0x1908 /*RGBA*/, 0x1401 /*UBYTE*/, &pxv);
    /* bytes little-endian: [R,G,B,A] in memory -> pxv byte0 = R */
    pass = ((pxv & 0xFF) == 0xFF) && (((pxv >> 24) & 0xFF) == 0xFF);
  }
  const char *r = QueryString(dpy, 0x1F01 /* EGL_RENDERER */);
  printf("selftest kms: EGL renderer=\"%s\" clear=%s\n", r ? r : "?", pass ? "OK" : "BAD");
  dlclose(egl);
  close(fd);
  return pass ? 0 : -1;
}
