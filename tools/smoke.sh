#!/bin/bash
# Headless QEMU boot check for the Losedoze ISO:
#  waits for console markers, takes a framebuffer screendump, verifies the
#  desktop actually drew (color variety + taskbar row).
#
# SMOKE_INSTALL=1 adds the disk installer check: the live system installs
# itself onto a second disk, that disk boots on its own, and a file written
# during the first installed boot is still there after the next reboot.
set -u
cd "$(cd "$(dirname "$0")/.." && pwd)"

ISO=${ISO:-build/losedoze.iso}
TIMEOUT_INIT=${TIMEOUT_INIT:-300}
TIMEOUT_SHELL=${TIMEOUT_SHELL:-300}
TIMEOUT_WIN32=${TIMEOUT_WIN32:-420}
TIMEOUT_INSTALL=${TIMEOUT_INSTALL:-1200}
TIMEOUT_TERM=${TIMEOUT_TERM:-120}
TIMEOUT_BROWSER=${TIMEOUT_BROWSER:-120}
# GPU under test: "std" (QEMU std VGA -> bochs-drm) or "virtio" (virtio-gpu
# driver). Both are open-source in-tree DRM drivers.
case "${SMOKE_GPU:-std}" in
    virtio) VGA_ARG="-device virtio-vga" ;;
    *)      VGA_ARG="-vga std" ;;
esac
# the guest's input path (PS/2 keyboard + relative mouse) is always present;
# SMOKE_UI only adds the interaction phase below, no extra device needed
QMP=/tmp/loseterm-qmp.sock
SER=/tmp/loseterm-serial.log
SER_SOCK=/tmp/loseterm-ser.sock
SHOT=${SHOT:-/tmp/loseterm.ppm}
TARGET=${TARGET:-/tmp/loseterm-target.img}
SWAP_MB=${SWAP_MB:-512}
INSTALL=0
[ "${SMOKE_INSTALL:-0}" = 1 ] && INSTALL=1

[ -f "$ISO" ] || { echo "missing $ISO — run ./build.sh first"; exit 1; }
KVM_ARG=""
[ -e /dev/kvm ] && KVM_ARG="-enable-kvm"
QPID=""
SER_PID=""
trap 'kill $QPID $SER_PID 2>/dev/null; wait $QPID $SER_PID 2>/dev/null' EXIT

# Stream the guest console over a unix socket instead of `-serial file:`: a
# file chardev is buffered by QEMU in 4 KB chunks, so the smoke would only see
# the markers once the whole boot had been buffered -- and the framebuffer
# shot would land long after the Win32 client window was covered by later
# apps. (A FIFO is not usable here: QEMU opens the pipe read/write and would
# steal half the console output from the reader.)
serial_reader() {   # $1 = log file
    rm -f "$SER_SOCK"
    python3 - "$SER_SOCK" "$1" <<'PY' &
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
while True:
    try:
        s.connect(sys.argv[1])
        break
    except OSError:
        time.sleep(0.2)
out = open(sys.argv[2], "wb", buffering=0)
while True:
    b = s.recv(65536)
    if not b:
        break
    out.write(b)
PY
    SER_PID=$!
}

launch() {   # $1 = serial log, rest = extra qemu arguments
    SER=$1
    shift
    # drop the previous run's console: the reader truncates only once it has
    # connected to QEMU, so a stale log would satisfy the marker greps and the
    # smoke would "see" a boot that is not this one
    rm -f "$QMP" "$SER"
    serial_reader "$SER"
    qemu-system-x86_64 $KVM_ARG -m 3072 -smp 2 $VGA_ARG \
        -netdev user,id=n0 -device e1000,netdev=n0 \
        -display none -chardev socket,id=ser0,path="$SER_SOCK",server=on,wait=off \
        -serial chardev:ser0 -no-reboot \
        -qmp "unix:$QMP,server,nowait" "$@" &
    QPID=$!
}

