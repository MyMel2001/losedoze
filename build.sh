#!/bin/bash
# Build the Losedoze live ISO.
#   ./build.sh                  release image (no smoke hooks)
#   SMOKE_AUTOSTART=1 ./build.sh  dev image (Win32 self-test on login)
#
# Graphics policy: NO Xorg/X11/Xext/xkbcomp/X libraries in the image.
# GPU display acceleration = in-tree DRM/KMS drivers (i915, amdgpu, nouveau,
# radeon, bochs) with hardware scanout + page flip; the shell composites on CPU.
# Fedora Mesa's libEGL_mesa hard-links libX11 (EGL_X11 platform), so Mesa EGL
# is excluded by the X ban; kms.c still dlopens libEGL/libgbm at runtime and
# degrades gracefully ("EGL: none") when absent. Ceiling + upgrade path: AGENT_NOTES.md.
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
say "0/10 checks"
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
say "1/10 staging overlay"
rm -rf "$ROOT" "$INITRD" build/iso
mkdir -p "$ROOT"
cp -a rootfs-overlay/. "$ROOT/"
mkdir -p "$ROOT"/{root,tmp,run,home,dev,proc,sys,newroot,var/log,var/tmp,var/cache}
chmod 1777 "$ROOT/tmp" "$ROOT/var/tmp"
chmod 755 "$ROOT/usr/local/bin/session.sh" "$ROOT/etc/init.d/rcS"

# ---------------------------------------------------------------- busybox
say "2/10 busybox userland"
busybox_install "$ROOT"

# ---------------------------------------------------------------- programs
say "3/10 native programs (loseshell, losyscall)"
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
say "4/10 wayland/xkb runtime + gpu firmware"
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

# ---------------------------------------------------------------- fonts + fontconfig
say "5/10 fonts"
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
say "6/10 kernel modules"
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
# PE32+ loader + own ntdll/kernel32/user32/gdi32 over Linux syscalls, hosted by
# w32host (ELF) which runs the PE app as a child of loseshell. No Wine anywhere.
say "7/10 win32 subsystem (w32host + PE apps)"
mkdir -p build build/winapps
python3 win/gen_ntsyscalls.py build/ntsyscalls.inc
python3 win/gen_importlibs.py win/w32host.c build/winapps
# w32host: ELF host process (hand-rolled Wayland client -> /run/losedoze0)
gcc -O2 -Wall -Wno-unused-function -o build/w32host \
    win/pe.c win/ntdll.c win/kernel32.c win/user32.c win/gdi32.c \
    win/wlclient.c win/w32host.c shell/input.c \
    -Iwin -Ishell -Ibuild -lm
# PE apps: freestanding PE32+ (ms x64 ABI), linked with -mi386pep
PE_CFLAGS="--target=x86_64-pc-windows-msvc -ffreestanding -fno-builtin -O2 -Iwin"
PE_LDFLAGS="-mi386pep"
build_pe_app() {  # $1=name  $2=subsystem(windows|console)
    local n=$1 sub=$2
    "$PE_CC" $PE_CFLAGS -c "win/apps/$n.c" -o "build/winapps/$n.o"
    "$PE_CC" $PE_CFLAGS -c win/apps/crt.s -o build/winapps/crt.o
    "$PE_LD" $PE_LDFLAGS --subsystem "$sub" -e "${n}_entry" \
        -L build/winapps -o "build/winapps/$n.exe" \
        "build/winapps/$n.o" build/winapps/crt.o \
        -lntdll -lkernel32 -luser32 -lgdi32
    need "build/winapps/$n.exe"
}
build_pe_app hello console
build_pe_app notes windows
put build/w32host "$ROOT/usr/bin/w32host"
mkdir -p "$ROOT/opt/losedoze/apps"
put build/winapps/hello.exe "$ROOT/opt/losedoze/apps/hello.exe"
put build/winapps/notes.exe  "$ROOT/opt/losedoze/apps/notes.exe"

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
EOF
    chmod 755 "$ROOT/etc/losedoze/autostart.sh"
fi

# ---------------------------------------------------------------- rootfs.ext4
say "8/10 rootfs.ext4"
USED_K=$(du -sk "$ROOT" | cut -f1)
SIZE_K=$((USED_K + USED_K / 2 + 262144))
printf 'used=%sK size=%sK\n' "$USED_K" "$SIZE_K"
rm -f build/rootfs.ext4
truncate -s "${SIZE_K}K" build/rootfs.ext4
mke2fs -F -t ext4 -L LOSEDOZE -d "$ROOT" build/rootfs.ext4 > build/mke2fs.log 2>&1 \
    || { cat build/mke2fs.log; die "mke2fs failed"; }

# ---------------------------------------------------------------- initramfs
say "9/10 initramfs"
busybox_install "$INITRD"
mkdir -p "$INITRD"/{dev,proc,sys,newroot,cdrom,modules,etc}
put tools/initramfs-init "$INITRD/init"
chmod 755 "$INITRD/init"
cp "$ROOT/lib/modules/$KVER/kernel/drivers/block/loop.ko" "$INITRD/modules/loop.ko"
cp "$ROOT/lib/modules/$KVER/kernel/fs/isofs/isofs.ko" "$INITRD/modules/isofs.ko"
( cd "$INITRD" && find . | cpio -o -H newc --quiet ) | gzip -9 > build/initramfs.cpio.gz

# ---------------------------------------------------------------- ISO
say "10/10 ISO"
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
