# AGENT_NOTES.md — losedoze: spec, plan, working notes

## Spec (requirements, in priority order)

1. Hobby OS "losedoze": Linux kernel + ext4 + Busybox + GRUB bootloader.
2. Homemade Win32 compatibility — **NO Wine anywhere**. `w32host` (x86-64 ELF) loads
   PE32+ executables and binds imports to our own ntdll/kernel32/user32/gdi32,
   implemented over Linux syscalls; GUI via hand-rolled Wayland client
   (`win/wlclient.c`, no libwayland-client). PE32 (i386) images run in the
   separate 32-bit host `w32host32` (static musl, built with zig) — a 64-bit
   process cannot execute 32-bit code, so each host refuses the other
   architecture and points at the right one. PE apps freestanding (clang
   `--target=x86_64-pc-windows-msvc`, `ld -mi386pep`; i386 via zig's
   `-target x86-windows-gnu`). Windows 11-style desktop
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
- QEMU guest GPU: `-vga std` (bochs-drm). bochs has no Mesa DRI driver → EGL/GBM screen
  creation fails on the guest too, but `losegl` passes via the surfaceless platform (llvmpipe)
  and `losvulkan` via lavapipe; the compositor composites on CPU. bochs-drm still has
  GEM/PRIME, so the guest Win32 client's dma-buf export/import works (`LOSEDOZE DMABUF GPU OK`).
- Host GPU: NVIDIA `10de:2504`, proprietary driver; Mesa EGL selftest skips gracefully.
- Mesa DRI drivers on host: i915, radeonsi, nouveau, virtio_gpu, swrast, kms_swrast.
- **X-free Mesa 26.2.4 built from source** (`tools/build-mesa.sh`, prefix=/usr, DESTDIR=/tmp/mesa/root):
  `-Dplatforms=wayland -Dgbm=enabled -Degl=enabled -Dgles2=enabled -Dopengl=true -Dglx=disabled
  -Dglvnd=disabled -Dgallium-drivers=llvmpipe -Dvulkan-drivers=swrast -Dshared-llvm=disabled`.
  Mesa 26 megadriver = `libgallium-26.2.4.so`, linked directly into libEGL (no `dri/` dir, no
  `swrast_dri.so`); only `gbm/dri_gbm.so` ships. Desktop GL *is* available via EGL
   (`-Dopengl=true`): `eglBindAPI(EGL_OPENGL_API)` + `EGL_OPENGL_BIT` pbuffer →
   `4.6 (Compatibility Profile) Mesa 26.2.4` (no libGL/libGLX/libOpenGL shipped; GL
   entry points come from `eglGetProcAddress` only). GLX is not built and not needed.
- **.NET runtime in the guest**: `/tmp/dotnet` (SDK) + `/tmp/dnapp/out` (published app) →
  `/opt/dotnet` + `losdotnet`. Linux .NET apps only (CoreCLR PE → NT layer deferred).
- Host DRM: only `/dev/dri/card1` + `/dev/dri/renderD128` (driver nvidia, 10DE:2504). PRIME export
  works (`export fd=4 handle=1`); dma-buf fd `mmap` works with PROT_READ only (PROT_WRITE → EACCES);
  `gbm_bo_import` fails ENOSYS here (no Mesa screen for the NVIDIA device) → compositor imports by
  mapping the client fd directly.
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
- Win32/GL facts: request no `EGL_CONTEXT_*_VERSION` attrs (3/2 → EGL_BAD_ATTRIBUTE
  12292); `glDisable(GL_DITHER)` before an 8-bit `glReadPixels` check (dither moved
  64,128 → 66,130); host Mesa for manual runs = `LD_LIBRARY_PATH=/tmp/mesa/root/usr/lib64`.
  `w32host` on the host connects to the session Wayland socket, so a GUI app can be
  run/observed without loseshell.
