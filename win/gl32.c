/* gl32.c — homemade opengl32.dll for the losedoze Win32 subsystem.
 * Desktop OpenGL (compatibility profile), not GLES: the context is created
 * with eglBindAPI(EGL_OPENGL_API) and an EGL_OPENGL_BIT pbuffer on the
 * surfaceless platform, so the app gets the full desktop GL API. Entry points
 * are resolved once from eglGetProcAddress. Presentation: gdi32!SwapBuffers
 * reads the pbuffer back into the window canvas (gl32_swap).
 * Original code; public EGL/GL ABI facts only. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include "host.h"

typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLConfig;
typedef unsigned int EGLenum;
typedef int EGLBoolean;
typedef int EGLint_;

#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_BIT 0x0008
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_OPENGL_API 0x30A2

#define GLCTX_MAGIC 0x33474c43u  /* "CLG3" */
#define GL_NO_CONTEXT ((EGLContext)0)

static void *eglh;
static struct {
  EGLDisplay (*GetPlatformDisplay)(EGLenum, void *, const EGLint_ *);
  EGLBoolean (*Initialize)(EGLDisplay, EGLint_ *, EGLint_ *);
  EGLBoolean (*BindAPI)(EGLenum);
  EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint_ *, EGLConfig *, EGLint_,
                             EGLint_ *);
  EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint_ *);
  EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext,
                              const EGLint_ *);
  EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
  EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
  EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
  EGLBoolean (*Terminate)(EGLDisplay);
  EGLint_ (*GetError)(void);
  void *(*GetProcAddress)(const char *);
} E;

static EGLDisplay dpy;
static EGLConfig cfg;
static int gl_ok, gl_tried;

typedef struct GlCtx {
  W32 magic;
  W32 *px;                     /* canvas pixels bound at wglMakeCurrent */
  int w, h;
  EGLSurface surf;
  EGLContext ctx;
  W8 *stg;                     /* glReadPixels staging (bottom-up) */
  size_t stgsz;
} GlCtx;

static GlCtx *cur;

static int gl_init(void) {
  if (gl_tried) return gl_ok;
  gl_tried = 1;
  const char *names[] = {"libEGL.so.1", "libEGL.so", NULL};
  for (int i = 0; names[i]; i++) {
    eglh = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
    if (eglh) break;
  }
  if (!eglh) return 0;
#define S(s) \
  do { *(void **)(&E.s) = dlsym(eglh, "egl" #s); if (!E.s) return 0; } while (0)
  S(GetPlatformDisplay); S(Initialize); S(BindAPI); S(ChooseConfig);
  S(CreatePbufferSurface); S(CreateContext); S(MakeCurrent); S(DestroySurface);
  S(DestroyContext); S(Terminate); S(GetError); S(GetProcAddress);
#undef S
  dpy = E.GetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY,
                             NULL);
  if (!dpy) return 0;
  EGLint_ maj = 0, min = 0;
  if (!E.Initialize(dpy, &maj, &min)) return 0;
  if (!E.BindAPI(EGL_OPENGL_API)) return 0;
  EGLint_ attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
  EGLint_ n = 0;
  if (!E.ChooseConfig(dpy, attr, &cfg, 1, &n) || n < 1) return 0;
  gl_ok = 1;
  return 1;
}

static GlCtx *glctx_of(HGLRC h) {
  GlCtx *g = (GlCtx *)h;
  return g && g->magic == GLCTX_MAGIC ? g : NULL;
}

/* ---------------- WGL ---------------- */

HGLRC wglCreateContext(HDC dc) {
  (void)dc;
  if (!gl_init()) return NULL;
  GlCtx *g = calloc(1, sizeof *g);
  if (!g) return NULL;
  /* no EGL_CONTEXT_*_VERSION attrs: Mesa hands out a 4.6 compatibility
   * context (a 3.2 request is refused) — fixed-function GL 1.x then works. */
  g->ctx = E.CreateContext(dpy, cfg, GL_NO_CONTEXT, NULL);
  if (!g->ctx) { free(g); return NULL; }
  g->magic = GLCTX_MAGIC;
  return (HGLRC)g;
}

BOOL wglMakeCurrent(HDC dc, HGLRC rc) {
  GlCtx *g = glctx_of(rc);
  W32Canvas *c = (W32Canvas *)dc;
  if (!g || !c || !c->px || c->w <= 0 || c->h <= 0) return W32_FALSE;
  if (g->px != c->px || g->w != c->w || g->h != c->h) { /* resize: new pbuffer */
    if (g->surf) {
      E.MakeCurrent(dpy, NULL, NULL, GL_NO_CONTEXT);
      E.DestroySurface(dpy, g->surf);
      g->surf = NULL;
    }
    EGLint_ pa[] = {EGL_WIDTH, (EGLint_)c->w, EGL_HEIGHT, (EGLint_)c->h,
                    EGL_NONE};
    g->surf = E.CreatePbufferSurface(dpy, cfg, pa);
    if (!g->surf) return W32_FALSE;
    g->px = c->px; g->w = c->w; g->h = c->h;
  }
  if (!E.MakeCurrent(dpy, g->surf, g->surf, g->ctx)) return W32_FALSE;
  cur = g;
  return W32_TRUE;
}

BOOL wglDeleteContext(HGLRC rc) {
  GlCtx *g = glctx_of(rc);
  if (!g) return W32_FALSE;
  if (cur == g) { E.MakeCurrent(dpy, NULL, NULL, GL_NO_CONTEXT); cur = NULL; }
  if (g->surf) E.DestroySurface(dpy, g->surf);
  E.DestroyContext(dpy, g->ctx);
  free(g->stg);
  g->magic = 0;
  free(g);
  return W32_TRUE;
}

/* Present the current GL context into a canvas: glReadPixels (bottom-up,
 * RGBA) -> canvas px (top-down, ARGB). One pass, flip folded in. */
int gl32_swap(W32Canvas *c) {
  if (!gl_ok || !cur || !cur->ctx || !c || !c->px) return 0;
  int w = cur->w, h = cur->h;
  if (w > c->w) w = c->w;
  if (h > c->h) h = c->h;
  if (w <= 0 || h <= 0) return 0;
  size_t need = (size_t)w * (size_t)h * 4;
  if (need > cur->stgsz) {
    free(cur->stg);
    cur->stg = malloc(need);
    if (!cur->stg) { cur->stgsz = 0; return 0; }
    cur->stgsz = need;
  }
  glFinish();
  glPixelStorei(0x0D05 /*GL_PACK_ALIGNMENT*/, 1);
  glReadPixels(0, 0, w, h, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/,
               cur->stg);
  const W32 *src = (const W32 *)cur->stg;
  for (int y = 0; y < h; y++) {
    const W32 *srow = src + (size_t)(h - 1 - y) * w;  /* GL is bottom-up */
    W32 *drow = c->px + (size_t)y * c->stride;
    for (int x = 0; x < w; x++) {                     /* RGBA -> ARGB px */
      W32 v = srow[x];
      drow[x] = 0xff000000u | ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 255);
    }
  }
  glGetError(); /* swallow benign errors from the readback path */
  return 1;
}

