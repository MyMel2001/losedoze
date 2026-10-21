/* wltest.c — host-side check for shell/wl.c against REAL libwayland-client.
 *
 * Build: gcc -O2 -Ishell tools/wltest.c shell/wl.c -o /tmp/wltest \
 *          $(pkg-config --cflags --libs wayland-client)
 * Run:   /tmp/wltest          (exit 0 = PASS)
 *
 * Parent runs our hand-rolled server and pumps it; the forked child drives a
 * genuine libwayland-client through: connect, registry roundtrip + binds,
 * shm pool + XRGB buffer, wl_surface attach/commit, xdg_toplevel create,
 * configure/ack, frame callback, pointer+key events from the server side,
 * destroy. Any wire bug in wl.c shows up as client abort, roundtrip hang
 * (parent timeout kills child -> FAIL) or wrong data.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include "xdg-shell-client-protocol.h"

#include "lost.h"

#define SOCK "/tmp/wltest-losedoze.sock"

/* ---- globals seen ---- */
static int n_globals;
static struct wl_compositor *g_comp;
static struct wl_shm *g_shm;
static struct wl_seat *g_seat;
static struct xdg_wm_base *g_wm;
static const struct wl_seat_listener seat_listener;   /* fwd */
static const struct xdg_wm_base_listener wm_listener; /* fwd */

static void reg_global(void *ud, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver) {
    (void)ud;
    n_globals++;
    if (!strcmp(iface, "wl_compositor"))
        g_comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, "wl_shm"))
        g_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "wl_seat")) {
        g_seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
        wl_seat_add_listener(g_seat, &seat_listener, NULL);   /* BEFORE events arrive */
    }
    else if (!strcmp(iface, "xdg_wm_base")) {
        g_wm = wl_registry_bind(r, name, &xdg_wm_base_interface, ver < 6 ? ver : 6);
        xdg_wm_base_add_listener(g_wm, &wm_listener, NULL);   /* ping arrives immediately */
    }
}
static void reg_global_remove(void *ud, struct wl_registry *r, uint32_t name) {
    (void)ud; (void)r; (void)name;
}
static const struct wl_registry_listener reg_listener = {
    .global = reg_global, .global_remove = reg_global_remove,
};

/* ---- xdg_wm_base ping/pong ---- */
static void wm_ping(void *ud, struct xdg_wm_base *b, uint32_t serial) {
    (void)ud; xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

/* ---- toplevel ---- */
static int toplevel_configured;
static void tl_configure(void *ud, struct xdg_toplevel *t, int32_t w, int32_t h,
                         struct wl_array *st) {
    (void)ud; (void)t; (void)st;
    if (w > 0 && h > 0) toplevel_configured = 1;
}
static void tl_close(void *ud, struct xdg_toplevel *t) { (void)ud; (void)t; }
static void tl_configure_bounds(void *ud, struct xdg_toplevel *t, int32_t w, int32_t h) {
    (void)ud; (void)t; (void)w; (void)h;
}
static void tl_wm_caps(void *ud, struct xdg_toplevel *t, struct wl_array *caps) {
    (void)ud; (void)t; (void)caps;
}
static const struct xdg_toplevel_listener tl_listener = {
    .configure = tl_configure, .configure_bounds = tl_configure_bounds,
    .wm_capabilities = tl_wm_caps,
};

static int xdg_cfg_seen;
static void xdg_surf_configure(void *ud, struct xdg_surface *s, uint32_t serial) {
    (void)ud;
    xdg_cfg_seen++;
    xdg_surface_ack_configure(s, serial);
}
static const struct xdg_surface_listener xdg_surf_listener = {
    .configure = xdg_surf_configure,
};

/* ---- frame callback ---- */
static int frame_seen;
static void frame_done(void *ud, struct wl_callback *cb, uint32_t cbdata) {
    (void)ud; (void)cbdata;
    frame_seen++;
    wl_callback_destroy(cb);
}
static const struct wl_callback_listener frame_listener = { .done = frame_done };

/* ---- seat: keymap must arrive with a valid fd ---- */
static int keymap_ok, kbd_entered;
static void kb_keymap(void *ud, struct wl_keyboard *kb, uint32_t fmt,
                      int32_t fd, uint32_t size) {
    (void)ud; (void)kb;
    char *buf = malloc(size ? size : 1);
    ssize_t n = 0, got = 0;
    while (size > 0 && got < (ssize_t)size - 1 &&
           (n = read(fd, buf + got, size - 1 - got)) > 0)
        got += n;
    n = got;
    fprintf(stderr, "DBG keymap fmt=%u size=%u n=%zd fd=%d\n", fmt, size, n, fd);
    if (fmt == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && n > 0 && size > 8) {
        buf[n] = 0;
        keymap_ok = (strstr(buf, "xkb_keymap") != NULL);
        if (!keymap_ok) fprintf(stderr, "DBG head: %.40s\n", buf);
    }
    free(buf);
    close(fd);
}
static void kb_enter(void *ud, struct wl_keyboard *kb, uint32_t ser,
                     struct wl_surface *sf, struct wl_array *keys) {
    (void)ud; (void)kb; (void)ser; (void)keys; (void)sf;
    kbd_entered = 1;
}
static void kb_leave(void *ud, struct wl_keyboard *kb, uint32_t ser, struct wl_surface *sf) {
    (void)ud; (void)kb; (void)ser; (void)sf;
}
static void kb_key(void *ud, struct wl_keyboard *kb, uint32_t ser, uint32_t t,
                   uint32_t key, uint32_t st) {
    (void)ud; (void)kb; (void)ser; (void)t; (void)key; (void)st;
}
static void kb_mods(void *ud, struct wl_keyboard *kb, uint32_t ser, uint32_t dep,
                    uint32_t latched, uint32_t locked, uint32_t group) {
    (void)ud; (void)kb; (void)ser; (void)dep; (void)latched; (void)locked; (void)group;
}
static void kb_repinfo(void *ud, struct wl_keyboard *kb, int32_t rate, int32_t delay) {
    (void)ud; (void)kb; (void)rate; (void)delay;
}
static const struct wl_keyboard_listener kb_listener = {
    .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave,
    .key = kb_key, .modifiers = kb_mods, .repeat_info = kb_repinfo,
};

static void seat_caps(void *ud, struct wl_seat *seat, uint32_t caps) {
    (void)ud;
    fprintf(stderr, "DBG seat caps=0x%x\n", caps);
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        struct wl_keyboard *kb = wl_seat_get_keyboard(seat);
        fprintf(stderr, "DBG get_keyboard -> %p\n", (void *)kb);
        wl_keyboard_add_listener(kb, &kb_listener, NULL);
    }
    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        struct wl_pointer *p = wl_seat_get_pointer(seat);
        (void)p;
    }
}
static void seat_name(void *ud, struct wl_seat *seat, const char *name) {
    (void)ud; (void)seat; (void)name;
}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

