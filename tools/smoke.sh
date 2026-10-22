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
QMP=/tmp/loseterm-qmp.sock
SER=/tmp/loseterm-serial.log
SHOT=${SHOT:-/tmp/loseterm.ppm}

[ -f "$ISO" ] || { echo "missing $ISO — run ./build.sh first"; exit 1; }
KVM_ARG=""
[ -e /dev/kvm ] && KVM_ARG="-enable-kvm"
rm -f "$QMP" "$SER" "$SHOT"

qemu-system-x86_64 $KVM_ARG -m 3072 -smp 2 \
    -cdrom "$ISO" -boot d -vga std \
    -display none -serial "file:$SER" -no-reboot \
    -qmp "unix:$QMP,server,nowait" &
QPID=$!
trap 'kill $QPID 2>/dev/null; wait $QPID 2>/dev/null' EXIT

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
wait_marker "LOSEDOZE READY"   "$TIMEOUT_SHELL"  || fail "loseshell"
wait_marker "EGL: "            30                || fail "egl marker"

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
    wait_marker "LOSEDOZE GL DEMO OK"    180           || fail "gl-demo"
    wait_marker "LOSEDOZE GL OK"     120                || fail "opengl"
    wait_marker "LOSEDOZE GL DESKTOP OK" 120            || fail "desktop-gl"
    wait_marker "LOSEDOZE VULKAN OK" 120                || fail "vulkan"
    wait_marker "LOSEDOZE DOTNET OK" 180                || fail "dotnet"
    WIN32_SEEN=1
    sleep 4   # let the compositor draw the mapped Win32 client
else
    sleep 8   # let the UI settle
fi

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
cmd({"execute": "quit"})
print("screendump:", shot)
PY
[ $? -eq 0 ] || fail "screendump"

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
    # the Win32 client (notes.exe) paints a white 660x460 canvas at +40+30
    # -> look for a big block of near-white pixels in that area
    x0, x1, y0, y1 = 42, 700, 64, 490
    white = 0
    for y in range(y0, y1, 3):
        base = y * w * 3
        for x in range(x0, x1, 3):
            o = base + x * 3
            r, g, b = px[o], px[o+1], px[o+2]
            if r >= 235 and g >= 235 and b >= 235:
                white += 1
    print(f"win32 client white pixels={white}")
    if white < 4000:
        sys.exit("win32 client window not visible in framebuffer")
    ok = True

if ok:
    print("SMOKE PASS")
else:
    sys.exit(1)
PY
[ $? -eq 0 ] || fail "framebuffer check (shot: $SHOT)"
exit 0