stop_qemu() {
    kill "$QPID" 2>/dev/null
    wait "$QPID" 2>/dev/null
    kill "$SER_PID" 2>/dev/null
    wait "$SER_PID" 2>/dev/null
    QPID=""
    SER_PID=""
}

wait_marker() {
    local mark=$1 timeout=$2 t=0
    while [ "$t" -lt "$timeout" ]; do
        grep -q "$mark" "$SER" 2>/dev/null && { echo "  marker: $mark"; return 0; }
        # fail fast on hard-fail markers instead of waiting out the timeout
        grep -qE "LOSEDOZE [A-Z0-9 ]*FAIL|modeset failed|no KMS device" "$SER" 2>/dev/null \
            && { echo "fail marker seen while waiting for '$mark'"; return 1; }
        kill -0 "$QPID" 2>/dev/null || { echo "qemu exited before '$mark'"; return 1; }
        sleep 2
        t=$((t + 2))
    done
    echo "timeout waiting for '$mark'"
    return 1
}
fail() {
    echo "SMOKE FAIL: $1"
    echo "--- serial tail ---"
    tail -40 "$SER" 2>/dev/null
    exit 1
}

# the disk the installer targets (IDE -> /dev/sda in the guest)
if [ "$INSTALL" = 1 ]; then
    rm -f "$TARGET"
    qemu-img create -f raw "$TARGET" 2G > /dev/null || fail "creating $TARGET"
fi

echo "booting $ISO (kvm: ${KVM_ARG:-no})"
if [ "$INSTALL" = 1 ]; then
    launch "$SER" -cdrom "$ISO" -boot d -hda "$TARGET"
else
    launch "$SER" -cdrom "$ISO" -boot d
fi
wait_marker "LOSEDOZE INIT OK" "$TIMEOUT_INIT"   || fail "init"
# NIC driver + DHCP (rcS runs losenet in the background): real address needed
wait_marker "LOSEDOZE NET OK" 120                || fail "network"
wait_marker "LOSEDOZE READY"   "$TIMEOUT_SHELL"  || fail "loseshell"
wait_marker "EGL: "            30                || fail "egl marker"

# QMP screendump of the guest framebuffer -> $SHOT
shot() {
    python3 - "$QMP" "$SHOT" <<'PY'
import json, socket, sys, time
qmp, shot = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(30):
    try:
        s.connect(qmp)
        break
    except OSError:
        time.sleep(1)
else:
    sys.exit("QMP connect failed")
f = s.makefile("rwb")
f.readline()
def cmd(obj):
    f.write(json.dumps(obj).encode() + b"\n")
    f.flush()
    while True:
        line = f.readline()
        if not line:
            sys.exit("QMP connection closed")
        m = json.loads(line)
        if "return" in m or "error" in m:
            return m
cmd({"execute": "qmp_capabilities"})
r = cmd({"execute": "screendump", "arguments": {"filename": shot}})
if "error" in r:
    sys.exit("screendump failed: %s" % r)
print("screendump:", shot)
PY
    [ $? -eq 0 ] || fail "screendump"
}

