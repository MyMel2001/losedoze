# losedoze

A hobby desktop operating system: Linux kernel + ext4 + Busybox + GRUB, a
from-scratch desktop shell ("loseshell"), and a homemade Win32 compatibility
subsystem (PE32+ loader + own ntdll/kernel32/user32/gdi32 over Linux syscalls —
no Wine anywhere) with a Windows-11-style — but entirely original and legally
distinct — desktop.

```
GRUB → Linux kernel (7.2.8) → Busybox init → loseshell (DRM/KMS compositor)
                                                     │
                              /run/losedoze0 (Wayland socket: hand-rolled server + client)
                                                     │
                w32host (PE32+ loader, builtin Win32 DLLs) → Win32 PE apps
                    │                              └─ losdotnet → .NET app (/opt/dotnet)
                    └─ shm or client DMA-BUF (linux-dmabuf v1)
```

## Features

| Area | What it is |
|---|---|
| Boot | GRUB2 → kernel + initramfs (loop+isofs) → ext4 rootfs on the ISO |
| Desktop | loseshell: wallpaper, desktop icons, taskbar, clock/calendar, start menu, run box, toasts, a native file manager (Files), window manager (move/resize/snap/maximize/minimize, 4 virtual desktops, Alt-Tab), lock screen, dark/light themes — all original code and artwork |
| Graphics | Raw DRM/KMS ioctls from scratch: atomic modesetting, hardware scanout planes, page flip, vblank pacing. GPU drivers: bochs (QEMU), i915 (Intel), amdgpu/radeon (AMD), nouveau (NVIDIA). CPU compositing into DRM dumb buffers, fbdev fallback |
| GL / Vulkan | Mesa 26.2.4 **built from source, X-free** (`-Dplatforms=wayland -Dglx=disabled -Dglvnd=disabled -Dgallium-drivers=llvmpipe -Dvulkan-drivers=swrast`; reproducer `tools/build-mesa.sh`): EGL + OpenGL ES 2.0 (llvmpipe) and Vulkan (lavapipe) ship in the image; guest probes `losegl` / `losvulkan` print `LOSEDOZE GL OK` / `LOSEDOZE VULKAN OK` |
| Input | Raw evdev (`EVIOCGRAB`), PS/2 + USB keyboards/mice via in-tree kernel drivers; US QWERTY tables; keymap shipped to Wayland clients as XKB_V1; the compositor **synthesizes key repeat** (500 ms delay / 40 ms period — evdev/atkbd deliver none) |
| Display server | Hand-rolled Wayland compositor (no libwayland-server): wl_compositor/shm/seat/keyboard/pointer/output/xdg-shell (toplevel + popup) + **linux-dmabuf v1** (client-allocated buffer ids), wire protocol implemented byte-by-byte in `shell/wl.c` |
| Win32 compat | Homemade subsystem (`win/`): `w32host` (ELF) loads PE32+ executables, binds imports to our own ntdll/kernel32/user32/gdi32 implemented over Linux syscalls, and renders through a hand-rolled Wayland client (`win/wlclient.c`, no libwayland-client). PE apps built freestanding with clang (`--target=x86_64-pc-windows-msvc`, `ld -mi386pep`); NT syscall numbers (from public reference CSVs) compiled into `build/ntsyscalls.inc` and the `losyscall` CLI |
| .NET | A Linux .NET runtime (muxer + `Microsoft.NETCore.App` + `host/fxr`) lives in the guest at `/opt/dotnet`; `losdotnet` runs the bundled demo app → `LOSEDOZE DOTNET OK` |
| Client DMA-BUF | The Win32 client allocates its window buffer as a GBM bo, exports it as a dma-buf fd (`DRM_IOCTL_PRIME_HANDLE_TO_FD`) and hands it over with `zwp_linux_dmabuf_v1`; the compositor imports it (`mmap` + PRIME proof) and composites the client's GPU memory directly; shm fallback on any failure |
| Verification | `loseshell --selftest`, `kms` host selftest, `tools/wltest.c` protocol test against real libwayland-client, `tools/dmabuf_test.sh` (host end-to-end dma-buf: the pixel checksum the client wrote is compared against the one the compositor computed from its own `mmap` of the buffer), `tools/test_wsprintf.c` (29/29), QEMU smoke test (`tools/smoke.sh`): console markers `LOSEDOZE INIT OK` / `READY` / `EGL:` / `WAYLAND OK` / `WIN32 OK` / `GL OK` / `VULKAN OK` / `DOTNET OK`, framebuffer screendump (taskbar band, color variety, white pixels of the Win32 client window) → `SMOKE PASS` |