/* ---- shm helper ---- */
static struct wl_buffer *make_buffer(struct wl_shm *shm, int w, int h) {
    int stride = w * 4, size = stride * h;
    int fd = memfd_create("wltest-shm", 0);
    if (fd < 0) return NULL;
    if (ftruncate(fd, size) < 0) { close(fd); return NULL; }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                      WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

/* ---- child = client ---- */
static int client_main(void) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "FAIL: wl_display_connect\n"); return 2; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    if (wl_display_roundtrip(dpy) < 0) { fprintf(stderr, "FAIL: roundtrip1\n"); return 3; }
    if (n_globals < 6 || !g_comp || !g_shm || !g_seat || !g_wm) {
        fprintf(stderr, "FAIL: globals=%d comp=%p shm=%p seat=%p wm=%p\n",
                n_globals, (void *)g_comp, (void *)g_shm, (void *)g_seat, (void *)g_wm);
        return 4;
    }
    struct wl_surface *sf = wl_compositor_create_surface(g_comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g_wm, sf);
    xdg_surface_add_listener(xs, &xdg_surf_listener, NULL);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &tl_listener, NULL);
    xdg_toplevel_set_title(tl, "wltest");
    xdg_toplevel_set_app_id(tl, "org.lolosedoze.wltest");

    struct wl_buffer *buf = make_buffer(g_shm, 64, 48);
    if (!buf) { fprintf(stderr, "FAIL: shm buffer\n"); return 5; }
    wl_surface_attach(sf, buf, 0, 0);
    wl_surface_damage_buffer(sf, 0, 0, 64, 48);
    struct wl_callback *fc = wl_surface_frame(sf);
    wl_callback_add_listener(fc, &frame_listener, NULL);
    wl_surface_commit(sf);
    wl_display_flush(dpy);   /* libwayland flushes lazily; force get_keyboard out */

    /* server pumps; wait for configure + frame + keymap (get_keyboard is
       marshalled when capabilities arrives — flush each round) */
    for (int i = 0; i < 200 && (!xdg_cfg_seen || !frame_seen || !keymap_ok); i++) {
        wl_display_flush(dpy);
        if (wl_display_dispatch(dpy) < 0) break;
    }
    if (!xdg_cfg_seen) { fprintf(stderr, "FAIL: no xdg configure\n"); return 6; }
    if (!frame_seen)   { fprintf(stderr, "FAIL: no frame.done\n"); return 7; }
    if (!keymap_ok)    { fprintf(stderr, "FAIL: xkb keymap not received/valid\n"); return 8; }

    wl_buffer_destroy(buf);
    xdg_toplevel_destroy(tl);
    xdg_surface_destroy(xs);
    wl_surface_destroy(sf);
    wl_display_flush(dpy);
    wl_display_disconnect(dpy);
    return 0;
}

int main(void) {
    setenv("XDG_RUNTIME_DIR", "/tmp", 1);
    setenv("WAYLAND_DISPLAY", SOCK, 1);   /* contains '/': used as path */
    unlink(SOCK);
    wlserver *srv = wlserver_create(SOCK);
    if (!srv) { fprintf(stderr, "FAIL: server create (%s)\n", SOCK); return 1; }

    pid_t pid = fork();
    if (pid == 0) _exit(client_main());

    int status = 0, reaped = 0;
    for (int i = 0; i < 400; i++) {
        wlserver_pump(srv);
        for (WlWin *w = wlserver_wins(srv); w; w = wlwin_next(w))
            if (w->mapped && w->px) wlserver_ack_paint(srv, w);   /* frame.done + buffer release */
        if (waitpid(pid, &status, WNOHANG) == pid) { reaped = 1; break; }
        usleep(10000);
    }
    if (!reaped) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        fprintf(stderr, "FAIL: client timed out\n");
        wlserver_destroy(srv);
        return 1;
    }
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 99;

    wlserver_destroy(srv);
    if (rc != 0) { fprintf(stderr, "FAIL: client rc=%d\n", rc); return rc; }
    printf("wltest: roundtrip, binds, shm buffer, xdg_toplevel, configure, frame, keymap all OK\n");
    printf("PASS\n");
    return 0;
}