- PE facts: host binutils has `-mi386pep` (PE32+) and knows `i386pe` (PE32) emulation but
  ships no `ldscripts/i386pe.x`; **zig** (bundled clang + LLD) links PE32 and ships a
  static i386 musl → the 32-bit host + apps are built with it. ld emits no `.reloc` →
  distinct `--image-base` per DLL. The EXE's
  directory must be published (`main_image`) *before* import binding, and every DLL
  search dir entry must be `strdup`'d (mixing literals with `free()` aborts).
- 32-bit ABI: Win32 on i386 is stdcall (callee pops) on both sides, so `W32API`/
  `WINAPI`/`CALLBACK` → `__attribute__((stdcall))` when `__i386__`; `WPARAM`/`LPARAM`
  are 4 bytes there (per-arch typedefs). Variadic stays cdecl (stdcall is ignored on
  variadic): `wsprintfA/W` on i386 read the args with `va_arg` by walking the format
  (the x64 path keeps its fixed 8 slots). mingw callers hand the host a 16-byte
  aligned stack that plain GCC i386 code mis-models → the 32-bit host is built with
  `-mstackrealign` (misaligned `movaps` → SIGSEGV in `wsprintfA` otherwise).

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
  pe.c          dual-arch PE loader (PE32 + PE32+): mmap image, zero .bss,
                HIGHLOW/DIR64 base relocations, bind imports (builtins first,
                then on-disk DLLs on the search path), export dir by name or
                #ordinal, refcount + real unload, --inspect report; an image
                runs only in the host of its own architecture (cross-arch:
                parsed + relocated, imports not bound, run refused)
  ntdll.c       NT syscall front-end; numbers from build/ntsyscalls.inc
  kernel32.c    files, stdio, process, module helpers over Linux
                (LoadLibraryA/W, GetProcAddress, FreeLibrary)
  user32.c      window classes, message loop, painting -> wlclient toplevels
  gdi32.c       software ARGB canvas + text metrics; SwapBuffers -> gl32_swap
  gl32.c        opengl32.dll builtin: WGL + GL entry points via eglGetProcAddress
                (desktop GL compat over EGL, presentation via SwapBuffers)
  wlclient.c    hand-rolled Wayland client (xdg_toplevel + wl_shm + DMA-BUF)
  w32host.c     host process: load the PE, register builtin DLLs, run entry
                (--inspect: parse report); built twice: w32host (x86-64) and
                w32host32 (i386, static musl via zig) — each refuses the other
                architecture's images
  apps/         freestanding PE apps, built for both arches (hello.exe,
                notes.exe, dlltest.exe, gldemo.exe) + on-disk DLLs (dllmath.dll,
                dllstate.dll)
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
  xdg_wm_base(1), wl_data_device_manager(3), wl_subcompositor(1), zwp_linux_dmabuf_v1(8).
- NOT advertised: viewporter, relative-pointer, cursor-shape, presentation-time,
  xdg-decoration (we draw SSD frames ourselves).
- wl_keyboard.keymap = format 1 (XKB_V1) + memfd with xkb string (build-time embed).
- wl_keyboard repeat_info: sent by server; the compositor also **synthesizes repeats**
  (evdev/atkbd deliver none): 500 ms delay, 40 ms period, `RepeatState` in loseshell.c.
