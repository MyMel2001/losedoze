# AGENT_NOTES.md — losedoze: spec, plan, working notes

## Spec (requirements, in priority order)

1. Hobby OS "losedoze": Linux kernel + ext4 + Busybox + GRUB bootloader.
2. Homemade Win32 compatibility — **NO Wine anywhere**. `w32host` (ELF) loads
   PE32+ executables and binds imports to our own ntdll/kernel32/user32/gdi32,
   implemented over Linux syscalls; GUI via hand-rolled Wayland client
   (`win/wlclient.c`, no libwayland-client). PE apps freestanding (clang
   `--target=x86_64-pc-windows-msvc`, `ld -mi386pep`). Windows 11-style desktop
   shell, legally distinct from Windows: original code/iconography, no
   Microsoft code. Reference syscall CSVs in `windows-syscalls/` (submodule)
   are notes only → `tools/gen_syscalls.py` + `win/gen_ntsyscalls.py` +
   `losyscall` CLI.
3. **Graphics stack from scratch (relatively speaking)** — NO Xorg, NO X11 libraries
   (no libX11/libXext/xkbcomp/XKB data), no libinput/xkbcommon in the shell.
   May use **libraries/interfaces from the Linux kernel**: DRM/KMS UAPI (raw ioctls),
   evdev (`/dev/input/event*`), fbdev, kernel GPU drivers.
4. **USB + PS/2 keyboard/mouse support from scratch** — userspace reads evdev raw;
   kernel in-tree drivers (atkbd/i8042, psmouse, usbhid/hid-generic) provide devices.
5. **NVIDIA/Intel/AMD accelerated graphics — NOT just framebuffer.**
   → KMS atomic modesetting + primary-plane page flips via GPU kernel drivers
   (i915/amdgpu/radeon/nouveau), plus EGL/GBM/GL ES2 compositing path (Mesa =
   the kernel's GPU userspace; dlopen'd, optional). fbdev only as last-resort fallback.
6. Pre-existing reference implementation: `/var/home/rocketbob/Code/iTouch-x86_64`
   (K93OS-x86, GPL-2.0). **DO NOT MODIFY IT.** Copy bits as needed
   (k93ui.c: drm_ioctl/prop resolution/atomic commit/flip, evdev open+mouse pump,
   scancode tables). uapi headers copied to `shell/uapi/` from its `build/uapi/`.
7. No user interaction until 100% finished. Reply terse, English.

## Key facts (verified on host)

- Host: Fedora Silverblue/Bazzite, **no root/sudo**, unprivileged builds only.
- Kernel: `7.2.8-ogc5.1.fc44.x86_64` (ostree). Modules tree:
  `/ostree/deploy/fedora/deploy/1988bd17.../usr/lib/modules/7.2.8-ogc5.1.fc44.x86_64`
  - Builtin: `CONFIG_INPUT_EVDEV=y`, `KEYBOARD_ATKBD=y`, `SERIO_I8042=y`, `MOUSE_PS2=y`,
    `USB_HID=y` (renamed from USBHID in this kernel!), `HID_GENERIC=y`, `HID=y`,
    `USB_XHCI/EHCI/UHCI_HCD=y`, `DRM=y`.
  - Modules present: `gpu/drm/tiny/bochs.ko`, i915, amdgpu, nouveau, radeon.
  - `virtio_gpu.ko` **MISSING** despite `CONFIG_DRM_VIRTIO_GPU=m` → no virtio-gpu in guest.
  - Firmware on host: `/usr/lib/firmware/{i915,amdgpu,radeon}` exist; **nouveau/ dir absent**
    → NVIDIA-via-nouveau firmware ceiling (note in README).
- QEMU guest GPU: `-vga std` (bochs-drm). bochs has no Mesa DRI driver → EGL on
  guest likely fails → CPU/kms path; selftest may show llvmpipe via render node if present.
- Host GPU: NVIDIA `10de:2504`, proprietary driver; Mesa EGL selftest skips gracefully.
- Mesa DRI drivers on host: i915, radeonsi, nouveau, virtio_gpu, swrast, kms_swrast.
- Host libs copied for runtime: `libwayland-client.so.0`, `libwayland-egl.so.1`,
  `libwayland-cursor.so.0`, `libxkbcommon.so.0`, `libxkbregistry.so.0`
  (deps of the hand-rolled Wayland client `win/wlclient.c` — verified via ldd).
