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
MODULES_TREE="kernel/drivers/gpu kernel/drivers/video kernel/drivers/cec kernel/lib"

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
say "0/13 checks"
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
say "1/13 staging overlay"
rm -rf "$ROOT" "$INITRD" build/iso
mkdir -p "$ROOT"
cp -a rootfs-overlay/. "$ROOT/"
mkdir -p "$ROOT"/{root,tmp,run,home,dev,proc,sys,newroot,var/log,var/tmp,var/cache}
chmod 1777 "$ROOT/tmp" "$ROOT/var/tmp"
chmod 755 "$ROOT/usr/local/bin/session.sh" "$ROOT/etc/init.d/rcS"

# ---------------------------------------------------------------- busybox
say "2/13 busybox userland"
busybox_install "$ROOT"

# ---------------------------------------------------------------- programs
say "3/13 native programs (loseshell, losyscall)"
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
put build/loseshell "$ROOT/usr/bin/loseshell"
put build/losyscall "$ROOT/usr/bin/losyscall"

# ---------------------------------------------------------------- wayland + firmware (no X, no Mesa-EGL)
say "4/13 wayland/xkb runtime + gpu firmware"
mkdir -p "$ROOT/usr/lib64" "$ROOT/lib/firmware"
# X-free runtime libs for the hand-rolled Wayland client (wlclient.c)
copy_named /usr/lib64/libwayland-client.so.0 /usr/lib64/libwayland-egl.so.1 \
           /usr/lib64/libwayland-cursor.so.0 /usr/lib64/libxkbcommon.so.0 \
           /usr/lib64/libxkbregistry.so.0
copy_libs /usr/lib64/libwayland-client.so.0 /usr/lib64/libwayland-egl.so.1 \
          /usr/lib64/libwayland-cursor.so.0 /usr/lib64/libxkbcommon.so.0 \
          /usr/lib64/libxkbregistry.so.0
# GPU microcode for Intel/AMD KMS drivers
for fw in i915 amdgpu radeon; do
    [ -d "/usr/lib/firmware/$fw" ] && cp -a "/usr/lib/firmware/$fw" "$ROOT/lib/firmware/"
done
# interpreter
mkdir -p "$ROOT/lib64"
put /lib64/ld-linux-x86-64.so.2 "$ROOT/lib64/ld-linux-x86-64.so.2"

# ---------------------------------------------------------------- mesa graphics stack
# X-free Mesa built from source (Fedora's libEGL_mesa hard-links libX11).
# Gives the guest EGL + GLES2 (llvmpipe) and Vulkan (lavapipe).
say "5/13 mesa graphics stack (EGL/GLES2/gbm/lavapipe)"
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
    # X-free runtime deps of the mesa stack
    copy_named /usr/lib64/libvulkan.so.1 /usr/lib64/libdrm.so.2 /usr/lib64/libexpat.so.1 \
               /usr/lib64/libzstd.so.1 /usr/lib64/libstdc++.so.6 /usr/lib64/libgcc_s.so.1 \
               /usr/lib64/libSPIRV-Tools.so
    copy_libs /usr/lib64/libvulkan.so.1 /usr/lib64/libdrm.so.2 /usr/lib64/libexpat.so.1 \
              /usr/lib64/libzstd.so.1 /usr/lib64/libstdc++.so.6 /usr/lib64/libgcc_s.so.1 \
              /usr/lib64/libSPIRV-Tools.so
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
say "6/13 fonts"
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
say "7/13 kernel modules"
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

# ---------------------------------------------------------------- homemade Win32 subsystem
# PE32+ (x86-64) and PE32 (i386) loader + own ntdll/kernel32/user32/gdi32
# over Linux syscalls, hosted by w32host (ELF) which runs the PE app as a child
# of loseshell. Parsing is dual-arch; execution is 64-bit only. No Wine anywhere.
say "8/13 win32 subsystem (w32host + PE apps)"
mkdir -p build build/winapps
python3 win/gen_ntsyscalls.py build/ntsyscalls.inc
python3 win/gen_importlibs.py win/w32host.c build/winapps
# PE32 (i386) fixture: no i386pe linker script on this host, so the test image
# is generated (see tools/gen_pe32.py) — the loader parses it for real.
python3 tools/gen_pe32.py build/winapps/hello32.exe
# w32host: ELF host process (hand-rolled Wayland client -> /run/losedoze0)
gcc -O2 -Wall -Wno-unused-function -o build/w32host \
    win/pe.c win/ntdll.c win/kernel32.c win/user32.c win/gdi32.c win/gl32.c \
    win/wsprintf.c win/wlclient.c win/w32host.c shell/input.c \
    -Iwin -Ishell -Ibuild -lm -ldl
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
put build/w32host "$ROOT/usr/bin/w32host"
mkdir -p "$ROOT/opt/losedoze/apps"
put build/winapps/hello.exe    "$ROOT/opt/losedoze/apps/hello.exe"
put build/winapps/notes.exe    "$ROOT/opt/losedoze/apps/notes.exe"
put build/winapps/dlltest.exe  "$ROOT/opt/losedoze/apps/dlltest.exe"
put build/winapps/gldemo.exe   "$ROOT/opt/losedoze/apps/gldemo.exe"
put build/winapps/hello32.exe  "$ROOT/opt/losedoze/apps/hello32.exe"
put build/winapps/dllmath.dll  "$ROOT/opt/losedoze/apps/dllmath.dll"
put build/winapps/dllstate.dll "$ROOT/opt/losedoze/apps/dllstate.dll"