- Keycodes sent = evdev code (xkb keycode = evdev+8; consistent with embedded keymap).
- Modifier masks hardcoded: shift=0x1 caps=0x2 ctrl=0x4 alt=0x8 logo=0x40 num=0x10.
- Buffers: wl_shm (ARGB8888/XRGB8888) **or client DMA-BUF**; release after composite copy;
  frame callbacks done at composite. **linux-dmabuf v1 (ours, both ends in-tree)**: global #8, the
  `format` event (XRGB8888 `0x34325258`) is sent on bind; requests are
  `create_params(new_id)`, `add(fd, plane_idx, offset, stride)` (fd via SCM_RIGHTS) and
  `create(new_id wl_buffer, width, height)` — the client allocates the buffer id, because our flat
  object table cannot hold the upstream 0xff000000 server-id range; the server replies
  `created(buffer)` (op 0) or `failed` (op 1) on the params object; `create` on an
  already-existing buffer id **refreshes it in place** (munmap the old mapping). Client side: gbm bo
  (`GBM_FORMAT_XRGB8888` + `GBM_BO_USE_LINEAR`) + `gbm_bo_map` + `DRM_IOCTL_PRIME_HANDLE_TO_FD`;
  the export is **done on every commit** (dumb-buffer PRIME export copies at export time, so a
  repaint needs a re-export; zero-copy devices re-import the same memory), with a cheap export
  probe at allocation time to decide dmabuf-vs-shm before any paint; shm fallback on any failure,
  reported once on stderr (`wlclient: dma-buf unavailable (...) - wl_shm fallback` /
  `wlclient: dma-buf buffer in use (client GPU memory)`). Compositor side: `mmap(fd, stride*h,
  PROT_READ)` + best-effort `DRM_IOCTL_PRIME_FD_TO_HANDLE` proof; FNV-1a pixel sum logged as
  `dmabuf: import WxH stride=S sum=X` (verification hook, compared by `tools/dmabuf_test.sh`).
- xdg_popup grab: basic (serial check + dismiss on outside click).
- Env for w32host: `XDG_RUNTIME_DIR=/run`, `WAYLAND_DISPLAY=losedoze0`.

## Build/image notes

- DONE: Xorg/xkbcomp/XKB-data/libX11/libXext/udev steps REMOVED from build.sh.
- **X-free Mesa now shipped** (replaces the old "no Mesa" ceiling): Fedora's
  `libEGL_mesa.so.0` hard-links `libX11.so.6` (EGL_X11 platform), so we build
  Mesa 26.2.4 from source instead (`tools/build-mesa.sh`; step 5/13). It gives
  the guest EGL + GLES2 + desktop GL and Vulkan (lavapipe). Built with the HW
  gallium drivers that match the shipped kernel drivers (`llvmpipe,nouveau,
  radeonsi`) **and `-Dshared-llvm=disabled`** (static LLVM inside
  `libgallium-26.2.4.so`) — the image must not depend on a libLLVM.so that is
  not on the host distro. Consequence: the Mesa runtime deps to copy are
  libvulkan/libdrm/libexpat/libzstd/libstdc++/libgcc_s/libSPIRV-Tools **plus
  libelf + libdrm_amdgpu** (radeonsi/nouveau in libgallium). The *display* path
  is still DRM/KMS (hardware scanout + page flip) with CPU compositing, because
  `card_egl_init`'s dri2 GBM screen creation has no gallium driver for
  bochs-drm/virtio-gpu (that needs virglrenderer); the EGL path is proven by the
  guest `losegl` probe on the surfaceless platform.
- **GPU**: in-tree DRM/KMS modules ship in the image (`kernel/drivers/gpu` plus
  the `virtio` bus for virtio-gpu/virtio-net), loaded in rcS with no early
  break; `tools/losgpu.c` prints the card + the bound kernel driver
  (`LOSEDOZE GPU OK card0=virtio_gpu`). `SMOKE_GPU=virtio` switches QEMU to
  `-device virtio-vga`, default stays bochs (`-vga std`).
- **.NET desktop**: the Win32 layer is also built as `libwin32.so`
  (`win/hostlib.c`, `-DW32_ABI_SYSV` because CoreCLR P/Invoke on Linux is SysV,
  not ms_abi); `tools/dotnet/Losedoze.Desktop` is a managed app that P/Invokes
  RegisterClassExW/CreateWindowExW + GDI and runs the message loop → a real
  window in the compositor (`losnetdesktop` → `LOSEDOZE .NET DESKTOP OK`).
