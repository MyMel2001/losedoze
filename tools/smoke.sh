#!/bin/bash
# Headless QEMU boot check for the Losedoze ISO:
#  waits for console markers, takes a framebuffer screendump, verifies the
#  desktop actually drew (color variety + taskbar row).
set -u
cd "$(cd "$(dirname "$0")/.." && pwd)"

ISO=${ISO:-build/losedoze.iso}
TIMEOUT_INIT=${TIMEOUT_INIT:-300}
TIMEOUT_SHELL=${TIMEOUT_SHELL:-300}
TIMEOUT_WIN32=${TIMEOUT_WIN32:-420}
# GPU under test: "std" (QEMU std VGA -> bochs-drm) or "virtio" (virtio-gpu
# driver). Both are open-source in-tree DRM drivers.
case "${SMOKE_GPU:-std}" in
    virtio) VGA_ARG="-device virtio-vga" ;;
    *)      VGA_ARG="-vga std" ;;
esac
QMP=/tmp/loseterm-qmp.sock
SER=/tmp/loseterm-serial.log
SER_SOCK=/tmp/loseterm-ser.sock
SHOT=${SHOT:-/tmp/loseterm.ppm}

[ -f "$ISO" ] || { echo "missing $ISO — run ./build.sh first"; exit 1; }
KVM_ARG=""
[ -e /dev/kvm ] && KVM_ARG="-enable-kvm"
rm -f "$QMP" "$SER" "$SER_SOCK" "$SHOT"
# Stream the guest console over a unix socket instead of `-serial file:`: a
# file chardev is buffered by QEMU in 4 KB chunks, so the smoke would only see
# the markers once the whole boot had been buffered -- and the framebuffer
# shot would land long after the Win32 client window was covered by later
# apps. (A FIFO is not usable here: QEMU opens the pipe read/write and would
# steal half the console output from the reader.)
python3 - "$SER_SOCK" "$SER" <<'PY' &
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

qemu-system-x86_64 $KVM_ARG -m 3072 -smp 2 \
    -cdrom "$ISO" -boot d $VGA_ARG \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -chardev socket,id=ser0,path="$SER_SOCK",server=on,wait=off \
    -serial chardev:ser0 -no-reboot \
    -qmp "unix:$QMP,server,nowait" &
QPID=$!
trap 'kill $QPID $SER_PID 2>/dev/null; wait $QPID $SER_PID 2>/dev/null' EXIT

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

echo "booting $ISO (kvm: ${KVM_ARG:-no})"
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

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
exit 0