- No Wine anywhere: Win32 GUI reaches our hand-rolled Wayland compositor via
  `win/wlclient.c` (no libwayland-server on host; protocol implemented by hand
  = forced + aligned with "from scratch").
- UAPI: host `/usr/include/drm/{drm.h,drm_mode.h}` exist; kernel `linux/types.h`,
  `linux/bits.h`, `linux/input.h`, `linux/fb.h` present. Shell vendors uapi copies
  (v7.2, matches guest kernel) in `shell/uapi/` + `#define __user` before include
  (k93 pattern).
- `xkbcomp` + `/usr/share/X11/xkb` on host → build-time keymap string generation for
  `wl_keyboard.keymap` (XKB_V1 fd). xkb keymap fd format == xkbcomp `-xkb` output.

## Architecture (final design)

```
shell/
  lost.h        shared types/API (Canvas, card, input, wlserver)
  uapi/         drm.h, drm_mode.h (copied from iTouch build/uapi, kernel v7.2)
  kms.c         DRM/KMS: open, caps, primary plane, MODE_ID blob, atomic full+fast flip,
                double dumb buffers, shadow canvas, fbdev fallback,
                EGL/GBM/GL ES2 via dlopen (GPU path), kms_selftest()
  input.c       evdev: input_open (grabs), input_is_pointer, input_read (keys+mouse,
                abs scaling), input_mods, input_key_char (US QWERTY tables from k93)
  wl.c          hand-rolled Wayland wire server: display/registry/compositor/shm/surface/
                seat/keyboard/pointer/output/data-device/subcompositor/xdg-shell(toplevel,
                popup, positioner) + embedded XKB keymap fd
  loseshell.c   desktop shell + WM (ported off X in place; see port table below)
  stb_truetype.h, uapi/...

win/
  pe.c          PE32+ loader: mmap image, zero .bss, base relocations, bind
                imports to the builtin DLL export tables (pe_register_dll)
  ntdll.c       NT syscall front-end; numbers from build/ntsyscalls.inc
  kernel32.c    files, stdio, process, module helpers over Linux
  user32.c      window classes, message loop, painting -> wlclient toplevels
  gdi32.c       software ARGB canvas + text metrics
  wlclient.c    hand-rolled Wayland client (xdg_toplevel + wl_shm only)
  w32host.c     host process: load the PE, register builtin DLLs, run entry
  apps/         freestanding PE32+ apps (hello.exe console, notes.exe GUI)
  gen_ntsyscalls.py, gen_importlibs.py   codegen (single source: the X-macros
                in w32host.c)
```

- Display: single framebuffer. `shadow` canvas: CPU path aliases scanout (k93-style
  direct draw + same-FB atomic recommit for vblank-paced flush); GPU path: separate
  malloc shadow → GL fullscreen passes (animated gradient wallpaper + full-canvas
  texture upload) → GBM front buffer → ADDFB2 → atomic flip.
- Composite order: wall → deskicons → client windows (frame border/title + content
  shm blit) → taskbar → popups (oldest→newest) → cursor sprite (k93 save/restore stamp
  for motion-only path).
- Input: evdev EVIOCGRAB → shell router: popup_kbd ? popup handler : global shortcuts
  (root_key ported to KEY_* codes + input_mods) else forward to wl focused client.
- Cursor: erase/stamp saved-rect (k93 cursor pattern) with embedded XBM arrow/hand/h/v
  arrays already in loseshell.c.
- Shell surfaces keep `.win` field as void* placeholder (avoids mass call-site edits);
  cv_put deleted → sw_draw marks composite.

## Port table for loseshell.c (X removal)