- **.NET WinForms**: the managed framework lives in `tools/dotnet/System.Windows.Forms`
  (assembly `Losedoze.Windows.Forms`, namespaces `System.Windows.Forms` +
  `System.Drawing`). It is *our own* WinForms on top of `libwin32.so`: `Form`
  creates a native top-level window via `CreateWindowExW`, an owner-drawn
  `Control`/`ControlCollection` hierarchy (Button/Label/TextBox/CheckBox/RadioButton/
  ListBox/Panel, DataGridView, WebBrowser), modal file dialogs, the designer
  lifecycle API (SuspendLayout/ResumeLayout/PerformLayout/AutoScaleMode for
  partial-class `InitializeComponent`), DC clipping (SaveDC/RestoreDC/
  IntersectClipRect/GetClipBox applied per child in `Control.PaintTree`),
  scalable fonts (`Font.Size` → `SetFontSize` → stb_truetype), `WM_SIZE`
  resize routing, keyboard via WM_KEYDOWN/WM_CHAR routing to the focused control,
  mouse via WM_* hit-test through `Control.HitTest` recursion, GDI painting
  (`FillRect`/`Rectangle`/`DrawString`/`GetTextExtentPoint32W` + black-brush fix),
  and desktop GL (`GlContext` + `wglGetProcAddress`, EGL-backed via `win/gl32.c`
  linked into the shared library).
  The demo `tools/dotnet/Losedoze.WinForms` (`loswinforms`) paints a form and
  prints `LOSEDOZE WINFORMS OK <version>` after the first WM_PAINT
  (`LOSEDOZE WINFORMS FAIL (no paint)`); smoke waits on that marker
  (`tools/smoke.sh`). No Wine, no Microsoft Windows Desktop runtime — the
  managed types are ours; only the runtime is Microsoft's CoreCLR.
- **Network**: NIC/wifi modules (e1000, e1000e, virtio-net, r8169, cfg80211,
  mac80211, iwlwifi, ath10k) + firmware in `/lib/firmware`; no udev → rcS
  coldplugs by PCI modalias; `rootfs-overlay/usr/local/bin/losenet`
  (busybox `ip`/`udhcpc`) brings the interface up + DHCP →
  `LOSEDOZE NET OK eth0=…`; `/etc/resolv.conf` → `/run/resolv.conf` (ro root).
- **Settings**: `tools/loseset.c` (native, linked against libwin32.so) edits
  `~/.config/losedoze.conf` (network/wifi, wallpaper, theme, brightness);
  loseshell polls the file mtime (2 s) and reloads it live.
- **Client DMA-BUF** (see "Wayland server requirements"): host end-to-end check
  `LD_LIBRARY_PATH=/tmp/mesa/root/usr/lib64 sh tools/dmabuf_test.sh` →
  `DMABUF PASS: compositor read both client GPU frames`. The guest exercises it too:
  bochs-drm has GEM/PRIME → the guest smoke reports `LOSEDOZE DMABUF GPU OK`; a device
  without export makes the client fall back to shm (`LOSEDOZE DMABUF SHM OK`).
- build.sh now: wayland-client/egl/cursor + xkbcommon/xkbregistry (X-free,
  deps of win/wlclient.c verified via ldd), firmware {i915,amdgpu,radeon}
  (nouveau dir absent on host), module trees gpu/video/cec/lib + depmod -b,
  loseshell = loseshell.c+kms.c+input.c+wl.c, -lm -ldl only, keymap gen step
  (build-time xkbcomp; only shell/keymap.h ships).