## What is NOT here (by design)

* **No X11 anywhere.** No Xorg, no libX11/Xext/xkbcomp/XKB data in the image,
  no libinput, no xkbcommon in the shell. Everything display/input related is
  DRM/KMS + evdev + the hand-rolled Wayland server above.
* **No distro Mesa.** Fedora's `libEGL_mesa` hard-links `libX11` (compiled-in
  EGL_X11 platform), so it cannot ship in an X-free image. Instead the image
  ships an **X-free Mesa 26.2.4 built from source** (`tools/build-mesa.sh`) for
  EGL/GLES2/Vulkan; the *display* path itself stays DRM/KMS (hardware scanout +
  page flip) with CPU compositing. See Ceilings.
* **No Wine.** The Win32 compatibility layer is written from scratch: our own
  PE32+ loader, syscall dispatch, and windowing/GL-free graphics over Linux
  syscalls + the hand-rolled Wayland client in `win/wlclient.c`.
* **No Microsoft code.** The Windows look-and-feel is reimplemented: original
  widgets, icons, sounds, and layout. Public syscall-number CSVs were used
  only as numeric reference data for `losyscall` and `build/ntsyscalls.inc`.

## Building

Host requirements: Fedora-like x86_64 host with the guest kernel + module tree,
busybox static binary, clang + `ld` with `-mi386pep` + `llvm-lib` (PE32+
toolchain), `grub2-mkrescue`, `mke2fs`, `cpio`, `xkbcomp` (build-time only,
for the embedded keyboard map), no root required.

```sh
# optional overrides (defaults shown)
export KVER=7.2.8-ogc5.1.fc44.x86_64
export KIMG=/boot/ostree/.../vmlinuz-$KVER
export MODSRC=/ostree/deploy/fedora/deploy/.../usr/lib/modules/$KVER
export BUSYBOX=/tmp/busybox
export PE_CC=clang
export PE_LD=ld
export PE_LIB=llvm-lib
# X-free Mesa stack (skip step 5 if unset) and .NET runtime (skip step 9 if unset):
#   MESA_ROOT=/tmp/mesa/root   (build it: sh tools/build-mesa.sh)
#   DOTNET_ROOT=/tmp/dotnet DOTNET_APP=/tmp/dnapp/out

./build.sh                    # → build/losedoze.iso
SMOKE_AUTOSTART=1 ./build.sh  # dev image: Win32 self-test on login
./tools/smoke.sh              # headless QEMU boot + screendump check
```

Host-side checks that need no ISO build:

```sh
gcc -O2 -o /tmp/loseshell shell/loseshell.c shell/kms.c shell/input.c shell/wl.c -lm -ldl
/tmp/loseshell --selftest
gcc -O2 -Wall -o /tmp/test_wsprintf tools/test_wsprintf.c win/wsprintf.c -lm && /tmp/test_wsprintf
# Wayland protocol conformance (needs libwayland-client + wayland-scanner xdg-shell):
sh tools/gen_keymap.sh shell/keymap.h
# Client DMA-BUF end-to-end (host DRM device needed; MESA_ROOT must be on LD_LIBRARY_PATH):
LD_LIBRARY_PATH=/tmp/mesa/root/usr/lib64 sh tools/dmabuf_test.sh
```

## Layout

