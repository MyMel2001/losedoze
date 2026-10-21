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

CS=$(echo "$CLIENT_OUT" | sed -n 's/.*dmabuf=\([0-9]*\).*sum=\([0-9a-f]*\).*/\1 \2/p')
CDMABUF=$(echo "$CS" | cut -d' ' -f1)
CSUM=$(echo "$CS" | cut -d' ' -f2)
SLOG=$(grep 'dmabuf: import' "$LOG" | tail -1)
SSUM=$(echo "$SLOG" | sed -n 's/.*sum=\([0-9a-f]*\).*/\1/p')

echo "client: dmabuf=$CDMABUF sum=$CSUM"
echo "server: $SLOG"

[ "$CDMABUF" = "1" ] || { echo "DMABUF FAIL: client fell back to shm"; exit 1; }
[ -n "$SSUM" ] || { echo "DMABUF FAIL: compositor did not import a dma-buf"; exit 1; }
[ "$CSUM" = "$SSUM" ] || { echo "DMABUF FAIL: pixel checksum mismatch"; exit 1; }
echo "DMABUF PASS: compositor read client GPU buffer (sum=$SSUM)"