/* ---------------- desktop GL entry points ----------------
 * Every GL symbol resolves lazily through eglGetProcAddress, so the host only
 * needs libEGL (no libGL/libGLX: desktop GL over EGL, X-free). */

static void *glres(const char *n) { return E.GetProcAddress ? E.GetProcAddress(n) : NULL; }

#define GLV(name, decl, call)                                                  \
  void name decl {                                                             \
    static void (*pf) decl = NULL;                                             \
    if (!pf) pf = (void (*) decl)glres(#name);                                 \
    if (pf) pf call;                                                           \
  }
#define GLF(name, ret, decl, call)                                             \
  ret name decl {                                                              \
    static ret (*pf) decl = NULL;                                              \
    if (!pf) pf = (ret (*) decl)glres(#name);                                  \
    return pf ? pf call : (ret)0;                                              \
  }

GLF(glGetString, const unsigned char *, (unsigned n), (n))
GLF(glGetError, unsigned, (void), ())
GLF(glGetIntegerv, void, (unsigned p, int *v), (p, v))
GLF(glCheckFramebufferStatus, unsigned, (unsigned t), (t))
GLF(glGenTextures, void, (int n, unsigned *t), (n, t))
GLF(glGenFramebuffers, void, (int n, unsigned *f), (n, f))
GLF(glDeleteTextures, void, (int n, const unsigned *t), (n, t))
GLF(glDeleteFramebuffers, void, (int n, const unsigned *f), (n, f))
GLF(glFramebufferTexture2D, void, (unsigned a, unsigned b, unsigned c, unsigned d, int e),
    (a, b, c, d, e))
GLV(glViewport, (int x, int y, int w, int h), (x, y, w, h))
GLV(glClearColor, (float r, float g, float b, float a), (r, g, b, a))
GLV(glClear, (unsigned m), (m))
GLV(glEnable, (unsigned c), (c))
GLV(glDisable, (unsigned c), (c))
GLV(glDepthMask, (unsigned char m), (m))
GLV(glFrontFace, (unsigned m), (m))
GLV(glShadeModel, (unsigned m), (m))
GLV(glMatrixMode, (unsigned m), (m))
GLV(glLoadIdentity, (void), ())
GLV(glPushMatrix, (void), ())
GLV(glPopMatrix, (void), ())
GLV(glOrtho, (double l, double r, double b, double t, double n, double f),
    (l, r, b, t, n, f))
GLV(glTranslatef, (float x, float y, float z), (x, y, z))
GLV(glRotatef, (float a, float x, float y, float z), (a, x, y, z))
GLV(glScalef, (float x, float y, float z), (x, y, z))
GLV(glBegin, (unsigned m), (m))
GLV(glEnd, (void), ())
GLV(glVertex2f, (float x, float y), (x, y))
GLV(glVertex3f, (float x, float y, float z), (x, y, z))
GLV(glColor3f, (float r, float g, float b), (r, g, b))
GLV(glColor4f, (float r, float g, float b, float a), (r, g, b, a))
GLV(glFlush, (void), ())
GLV(glFinish, (void), ())
GLV(glPixelStorei, (unsigned p, int a), (p, a))
GLV(glBindTexture, (unsigned t, unsigned x), (t, x))
GLV(glBindFramebuffer, (unsigned t, unsigned f), (t, f))
GLV(glTexImage2D, (unsigned t, int l, int ifmt, int w, int h, int b, unsigned f,
                   unsigned ty, const void *px),
    (t, l, ifmt, w, h, b, f, ty, px))
GLV(glTexParameteri, (unsigned t, unsigned p, int v), (t, p, v))
GLV(glReadPixels, (int x, int y, int w, int h, unsigned f, unsigned ty, void *px),
    (x, y, w, h, f, ty, px))
GLV(glDrawArrays, (unsigned m, int first, int count), (m, first, count))

#undef GLV
#undef GLF