# ---------------------------------------------------------------- .NET runtime
# A Linux .NET runtime lives in the guest so the OS can run .NET programs.
# (Running .NET *PE* apps through the Win32 layer needs a real NT layer --
# see AGENT_NOTES.md "ceilings".)
say "9/13 .NET runtime"
DOTNET_ROOT=${DOTNET_ROOT:-/tmp/dotnet}
DOTNET_APP=${DOTNET_APP:-/tmp/dnapp/out}
if [ -x "$DOTNET_ROOT/dotnet" ] && [ -f "$DOTNET_APP/dnapp.dll" ]; then
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
    mkdir -p "$ROOT/opt/dotnet/apps/dnapp"
    cp -a "$DOTNET_APP/." "$ROOT/opt/dotnet/apps/dnapp/"
    copy_named /usr/lib64/libstdc++.so.6 /usr/lib64/libgcc_s.so.1 /usr/lib64/libz.so.1 \
               /usr/lib64/libdl.so.2 /usr/lib64/libpthread.so.0 /usr/lib64/librt.so.1
    copy_libs /usr/lib64/libstdc++.so.6 /usr/lib64/libdl.so.2 /usr/lib64/libpthread.so.0 \
              /usr/lib64/librt.so.1
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
else
    printf 'skip dotnet: runtime %s or app %s missing\n' "$DOTNET_ROOT" "$DOTNET_APP"
fi

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
echo "LOSEDOZE AUTOSTART BEGIN" > /dev/console
export XDG_RUNTIME_DIR=/run WAYLAND_DISPLAY=losedoze0
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

# PE32 (i386): parsed, relocated, imports bound -> then refused, because only
# 64-bit PE images are executed (see w32host --inspect / pe_refuse()).
w32host --inspect /opt/losedoze/apps/hello32.exe > /tmp/w32pe32 2>&1
w32host /opt/losedoze/apps/hello32.exe >> /tmp/w32pe32 2>&1
if grep -q "LOSEDOZE PE32 PARSE OK" /tmp/w32pe32 2>/dev/null && \
   grep -q "cannot execute in a 64-bit process" /tmp/w32pe32 2>/dev/null; then
    echo "LOSEDOZE PE32 PARSE OK" > /dev/console
else
    echo "LOSEDOZE PE32 FAIL" > /dev/console
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
grep -q "LOSEDOZE GL OK"      /tmp/losegl.log    2>/dev/null && echo "LOSEDOZE GL OK"      > /dev/console || echo "LOSEDOZE GL FAIL"      > /dev/console
grep -q "LOSEDOZE GL DESKTOP OK" /tmp/losegl.log 2>/dev/null && echo "LOSEDOZE GL DESKTOP OK" > /dev/console || echo "LOSEDOZE GL DESKTOP FAIL" > /dev/console
grep -q "LOSEDOZE VULKAN OK"  /tmp/losvulkan.log 2>/dev/null && echo "LOSEDOZE VULKAN OK"  > /dev/console || echo "LOSEDOZE VULKAN FAIL"  > /dev/console
grep -q "LOSEDOZE DOTNET OK"  /tmp/losdotnet.log 2>/dev/null && echo "LOSEDOZE DOTNET OK"  > /dev/console || echo "LOSEDOZE DOTNET FAIL"  > /dev/console
cat /tmp/losegl.log /tmp/losvulkan.log /tmp/losdotnet.log > /dev/console
cat /tmp/w32dll /tmp/w32pe32 /tmp/w32gl > /dev/console 2>/dev/null
EOF
    chmod 755 "$ROOT/etc/losedoze/autostart.sh"
fi

# ---------------------------------------------------------------- rootfs.ext4
say "11/13 rootfs.ext4"
USED_K=$(du -sk "$ROOT" | cut -f1)
SIZE_K=$((USED_K + USED_K / 2 + 262144))
printf 'used=%sK size=%sK\n' "$USED_K" "$SIZE_K"
rm -f build/rootfs.ext4
truncate -s "${SIZE_K}K" build/rootfs.ext4
mke2fs -F -t ext4 -L LOSEDOZE -d "$ROOT" build/rootfs.ext4 > build/mke2fs.log 2>&1 \
    || { cat build/mke2fs.log; die "mke2fs failed"; }

# ---------------------------------------------------------------- initramfs
say "12/13 initramfs"
busybox_install "$INITRD"
mkdir -p "$INITRD"/{dev,proc,sys,newroot,cdrom,modules,etc}
put tools/initramfs-init "$INITRD/init"
chmod 755 "$INITRD/init"
cp "$ROOT/lib/modules/$KVER/kernel/drivers/block/loop.ko" "$INITRD/modules/loop.ko"
cp "$ROOT/lib/modules/$KVER/kernel/fs/isofs/isofs.ko" "$INITRD/modules/isofs.ko"
( cd "$INITRD" && find . | cpio -o -H newc --quiet ) | gzip -9 > build/initramfs.cpio.gz

# ---------------------------------------------------------------- ISO
say "13/13 ISO"
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
