#!/bin/sh
# dmabuf_test.sh — host end-to-end check for client DMA-BUF.
#
# Runs a headless loseshell (Wayland server, no KMS), points the test client at
# it, and compares the pixel checksum the client wrote with the checksum the
# compositor computed from its own mmap of the exported dma-buf.
set -e
cd "$(dirname "$0")/.."

RUN=${XDG_RUNTIME_DIR:-/tmp/opencode/loserun}
mkdir -p "$RUN"
LOG=$RUN/dmabuf-test.log
SOCK=$RUN/losedoze0

gcc -O2 -Wall -Wno-unused-function -o /tmp/dmabuf_client \
    tools/dmabuf_client.c win/wlclient.c -Iwin -Ishell -ldl

XDG_RUNTIME_DIR="$RUN" LOSESHELL_SOCKET="$SOCK" LOSESHELL_HEADLESS=1 \
    /tmp/loseshell_t >"$LOG" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.1; done

CLIENT_OUT=$(XDG_RUNTIME_DIR="$RUN" WAYLAND_DISPLAY="$SOCK" /tmp/dmabuf_client)
echo "$CLIENT_OUT"

# Both frames must be re-exported: the compositor's two import lines have to match
# the client's two checksums in order (a copy-on-export device that was not
# re-exported would repeat the first frame's checksum).
CS=$(echo "$CLIENT_OUT" | sed -n 's/.*dmabuf=\([0-9]*\).*sum1=\([0-9a-f]*\) sum2=\([0-9a-f]*\).*/\1 \2 \3/p')
CDMABUF=$(echo "$CS" | cut -d' ' -f1)
CSUM1=$(echo "$CS" | cut -d' ' -f2)
CSUM2=$(echo "$CS" | cut -d' ' -f3)
SLOGS=$(grep 'dmabuf: import' "$LOG" | tail -2)
SSUM1=$(echo "$SLOGS" | head -1 | sed -n 's/.*sum=\([0-9a-f]*\).*/\1/p')
SSUM2=$(echo "$SLOGS" | tail -1 | sed -n 's/.*sum=\([0-9a-f]*\).*/\1/p')

echo "client: dmabuf=$CDMABUF sum1=$CSUM1 sum2=$CSUM2"
echo "server: $(echo "$SLOGS" | head -1)"
echo "server: $(echo "$SLOGS" | tail -1)"

[ "$CDMABUF" = "1" ] || { echo "DMABUF FAIL: client fell back to shm"; exit 1; }
[ -n "$SSUM1" ] && [ -n "$SSUM2" ] || { echo "DMABUF FAIL: compositor did not import both dma-bufs"; exit 1; }
[ "$CSUM1" = "$SSUM1" ] || { echo "DMABUF FAIL: frame 1 pixel checksum mismatch"; exit 1; }
[ "$CSUM2" = "$SSUM2" ] || { echo "DMABUF FAIL: frame 2 pixel checksum mismatch (stale buffer?)"; exit 1; }
echo "DMABUF PASS: compositor read both client GPU frames (sum1=$SSUM1 sum2=$SSUM2)"
