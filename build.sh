#!/bin/bash
# Build the Losedoze live ISO.
#   ./build.sh                  release image (no smoke hooks)
#   SMOKE_AUTOSTART=1 ./build.sh  dev image (Win32 self-test on login)
#
# Graphics policy: NO Xorg/X11/Xext/xkbcomp/X libraries in the image.
# Fedora's libEGL_mesa hard-links libX11 (EGL_X11 platform), so Mesa is built
# from source instead (tools/build-mesa.sh): X-free Mesa 26.2.4 gives the guest
# EGL + OpenGL ES 2 (llvmpipe) and Vulkan (lavapipe). The *display* path stays
# DRM/KMS (hardware scanout + page flip) with CPU compositing; kms.c dlopens
# libEGL/libgbm at runtime and degrades gracefully ("EGL: none") when absent.
# Ceilings + upgrade paths: AGENT_NOTES.md.
set -eu
cd "$(cd "$(dirname "$0")" && pwd)"

KVER=${KVER:-7.2.8-ogc5.1.fc44.x86_64}
KIMG=${KIMG:-/boot/ostree/fedora-6ee911fc79fb33a82b3091b9973d9b29827059b67db8bf4697c8e504452f235a/vmlinuz-$KVER}
MODSRC=${MODSRC:-/ostree/deploy/fedora/deploy/1988bd1718c9a4ec324454eede2550c2719ceda6347c56b35dc3d23db35bdc52.0/usr/lib/modules/$KVER}
BUSYBOX=${BUSYBOX:-/tmp/busybox}
# Toolchain for the homemade Win32 subsystem (PE32+ apps + w32host)
PE_CC=${PE_CC:-clang}
PE_LD=${PE_LD:-ld}
PE_LIB=${PE_LIB:-llvm-lib}

ROOT=build/rootfs
INITRD=build/initrd
ISO=build/losedoze.iso
# loop/isofs/e1000 stay explicit (initrd + net); GPU tree gives KMS drivers
MODULES_REL="kernel/drivers/block/loop.ko kernel/fs/isofs/isofs.ko kernel/drivers/net/ethernet/intel/e1000/e1000.ko"
# net: virtio-net + common NIC drivers; wireless: mac80211/cfg80211 + iwlwifi
# and ath10k (firmware for both ships in /usr/lib/firmware)
MODULES_REL="$MODULES_REL kernel/drivers/net/virtio_net.ko kernel/drivers/net/ethernet/intel/e1000e/e1000e.ko kernel/drivers/net/ethernet/realtek/r8169.ko kernel/net/wireless/cfg80211.ko kernel/net/mac80211/mac80211.ko"
# QEMU/Boxes default NIC is rtl8139 -> 8139too (needs mii)
MODULES_REL="$MODULES_REL kernel/drivers/net/ethernet/realtek/8139too.ko kernel/drivers/net/mii.ko"
# gpu: bochs-drm/virtio-gpu/i915/xe/amdgpu/nouveau/radeon; virtio: the bus
# (virtio_pci) that carries virtio-gpu + virtio-net
MODULES_TREE="kernel/drivers/gpu kernel/drivers/video kernel/drivers/cec kernel/lib kernel/drivers/virtio"
MODULES_TREE="$MODULES_TREE kernel/drivers/net/wireless/intel/iwlwifi kernel/drivers/net/wireless/ath/ath10k"
# drm module deps that live outside the gpu tree: without these, modprobe of
# radeon/i915/xe/amdgpu/nouveau dies on unresolved symbols ("unknown symbol")
MODULES_REL="$MODULES_REL kernel/drivers/acpi/video.ko kernel/drivers/media/cec/core/cec.ko kernel/drivers/i2c/algos/i2c-algo-bit.ko kernel/drivers/platform/x86/intel/intel-vsec.ko kernel/drivers/platform/x86/mxm-wmi.ko kernel/drivers/platform/wmi/wmi.ko"