| Section | Fate |
|---|---|
| logmsg, theme, mix, glyphs/text, in_round/cv_* draw, util, cfg, apps, icons, panel, tray, time, taskbar, all draw_*/click_* widgets, menu_do, key actions, alt_build/commit, notify fifo, selftest | keep (X-free already); tiny edits as found |
| includes/X globals (dpy/root/gc/vis/depth) | delete → card/sw/sh from kms |
| init_masks/px | hardcode XRGB8888 masks (r16/g8/b0, pos8) |
| Canvas XImage wrapper | plain malloc buffer (keep `data` field → zero edits in cv_*) |
| cv_put | delete → composite() |
| make_cursors | delete → own sprite stamping (keep XBM arrays) |
| sw_apply_shape (XShape) | no-op (alpha-blend corners in composite) |
| sw_get/sw_resize/sw_show/sw_hide | drop X calls; popup/grab logic → input router |
| sw_find_win(Window) | delete |
| wallpaper_set (root pixmap) | draw into static full-screen `wall` canvas |
| atoms/EWMH/ewmh_init/update_client_list/set_*_state/round_win | delete/no-op |
| Client struct (Window frame,win) | `WlWin *wl` + frame geometry; get_title_class → wl title/appid |
| manage/unmanage/focus_client/desk_switch | port to wlserver calls |
| draw_frame_title (XFillRectangle) | canvas strip ops → blitted in composite |
| frame_event | rewrite on synthetic input structs (logic kept) |
| root_key/alt_key/popup_key | port KeySym switch → KEY_* + input_mods() |
| grabs_init/grab_key | delete (input_open grabs evdev) |
| dispatch(XEvent) | rewrite: hit-test router (surface/frame/content zones) |
| main poll loop | poll(input fds, wl fd, fifo) → dispatch → present() |

## Wayland server requirements (w32host / win/wlclient.c)

- Advertised globals: wl_compositor(4), wl_shm(1), wl_seat(5), wl_output(2),
  xdg_wm_base(1), wl_data_device_manager(3), wl_subcompositor(1).
- NOT advertised (win/wlclient.c does not need them): zwp_linux_dmabuf, viewporter,
  relative-pointer, cursor-shape, presentation-time, xdg-decoration (we draw SSD frames ourselves).
- wl_keyboard.keymap = format 1 (XKB_V1) + memfd with xkb string (build-time embed).
- wl_keyboard repeat_info: sent by server; w32host ignores it (no client-side repeat).
- Keycodes sent = evdev code (xkb keycode = evdev+8; consistent with embedded keymap).
- Modifier masks hardcoded: shift=0x1 caps=0x2 ctrl=0x4 alt=0x8 logo=0x40 num=0x10.
- Buffers: wl_shm only (ARGB8888/XRGB8888); release after composite copy;
  frame callbacks done at composite. Ceiling: client GL via linux-dmabuf not implemented.
- xdg_popup grab: basic (serial check + dismiss on outside click).
- Env for w32host: `XDG_RUNTIME_DIR=/run`, `WAYLAND_DISPLAY=losedoze0`.

## Build/image notes

- DONE: Xorg/xkbcomp/XKB-data/libX11/libXext/udev steps REMOVED from build.sh.
- **Mesa EGL exclusion (verified)**: Fedora `libEGL_mesa.so.0` hard-links
  `libX11.so.6`/`libxcb*` (EGL_X11 platform compiled in) → X ban excludes it.
  `libEGL.so.1` (glvnd), `libgbm.so.1`, dri drivers are X-free, but EGL can never
  load without the vendor lib. => NO Mesa in the image; kms.c dlopen degrades
  to "EGL: none" (CPU composite + DRM dumb buffers). GPU acceleration =
  in-tree KMS drivers (i915/amdgpu/nouveau/radeon/bochs): hw scanout, page
  flip, vblank pacing. Upgrade path (ponytail): X-free mesa build
  (mesa -Dplatforms=gbm, or once ban lifts) → drop-in, no code changes.
- build.sh now: wayland-client/egl/cursor + xkbcommon/xkbregistry (X-free,
  deps of win/wlclient.c verified via ldd), firmware {i915,amdgpu,radeon}
  (nouveau dir absent on host), module trees gpu/video/cec/lib + depmod -b,
  loseshell = loseshell.c+kms.c+input.c+wl.c, -lm -ldl only, keymap gen step
  (build-time xkbcomp; only shell/keymap.h ships).