- rcS: devtmpfs first; modprobe bochs_drm/virtio_gpu/i915/xe/amdgpu/nouveau/radeon
  (no early break: every candidate, the first to bind a card wins); PCI coldplug by
  modalias (no udevd — devtmpfs gives /dev/input/* + /dev/dri/*); `losenet up` in
  the background (DHCP does not block boot).
- session.sh: no Xorg — exports XDG_RUNTIME_DIR=/run, WAYLAND_DISPLAY=losedoze0,
  waits for /run/losedoze0 then prints `LOSEDOZE WAYLAND OK` + runs autostart.
- Smoke: serial markers `LOSEDOZE INIT OK`, `LOSEDOZE NET OK`, `LOSEDOZE READY`,
  `EGL: <renderer|none>`, `LOSEDOZE WAYLAND OK`, `LOSEDOZE WIN32 OK`,
  `LOSEDOZE GPU OK`, `LOSEDOZE .NET DESKTOP OK`, `LOSEDOZE SETTINGS OK`; QMP
  screendump: taskbar band + color variety + client-drawn pixels counted over the
  whole desktop (white canvas of the Win32 client window + orange row of the
  settings app) — the later client windows stack on top of notes.exe, so a fixed
  rect would be covered; counting over the whole desktop is state-based, not
  timing-based. The guest console is streamed over a unix socket, not `-serial file:`:
  QEMU buffers a file chardev in 4 KB chunks, so the smoke would only see the
  markers at the end of the boot and the screendump would land after the Win32
  window was covered by the later apps (settings, .NET desktop). A FIFO does not
  work: QEMU opens the pipe read/write and steals half the console output.
  `SMOKE_AUTOSTART=1` installs a
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

## Disk installer (losinstall)

- `tools/losinstall.c` — native GUI over `libwin32.so` (same pattern as
  `tools/loseset.c`), plus a headless mode `--yes --device=DEV --swap=MB`
  (used by the smoke), `--list` (block devices) and `--selftest` (host-side
  MBR/boot-sector check on a file — the build runs it, so a broken layout
  fails the build).
- Layout it writes: MBR (DOS) table, p1 = ext4 (0x83) at LBA 2048 labelled
  `LOSEDOZE`, p2 = swap (0x82). The MBR is written by the installer itself
  (512 bytes, C) + `BLKRRPART`; the kernel then creates /dev/sdN1/2.
- Bootloader: **GRUB i386-pc** — `boot.img` = first 446 bytes of GRUB's
  boot sector (the partition table behind it stays intact), `core.img` in the
  gap after the MBR. Both are built at build time with
  `grub2-mkimage -O i386-pc -c embedded.cfg -p /boot/grub ext2 part_msdos
  biosdisk configfile search search_label linux serial terminal normal boot`
  and shipped in `/usr/lib/losedoze/installer/`. The embedded config is
  `search --label LOSEDOZE --set=root` + `configfile ($root)/boot/grub/grub.cfg`
  → the installed system finds its own root without a `root=` parameter.
- Installed root discovery (initramfs): scan `/sys/block/*` (skip loop/sr/dm/…),
  for each partition read the ext4 superblock magic `0xEF53` at file offset 1080
  and the label at 1144; the partition labelled `LOSEDOZE` is the root.
  No udev, no `root=`, no UUID symlinks.
- The live root is a **read-only** loop mount of `rootfs.ext4` on the ISO:
  the installer must not write there. All work dirs are tmpfs
  (`/tmp/losinstall-{target,src,root}`); `/mnt` does not exist on the live
  root and cannot be created → the first attempt failed with ENOENT at mount.
- Pitfall: the image has **busybox symlinks** for `mke2fs`, `mount`, `tar`,
  `cp` in /bin and /usr/bin. busybox's `mke2fs` is ext2 only (no `-t ext4`)
  and busybox ash's default PATH is `:/bin:/usr/bin`, so a bare `mke2fs` can
  hit the wrong binary. The installer calls absolute paths
  (`/usr/sbin/mke2fs` = e2fsprogs, `/bin/mkswap`, `/bin/mount`, `/bin/tar`) and
  sets PATH itself. e2fsprogs' `mke2fs` + its libs + `/etc/mke2fs.conf` are
  shipped (busybox's mke2fs cannot create ext4).
- The install source is the pristine `rootfs.ext4` on the ISO (loop-mounted
  read-only) → the ISO must be in the drive to install. The target gets a
  tar copy of that rootfs, the kernel and initramfs under /boot, a grub.cfg
  and `/etc/losedoze/installed` (the marker that turns the live-media
  behaviour into installed behaviour: /var and /root stay on the disk,
  `swapon -a` runs).
- `/etc/fstab` on the installed system uses **device paths** (`/dev/sda1`):
  no udev means no `UUID=`/`LABEL=` resolution.
- Smoke: `SMOKE_INSTALL=1` adds a second disk (`-hda`, 2 GB sparse), the live
  system installs itself onto it, the host checks the disk layout (MBR, ext4
  magic + label, swap header, GRUB boot sector + core image), the installed
  disk then boots **without the ISO** (initramfs finds the root by label,
  swap shows in /proc/meminfo, a file written in the first installed boot
  survives the next reboot).

## Ceilings (ponytail) to document in README

- Compositor EGL-GBM screen creation still uses the CPU path for the QEMU GPUs
  (dri2 has no gallium driver for bochs-drm/virtio-gpu — that needs virglrenderer;
  the surfaceless platform works and is used by `losegl`). Real Intel/AMD/NVIDIA
  cards get their driver now (nouveau/radeonsi in libgallium). CPU compositing is
  the working path; a matching gallium driver makes `card_egl_init` take over
  with no shell changes.
- Desktop GL comes through EGL, not GLX (`-Dglx=disabled`, no libGL/libGLX/libOpenGL
  in the image): `eglBindAPI(EGL_OPENGL_API)` + `EGL_OPENGL_BIT` pbuffer on the
  surfaceless platform → Mesa hands out `4.6 (Compatibility Profile)`. Passing
  `EGL_CONTEXT_MAJOR/MINOR_VERSION` 3/2 fails (EGL_BAD_ATTRIBUTE, 12292) → request
  no version attrs. `win/gl32.c` (opengl32.dll builtin) resolves every GL entry point
  with `eglGetProcAddress`; `kms.c` still composites on CPU (ES2 only).
- 32-bit x86 (PE32) images are parsed, relocated and imports-bound, then refused at
  run time (`pe_refuse()`); the fixture `build/winapps/hello32.exe` is *generated*
  (`tools/gen_pe32.py`) because host binutils knows `i386pe` emulation but ships no
  `ldscripts/i386pe.x` (and there is no lld/mingw). Fixture ImageBase=0x1000 (below
  `vm.mmap_min_addr`) so the HIGHLOW rebase path always runs.
- ld `-mi386pep` emits no `.reloc` section → a DLL can only load at its preferred
  ImageBase; each on-disk DLL gets a distinct `--image-base` (`0x180000000` /
  `0x181000000`) so the loader never has to rebase one.
- Guest DMA-BUF works: bochs-drm (kernel 7.2.8) has GEM/PRIME → the guest smoke
  logs `LOSEDOZE KMS: dmabuf: PRIME import ok handle=N` + `dmabuf: import WxH
  stride=S sum=X` and the marker `LOSEDOZE DMABUF GPU OK`; a device without export
  makes the client fall back to shm (`LOSEDOZE DMABUF SHM OK`). Copy-on-export
  devices snapshot at export time → the client re-exports on every commit and the
  server re-imports into the same buffer id (both frames checked by dmabuf_test.sh).
- .NET PE apps via the Win32 layer need a real NT layer (CoreCLR) → deferred.
- .NET *desktop* = our own Win32 layer as `libwin32.so` (SysV ABI for P/Invoke),
  not Microsoft's: WinForms/WPF are Windows-only binaries → out of reach, so
  losedoze ships a **managed WinForms framework** instead
  (`tools/dotnet/System.Windows.Forms`, assembly `Losedoze.Windows.Forms`,
  `System.Windows.Forms` + `System.Drawing` namespaces: Form/Control/Application/
  MessageBox/SystemColors/Button/Label/TextBox/CheckBox/RadioButton/ListBox/Panel,
  plus DataGridView, WebBrowser, modal OpenFileDialog/SaveFileDialog/
  FolderBrowserDialog, GlContext, and the designer lifecycle API
  SuspendLayout/ResumeLayout/PerformLayout/AutoScaleMode for partial-class
  `InitializeComponent`). The six former ceilings are fixed: generated
  code-behind works against the layout API (partial classes are plain C#; a
  `.resx` ComponentResourceManager is still not read), DataGrid/WebBrowser/
  dialogs exist, children are clipped to their bounds (DC clip +
  SaveDC/RestoreDC/IntersectClipRect/GetClipBox, applied by Control.PaintTree),
  fonts scale (`Font.Size` -> `SetFontSize` -> stb_truetype pixel height),
  `WM_SIZE` is posted by `user32_configure_event` and adopted by `Form`
  (resize works), and the shared library links `win/gl32.c` so managed callers
  get the same EGL-backed WGL/GL entry points + `wglGetProcAddress` (presented
  by `SwapBuffers`). Still architectural, not a ceiling: controls are
  owner-drawn by the Form (no child HWNDs), one font family (Liberation Sans),
  `Form.GetWindowRect` reports the content origin (decorations are
  compositor-side).
- No `iris` (Intel gallium): its SPIR-V shaders are built by `mesa_clc`, which
  aborts ("double free or corruption") in a static-LLVM build. Intel keeps KMS
  modesetting through i915/xe; add `iris` back when mesa_clc is fixed.
- WiFi needs hardware: QEMU emulates no WiFi device, so the guest can only show
  that the driver+firmware stack is there; joining needs a real adapter
  (`losenet wifi <ssid> <psk>`). Ethernet is proven end-to-end (QEMU e1000).
- wsprintf: 8 vararg slots (MS x64 variadic ABI; no va_start on clang → explicit arity).
- Nouveau firmware dir missing on host → NVIDIA accel needs linux-firmware copy.
- virtio-gpu has no virgl → QEMU `-device virtio-vga` gets KMS modesetting but
  renders through llvmpipe/swrast.
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
- [x] X-free Mesa 26.2.4 in the image (EGL/GLES2 llvmpipe + Vulkan lavapipe; guest probes)
- [x] .NET runtime in the image (`losdotnet` → LOSEDOZE DOTNET OK)
- [x] Key repeat (compositor-synthesized: 500 ms / 40 ms) + wsprintf (ms_abi variadic, 29/29)
- [x] Client DMA-BUF: linux-dmabuf v1 (both ends) + host end-to-end test (`tools/dmabuf_test.sh`,
      two frames so the per-commit re-export is checked) + guest evidence (`LOSEDOZE DMABUF GPU OK`)
- [x] Guest smoke → SMOKE PASS (INIT/READY/EGL/WAYLAND/WIN32/DMABUF/GL/VULKAN/DOTNET + screendump)
- [x] Desktop GL (this task): `eglBindAPI(EGL_OPENGL_API)` + `EGL_OPENGL_BIT` pbuffer,
      no version attrs → 4.6 compatibility profile; `tools/losegl.c` second phase
      (`LOSEDOZE GL DESKTOP OK <renderer> | <version> | EGL 1.5 | readback 64,128,255,255`)
      + `glDisable(GL_DITHER)` (dither shifted the 8-bit readback to 66,130)
- [x] `opengl32.dll` builtin (`win/gl32.c`, GL32_LIST in w32host, `libopengl32.a` from
      gen_importlibs.py): WGL (CreateContext/MakeCurrent/GetCurrentContext/SwapBuffers)
      + GL entry points via `eglGetProcAddress`; presented by `gdi32!SwapBuffers` →
      `gl32_swap()` (one `glReadPixels` pass, Y-flip + RGBA→ARGB swizzle)
- [x] On-disk PE DLLs (`win/pe.c`): search path (EXE dir → `LOSEDOZE_DLL_PATH` →
      `/opt/losedoze/{lib,apps}`, all `strdup`'d), refcount + real unload
      (`pe_release` → DllMain detach → munmap), exports by name and `#ordinal`,
      imports fall back builtin → on-disk; `LoadLibraryA/W`+`GetProcAddress`+`FreeLibrary`
      in kernel32; apps `dllmath.c`/`dllstate.c` (+.def) and `dlltest.c` probe
      (`LOSEDOZE DLL static=1 dynamic=1 unload=1 handle=1 missing=1`)
- [x] Dual-arch PE loader: PE32 (i386) opt+28/+96/+92 vs PE32+ opt+24/+112/+108,
      HIGHLOW(3)+DIR64(10) relocs, 32-bit IAT thunks; `pe_entry()` returns NULL for
      bits==32 → clear refusal message; `pe_inspect()` (w32host --inspect) prints the
      parse report + `LOSEDOZE PE32 PARSE OK <path>`; export dir maps
      +28/+32/+36 → AddressOfFunctions/Names/NameOrdinals (fix: were swapped)
- [x] Build/autostart/smoke: DLL + PE32 + GL-demo probes (markers `/dev/console`,
      app logs catted to console), `tools/smoke.sh` waits
      DLL OK / PE32 PARSE OK / GL DEMO OK / GL DESKTOP OK, fail regex broadened to
      `LOSEDOZE [A-Z0-9 ]*FAIL`
- [x] Evidence (host): `w32host --inspect hello32.exe` parse+refusal rc=1; dlltest.exe
      `LOSEDOZE DLL OK`; gldemo.exe `LOSEDOZE GL DEMO OK llvmpipe (LLVM 23.1.3, 256 bits) |
      4.6 (Compatibility Profile) Mesa 26.2.4`; losegl both markers. Regressions:
      `loseshell --selftest` OK, `tools/dmabuf_test.sh` DMABUF PASS
- [x] Evidence (guest): SMOKE PASS with INIT/READY/EGL/AUTOSTART/WAYLAND/WIN32/DMABUF/
      DLL OK/PE32 PARSE OK/GL DEMO OK/GL OK/GL DESKTOP OK/VULKAN/DOTNET, framebuffer
      1280x800 colors=1082 taskbar=True white=8387
- [x] GPU drivers: in-tree DRM/KMS modules in the image (bochs_drm, virtio_gpu,
      i915, xe, amdgpu, nouveau, radeon + the virtio bus), rcS loads every
      candidate (no early break), PCI coldplug by modalias, guest probe
      `tools/losgpu.c` → `LOSEDOZE GPU OK card0=…`; Mesa rebuilt with the matching
      HW gallium drivers (`llvmpipe,nouveau,radeonsi`, static LLVM → no libLLVM dep)
- [x] .NET desktop: the Win32 layer as `libwin32.so` (SysV ABI for P/Invoke) +
      `tools/dotnet/Losedoze.Desktop` (managed window, GDI painting) →
      `LOSEDOZE .NET DESKTOP OK`
- [x] .NET WinForms: `tools/dotnet/System.Windows.Forms` (`Losedoze.Windows.Forms`
      assembly) — Form over `CreateWindowExW`, Control tree owner-drawn by the
      Form over GDI, WM_KEYDOWN/WM_CHAR → focused control, WM_* hit-test through
      `Control.HitTest`, `System.Drawing` Graphics/Brush/Pen/Font +
      `GetTextExtentPoint32W` (black-brush FillRect/Rectangle fix), plus the
      fixed ceilings: DataGridView/WebBrowser/dialogs, designer lifecycle API,
      DC clipping, scalable fonts, WM_SIZE resize, EGL desktop GL via
      `GlContext`/`SwapBuffers` (win/gl32.c in libwin32.so); demo
      `tools/dotnet/Losedoze.WinForms` (`loswinforms`) → `LOSEDOZE WINFORMS OK`;
      smoke waits on it; losnetdesktop switched to the library (no duplicate
      Form/Button)
- [x] Network: NIC + wireless modules and firmware, modalias coldplug,
      `losenet` (busybox ip/udhcpc) → `LOSEDOZE NET OK eth0=…`; QEMU e1000 NIC
- [x] Settings: `loseset` (native GUI over libwin32.so) writes
      `~/.config/losedoze.conf`; loseshell reloads it on mtime change (live)
- [x] Disk installer: `losinstall` (native GUI over libwin32.so) → MBR + ext4
      (`LOSEDOZE` label) + swap on the target, tar copy of the live rootfs,
      GRUB i386-pc boot sector + core image; the initramfs finds the installed
      root by its ext4 label, /var stays on the disk, swap comes up from
      /etc/fstab. Evidence: `SMOKE_INSTALL=1` → host-side disk layout check +
      two boots of the installed disk (no ISO) + a file that survives a reboot
- [x] Final report