say()  { printf '\n=== %s\n' "$*"; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
need() { [ -e "$1" ] || die "missing $1"; }

# Copy that never writes through an existing busybox symlink.
put() {
    mkdir -p "$(dirname "$2")"
    rm -f "$2"
    cp -L "$1" "$2"
}
copy_named() {
    for f in "$@"; do [ -e "$f" ] && put "$f" "$ROOT$f"; done
    return 0
}
lddeps() {
    for f in "$@"; do
        [ -e "$f" ] || continue
        ldd "$f" 2>/dev/null || true
    done | awk '{for(i=1;i<=NF;i++) if ($i ~ /^\//) print $i}' | sort -u
}
copy_libs() {
    lddeps "$@" | while read -r lib; do
        put "$lib" "$ROOT/usr/lib64/$(basename "$lib")"
    done
}
busybox_install() {   # $1 = target root
    local dest=$1 a d
    mkdir -p "$dest/bin"
    put "$BUSYBOX" "$dest/bin/busybox"
    chmod 755 "$dest/bin/busybox"
    for a in $("$dest/bin/busybox" --list); do
        for d in bin sbin usr/bin usr/sbin usr/local/bin; do
            [ -e "$dest/$d/$a" ] && continue
            mkdir -p "$dest/$d"
            ln -s /bin/busybox "$dest/$d/$a"
        done
    done
}

# ---------------------------------------------------------------- checks
say "0/14 checks"
need "$KIMG"
need "$MODSRC/modules.dep"
need "$BUSYBOX"
need windows-syscalls/x64/csv/nt.csv
command -v "$PE_CC" >/dev/null 2>&1 || die "$PE_CC not found (need clang for PE apps)"
command -v "$PE_LD" >/dev/null 2>&1 || die "$PE_LD not found (need ld with -mi386pep)"
"$PE_LD" -mi386pep --help >/dev/null 2>&1 || die "ld lacks -mi386pep (PE32+ target)"
command -v "$PE_LIB" >/dev/null 2>&1 || die "$PE_LIB not found (need llvm-lib for import libs)"
[ -e /dev/kvm ] && KVM_NOTE="kvm=yes" || KVM_NOTE="kvm=no"
printf 'kernel:   %s\nbusybox:  %s\npe-cc:    %s\n%s\n' "$KIMG" "$BUSYBOX" "$PE_CC" "$KVM_NOTE"

# ---------------------------------------------------------------- stage overlay
say "1/14 staging overlay"
rm -rf "$ROOT" "$INITRD" build/iso
mkdir -p "$ROOT"
cp -a rootfs-overlay/. "$ROOT/"
mkdir -p "$ROOT"/{root,tmp,run,home,dev,proc,sys,newroot,var/log,var/tmp,var/cache}
chmod 1777 "$ROOT/tmp" "$ROOT/var/tmp"
chmod 755 "$ROOT/usr/local/bin/session.sh" "$ROOT/etc/init.d/rcS"

# ---------------------------------------------------------------- busybox
say "2/14 busybox userland"
busybox_install "$ROOT"

# ---------------------------------------------------------------- programs
say "3/14 native programs (loseshell, losyscall, mke2fs)"
mkdir -p build
python3 tools/gen_syscalls.py build/syscalls.inc
# wl_keyboard.keymap (XKB_V1) — build-time host tool only, nothing X ships
if command -v xkbcomp >/dev/null 2>&1 && [ -d /usr/share/X11/xkb ]; then
    sh tools/gen_keymap.sh shell/keymap.h
fi
[ -f shell/keymap.h ] || die "shell/keymap.h missing (run sh tools/gen_keymap.sh shell/keymap.h)"
gcc -O2 -Wall -o build/loseshell shell/loseshell.c shell/kms.c shell/input.c shell/wl.c -lm -ldl
gcc -O2 -Wall -o build/losyscall tools/losyscall.c -Ibuild
./build/losyscall --selftest >/dev/null
./build/loseshell --selftest >/dev/null
gcc -O2 -Wall -o build/loscmd tools/loscmd.c
./build/loscmd --selftest >/dev/null
gcc -O2 -Wall -o build/losrun tools/losrun.c
./build/losrun --selftest >/dev/null
put build/loseshell "$ROOT/usr/bin/loseshell"
put build/losyscall "$ROOT/usr/bin/losyscall"
put build/loscmd "$ROOT/usr/bin/loscmd"
put build/losrun "$ROOT/usr/bin/losrun"
# e2fsprogs mke2fs: busybox's mke2fs is ext2 only, the installer needs ext4
MK2FS=$(command -v mke2fs || true)
[ -n "$MK2FS" ] || die "mke2fs not found (e2fsprogs; the installer needs ext4)"
put "$MK2FS" "$ROOT/usr/sbin/mke2fs"
copy_libs "$MK2FS"
[ -e /etc/mke2fs.conf ] && put /etc/mke2fs.conf "$ROOT/etc/mke2fs.conf"

# ---------------------------------------------------------------- wayland + firmware (no X, no Mesa-EGL)
say "4/14 wayland/xkb runtime + gpu firmware"
mkdir -p "$ROOT/usr/lib64" "$ROOT/lib/firmware"
# X-free runtime libs for the hand-rolled Wayland client (wlclient.c)
copy_named /usr/lib64/libwayland-client.so.0 /usr/lib64/libwayland-egl.so.1 \
           /usr/lib64/libwayland-cursor.so.0 /usr/lib64/libxkbcommon.so.0 \
           /usr/lib64/libxkbregistry.so.0
copy_libs /usr/lib64/libwayland-client.so.0 /usr/lib64/libwayland-egl.so.1 \
          /usr/lib64/libwayland-cursor.so.0 /usr/lib64/libxkbcommon.so.0 \
          /usr/lib64/libxkbregistry.so.0
# GPU microcode for Intel/AMD KMS drivers + wifi firmware (the drivers load
# it from /lib/firmware at probe time; the image has no initramfs firmware)
for fw in i915 amdgpu radeon ath10k; do
    [ -d "/usr/lib/firmware/$fw" ] && cp -a "/usr/lib/firmware/$fw" "$ROOT/lib/firmware/"
done
for f in /usr/lib/firmware/iwlwifi-*; do
    [ -e "$f" ] && cp -a "$f" "$ROOT/lib/firmware/"
done
# DHCP writes /etc/resolv.conf; the image root is read-only -> /run (tmpfs)
ln -sf /run/resolv.conf "$ROOT/etc/resolv.conf"
# interpreter
mkdir -p "$ROOT/lib64"
put /lib64/ld-linux-x86-64.so.2 "$ROOT/lib64/ld-linux-x86-64.so.2"

# ---------------------------------------------------------------- mesa graphics stack
# X-free Mesa built from source (Fedora's libEGL_mesa hard-links libX11).
# Gives the guest EGL + GLES2 (llvmpipe) and Vulkan (lavapipe).
say "5/14 mesa graphics stack (EGL/GLES2/gbm/lavapipe)"
MESA_ROOT=${MESA_ROOT:-/tmp/mesa/root}
MESA_SRC=${MESA_SRC:-/tmp/mesa/mesa-src}
if [ -d "$MESA_ROOT/usr/lib64" ]; then
    mkdir -p "$ROOT/usr/lib64/gbm" "$ROOT/usr/share/vulkan/icd.d"
    for f in libEGL.so.1 libGLESv2.so.2 libGLESv1_CM.so.1 libgbm.so.1 \
             libgallium-26.2.4.so libvulkan_lvp.so; do
        [ -e "$MESA_ROOT/usr/lib64/$f" ] && put "$MESA_ROOT/usr/lib64/$f" "$ROOT/usr/lib64/$f"
    done
    [ -e "$MESA_ROOT/usr/lib64/gbm/dri_gbm.so" ] && \
        put "$MESA_ROOT/usr/lib64/gbm/dri_gbm.so" "$ROOT/usr/lib64/gbm/dri_gbm.so"
    for j in "$MESA_ROOT/usr/share/vulkan/icd.d/"*.json; do
        [ -e "$j" ] && put "$j" "$ROOT/usr/share/vulkan/icd.d/$(basename "$j")"
    done
    # X-free runtime deps of the mesa stack (libelf/libdrm_amdgpu come from the
    # HW gallium drivers radeonsi/nouveau linked into libgallium)
    copy_named /usr/lib64/libvulkan.so.1 /usr/lib64/libdrm.so.2 /usr/lib64/libexpat.so.1 \
               /usr/lib64/libzstd.so.1 /usr/lib64/libstdc++.so.6 /usr/lib64/libgcc_s.so.1 \
               /usr/lib64/libSPIRV-Tools.so /usr/lib64/libelf.so.1 /usr/lib64/libdrm_amdgpu.so.1
    copy_libs /usr/lib64/libvulkan.so.1 /usr/lib64/libdrm.so.2 /usr/lib64/libexpat.so.1 \
              /usr/lib64/libzstd.so.1 /usr/lib64/libstdc++.so.6 /usr/lib64/libgcc_s.so.1 \
              /usr/lib64/libSPIRV-Tools.so /usr/lib64/libelf.so.1 /usr/lib64/libdrm_amdgpu.so.1
    # guest-side probes (EGL/GLES2 + Vulkan) that print machine-checkable markers
    gcc -O2 -Wall -o build/losegl tools/losegl.c -I"$MESA_ROOT/usr/include" \
        -L"$MESA_ROOT/usr/lib64" -lEGL -lGLESv2 -lgbm
    gcc -O2 -Wall -o build/losvulkan tools/losvulkan.c -I"$MESA_SRC/include" \
        -L/usr/lib64 -l:libvulkan.so.1
    put build/losegl "$ROOT/usr/bin/losegl"
    put build/losvulkan "$ROOT/usr/bin/losvulkan"
else
    printf 'skip mesa: %s missing (build it with tools/build-mesa.sh)\n' "$MESA_ROOT"
fi

# ---------------------------------------------------------------- fonts + fontconfig
say "6/14 fonts"
mkdir -p "$ROOT/usr/share/fonts/TTF" "$ROOT/var/cache/fontconfig"
for f in /usr/share/fonts/liberation-sans-fonts/*.ttf; do
    put "$f" "$ROOT/usr/share/fonts/TTF/$(basename "$f")"
done
if [ -d /etc/fonts ]; then
    rm -rf "$ROOT/etc/fonts"
    cp -a /etc/fonts "$ROOT/etc/fonts"
fi
copy_named /usr/bin/fc-cache
copy_libs /usr/bin/fc-cache

# ---------------------------------------------------------------- kernel modules (DRM/KMS included)
say "7/14 kernel modules"
for m in $MODULES_REL; do
    need "$MODSRC/$m"
    mkdir -p "$ROOT/lib/modules/$KVER/$(dirname "$m")"
    cp "$MODSRC/$m" "$ROOT/lib/modules/$KVER/$m"
done
for t in $MODULES_TREE; do
    [ -d "$MODSRC/$t" ] || continue
    ( cd "$MODSRC" && find "$t" -name '*.ko*' -print0 ) | while IFS= read -r -d '' m; do
        mkdir -p "$ROOT/lib/modules/$KVER/$(dirname "$m")"
        cp "$MODSRC/$m" "$ROOT/lib/modules/$KVER/$m"
    done
done
for f in modules.builtin modules.builtin.modinfo modules.order; do
    [ -e "$MODSRC/$f" ] && cp "$MODSRC/$f" "$ROOT/lib/modules/$KVER/$f"
done
depmod -b "$ROOT" "$KVER" > build/depmod.log 2>&1 || \
    printf 'depmod warnings (see build/depmod.log)\n'

# GPU probe: which DRM devices exist and which kernel driver bound them
gcc -O2 -Wall -o build/losgpu tools/losgpu.c -Ishell
put build/losgpu "$ROOT/usr/bin/losgpu"

# ---------------------------------------------------------------- homemade Win32 subsystem
# PE32+ (x86-64) and PE32 (i386) loader + own ntdll/kernel32/user32/gdi32
# over Linux syscalls: w32host (x86-64 ELF) runs PE32+ apps, w32host32 (i386,
# static musl via zig) runs PE32 apps; each refuses the other architecture.
say "8/14 win32 subsystem (w32host + PE apps)"
mkdir -p build build/winapps
python3 win/gen_ntsyscalls.py build/ntsyscalls.inc
python3 win/gen_importlibs.py win/w32host.c build/winapps
# PE32 (i386) parse fixture: generated image (no i386pe linker script exists
# in binutils here) with a low preferred base -> the 64-bit host must rebase
# it (HIGHLOW) and then refuse to execute it (pe_refuse()).
python3 tools/gen_pe32.py build/winapps/pe32parse.exe
# w32host: ELF host process (hand-rolled Wayland client -> /run/losedoze0)
gcc -O2 -Wall -Wno-unused-function -o build/w32host \
    win/pe.c win/ntdll.c win/kernel32.c win/user32.c win/gdi32.c win/gl32.c \
    win/wsprintf.c win/wlclient.c win/w32host.c shell/input.c \
    -Iwin -Ishell -Ibuild -lm -ldl
# w32host32 + PE32 apps: zig ships a static musl for i386 (no i386 glibc
# devel needed) and an LLD that links PE32. -mstackrealign because mingw
# callers hand the host a 16-byte-aligned stack that plain GCC i386 code
# mis-models (misaligned SSE stores -> SIGSEGV).
ZIG=${ZIG:-zig}
if command -v "$ZIG" > /dev/null 2>&1; then
    "$ZIG" cc -target x86-linux-musl -static -O2 -Wall -Wno-unused-function \
        -mstackrealign -o build/w32host32 \
        win/pe.c win/ntdll.c win/kernel32.c win/user32.c win/gdi32.c win/gl32.c \
        win/wsprintf.c win/wlclient.c win/w32host.c shell/input.c \
        -Iwin -Ishell -Ibuild -lm -ldl
    need build/w32host32
else
    say "  zig not found: 32-bit host and PE32 apps skipped"
fi
# PE apps: freestanding PE32+ (ms x64 ABI), linked with -mi386pep
PE_CFLAGS="--target=x86_64-pc-windows-msvc -ffreestanding -fno-builtin -O2 -Iwin"
PE_LDFLAGS="-mi386pep"
build_pe_app() {  # $1=name  $2=subsystem(windows|console)  [$3=extra libs]
    local n=$1 sub=$2
    "$PE_CC" $PE_CFLAGS -c "win/apps/$n.c" -o "build/winapps/$n.o"
    "$PE_CC" $PE_CFLAGS -c win/apps/crt.s -o build/winapps/crt.o
    "$PE_LD" $PE_LDFLAGS --subsystem "$sub" -e "${n}_entry" \
        -L build/winapps -o "build/winapps/$n.exe" \
        "build/winapps/$n.o" build/winapps/crt.o \
        -lntdll -lkernel32 -luser32 -lgdi32 ${3:-}
    need "build/winapps/$n.exe"
}
build_pe_app hello console
build_pe_app notes windows
# PE DLLs on disk: exports come from --export-all-symbols, entry = the app's
# DllMain. Each DLL gets its own preferred ImageBase: binutils does not emit a
# .reloc section for these, so the loader can only map it at its preferred base.
build_pe_dll() {  # $1=name  $2=image base (win/apps/$1.def lists its exports)
    local n=$1 base=$2
    "$PE_LIB" /def:"win/apps/$n.def" /machine:x64 /out:"build/winapps/lib$n.a"
    "$PE_CC" $PE_CFLAGS -c "win/apps/$n.c" -o "build/winapps/$n.o"
    "$PE_LD" $PE_LDFLAGS --dll -e "${n}_dllmain" --export-all-symbols \
        --image-base "$base" \
        -o "build/winapps/$n.dll" "build/winapps/$n.o"
    need "build/winapps/$n.dll"
}
build_pe_dll dllmath 0x180000000
build_pe_dll dllstate 0x181000000
build_pe_app dlltest console "-ldllmath"
build_pe_app gldemo windows "-lopengl32"
# --- i386 (PE32): the same app sources, linked by zig's LLD. Win32 imports
# resolve through zig's stdcall import libs (mingw defs); the own-DLL pairs
# stay plain cdecl (llvm-lib /machine:x86 import libs).
if [ -x build/w32host32 ]; then
    PE32_CFLAGS="--target=x86-windows-gnu -ffreestanding -fno-builtin -O2 -Iwin"
    build_pe32_app() {  # $1=name  $2=subsystem(console|windows)  [$3=extra libs]
        local n=$1 sub=$2
        "$ZIG" cc $PE32_CFLAGS -c "win/apps/$n.c" -o "build/winapps/$n.32.o"
        "$ZIG" cc -target x86-windows-gnu -nostdlib -Wl,--subsystem,"$sub" \
            -Wl,-e,"${n}_entry" -o "build/winapps/${n}32.exe" \
            "build/winapps/$n.32.o" -lntdll -lkernel32 -luser32 -lgdi32 ${3:-}
        need "build/winapps/${n}32.exe"
    }
    build_pe32_dll() {  # $1=name  $2=image base (win/apps/$1.def lists exports)
        local n=$1 base=$2
        "$PE_LIB" /def:"win/apps/$n.def" /machine:x86 /out:"build/winapps/lib${n}32.a"
        "$ZIG" cc $PE32_CFLAGS -c "win/apps/$n.c" -o "build/winapps/$n.32.o"
        "$ZIG" cc -target x86-windows-gnu -nostdlib -shared -Wl,-e,"${n}_dllmain" \
            -Wl,--export-all-symbols -Wl,--image-base,"$base" \
            -o "build/winapps/${n}32.dll" "build/winapps/$n.32.o"
        need "build/winapps/${n}32.dll"
    }
    build_pe32_app hello console
    build_pe32_app notes windows
    build_pe32_dll dllmath 0x10000000
    build_pe32_dll dllstate 0x10100000
    build_pe32_app dlltest console "build/winapps/libdllmath32.a"
fi
put build/w32host "$ROOT/usr/bin/w32host"
mkdir -p "$ROOT/opt/losedoze/apps"
put build/winapps/hello.exe    "$ROOT/opt/losedoze/apps/hello.exe"
put build/winapps/notes.exe    "$ROOT/opt/losedoze/apps/notes.exe"
put build/winapps/dlltest.exe  "$ROOT/opt/losedoze/apps/dlltest.exe"
put build/winapps/gldemo.exe   "$ROOT/opt/losedoze/apps/gldemo.exe"
put build/winapps/pe32parse.exe "$ROOT/opt/losedoze/apps/pe32parse.exe"
put build/winapps/dllmath.dll  "$ROOT/opt/losedoze/apps/dllmath.dll"
put build/winapps/dllstate.dll "$ROOT/opt/losedoze/apps/dllstate.dll"
# 32-bit world: its own dir (SysWOW64-style) because DLL names would collide
# by architecture; the loader's arch check is the second line of defense.
if [ -x build/w32host32 ]; then
    mkdir -p "$ROOT/opt/losedoze/apps32"
    put build/w32host32 "$ROOT/usr/bin/w32host32"
    put build/winapps/hello32.exe    "$ROOT/opt/losedoze/apps32/hello32.exe"
    put build/winapps/notes32.exe    "$ROOT/opt/losedoze/apps32/notes32.exe"
    put build/winapps/dlltest32.exe  "$ROOT/opt/losedoze/apps32/dlltest32.exe"
    put build/winapps/dllmath32.dll  "$ROOT/opt/losedoze/apps32/dllmath.dll"
    put build/winapps/dllstate32.dll "$ROOT/opt/losedoze/apps32/dllstate.dll"
fi

# ---------------------------------------------------------------- .NET runtime
# A Linux .NET runtime lives in the guest so the OS can run .NET programs.
# The desktop app drives the Win32 layer (libwin32.so) through P/Invoke:
# our own Win32 implementation over the compositor, not Microsoft's Windows
# Desktop runtime (see AGENT_NOTES.md "ceilings").
say "9/14 .NET runtime"
DOTNET_ROOT=${DOTNET_ROOT:-/tmp/dotnet}
DNAPP_OUT=${DNAPP_OUT:-/tmp/dnapp/out}
# build the in-tree apps when the SDK is here (offline: no package refs)
if [ -x "$DOTNET_ROOT/dotnet" ] && [ -d tools/dotnet ]; then
    for p in tools/dotnet/*/; do
        [ -f "$p"/*.csproj ] || continue
        n=$(basename "$p")
        "$DOTNET_ROOT/dotnet" publish "$p" -c Release -o "build/dotnet/$n" \
            --nologo >/dev/null 2>&1 || printf 'skip dotnet app %s (publish failed)\n' "$n"
    done
    [ -f build/dotnet/dnapp/dnapp.dll ] && DNAPP_OUT=build/dotnet/dnapp
fi
if [ -x "$DOTNET_ROOT/dotnet" ]; then
    mkdir -p "$ROOT/opt/dotnet"
    put "$DOTNET_ROOT/dotnet" "$ROOT/opt/dotnet/dotnet"
    for d in "$DOTNET_ROOT/shared/"*; do
        [ -d "$d" ] || continue
        mkdir -p "$ROOT/opt/dotnet/shared/$(basename "$d")"
        cp -a "$d/." "$ROOT/opt/dotnet/shared/$(basename "$d")/"
    done
    for d in "$DOTNET_ROOT/host/fxr/"*; do
        [ -d "$d" ] || continue
        mkdir -p "$ROOT/opt/dotnet/host/fxr/$(basename "$d")"
        cp -a "$d/." "$ROOT/opt/dotnet/host/fxr/$(basename "$d")/"
    done
    copy_named /usr/lib64/libstdc++.so.6 /usr/lib64/libgcc_s.so.1 /usr/lib64/libz.so.1 \
               /usr/lib64/libdl.so.2 /usr/lib64/libpthread.so.0 /usr/lib64/librt.so.1
    copy_libs /usr/lib64/libstdc++.so.6 /usr/lib64/libdl.so.2 /usr/lib64/libpthread.so.0 \
              /usr/lib64/librt.so.1
    # apps are optional: the runtime ships regardless so the desktop app works
    if [ -f "$DNAPP_OUT/dnapp.dll" ]; then
        mkdir -p "$ROOT/opt/dotnet/apps/dnapp"
        cp -a "$DNAPP_OUT/." "$ROOT/opt/dotnet/apps/dnapp/"
        cat > "$ROOT/usr/bin/losdotnet" <<'EOF'
#!/bin/sh
# Run the bundled .NET demo app on the guest runtime.
export DOTNET_ROOT=/opt/dotnet
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1   # no ICU in the image
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_NOLOGO=1
exec /opt/dotnet/dotnet /opt/dotnet/apps/dnapp/dnapp.dll "$@"
EOF
        chmod 755 "$ROOT/usr/bin/losdotnet"
    fi
    if [ -f build/dotnet/Losedoze.Desktop/netdesktop.dll ]; then
        mkdir -p "$ROOT/opt/dotnet/apps/netdesktop"
        cp -a build/dotnet/Losedoze.Desktop/. "$ROOT/opt/dotnet/apps/netdesktop/"
        cat > "$ROOT/usr/bin/losnetdesktop" <<'EOF'
#!/bin/sh
# .NET desktop app: a managed window drawn through the Win32 layer
# (libwin32.so -> the losedoze compositor).
export DOTNET_ROOT=/opt/dotnet
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_NOLOGO=1
exec /opt/dotnet/dotnet /opt/dotnet/apps/netdesktop/netdesktop.dll "$@"
EOF
        chmod 755 "$ROOT/usr/bin/losnetdesktop"
    fi
    if [ -f build/dotnet/Losedoze.WinForms/loswinforms.dll ]; then
        mkdir -p "$ROOT/opt/dotnet/apps/winforms"
        cp -a build/dotnet/Losedoze.WinForms/. "$ROOT/opt/dotnet/apps/winforms/"
        cat > "$ROOT/usr/bin/loswinforms" <<'EOF'
#!/bin/sh
# Managed WinForms app: a real window in the compositor, painted by managed
# controls over GDI, run through the losedoze WinForms framework (over libwin32.so).
export DOTNET_ROOT=/opt/dotnet
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_NOLOGO=1
exec /opt/dotnet/dotnet /opt/dotnet/apps/winforms/loswinforms.dll "$@"
EOF
        chmod 755 "$ROOT/usr/bin/loswinforms"
    fi
else
    printf 'skip dotnet: runtime %s missing\n' "$DOTNET_ROOT"
fi

# ---------------------------------------------------------------- Win32 as a shared library
# The same user32/gdi32/wlclient sources the PE host uses, built as a Linux
# .so with the SysV ABI (that is what .NET P/Invoke uses on Linux). Managed
# apps and native tools link/P/Invoke it to open windows and paint.
say "10/14 win32 shared library + settings app + installer"
gcc -shared -fPIC -O2 -Wall -DW32_ABI_SYSV -o build/libwin32.so \
    win/hostlib.c win/ntdll.c win/kernel32.c win/user32.c win/gdi32.c \
    win/gl32.c win/wlclient.c win/pe.c shell/input.c \
    -Iwin -Ishell -Ibuild -ldl -lm
put build/libwin32.so "$ROOT/usr/lib64/libwin32.so"
gcc -O2 -Wall -DW32_ABI_SYSV -Iwin -o build/loseset tools/loseset.c \
    -Lbuild -lwin32 -Wl,-rpath,/usr/lib64
put build/loseset "$ROOT/usr/bin/loseset"
# disk installer: same Win32 layer, so it is a real desktop app
gcc -O2 -Wall -DW32_ABI_SYSV -Iwin -o build/losinstall tools/losinstall.c \
    -Lbuild -lwin32 -Wl,-rpath,/usr/lib64
LD_LIBRARY_PATH=build ./build/losinstall --selftest > /dev/null
put build/losinstall "$ROOT/usr/bin/losinstall"

# wireless tools: scan/join (iw) and WPA/WPA2 supplicant (wpa_supplicant)
for t in /usr/sbin/iw /usr/sbin/wpa_supplicant; do
    [ -x "$t" ] || continue
    put "$t" "$ROOT/usr/sbin/$(basename "$t")"
    copy_libs "$t"
done
chmod 755 "$ROOT/usr/local/bin/losenet" "$ROOT/usr/share/udhcpc/default.script" 2>/dev/null || true

# ---------------------------------------------------------------- ld cache
cat > "$ROOT/etc/ld.so.conf" <<'EOF'
/usr/lib64
/lib64
EOF
ldconfig -r "$ROOT" 2>/dev/null || true

# ---------------------------------------------------------------- dev smoke hook (not in release)
if [ "${SMOKE_AUTOSTART:-0}" = 1 ]; then
    say "SMOKE_AUTOSTART=1: installing login self-test"
    mkdir -p "$ROOT/etc/losedoze"
    cat > "$ROOT/etc/losedoze/autostart.sh" <<'EOF'
#!/bin/sh
echo "LOSEDOZE AUTOSTART BEGIN pid=$$ ppid=$PPID" > /dev/console
export XDG_RUNTIME_DIR=/run WAYLAND_DISPLAY=losedoze0
EOF
    # disk installer: the live system installs itself onto the target disk
    # (the smoke attaches it as an IDE disk -> /dev/sda). Runs first so the
    # smoke does not have to wait for the whole self-test suite.
    if [ "${SMOKE_INSTALL:-0}" = 1 ]; then
        cat >> "$ROOT/etc/losedoze/autostart.sh" <<'EOF'
# persistence probe: on an installed system /var is on the disk, so this
# counter survives the reboot
if [ -f /etc/losedoze/installed ]; then
    N=$(cat /var/losedoze-boots 2>/dev/null || echo 0)
    N=$((N + 1))
    echo "$N" > /var/losedoze-boots
    echo "LOSEDOZE BOOT COUNT $N" > /dev/console
    # flush it to the disk: the smoke kills QEMU right after READY, and a
    # write that is still in the page cache would be lost
    sync
fi
if [ ! -f /etc/losedoze/installed ] && [ -b /dev/sda ]; then
    losinstall --yes --device=/dev/sda --swap=512 > /tmp/losinstall-smoke.log 2>&1
    grep -q "LOSEDOZE INSTALL OK" /tmp/losinstall-smoke.log 2>/dev/null \
        && echo "LOSEDOZE INSTALL OK" > /dev/console \
        || echo "LOSEDOZE INSTALL FAIL" > /dev/console
fi
EOF
    fi
    cat >> "$ROOT/etc/losedoze/autostart.sh" <<'EOF'
# console app: stdout carries the marker
w32host /opt/losedoze/apps/hello.exe > /tmp/w32hello 2>&1; HRC=$?
grep -q "LOSEDOZE WIN32 APP OK" /tmp/w32hello 2>/dev/null && HOK=1 || HOK=0
# gui app: window created (/tmp/w32ok) AND mapped by the compositor
# (loseshell logs "manage:" for every mapped client toplevel).
LOG=/var/log/losedoze.log
count_manage() {
    c=$(grep -c "manage:" "$LOG" 2>/dev/null)
    [ -n "$c" ] || c=0
    echo "$c"
}
BASE=$(count_manage)
w32host /opt/losedoze/apps/notes.exe > /tmp/w32notes 2>&1 &
i=0
while [ ! -s /tmp/w32ok ] && [ $i -lt 150 ]; do sleep 0.2; i=$((i + 1)); done
[ -s /tmp/w32ok ] && NOK=1 || NOK=0
i=0
while [ $i -lt 150 ]; do
    NOW=$(count_manage)
    [ "$NOW" -gt "$BASE" ] && break
    sleep 0.2; i=$((i + 1))
done
NOW=$(count_manage)
[ "$NOW" -gt "$BASE" ] && MOK=1 || MOK=0
if [ "$HOK" = 1 ] && [ "$NOK" = 1 ] && [ "$MOK" = 1 ]; then
    echo "LOSEDOZE WIN32 OK" > /dev/console
else
    echo "LOSEDOZE WIN32 FAIL (hello=$HOK notes=$NOK mapped=$MOK hrc=$HRC)" > /dev/console
fi

# Client DMA-BUF: the client hands its GPU buffer over with linux-dmabuf v1.
# On the guest (bochs-drm with GEM/PRIME) that succeeds; on a device without
# dma-buf export the client says so and falls back to wl_shm.
if grep -q "dma-buf buffer in use" /tmp/w32notes 2>/dev/null; then
    echo "LOSEDOZE DMABUF GPU OK" > /dev/console
elif grep -q "dma-buf unavailable" /tmp/w32notes 2>/dev/null; then
    echo "LOSEDOZE DMABUF SHM OK" > /dev/console
fi

# --- homemade Win32 subsystem: PE DLLs on disk, dual-arch PE loader, desktop GL
# DLLs: static import at load time + LoadLibraryA/GetProcAddress/FreeLibrary,
# including a real unload/reload (the DLL's own state starts over).
w32host /opt/losedoze/apps/dlltest.exe > /tmp/w32dll 2>&1
grep -q "LOSEDOZE DLL OK" /tmp/w32dll 2>/dev/null && echo "LOSEDOZE DLL OK" > /dev/console || echo "LOSEDOZE DLL FAIL" > /dev/console

# PE32 (i386) in the 64-bit host: parsed and rebased (HIGHLOW), then refused
# with a pointer to the 32-bit host (see pe_refuse()).
w32host --inspect /opt/losedoze/apps/pe32parse.exe > /tmp/w32pe32 2>&1
w32host /opt/losedoze/apps/pe32parse.exe >> /tmp/w32pe32 2>&1
if grep -q "LOSEDOZE PE32 PARSE OK" /tmp/w32pe32 2>/dev/null && \
   grep -q "run it with w32host32" /tmp/w32pe32 2>/dev/null; then
    echo "LOSEDOZE PE32 PARSE OK" > /dev/console
else
    echo "LOSEDOZE PE32 FAIL" > /dev/console
fi

# 32-bit host w32host32 (static i386 musl): real PE32 EXEs/DLLs execute —
# hello32 (raw WriteFile marker + a wsprintfA-formatted line), dlltest32
# (static import + LoadLibrary/GetProcAddress/FreeLibrary with 32-bit DLLs),
# notes32 (window mapped by the compositor, stdcall wndproc), and a 64-bit
# image must be refused here. No w32host32 in the image: honest SKIP.
if [ -x /usr/bin/w32host32 ]; then
    /usr/bin/w32host32 /opt/losedoze/apps32/hello32.exe > /tmp/w32hello32 2>&1
    E32=$?
    /usr/bin/w32host32 /opt/losedoze/apps/hello.exe > /tmp/w32rev32 2>&1
    R32=$?
    if [ "$E32" = 0 ] && grep -q "LOSEDOZE WIN32 APP OK" /tmp/w32hello32 2>/dev/null && \
       grep -q "LOSEDOZE WS32 OK" /tmp/w32hello32 2>/dev/null && [ "$R32" != 0 ] && \
       grep -q "64-bit host: run it with w32host$" /tmp/w32rev32 2>/dev/null; then
        echo "LOSEDOZE PE32 EXEC OK" > /dev/console
    else
        echo "LOSEDOZE PE32 EXEC FAIL (rc=$E32 rev=$R32)" > /dev/console
    fi
    /usr/bin/w32host32 /opt/losedoze/apps32/dlltest32.exe > /tmp/w32dll32 2>&1
    if grep -q "LOSEDOZE DLL OK" /tmp/w32dll32 2>/dev/null; then
        echo "LOSEDOZE PE32 DLL OK" > /dev/console
    else
        echo "LOSEDOZE PE32 DLL FAIL" > /dev/console
    fi
    B32=$(count_manage)
    /usr/bin/w32host32 /opt/losedoze/apps32/notes32.exe > /tmp/w32notes32 2>&1 &
    i=0
    while [ $i -lt 150 ]; do
        NOW=$(count_manage)
        [ "$NOW" -gt "$B32" ] && break
        sleep 0.2; i=$((i + 1))
    done
    NOW=$(count_manage)
    [ "$NOW" -gt "$B32" ] && echo "LOSEDOZE PE32 GUI OK" > /dev/console || \
        echo "LOSEDOZE PE32 GUI FAIL" > /dev/console
else
    echo "LOSEDOZE PE32 EXEC SKIP (no w32host32)" > /dev/console
fi

# desktop GL demo: opengl32.dll (desktop GL over EGL) bound to the window DC,
# drawn with fixed-function GL, presented with SwapBuffers. The marker carries
# GL's own renderer/version and only appears after the frame reached the surface.
G0=$(count_manage)
rm -f /tmp/w32glok
w32host /opt/losedoze/apps/gldemo.exe > /tmp/w32gl 2>&1 &
i=0
while [ ! -s /tmp/w32glok ] && [ $i -lt 150 ]; do sleep 0.2; i=$((i + 1)); done
[ -s /tmp/w32glok ] && GLOK=1 || GLOK=0
i=0
while [ $i -lt 150 ]; do
    NOW=$(count_manage)
    [ "$NOW" -gt "$G0" ] && break
    sleep 0.2; i=$((i + 1))
done
NOW=$(count_manage)
[ "$NOW" -gt "$G0" ] && GMOK=1 || GMOK=0
if [ "$GLOK" = 1 ] && [ "$GMOK" = 1 ]; then
    echo "LOSEDOZE GL DEMO OK" > /dev/console
else
    echo "LOSEDOZE GL DEMO FAIL (marker=$GLOK mapped=$GMOK)" > /dev/console
fi

# graphics + .NET probes (EGL/GLES2 + desktop GL, Vulkan/lavapipe, .NET runtime)
losegl    > /tmp/losegl.log    2>&1
losvulkan > /tmp/losvulkan.log 2>&1
losdotnet > /tmp/losdotnet.log 2>&1
losgpu    > /tmp/losgpu.log    2>&1
grep -q "LOSEDOZE GL OK"      /tmp/losegl.log    2>/dev/null && echo "LOSEDOZE GL OK"      > /dev/console || echo "LOSEDOZE GL FAIL"      > /dev/console
grep -q "LOSEDOZE GL DESKTOP OK" /tmp/losegl.log 2>/dev/null && echo "LOSEDOZE GL DESKTOP OK" > /dev/console || echo "LOSEDOZE GL DESKTOP FAIL" > /dev/console
grep -q "LOSEDOZE VULKAN OK"  /tmp/losvulkan.log 2>/dev/null && echo "LOSEDOZE VULKAN OK"  > /dev/console || echo "LOSEDOZE VULKAN FAIL"  > /dev/console
grep -q "LOSEDOZE DOTNET OK"  /tmp/losdotnet.log 2>/dev/null && echo "LOSEDOZE DOTNET OK"  > /dev/console || echo "LOSEDOZE DOTNET FAIL"  > /dev/console
grep -q "LOSEDOZE GPU OK"     /tmp/losgpu.log    2>/dev/null && echo "LOSEDOZE GPU OK"     > /dev/console || echo "LOSEDOZE GPU SKIP"     > /dev/console
cat /tmp/losegl.log /tmp/losvulkan.log /tmp/losdotnet.log /tmp/losgpu.log > /dev/console
cat /tmp/w32dll /tmp/w32pe32 /tmp/w32gl > /dev/console 2>/dev/null
cat /tmp/w32hello32 /tmp/w32dll32 /tmp/w32rev32 /tmp/w32notes32 > /dev/console 2>/dev/null
# .NET desktop: a managed app (CoreCLR) that opens a window through the Win32
# layer (libwin32.so) and paints it with GDI. Needs the app's marker AND the
# compositor to have mapped the window (same evidence as the PE GUI app).
if [ -x /usr/bin/losnetdesktop ]; then
    N0=$(count_manage)
    losnetdesktop > /tmp/netdesktop.log 2>&1 &
    i=0
    while ! grep -q "LOSEDOZE .NET DESKTOP" /tmp/netdesktop.log 2>/dev/null && [ $i -lt 250 ]; do sleep 0.2; i=$((i + 1)); done
    i=0
    while [ $i -lt 150 ]; do
        NOW=$(count_manage)
        [ "$NOW" -gt "$N0" ] && break
        sleep 0.2; i=$((i + 1))
    done
    NOW=$(count_manage)
    if grep -q "LOSEDOZE .NET DESKTOP OK" /tmp/netdesktop.log 2>/dev/null && [ "$NOW" -gt "$N0" ]; then
        echo "LOSEDOZE .NET DESKTOP OK" > /dev/console
    else
        echo "LOSEDOZE .NET DESKTOP FAIL (mapped=$((NOW - N0)))" > /dev/console
    fi
fi

# settings app: the same Win32 layer used by a native tool (network on/off,
# wifi, wallpaper, theme, brightness -> ~/.config/losedoze.conf)
if [ -x /usr/bin/loseset ]; then
    loseset > /tmp/loseset.log 2>&1 &
    i=0
    while ! grep -q "LOSEDOZE SETTINGS" /tmp/loseset.log 2>/dev/null && [ $i -lt 150 ]; do sleep 0.2; i=$((i + 1)); done
    grep -q "LOSEDOZE SETTINGS OK" /tmp/loseset.log 2>/dev/null && echo "LOSEDOZE SETTINGS OK" > /dev/console || echo "LOSEDOZE SETTINGS FAIL" > /dev/console
    # live reload: rewrite the shell's config file and wait for the shell to
    # notice the new mtime and apply it (that is the settings app's contract)
    H=${HOME:-/root}
    mkdir -p "$H/.config"
    echo "bright=90" > "$H/.config/losedoze.conf"
    i=0
    while ! grep -q "config reloaded" /var/log/losedoze.log 2>/dev/null && [ $i -lt 75 ]; do sleep 0.2; i=$((i + 1)); done
    grep -q "config reloaded" /var/log/losedoze.log 2>/dev/null \
        && echo "LOSEDOZE SETTINGS RELOAD OK" > /dev/console \
        || echo "LOSEDOZE SETTINGS RELOAD FAIL" > /dev/console
fi
# .NET WinForms: a managed app that runs a window through the losedoze WinForms
# framework (our System.Windows.Forms on libwin32.so) and paints it with GDI.
if [ -x /usr/bin/loswinforms ]; then
    F0=$(count_manage)
    loswinforms > /tmp/winforms.log 2>&1 &
    i=0
    while ! grep -q "LOSEDOZE WINFORMS" /tmp/winforms.log 2>/dev/null && [ $i -lt 250 ]; do sleep 0.2; i=$((i + 1)); done
    i=0
    while [ $i -lt 150 ]; do
        NOW=$(count_manage)
        [ "$NOW" -gt "$F0" ] && break
        sleep 0.2; i=$((i + 1))
    done
    NOW=$(count_manage)
    if grep -q "LOSEDOZE WINFORMS OK" /tmp/winforms.log 2>/dev/null && [ "$NOW" -gt "$F0" ]; then
        echo "LOSEDOZE WINFORMS OK" > /dev/console
    else
        echo "LOSEDOZE WINFORMS FAIL (mapped=$((NOW - F0)))" > /dev/console
    fi
fi

cat /tmp/netdesktop.log /tmp/loseset.log /tmp/winforms.log > /dev/console 2>/dev/null
cat /tmp/winforms.log > /dev/console 2>/dev/null

# crash evidence: kernel fault reports (ip + module offset) and the
# compositor's own log tail (its last actions before dying)
dmesg 2>/dev/null | grep -i "segfault\|trap\|Call Trace\|general protection" > /dev/console
tail -40 /var/log/losedoze.log > /dev/console 2>&1
EOF
    chmod 755 "$ROOT/etc/losedoze/autostart.sh"
fi

# ---------------------------------------------------------------- GRUB install bits
say "11/14 GRUB install bits (boot sector + core image)"
command -v grub2-mkimage >/dev/null 2>&1 || die "grub2-mkimage not found"
[ -e /usr/lib/grub/i386-pc/boot.img ] || die "GRUB i386-pc boot.img missing"
# The core image finds the installed root by its filesystem label and loads
# the grub.cfg on it. The boot sector (first 446 bytes of sector 0) goes in
# front of the partition table, the core image in the gap behind the MBR.
cat > build/grub-embedded.cfg <<'EOF'
search --label LOSEDOZE --set=root
configfile ($root)/boot/grub/grub.cfg
EOF
grub2-mkimage -O i386-pc -o build/grub-core.img -c build/grub-embedded.cfg \
    -p /boot/grub ext2 part_msdos biosdisk configfile search search_label \
    linux serial terminal normal boot > build/grub-mkimage.log 2>&1 \
    || { cat build/grub-mkimage.log; die "grub2-mkimage failed"; }
cp /usr/lib/grub/i386-pc/boot.img build/grub-boot.img
mkdir -p "$ROOT/usr/lib/losedoze/installer"
put build/grub-boot.img "$ROOT/usr/lib/losedoze/installer/boot.img"
put build/grub-core.img "$ROOT/usr/lib/losedoze/installer/core.img"

# ---------------------------------------------------------------- rootfs.ext4
say "12/14 rootfs.ext4"
USED_K=$(du -sk "$ROOT" | cut -f1)
SIZE_K=$((USED_K + USED_K / 2 + 262144))
printf 'used=%sK size=%sK\n' "$USED_K" "$SIZE_K"
rm -f build/rootfs.ext4
truncate -s "${SIZE_K}K" build/rootfs.ext4
mke2fs -F -t ext4 -L LOSEDOZE -d "$ROOT" build/rootfs.ext4 > build/mke2fs.log 2>&1 \
    || { cat build/mke2fs.log; die "mke2fs failed"; }

# ---------------------------------------------------------------- initramfs
say "13/14 initramfs"
busybox_install "$INITRD"
mkdir -p "$INITRD"/{dev,proc,sys,newroot,cdrom,modules,etc}
put tools/initramfs-init "$INITRD/init"
chmod 755 "$INITRD/init"
cp "$ROOT/lib/modules/$KVER/kernel/drivers/block/loop.ko" "$INITRD/modules/loop.ko"
cp "$ROOT/lib/modules/$KVER/kernel/fs/isofs/isofs.ko" "$INITRD/modules/isofs.ko"
( cd "$INITRD" && find . | cpio -o -H newc --quiet ) | gzip -9 > build/initramfs.cpio.gz

# ---------------------------------------------------------------- ISO
say "14/14 ISO"
mkdir -p build/iso/boot/grub
put "$KIMG" build/iso/boot/vmlinuz
cp build/initramfs.cpio.gz build/iso/boot/initramfs.cpio.gz
cp build/rootfs.ext4 build/iso/rootfs.ext4
cp grub/grub.cfg build/iso/boot/grub/grub.cfg
grub2-mkrescue -o "$ISO" build/iso > build/grub-mkrescue.log 2>&1 \
    || { tail -20 build/grub-mkrescue.log; die "grub2-mkrescue failed"; }

say "done"
ls -lh "$ISO"
printf 'boot test: ./tools/smoke.sh\n'