# The desktop self-test suite belongs to the plain smoke run; the install
# smoke only needs the boot path (the installer runs in the autostart).
if [ "$INSTALL" = 0 ]; then
WIN32_SEEN=0
# dev images print AUTOSTART BEGIN right after login; release images never do.
# Wait for it (a one-shot grep races the login prompt and silently skips all
# the Win32 checks).
if wait_marker "LOSEDOZE AUTOSTART BEGIN" 90; then
    wait_marker "LOSEDOZE WAYLAND OK" "$TIMEOUT_SHELL" || fail "wayland-socket"
    wait_marker "LOSEDOZE WIN32 OK" "$TIMEOUT_WIN32"   || fail "win32"
    wait_marker "LOSEDOZE DMABUF" 120                  || fail "dmabuf"
    wait_marker "LOSEDOZE DLL OK"        120           || fail "pe-dll"
    wait_marker "LOSEDOZE PE32 PARSE OK" 120           || fail "pe32-dualarch"
    # 32-bit host: PE32 EXEs/DLLs execute (SKIP is the honest state when the
    # image was built without zig and thus has no w32host32)
    if wait_marker "LOSEDOZE PE32 EXEC" 180; then
        if grep -q "LOSEDOZE PE32 EXEC SKIP" "$SER" 2>/dev/null; then
            echo "  (no w32host32 in image: PE32 execution skipped)"
        else
            wait_marker "LOSEDOZE PE32 DLL" 120 || fail "pe32-dll-exec"
            wait_marker "LOSEDOZE PE32 GUI" 180 || fail "pe32-gui"
        fi
    else
        fail "pe32-exec"
    fi
    wait_marker "LOSEDOZE GL DEMO OK"    180           || fail "gl-demo"
    wait_marker "LOSEDOZE GL OK"     120                || fail "opengl"
    wait_marker "LOSEDOZE GL DESKTOP OK" 120            || fail "desktop-gl"
    wait_marker "LOSEDOZE VULKAN OK" 120                || fail "vulkan"
    wait_marker "LOSEDOZE DOTNET OK" 180                || fail "dotnet"
    wait_marker "LOSEDOZE GPU OK" 120                   || fail "gpu-probe"
    wait_marker "LOSEDOZE .NET DESKTOP OK" 250          || fail "dotnet-desktop"
    wait_marker "LOSEDOZE WINFORMS OK" 250             || fail "winforms"
    wait_marker "LOSEDOZE SETTINGS OK" 180              || fail "settings-app"
    wait_marker "LOSEDOZE SETTINGS RELOAD OK" 60        || fail "settings-reload"
    WIN32_SEEN=1
    sleep 4   # let the compositor draw the last client windows
    shot
else
    sleep 8   # let the UI settle
    shot
fi

sleep 2
python3 - "$SHOT" "$WIN32_SEEN" <<'PY'
import sys
p = sys.argv[1]
want_win32 = sys.argv[2] == "1"
try:
    data = open(p, "rb").read()
except OSError as e:
    sys.exit("no screendump: %s" % e)

# header: magic, width, height, maxval (comments allowed)
toks, i = [], 0
while len(toks) < 4:
    while i < len(data) and data[i:i+1].isspace():
        i += 1
    if data[i:i+1] == b"#":
        while i < len(data) and data[i:i+1] != b"\n":
            i += 1
        continue
    j = i
    while j < len(data) and not data[j:j+1].isspace():
        j += 1
    toks.append(data[i:j]); i = j
i += 1
w, h = int(toks[1]), int(toks[2])
px = data[i:i + w * h * 3]

colors = set()
for off in range(0, len(px) - 3, 3 * 17):
    colors.add(bytes(px[off:off+3]))

# loseshell dark taskbar ~ #1f1f1f drawn as an opaque band at the bottom
found_tb = False
row = (h - 30) * w * 3
for x in range(0, w, 4):
    o = row + x * 3
    r, g, b = px[o], px[o+1], px[o+2]
    if abs(r-g) < 5 and abs(g-b) < 5 and 0x1c <= r <= 0x3a:
        found_tb = True
        break

print(f"framebuffer {w}x{h}, distinct colors (sampled)={len(colors)}, taskbar band={found_tb}")

ok = len(colors) >= 12 and found_tb

if want_win32:
    # Client-drawn pixels, not one fixed rect: the later client windows (GL
    # demo, .NET desktop, settings) stack on top of notes.exe, so its white
    # canvas only shows in strips -- but nothing the shell draws is anywhere
    # near white, so a large white area can only come from a Win32 client.
    white = 0
    for y in range(40, h - 60, 2):        # everything above the taskbar band
        base = y * w * 3
        for x in range(0, w, 2):
            o = base + x * 3
            r, g, b = px[o], px[o+1], px[o+2]
            if r >= 235 and g >= 235 and b >= 235:
                white += 1
    # the settings app highlights the active row in orange (237,111,47):
    # a second, independent client-drawn colour
    orange = 0
    for y in range(0, h - 60):
        base = y * w * 3
        for x in range(0, w):
            o = base + x * 3
            if px[o] == 237 and px[o+1] == 111 and px[o+2] == 47:
                orange += 1
    print(f"win32 client white pixels={white}, orange pixels={orange}")
    if white < 8000:
        sys.exit("win32 client window not visible in framebuffer")
    if orange < 5000:
        sys.exit("settings client window not visible in framebuffer")
    ok = True