```
build.sh                 image build (rootfs.ext4 → initramfs → ISO)
grub/                    grub.cfg
tools/initramfs-init     stage-1 init (mounts the ext4 rootfs, switch_root)
rootfs-overlay/          etc/inittab, rcS, session.sh, profile
shell/                   loseshell: lost.h, kms.c, input.c, wl.c, loseshell.c
tools/gen_syscalls.py    NT syscall CSV → build/syscalls.inc
tools/losyscall.c        syscall database CLI (losyscall)
win/                     homemade Win32 subsystem: pe.c (PE32+ loader),
                         ntdll.c/kernel32.c/user32.c/gdi32.c (over Linux),
                         wlclient.c (hand-rolled Wayland client: shm + DMA-BUF),
                         wsprintf.c (ms_abi variadic formatter), w32host.c
                         (host process), apps/ (hello, notes),
                         gen_ntsyscalls.py/gen_importlibs.py (codegen)
tools/gen_keymap.sh      build-time XKB keymap → shell/keymap.h
tools/build-mesa.sh      X-free Mesa 26.2.4 reproducer (llvmpipe + lavapipe)
tools/losegl.c           guest EGL/GLES2 probe (→ LOSEDOZE GL OK)
tools/losvulkan.c        guest Vulkan/lavapipe probe (→ LOSEDOZE VULKAN OK)
tools/dmabuf_client.c    host test client for the client DMA-BUF path
tools/dmabuf_test.sh     host end-to-end dma-buf check (pixel checksum compare)
tools/test_wsprintf.c    wsprintf formatter tests (29/29)
tools/wltest.c           Wayland server protocol test (host)
tools/smoke.sh, guest.py QEMU smoke test
windows-syscalls/        reference CSVs (git submodule, data only)
AGENT_NOTES.md           working spec / design notes
```

## Ceilings (known, deliberate)

* **Compositor EGL-GBM screen creation fails with the shipped Mesa build.**
  `eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, gbm_dev, NULL)` works, but
  dri2 screen creation fails (`egl: failed to create dri2 screen` — no gallium
  driver matches bochs-drm/NVIDIA). The compositor therefore composites on CPU
  (which works) and the EGL/GLES2 path is exercised by the guest `losegl` probe
  on the surfaceless platform. Upgrade path: a gallium driver for the card
  (`swrast` via `MESA_LOADER_DRIVER_OVERRIDE` did not help on this stack) → the
  same `card_egl_init` path takes over with no shell changes.
* **Desktop GL is not built** (`-Dglx=disabled`) → the project ships OpenGL ES
  2.0 (EGL + GLESv2); `kms.c` uses `EGL_OPENGL_ES_API` only.
* **Guest DMA-BUF is untestable.** bochs-drm has no GEM/PRIME, so the Win32
  client's export probe fails and it falls back to `wl_shm` in the guest; the
  host test (`tools/dmabuf_test.sh`) is the real evidence. On real hardware with a
  GEM-capable card the same path imports client GPU memory.
* **Copy-on-export devices.** Some drivers (e.g. the host NVIDIA dumb buffer)
  snapshot the buffer at PRIME export time, so a client that repaints the same bo
  would show the first frame; the client therefore exports lazily at the first
  commit after painting. Zero-copy GPUs are unaffected.
* **.NET *PE* apps** through the Win32 layer need a real NT layer (CoreCLR) —
  the shipped runtime runs Linux .NET apps (`losdotnet`). Deferred.
* **wsprintf** carries 8 vararg slots (`ponytail:` ceiling; MS x64 variadic call
  ABI has no va_start on clang — explicit arity instead).
* Nouveau firmware is absent from the build host → NVIDIA KMS works, but
  accelerated firmware paths need `/usr/lib/firmware/nouveau` added.
* No virtio-gpu module in the guest kernel tree → QEMU uses `-vga std` (bochs).
* Clipboard: basic offer/transfer; drag-and-drop is a no-op.
* Keymap: US QWERTY only.

## Legal

Original code and artwork throughout; no Microsoft, X.Org, or other desktop
project code is shipped. Linux kernel, Busybox, GRUB, and the fonts
(Liberation) are their respective open-source projects. "Windows" is a
trademark of Microsoft Corporation; losedoze is not affiliated with or endorsed
by Microsoft.
