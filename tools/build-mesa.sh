#!/bin/bash
# Build an X-free Mesa for the losedoze image: EGL + GLES2 + gbm (llvmpipe)
# and Vulkan (lavapipe). No X11/xcb anywhere -- Fedora's libEGL_mesa hard-links
# libX11, so Mesa is built from source instead.
#
#   sh tools/build-mesa.sh
#
# Result: $DESTDIR/usr/{lib64,include,share} -- a DESTDIR tree that build.sh
# copies into the image (MESA_ROOT=$DESTDIR). build.sh also needs the Vulkan
# headers, which Mesa does not install, so they are copied from the source tree.
set -eu

MESA_VERSION=${MESA_VERSION:-mesa-26.2.4}
WORK=${WORK:-/tmp/mesa}
DESTDIR=${DESTDIR:-$WORK/root}
PREFIX=${PREFIX:-/usr}          # must stay /usr: the driver path is compiled in
NPROC=${NPROC:-$(nproc)}
URL="https://gitlab.freedesktop.org/api/v4/projects/mesa%2Fmesa/repository/archive.tar.gz?sha=$MESA_VERSION"

say() { printf '\n=== %s\n' "$*"; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing $1" >&2; exit 1; }; }

need meson
need ninja
need llvm-config
need cc
need pkg-config

SRC="$WORK/mesa-src"
BUILD="$SRC/build"

say "fetch $MESA_VERSION"
mkdir -p "$WORK"
if [ ! -d "$SRC" ]; then
    curl -sSL -o "$WORK/mesa.tar.gz" "$URL"
    mkdir -p "$SRC"
    tar xf "$WORK/mesa.tar.gz" -C "$SRC" --strip-components=1
fi

say "configure (X-free: wayland platform only, no glx, no glvnd)"
if [ ! -d "$BUILD" ]; then
    meson setup "$BUILD" "$SRC" \
        --prefix="$PREFIX" \
        -Dplatforms=wayland \
        -Dgbm=enabled \
        -Degl=enabled \
        -Dgles2=enabled \
        -Dopengl=true \
        -Dglvnd=disabled \
        -Dglx=disabled \
        -Dgallium-drivers=llvmpipe,nouveau,radeonsi \
        -Dvulkan-drivers=swrast \
        -Dllvm=enabled \
        -Dshared-llvm=disabled
        # ponytail: no iris (Intel gallium) — its SPIR-V shaders need mesa_clc,
        # which aborts ("double free or corruption") in a static-LLVM build.
        # Add "iris" back once mesa_clc is fixed; Intel then gets 3D accel.
fi

say "build + install into $DESTDIR"
ninja -C "$BUILD"
DESTDIR="$DESTDIR" ninja -C "$BUILD" install

say "stage the vulkan headers (mesa vendors them but does not install them)"
mkdir -p "$DESTDIR/usr/include/vulkan"
cp -a "$SRC/include/vulkan/." "$DESTDIR/usr/include/vulkan/"

say "done"
printf 'build the image with:\n  MESA_ROOT=%s MESA_SRC=%s SMOKE_AUTOSTART=1 ./build.sh\n' \
    "$DESTDIR" "$SRC"
printf 'and check the stack:\n'
printf '  ldd %s/usr/lib64/libEGL.so.1 | grep -iE "x11|xcb" && echo "X LEAK"\n' "$DESTDIR"