if ok:
    print("SMOKE PASS")
else:
    sys.exit(1)
PY
[ $? -eq 0 ] || fail "framebuffer check (shot: $SHOT)"
fi

# the last two client apps (terminal, browser) open windows that stack on top
# of the settings client -- verify their markers AFTER the framebuffer check so
# the screenshot still shows the settings window the check looks for
if wait_marker "LOSEDOZE TERMINAL OK" "$TIMEOUT_TERM"; then
    wait_marker "LOSEDOZE BROWSER OK" "$TIMEOUT_BROWSER" || fail "browser-app"
fi

# ------------------------------------------------- optional UI interaction
# SMOKE_UI=1: drive the running desktop over QMP (keys + absolute mouse) and
# prove the shell reacts: start menu opens WITH text, search filters, and a
# client window can be dragged by its title bar (BUGS.md: dragging missing).
if [ "${SMOKE_UI:-0}" = 1 ]; then
    python3 - "$QMP" "$SHOT" <<'PY' || fail "ui-interaction"
import json, socket, sys, time
qmp, shot = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(30):
    try:
        s.connect(qmp)
        break
    except OSError:
        time.sleep(1)
else:
    sys.exit("QMP connect failed")
f = s.makefile("rwb")
f.readline()
def cmd(obj):
    f.write(json.dumps(obj).encode() + b"\n")
    f.flush()
    while True:
        line = f.readline()
        if not line:
            sys.exit("QMP connection closed")
        m = json.loads(line)
        if "return" in m or "error" in m:
            return m
cmd({"execute": "qmp_capabilities"})

def send(events):
    r = cmd({"execute": "input-send-event", "arguments": {"events": events}})
    if "error" in r:
        sys.exit("input-send-event failed: %s" % r)

