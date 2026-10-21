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
```

## Features

| Area | What it is |
|---|---|
| Boot | GRUB2 → kernel + initramfs (loop+isofs) → ext4 rootfs on the ISO |
| Desktop | loseshell: wallpaper, desktop icons, taskbar, clock/calendar, start menu, run box, toasts, a native file manager (Files), window manager (move/resize/snap/maximize/minimize, 4 virtual desktops, Alt-Tab), lock screen, dark/light themes — all original code and artwork |
| Graphics | Raw DRM/KMS ioctls from scratch: atomic modesetting, hardware scanout planes, page flip, vblank pacing. GPU drivers: bochs (QEMU), i915 (Intel), amdgpu/radeon (AMD), nouveau (NVIDIA). CPU compositing into DRM dumb buffers, fbdev fallback |
| Input | Raw evdev (`EVIOCGRAB`), PS/2 + USB keyboards/mice via in-tree kernel drivers; US QWERTY tables; keymap shipped to Wayland clients as XKB_V1 |
| Display server | Hand-rolled Wayland compositor (no libwayland-server): wl_compositor/shm/seat/keyboard/pointer/output/xdg-shell (toplevel + popup), wire protocol implemented byte-by-byte in `shell/wl.c` |
| Win32 compat | Homemade subsystem (`win/`): `w32host` (ELF) loads PE32+ executables, binds imports to our own ntdll/kernel32/user32/gdi32 implemented over Linux syscalls, and renders through a hand-rolled Wayland client (`win/wlclient.c`, no libwayland-client). PE apps built freestanding with clang (`--target=x86_64-pc-windows-msvc`, `ld -mi386pep`); NT syscall numbers (from public reference CSVs) compiled into `build/ntsyscalls.inc` and the `losyscall` CLI |
| Verification | `loseshell --selftest`, `kms` host selftest, `tools/wltest.c` protocol test against real libwayland-client, QEMU smoke test (`tools/smoke.sh`): console markers `LOSEDOZE INIT OK` / `READY` / `EGL:` / `WAYLAND OK` / `WIN32 OK`, framebuffer screendump (taskbar band, color variety, white pixels of the Win32 client window) → `SMOKE PASS` |

## What is NOT here (by design)

* **No X11 anywhere.** No Xorg, no libX11/Xext/xkbcomp/XKB data in the image,
  no libinput, no xkbcommon in the shell. Everything display/input related is
  DRM/KMS + evdev + the hand-rolled Wayland server above.
* **No Mesa EGL.** Fedora's `libEGL_mesa` hard-links `libX11` (compiled-in
  EGL_X11 platform), so it cannot ship in an X-free image. The shell still
  dlopens `libEGL/libgbm` and reports `EGL: none` when absent; hardware
  acceleration of the *display* comes from the KMS drivers (scanout + page
  flip), compositing runs on CPU. See Ceilings.
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

./build.sh                    # → build/losedoze.iso
SMOKE_AUTOSTART=1 ./build.sh  # dev image: Win32 self-test on login
./tools/smoke.sh              # headless QEMU boot + screendump check
```

Host-side checks that need no ISO build:

```sh
gcc -O2 -o /tmp/loseshell shell/*.c -lm -ldl && /tmp/loseshell --selftest
# Wayland protocol conformance (needs libwayland-client + wayland-scanner xdg-shell):
sh tools/gen_keymap.sh shell/keymap.h
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
                         wlclient.c (hand-rolled Wayland client),
                         w32host.c (host process), apps/ (hello, notes),
                         gen_ntsyscalls.py/gen_importlibs.py (codegen)
tools/gen_keymap.sh      build-time XKB keymap → shell/keymap.h
tools/wltest.c           Wayland server protocol test (host)
tools/smoke.sh, guest.py QEMU smoke test
windows-syscalls/        reference CSVs (git submodule, data only)
AGENT_NOTES.md           working spec / design notes
```

## Ceilings (known, deliberate)

* Mesa EGL/GL omitted (links X11) → CPU compositing; hardware scanout/page-flip
  only. Upgrade path: an X-free Mesa build (`-Dplatforms=gbm`) drops in without
  shell changes.
* Client GL/DMA-BUF buffers not implemented — Win32 app windows are `wl_shm`
  (software) only.
* Nouveau firmware is absent from the build host → NVIDIA KMS works, but
  accelerated firmware paths need `/usr/lib/firmware/nouveau` added.
* No virtio-gpu module in the guest kernel tree → QEMU uses `-vga std` (bochs).
* Clipboard: basic offer/transfer; drag-and-drop is a no-op.
* Keymap: US QWERTY only; no shell-side key repeat.

## Legal

Original code and artwork throughout; no Microsoft, X.Org, or other desktop
project code is shipped. Linux kernel, Busybox, GRUB, and the fonts
(Liberation) are their respective open-source projects. "Windows" is a
trademark of Microsoft Corporation; losedoze is not affiliated with or endorsed
by Microsoft.
