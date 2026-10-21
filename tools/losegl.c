/* losegl.c — guest-side OpenGL/EGL probe.
 *
 * Picks a display (GBM device, then surfaceless, then default), creates a GLES
 * 2.0 context, renders into an FBO and reads the pixel back. Prints a
 * machine-checkable marker:
 *
 *   LOSEDOZE GL OK <renderer> | <version> | EGL <maj>.<min>
 *
 * Built against Mesa's headers (build-time only; the image ships no headers).
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#ifndef EGL_PLATFORM_GBM_MESA
#define EGL_PLATFORM_GBM_MESA 0x1D87
#endif
#ifndef EGL_NO_CONFIG_KHR
#define EGL_NO_CONFIG_KHR ((EGLConfig)0x31C0)
#endif
typedef EGLDisplay (*PFN_eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);

/* --- minimal gbm declarations (the image ships libgbm, not its header) --- */
typedef struct gbm_device gbm_device;
extern gbm_device *gbm_create_device(int fd);
extern void gbm_device_destroy(gbm_device *);

#define GBM_BO_USE_SCANOUT 0x00000001u

static const char *egl_err(EGLint e) {
  switch (e) {
    case EGL_SUCCESS: return "SUCCESS";
    case EGL_NOT_INITIALIZED: return "NOT_INITIALIZED";
    case EGL_BAD_DISPLAY: return "BAD_DISPLAY";
    case EGL_BAD_CONFIG: return "BAD_CONFIG";
    case EGL_BAD_CONTEXT: return "BAD_CONTEXT";
    case EGL_BAD_MATCH: return "BAD_MATCH";
    case EGL_BAD_ALLOC: return "BAD_ALLOC";
    case EGL_BAD_PARAMETER: return "BAD_PARAMETER";
    default: return "?";
  }
}

int main(void) {
  PFN_eglGetPlatformDisplayEXT getPlatformDisplay =
      (PFN_eglGetPlatformDisplayEXT)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay dpy = EGL_NO_DISPLAY;
  EGLint maj = 0, min = 0;

  if (getPlatformDisplay) {
    dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy != EGL_NO_DISPLAY && !eglInitialize(dpy, &maj, &min)) dpy = EGL_NO_DISPLAY;
  }
  if (dpy == EGL_NO_DISPLAY && getPlatformDisplay) {
    /* GBM: works on the guest's bochs-drm card (and any DRM render node). */
    for (int i = 0; i < 4 && dpy == EGL_NO_DISPLAY; i++) {
      char path[32];
      snprintf(path, sizeof path, "/dev/dri/card%d", i);
      int fd = open(path, O_RDWR | O_CLOEXEC);
      if (fd < 0) continue;
      gbm_device *dev = gbm_create_device(fd);
      close(fd);
      if (!dev) continue;
      EGLDisplay cand = getPlatformDisplay(EGL_PLATFORM_GBM_MESA, dev, NULL);
      if (cand != EGL_NO_DISPLAY && eglInitialize(cand, &maj, &min)) dpy = cand;
      else if (cand != EGL_NO_DISPLAY) gbm_device_destroy(dev);
    }
  }
  if (dpy == EGL_NO_DISPLAY) {
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy != EGL_NO_DISPLAY && !eglInitialize(dpy, &maj, &min)) dpy = EGL_NO_DISPLAY;
  }
  if (dpy == EGL_NO_DISPLAY) {
    printf("LOSEDOZE GL FAIL no EGL display\n");
    return 1;
  }

  const char *vendor = eglQueryString(dpy, EGL_VENDOR);
  const char *caps = eglQueryString(dpy, EGL_CLIENT_APIS);
  printf("EGL %d.%d %s [%s]\n", maj, min, vendor ? vendor : "?", caps ? caps : "?");

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    printf("LOSEDOZE GL FAIL eglBindAPI\n");
    return 1;
  }

  EGLConfig cfg = NULL;
  EGLint ncfg = 0;
  EGLint cfg_attr[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
    EGL_NONE
  };
  if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
    printf("LOSEDOZE GL FAIL eglChooseConfig %s\n", egl_err(eglGetError()));
    return 1;
  }

  static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  EGLContext ctx = eglCreateContext(dpy, cfg ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
  if (ctx == EGL_NO_CONTEXT) {
    printf("LOSEDOZE GL FAIL eglCreateContext %s\n", egl_err(eglGetError()));
    return 1;
  }

  EGLint pb_attr[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
  EGLSurface surf = EGL_NO_SURFACE;
  if (eglGetError() == EGL_SUCCESS) surf = eglCreatePbufferSurface(dpy, cfg ? cfg : EGL_NO_CONFIG_KHR, pb_attr);
  if (surf == EGL_NO_SURFACE) {
    /* no pbuffer config: render into an FBO on a surfaceless context */
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
      printf("LOSEDOZE GL FAIL eglMakeCurrent %s\n", egl_err(eglGetError()));
      return 1;
    }
  } else if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
    printf("LOSEDOZE GL FAIL eglMakeCurrent %s\n", egl_err(eglGetError()));
    return 1;
  }

  GLuint tex = 0, fbo = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    printf("LOSEDOZE GL FAIL framebuffer incomplete\n");
    return 1;
  }

  glViewport(0, 0, 64, 64);
  glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  unsigned char px[4] = {0, 0, 0, 0};
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  GLenum err = glGetError();
  if (err != GL_NO_ERROR) {
    printf("LOSEDOZE GL FAIL glGetError 0x%x\n", err);
    return 1;
  }
  if (px[0] != 64 || px[1] != 128 || px[2] != 191 || px[3] != 255) {
    printf("LOSEDOZE GL FAIL pixel %u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
    return 1;
  }

  const char *renderer = (const char *)glGetString(GL_RENDERER);
  const char *version = (const char *)glGetString(GL_VERSION);
  printf("LOSEDOZE GL OK %s | %s | EGL %d.%d | readback %u,%u,%u,%u\n",
         renderer ? renderer : "?", version ? version : "?", maj, min,
         px[0], px[1], px[2], px[3]);
  fflush(stdout);

  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &tex);
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(dpy, ctx);
  if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
  eglTerminate(dpy);
  return 0;
}