def tap(q):
    send([{"type": "key", "data": {"down": True,  "key": {"type": "qcode", "data": q}}},
          {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": q}}}])
    time.sleep(0.2)

# pointer moves via QMP input-send-event abs (axis values are 0..32767 over
# the surface; FB=1/TITLE_H frame geometry means the guest scales 0..32767 -> 0..w-1)
def move(x, y):
    send([{"type": "abs", "data": {"axis": "x", "value": round(x * 32767 / 1279)}},
          {"type": "abs", "data": {"axis": "y", "value": round(y * 32767 / 799)}}])
    time.sleep(0.15)

def button(down):
    send([{"type": "btn", "data": {"down": down, "button": "left"}}])
    time.sleep(0.1)

def snap():
    r = cmd({"execute": "screendump", "arguments": {"filename": shot}})
    if "error" in r:
        sys.exit("screendump failed: %s" % r)
    time.sleep(0.4)
    d = open(shot, "rb").read()
    toks, i = [], 0
    while len(toks) < 4:
        while d[i:i+1].isspace():
            i += 1
        if d[i:i+1] == b"#":
            while d[i:i+1] != b"\n":
                i += 1
            continue
        j = i
        while not d[j:j+1].isspace():
            j += 1
        toks.append(d[i:j]); i = j
    i += 1
    w, h = int(toks[1]), int(toks[2])
    return w, h, d[i:i + w * h * 3]

def P(px, w, x, y):
    o = (y * w + x) * 3
    return px[o], px[o+1], px[o+2]

def orange_bbox(w, h, px):
    minx, miny, maxx, maxy = 10**9, 10**9, -1, -1
    for y in range(0, h - 60):
        base = y * w * 3
        for x in range(w):
            o = base + x * 3
            if px[o] == 237 and px[o+1] == 111 and px[o+2] == 47:
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    if maxx < 0:
        return None
    return minx, miny, maxx, maxy

def ink(w, px, x0, y0, x1, y1, thresh=25):
    """count pixels that stand out from the band's dominant colour"""
    from collections import Counter
    c = Counter()
    for y in range(y0, y1):
        for x in range(x0, x1):
            c[P(px, w, x, y)] += 1
    bg = c.most_common(1)[0][0]
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = P(px, w, x, y)
            if max(abs(p[i] - bg[i]) for i in range(3)) > thresh:
                n += 1
    return n

def changed(w, ax, ay, bx, by, prev, cur):
    n = 0
    for y in range(ay, by):
        for x in range(ax, bx):
            if P(prev, w, x, y) != P(cur, w, x, y):
                n += 1
    return n

def fail(msg):
    print("ui: %s" % msg)
    sys.exit(1)

# geometry the shell uses for its start popup (START_W/START_H/TB_H)
POP_W, POP_H, TB_H = 600, 640, 48

w, h, base = snap()
bb0 = orange_bbox(w, h, base)
if not bb0:
    fail("no orange settings highlight in baseline shot (settings window missing)")
print("ui: baseline orange bbox %s" % (bb0,))

# 1) open the start menu (Super key-up toggles it in root_key())
tap("meta_l")
time.sleep(0.6)
w, h, s2 = snap()
px0 = (w - POP_W) // 2
py0 = h - TB_H - POP_H - 8
n_ink = ink(w, s2, px0 + 24, py0 + 22, px0 + POP_W - 24, py0 + 60)
print("ui: search-field ink pixels=%d" % n_ink)
if n_ink < 120:
    fail("start menu has no visible text (search placeholder area ink=%d)" % n_ink)

# 1b) the pinned grid must actually list apps: default pinned set (Files, Hello,
# Notepad, Terminal, Browser, Settings) packs six entries into grid row 0
# (item y = 104 + row*92, text at y+54; recommended section moves to y=384)
y0 = py0 + 104 + 40
y1 = py0 + 104 + 92
r0 = ink(w, s2, px0 + 24, y0, px0 + POP_W - 24, y1)
print("ui: pinned grid row0 ink=%d" % r0)
if r0 < 150:
    fail("start menu pinned grid has no visible app text (ink=%d)" % r0)

# 2) type a query -> the result list must change
for ch in "term":
    tap(ch)
time.sleep(0.8)
w, h, s3 = snap()
n_chg = changed(w, px0 + 24, py0 + 70, px0 + POP_W - 24, py0 + 250, s2, s3)
print("ui: pixels changed after typing query=%d" % n_chg)
if n_chg < 300:
    fail("typing in start search did not render results (changed=%d)" % n_chg)

tap("esc")
time.sleep(0.6)

# 3) drag the settings window by its title bar; the orange highlight moves along
# grab point from the baseline geometry: client origin is (frame_x+FB,
# frame_y+TITLE_H) and the orange row sits at client (16, 76) (FB=1, TITLE_H=32)
cx = bb0[0] - 17
cy = bb0[1] - 108
gx, gy = cx + 100, cy + 16
tx, ty = gx + 360, gy + 300
if tx > w - 40 or ty > h - 60:
    fail("drag target off-screen: window at %d,%d" % (cx, cy))
move(gx, gy); button(True)
move(tx, ty)
button(False)
time.sleep(0.8)
w, h, s4 = snap()
bb1 = orange_bbox(w, h, s4)
if not bb1:
    fail("settings window disappeared after drag")
dx, dy = bb1[0] - bb0[0], bb1[1] - bb0[1]
print("ui: orange bbox after drag %s dx=%d dy=%d" % (bb1, dx, dy))
if abs(dx) < 200 or abs(dy) < 200:
    fail("title-bar drag did not move the window (dx=%d dy=%d)" % (dx, dy))
print("ui: OK")
PY
fi

# ---------------------------------------------------------------- installer
# The live system installed itself onto $TARGET: check the disk layout, then
# boot that disk on its own (no ISO) and prove that a file written in the
# first installed boot survives the next reboot.
if [ "$INSTALL" = 1 ]; then
    echo "== installer check =="
    wait_marker "LOSEDOZE INSTALL OK" "$TIMEOUT_INSTALL" || fail "installer"
    stop_qemu

    echo "-- installed disk layout --"
    python3 - "$TARGET" <<'PY' || fail "installed disk layout"
import struct, sys
img = sys.argv[1]
d = open(img, "rb")
mbr = d.read(512)
if mbr[510:512] != b"\x55\xaa":
    sys.exit("no MBR signature")
parts = []
for i in range(4):
    e = mbr[446 + 16 * i:446 + 16 * (i + 1)]
    if e[4] == 0:
        continue
    start, cnt = struct.unpack("<II", e[8:16])
    parts.append((e[4], start, cnt))
if len(parts) != 2:
    sys.exit("expected 2 partitions, got %r" % (parts,))
if parts[0][0] != 0x83:
    sys.exit("partition 1 is not a Linux filesystem")
if parts[1][0] != 0x82:
    sys.exit("partition 2 is not swap")
if b"GRUB" not in mbr[:446]:
    sys.exit("no GRUB boot sector in front of the partition table")
# the boot sector and the core image must be exactly what the build made:
# the installer wrote them, so a byte compare is the honest check
boot = open("build/grub-boot.img", "rb").read(446)
core = open("build/grub-core.img", "rb").read()
if mbr[:446] != boot:
    sys.exit("boot sector differs from build/grub-boot.img")
d.seek(512)
if d.read(len(core)) != core:
    sys.exit("core image differs from build/grub-core.img")
print("boot sector + core image (%d bytes) match the build" % len(core))
d.seek(parts[0][1] * 512 + 1024)
sb = d.read(512)
if struct.unpack("<H", sb[56:58])[0] != 0xEF53:
    sys.exit("partition 1 is not ext2/3/4")
label = sb[120:136].split(b"\0")[0].decode("ascii", "replace")
if label != "LOSEDOZE":
    sys.exit("root label is %r" % label)
# s_feature_incompat bit 0x40 = EXTENTS: that is what makes it ext4
if not (struct.unpack("<I", sb[96:100])[0] & 0x40):
    sys.exit("partition 1 has no ext4 feature flag")
# mkswap (v1 with a UUID) puts the "SWAPSPACE2" signature at the end of the
# first page; the kernel accepts it there and at offset 0
d.seek(parts[1][1] * 512)
sw = d.read(4096)
if b"SWAPSPACE2" not in sw and b"SWAP-SPACE" not in sw:
    sys.exit("partition 2 has no swap header")
print("disk ok: p1 ext4 label=%s lba=%d sectors=%d, p2 swap sectors=%d" %
      (label, parts[0][1], parts[0][2], parts[1][2]))
PY

    echo "-- booting the installed disk (no ISO) --"
    launch "/tmp/loseterm-installed1.log" -hda "$TARGET"
    wait_marker "LOSEDOZE INIT: installed root on" 120 || fail "installed root not found"
    wait_marker "LOSEDOZE INIT OK" 120           || fail "installed init"
    wait_marker "LOSEDOZE SWAP: SwapTotal: *[1-9]" 120 || fail "swap not active"
    wait_marker "LOSEDOZE BOOT COUNT 1" 300      || fail "installed boot count"
    wait_marker "LOSEDOZE READY" "$TIMEOUT_SHELL" || fail "installed desktop"
    stop_qemu

    echo "-- rebooting the installed disk (files must survive) --"
    launch "/tmp/loseterm-installed2.log" -hda "$TARGET"
    wait_marker "LOSEDOZE BOOT COUNT 2" 300 || fail "file did not survive the reboot"
    stop_qemu
    echo "INSTALL SMOKE PASS"
fi

echo "SMOKE PASS"
exit 0