- rcS: devtmpfs first; modprobe bochs/i915/amdgpu/nouveau/radeon (stop at first
  /dev/dri/card0); NO udevd (devtmpfs gives /dev/input/* + /dev/dri/*).
- session.sh: no Xorg — exports XDG_RUNTIME_DIR=/run, WAYLAND_DISPLAY=losedoze0,
  waits for /run/losedoze0 then prints `LOSEDOZE WAYLAND OK` + runs autostart.
- Smoke: serial markers `LOSEDOZE INIT OK`, `LOSEDOZE READY`, `EGL: <renderer|none>`,
  `LOSEDOZE WAYLAND OK`, `LOSEDOZE WIN32 OK`; QMP screendump: taskbar band + color variety
  + white-pixel count for the Win32 client window. `SMOKE_AUTOSTART=1` installs a
  login self-test (hello.exe console marker + notes.exe GUI); the release image ships
  without it (`build.sh` prints `=== done`, `rc=0`).
- Desktop file manager is native: `app_add("Files", "__files__")` + `files_open/
  draw_files/click_files/files_browse` in loseshell.c (the previous Wine Explorer
  port is gone).
- Host-side `wsprintf*`: GCC's `ms_abi` variadic support is broken (bogus register
  save area → garbage varargs, and clang refuses `va_start` in a Win64 function), so
  user32.c defines `W32_NO_VSPRINTF_DECL` and implements `wsprintfA/W` with an explicit
  arity (vararg #1 R8, #2 R9, #3+ stack) while apps keep the variadic prototypes.
- Compositor size contract: `xdg_send_configure` never writes the requested size;
  `wlwin_configure` stores it, `wlwin_set_activated` falls back to the client's current
  buffer size (else 1). A bogus `1x1` configure makes Win32 clients resize to nothing.
- tools/wltest.c = host-side protocol conformance test (real libwayland-client
  + wayland-scanner xdg-shell headers against wl.c; prints PASS). Run manually:
  gcc -Ishell -I<xdggen> tools/wltest.c shell/wl.c <xdg-protocol.c> $(pkg-config --cflags --libs wayland-client)
- Known quirks (fixed earlier, keep): no /dev/null before devtmpfs (no stderr redirects
  in early rcS); ro rootfs → tmpfs on /root /var /tmp /run; `mount -o loop`
  (CONFIG_BLK_DEV_LOOP_MIN_COUNT=0 → no loop nodes).
- Wire-protocol bugs wltest caught in wl.c (fixed; do not regress):
  header = word0 obj, word1 size<<16|op; fd args occupy ZERO stream bytes;
  create_pool stream = (id, size); wm_capabilities = xdg_toplevel op 3 (array);
  server must send initial xdg configure on first commit; keymap memfd lseek(0);
  pump loop must unlink dead clients (UAF); object id table grows dynamically.
- DRM bug found by guest smoke (kms.c, fixed): 2-call GETCONNECTOR — first call
  fills count_modes/count_props/count_encoders; second call MUST pass valid
  arrays for ALL three (libdrm pattern) or kernel copy_to_user(NULL) → EFAULT
  ("Bad address"). Same for GETRESOURCES (4 ptrs) and OBJ_GETPROPERTIES.

## Ceilings (ponytail) to document in README

- Mesa EGL absent by design (libEGL_mesa links libX11; X ban wins) → CPU composite
  + hardware KMS scanout/pageflip; `EGL: none`. Upgrade: X-free mesa build, drop-in.
- Client GL via linux-dmabuf: not implemented (shm only) → add when needed.
- Nouveau firmware dir missing on host → NVIDIA accel needs linux-firmware copy.
- No virtio-gpu module in guest kernel tree → QEMU uses bochs (CPU path); real HW
  gets i915/amdgpu/radeon/nouveau accel.
- Keypad keys not in shell keymap; key repeat for shell fields not implemented.
- wl_data_device clipboard: minimal (offer transfer implemented basic; drag-drop no-op).
- Mode setting: preferred mode only, no modeline selection.

## TODO

- [x] Gather facts (kernel config, modules, mesa, firmware, uapi)
- [x] kms.c compile + host selftest run
- [x] input.c compile
- [x] loseshell.c port off X — compiles + --selftest passes, no X symbols
- [x] wl.c hand-rolled Wayland server — tools/wltest.c PASS vs real libwayland-client
      (roundtrip, binds, shm buffer, xdg_toplevel, configure, frame, keymap)
- [x] Composite/cursor code in loseshell.c; event router; main loop
      (pump every iteration + xdg_popup composite pass + popup ack)
- [x] build.sh/rcS/session.sh/smoke.sh updates (X removed end-to-end)
- [x] Guest smoke → SMOKE PASS (markers + screendump incl. Win32 app window:
      white pixels in the notes.exe client area, status bar reads `chars: 0`)
- [x] README.md (feature table, build steps, legal-distinctness, ceilings)
- [x] Final report
