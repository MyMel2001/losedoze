# losedoze

A hobby desktop operating system: Linux kernel + ext4 + Busybox + GRUB, a
from-scratch desktop shell ("loseshell"), and a homemade Win32 compatibility
subsystem (dual-arch PE loader + own ntdll/kernel32/user32/gdi32/opengl32 over
Linux syscalls — no Wine anywhere) with a Windows-11-style — but entirely original
and legally distinct — desktop.

```
GRUB → Linux kernel (7.2.8) → Busybox init → loseshell (DRM/KMS compositor)
                                                     │
                              /run/losedoze0 (Wayland socket: hand-rolled server + client)
                                                     │
                  w32host (PE32+, x86-64) / w32host32 (PE32, i386) hosts   │  losdotnet
                    → Win32 PE apps + on-disk PE DLLs (shm or DMA-BUF) → .NET app (/opt/dotnet)
```

## Features

| Area | What it is |
|---|---|
| Boot | GRUB2 → kernel + initramfs (loop+isofs) → ext4 rootfs on the ISO |
| Desktop | loseshell: wallpaper, desktop icons, taskbar, clock/calendar, start menu, run box, toasts, a native file manager (Files), window manager (move/resize/snap/maximize/minimize, 4 virtual desktops, Alt-Tab), lock screen, dark/light themes — all original code and artwork |
| Graphics | Raw DRM/KMS ioctls from scratch: atomic modesetting, hardware scanout planes, page flip, vblank pacing. In-tree kernel drivers ship as modules: bochs (QEMU `-vga std`), virtio-gpu (`virtio_pci` bus + `virtio-gpu`, QEMU `-device virtio-vga`), i915 (Intel), xe, amdgpu/radeon (AMD), nouveau (NVIDIA); firmware for i915/amdgpu/radeon in `/lib/firmware`. `losgpu` prints the bound driver per card (`LOSEDOZE GPU OK card0=…`). Mesa was rebuilt with the matching **HW gallium drivers** (`-Dgallium-drivers=llvmpipe,nouveau,radeonsi -Dshared-llvm=disabled`, inside `libgallium-26.2.4.so`): EGL/GLES2 get the real driver when one exists and fall back to llvmpipe/swrast. Intel keeps KMS modesetting (i915/xe) — see Ceilings for `iris`. CPU compositing into DRM dumb buffers, fbdev fallback |
| GL / Vulkan | Mesa 26.2.4 **built from source, X-free** (`-Dplatforms=wayland -Dglx=disabled -Dglvnd=disabled -Dgallium-drivers=llvmpipe,nouveau,radeonsi -Dshared-llvm=disabled -Dvulkan-drivers=swrast`; reproducer `tools/build-mesa.sh`): EGL + OpenGL ES 2.0 (llvmpipe, or the card's own driver — nouveau/radeonsi are in `libgallium-26.2.4.so`) **and desktop OpenGL** (`eglBindAPI(EGL_OPENGL_API)` + `EGL_OPENGL_BIT` pbuffer → compatibility profile, no GLX/libGL needed) and Vulkan (lavapipe) ship in the image; guest probes `losegl` / `losvulkan` print `LOSEDOZE GL OK` / `LOSEDOZE GL DESKTOP OK` / `LOSEDOZE VULKAN OK` |
| Input | Raw evdev (`EVIOCGRAB`), PS/2 + USB keyboards/mice via in-tree kernel drivers; US QWERTY tables; keymap shipped to Wayland clients as XKB_V1; the compositor **synthesizes key repeat** (500 ms delay / 40 ms period — evdev/atkbd deliver none) |
| Display server | Hand-rolled Wayland compositor (no libwayland-server): wl_compositor/shm/seat/keyboard/pointer/output/xdg-shell (toplevel + popup) + **linux-dmabuf v1** (client-allocated buffer ids), wire protocol implemented byte-by-byte in `shell/wl.c` |
| Win32 compat | Homemade subsystem (`win/`): `w32host` (x86-64 ELF) and `w32host32` (i386 ELF, static musl) run PE images through one **dual-arch loader** (`pe.c`: parses PE32/i386 *and* PE32+/x86-64 — headers, sections, base relocations, import/export directories; each image executes in the host of its own architecture, the other host refuses it with a pointer at the right one), binds imports to our own ntdll/kernel32/user32/gdi32/opengl32 implemented over Linux syscalls (stdcall on i386, ms ABI on x64), loads **on-disk PE DLLs** (search path, refcounting, real unload, DllMain attach/detach, exports by name or `#ordinal`) and renders through a hand-rolled Wayland client (`win/wlclient.c`, no libwayland-client). Desktop GL for apps: `opengl32.dll` (WGL + GL entry points via `eglGetProcAddress`) presented with `gdi32!SwapBuffers` (64-bit host; there is no i386 Mesa — see Ceilings). PE32+ apps built freestanding with clang (`--target=x86_64-pc-windows-msvc`, `ld -mi386pep`); PE32 apps/DLLs with zig's bundled clang + LLD (`-target x86-windows-gnu`); NT syscall numbers (from public reference CSVs) compiled into `build/ntsyscalls.inc` and the `losyscall` CLI |
| .NET | A Linux .NET runtime (muxer + `Microsoft.NETCore.App` + `host/fxr`) lives in the guest at `/opt/dotnet`; `losdotnet` runs the bundled demo app → `LOSEDOZE DOTNET OK` |
| .NET desktop | The same user32/gdi32/wlclient sources are built as **`libwin32.so`** (`win/hostlib.c`, SysV ABI so .NET P/Invoke can call it). Losedoze ships its **own .NET WinForms framework** (`tools/dotnet/System.Windows.Forms`, assembly `Losedoze.Windows.Forms`, namespaces `System.Windows.Forms` + `System.Drawing`) on top of that library — a real window in the compositor, controls drawn owner-draw by the form over GDI, keyboard/mouse routed through the Win32 layer's `WM_*`. The managed apps (`tools/dotnet/Losedoze.WinForms` demo via `loswinforms`, plus `tools/dotnet/Losedoze.Desktop` via `losnetdesktop`) P/Invoke the entry points and run the message loop (`LOSEDOZE .NET DESKTOP OK` / `LOSEDOZE WINFORMS OK <version>`). Microsoft's WinForms/WPF are not supported (Windows-only binaries — see Ceilings) |
| Network | Kernel NIC drivers as modules (e1000, e1000e, virtio-net, r8169) plus wireless (cfg80211 + mac80211, iwlwifi, ath10k) with firmware in `/lib/firmware`; userspace is busybox only: `/usr/local/bin/losenet up\|down\|status\|wifi` brings the interface up and runs DHCP (udhcpc) → `LOSEDOZE NET OK eth0=…`; PCI coldplug loads the driver each device's modalias names (no udev) |
| Settings | `loseset` (native, linked against `libwin32.so`) edits the shell's config: network/wifi on-off, wallpaper preset, dark/light theme, brightness → `~/.config/losedoze.conf`; the shell reloads the file when it changes (mtime poll) so changes apply live; menu entry `/usr/share/applications/loseset.desktop` |
| Disk install | `losinstall` (native GUI over `libwin32.so`, start-menu entry "Install to disk") installs the live system onto another block device (HDD, SSD, USB stick, NVMe): it writes an MBR partition table (ext4 root labelled `LOSEDOZE` + swap), formats both with `mke2fs`/`mkswap`, copies the rootfs from the live medium, puts the kernel and initramfs on the root partition and writes a GRUB boot sector plus core image into the gap behind the MBR. The installed system boots on its own: the initramfs looks for the ext4 partition labelled `LOSEDOZE` on any disk (no `root=` kernel parameter, no udev), `/var` and `/root` stay on the disk so files survive a reboot, and the swap partition is activated from `/etc/fstab` |
| Client DMA-BUF | The Win32 client allocates its window buffer as a GBM bo, exports it as a dma-buf fd (`DRM_IOCTL_PRIME_HANDLE_TO_FD`) and hands it over with `zwp_linux_dmabuf_v1` **on every commit**; the compositor imports it (`mmap` + PRIME proof) and composites the client's GPU memory directly; shm fallback on any failure |
| Verification | `loseshell --selftest`, `kms` host selftest, `tools/wltest.c` protocol test against real libwayland-client, `tools/dmabuf_test.sh` (host end-to-end dma-buf: two frames with different content — both pixel checksums the client wrote are compared against the ones the compositor computed from its own `mmap` of the buffer), `tools/test_wsprintf.c` (29/29), host runs of `build/w32host` (PE DLL test app, desktop-GL demo, `--inspect` of a 32-bit PE32 image) and `build/w32host32` (32-bit hello/dlltest PE32 apps + i386 DLLs, refusal of a PE32+ image), QEMU smoke test (`tools/smoke.sh`): console markers `LOSEDOZE INIT OK` / `READY` / `EGL:` / `WAYLAND OK` / `WIN32 OK` / `DMABUF GPU OK` or `DMABUF SHM OK` / `DLL OK` / `PE32 PARSE OK` / `PE32 EXEC OK` / `PE32 DLL OK` / `PE32 GUI OK` / `GL DEMO OK` / `GL OK` / `GL DESKTOP OK` / `VULKAN OK` / `DOTNET OK` / `GPU OK` / `NET OK` / `.NET DESKTOP OK` / `WINFORMS OK` / `SETTINGS OK`, framebuffer screendump (taskbar band, color variety, client-drawn pixels: the white canvas of the Win32 client window + the orange row of the settings app — counted over the whole desktop, so the check does not depend on which client window is on top) → `SMOKE PASS`; `SMOKE_GPU=virtio` boots the same image with QEMU's `-device virtio-vga` (default: bochs). Dev-image self-test also drives `losenet up` against QEMU's user-mode network and dumps the `.NET`/settings app logs to the console. `SMOKE_INSTALL=1` adds the installer check → `INSTALL SMOKE PASS`: the live system installs itself onto a second disk, the host checks the disk layout (MBR partition table, ext4 magic + label, swap header, GRUB boot sector and core image), that disk then boots **without the ISO** (the initramfs finds the root by its label), the swap shows up in `/proc/meminfo`, and a file written in the first installed boot is still there after the next reboot |

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
  dual-arch PE loader (PE32 + PE32+ parsed by both hosts, executed by the
  matching one), syscall dispatch, and windowing/GL rendering over Linux
  syscalls + the hand-rolled Wayland client in `win/wlclient.c`.
* **No Microsoft code.** The Windows look-and-feel is reimplemented: original
  widgets, icons, sounds, and layout. Public syscall-number CSVs were used
  only as numeric reference data for `losyscall` and `build/ntsyscalls.inc`.

## Building

Host requirements: Fedora-like x86_64 host with the guest kernel + module tree,
busybox static binary, clang + `ld` with `-mi386pep` + `llvm-lib` (PE32+
toolchain) and `zig` (bundles a static i386 musl for `w32host32` and an LLD
that links PE32 — binutils here ships no `i386pe` linker script; without zig
the build skips the 32-bit host/apps and the smoke test reports an honest
`PE32 EXEC SKIP`), `grub2-mkrescue` + `grub2-mkimage` (the ISO menu and the
installer's boot sector/core image), `mke2fs`, `cpio`, `xkbcomp` (build-time
only, for the embedded keyboard map), no root required.

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
                              # (SMOKE_GPU=virtio → QEMU -device virtio-vga)
# disk installer: the live system installs itself onto a second disk, that
# disk boots on its own and keeps its files across a reboot
SMOKE_AUTOSTART=1 SMOKE_INSTALL=1 ./build.sh
SMOKE_INSTALL=1 ./tools/smoke.sh
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
rootfs-overlay/          etc/inittab, rcS, session.sh, profile;
                         usr/local/bin/losenet (NIC bring-up + DHCP/WPA),
                         usr/share/udhcpc/default.script, loseset.desktop
shell/                   loseshell: lost.h, kms.c, input.c, wl.c, loseshell.c
tools/gen_syscalls.py    NT syscall CSV → build/syscalls.inc
tools/losyscall.c        syscall database CLI (losyscall)
win/                     homemade Win32 subsystem: pe.c (dual-arch PE loader:
                         PE32/PE32+ parse + reloc + import binding; executes
                         the matching architecture only, on-disk DLLs,
                         exports), ntdll.c/kernel32.c/user32.c/gdi32.c/gl32.c
                         (over Linux), wlclient.c (hand-rolled Wayland client:
                         shm + DMA-BUF), wsprintf.c (variadic formatter:
                         x64 fixed slots, i386 stdarg), hostlib.c (the same
                         layer as a Linux .so, SysV ABI for managed callers),
                         w32host.c (host process; built twice: w32host x86-64,
                         w32host32 i386), apps/ (hello, notes, dlltest, gldemo,
                         dllmath/dllstate DLLs — built for both arches),
                         gen_ntsyscalls.py/gen_importlibs.py (codegen)
tools/gen_pe32.py        PE32 (i386) fixture image for the dual-arch parse check
tools/gen_keymap.sh      build-time XKB keymap → shell/keymap.h
tools/build-mesa.sh      X-free Mesa 26.2.4 reproducer (llvmpipe + nouveau +
                         radeonsi gallium, static LLVM; + lavapipe)
tools/losegl.c           guest EGL probe: GLES2 + desktop GL (→ LOSEDOZE GL OK
                         / LOSEDOZE GL DESKTOP OK)
tools/losvulkan.c        guest Vulkan/lavapipe probe (→ LOSEDOZE VULKAN OK)
tools/losgpu.c           guest DRM probe: card + bound driver per /dev/dri
tools/loseset.c          settings app (native GUI over libwin32.so)
tools/losinstall.c       disk installer (native GUI over libwin32.so; headless
                         --yes --device=DEV --swap=MB, --list, --selftest)
tools/dotnet/            .NET apps (Losedoze.WinForms: managed WinForms app via
                          System.Windows.Forms over libwin32.so; Losedoze.Desktop:
                          managed window via P/Invoke into libwin32.so)
tools/dmabuf_client.c    host test client for the client DMA-BUF path
tools/dmabuf_test.sh     host end-to-end dma-buf check (pixel checksum compare)
tools/test_wsprintf.c    wsprintf formatter tests (29/29)
tools/wltest.c           Wayland server protocol test (host)
tools/smoke.sh, guest.py QEMU smoke test
windows-syscalls/        reference CSVs (git submodule, data only)
AGENT_NOTES.md           working spec / design notes
```

## Ceilings (known, deliberate)

* **Compositor EGL-GBM screen creation still uses the CPU path for QEMU GPUs.**
  `eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, gbm_dev, NULL)` works, but
  dri2 screen creation has no gallium driver for bochs-drm/virtio-gpu (those
  need `virglrenderer`, which is not on the build host) → Mesa falls back to
  `kms_swrast`/llvmpipe. Real AMD/NVIDIA cards do get their driver now
  (nouveau/radeonsi in `libgallium-26.2.4.so`). Upgrade path: build
  virglrenderer → the same `card_egl_init` path takes over with no shell changes.
* **No `iris` (Intel 3D).** Intel's gallium driver needs SPIR-V shaders built by
  `mesa_clc`, which aborts ("double free or corruption") in a static-LLVM build —
  and a dynamic LLVM would put a 188 MB `libLLVM.so` in the image. Intel GPUs
  therefore get KMS modesetting (i915/xe) with CPU rendering. Add `iris` back to
  `-Dgallium-drivers` once `mesa_clc` is fixed.
* **Desktop GL comes through EGL, not GLX** (`-Dglx=disabled`): Win32 apps get a
  desktop GL **compatibility profile** context by binding `EGL_OPENGL_API` with
  an `EGL_OPENGL_BIT` pbuffer on the surfaceless platform (`win/gl32.c`,
  `tools/losegl.c`) — so no libGL/libGLX/libOpenGL ships in the X-free image.
  The compositor itself still composites on CPU (`kms.c` uses
  `EGL_OPENGL_ES_API` only, see the ceiling above).
* **32-bit x86 (PE32) apps run in a separate 32-bit host.** A 64-bit process
  cannot execute 32-bit code, so PE32 EXEs/DLLs run in `w32host32` (i386 ELF,
  static musl): `w32host32 apps32/hello32.exe` executes, `w32host` (x86-64)
  refuses the same image with "run it with w32host32" — and vice versa. The
  loader itself is dual-arch: both hosts parse, relocate and inspect PE32 and
  PE32+ images; only the matching one binds imports and executes. The 32-bit
  side is built with `zig` (bundled static i386 musl + an LLD that links PE32 —
  this host's binutils ships no `ldscripts/i386pe.x`); without zig the build
  skips `w32host32` and the smoke reports an honest `PE32 EXEC SKIP`.
* **No 32-bit GL.** There is no i386 Mesa, so `w32host32` cannot load
  libgbm/libEGL: its Wayland client falls back to `wl_shm` and `opengl32.dll`
  (WGL/desktop GL) stays 64-bit-only. Upgrade path: build Mesa for i386.
* The generated PE32 image (`tools/gen_pe32.py`) still exists as the
  *rebase* fixture: the 64-bit host must map it away from its preferred base
  and apply its HIGHLOW relocations before it refuses to run it.
* **Guest DMA-BUF works.** bochs-drm in this kernel has GEM/PRIME, so the guest
  smoke shows the compositor importing the client's buffer
  (`LOSEDOZE KMS: dmabuf: import 660x460 ...` → `LOSEDOZE DMABUF GPU OK`). On a
  device without dma-buf export the client says so and falls back to `wl_shm`
  (`LOSEDOZE DMABUF SHM OK`).
* **Copy-on-export devices.** Some drivers (e.g. the host NVIDIA dumb buffer)
  snapshot the buffer at PRIME export time, so the client re-exports on every
  commit and the compositor re-imports into the same buffer id. Zero-copy GPUs
  are unaffected.
* **.NET *PE* apps** through the Win32 layer need a real NT layer (CoreCLR) —
  the shipped runtime runs Linux .NET apps (`losdotnet`). Deferred.
* **.NET desktop = our Win32 layer, not Microsoft's.** `libwin32.so` exports the
  Win32 entry points with the SysV ABI (`-DW32_ABI_SYSV`) so CoreCLR can
  P/Invoke them; WinForms/WPF are Windows-only binaries and are not supported.
  Instead the repo ships a **managed WinForms framework** — `tools/dotnet/System.Windows.Forms`
  (`System.Windows.Forms` + `System.Drawing` namespaces, `Losedoze.Windows.Forms`
  assembly) with `Form`, `Control`, `Application`, `MessageBox`, `SystemColors`,
  basic controls (Button/Label/TextBox/CheckBox/RadioButton/ListBox/Panel) drawn
  over GDI in the compositor. Desktop GL is not wired into the shared library either
  (`gl32_swap` is a stub) — managed callers get CPU GDI rendering only.
* **WiFi needs hardware.** No WiFi device is emulated by QEMU, so the guest can
  only prove the driver/firmware stack is present (`modprobe iwlwifi` loads);
  joining a network needs a real adapter + `losenet wifi <ssid> <psk>`.
  Ethernet is proven end-to-end: QEMU's e1000 + `udhcpc` → `LOSEDOZE NET OK`.
* **NVIDIA** still needs `/usr/lib/firmware/nouveau` copied in (absent from the
  build host) for the accelerated firmware paths.
* No virtio-gpu **virgl** → QEMU `-device virtio-vga` gets KMS modesetting but
  renders through llvmpipe/swrast.
* **wsprintf** carries 8 vararg slots (`ponytail:` ceiling; MS x64 variadic call
  ABI has no va_start on clang — explicit arity instead). On i386 the exports are
  plain cdecl variadic (stdcall is ignored on variadic) and the args are read
  with `va_arg` by walking the format, so the same 8-slot ceiling applies there.
* Clipboard: basic offer/transfer; drag-and-drop is a no-op.
* Keymap: US QWERTY only.
* **The installer installs from the live medium.** The ISO has to be in the
  drive: the pristine `rootfs.ext4` on it is the install source (the running
  root is a loop mount of read-only media and cannot be written). Reinstalling
  from an already installed system is not supported yet.
* **MBR, no GPT.** The installer writes a DOS partition table, so the target is
  limited to 2 TB and there is no partition GUID. Upgrade path: write a GPT
  header instead of the MBR and let GRUB's `part_gpt` module read it.
* **`/etc/fstab` uses device paths** (`/dev/sda1`), because the image has no
  udev: `UUID=`/`LABEL=` cannot be resolved without `/dev/disk/by-*`. The root
  is found by its ext4 label at boot, so a changed disk order still boots; the
  swap entry in `fstab` is the part that can go stale.
* **The installed root is found by the ext4 label `LOSEDOZE`.** With two
  installed disks the first one the initramfs finds wins.
* **One install target at a time, no resize, no bootloader for other systems.**
  The installer wipes the chosen disk; it does not add a partition to a disk
  that already has one, and it does not chain-load another OS.

## Legal

Original code and artwork throughout; no Microsoft, X.Org, or other desktop
project code is shipped. Linux kernel, Busybox, GRUB, and the fonts
(Liberation) are their respective open-source projects. "Windows" is a
trademark of Microsoft Corporation; losedoze is not affiliated with or endorsed
by Microsoft.
