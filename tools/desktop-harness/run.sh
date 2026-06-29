#!/usr/bin/env bash
# run.sh — launch the desktop harness against Monado's simulated HMD, headless.
#
# Brings up monado-service with the NULL compositor (no display needed -> works over SSH /
# in CI), points the OpenXR loader at Monado, then runs the harness which drives the shim's
# ovrp_* -> OpenXR path. Output: build/host/monado.log + build/host/harness.log.
#
# Prereqs (Debian/Ubuntu):  sudo apt-get install monado-service libopenxr1-monado \
#                                                libopenxr-loader1 libopenxr-dev
# Build first:               shim/build_host.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/host"
FRAMES="${1:-300}"

[ -x "$OUT/harness" ] || { echo "no $OUT/harness — run shim/build_host.sh first"; exit 1; }

# Point the loader at Monado explicitly (in case another runtime is also registered).
MONADO_JSON="$(ls /usr/share/openxr/1/openxr_monado*.json 2>/dev/null | head -1 || true)"
[ -n "$MONADO_JSON" ] && export XR_RUNTIME_JSON="$MONADO_JSON"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/re4vr-monado-rt}"
mkdir -p "$XDG_RUNTIME_DIR"

# Simulated HMD driver is auto-selected when no real hardware is present.
# Default = headless: NULL compositor renders to nothing, so it needs no X/Wayland (SSH/CI).
# VISIBLE=1 = windowed: the main compositor opens a mirror window + Monado's imgui debug GUI
# (swapchain preview), so you can WATCH the harness's animated eye fill. Needs a display, so
# run it from the physical desktop session, not over SSH, and give it a big frame count to
# watch, e.g.  VISIBLE=1 tools/desktop-harness/run.sh 3600
if [ "${VISIBLE:-0}" = 1 ]; then
  export XRT_DEBUG_GUI=1
  echo "VISIBLE mode: main compositor + debug GUI (needs DISPLAY/WAYLAND — run locally)"
else
  export XRT_COMPOSITOR_NULL=1
fi
export QWERTY_ENABLE=0
export U_PACING_APP_USE_MIN_FRAME_PERIOD=1

SOCK="$XDG_RUNTIME_DIR/monado_comp_ipc"
rm -f "$SOCK"
# monado-service adds stdin to its epoll loop (for its "press a key to quit" handler).
# A redirected /dev/null or regular file isn't epoll-able -> epoll_ctl fails -> the IPC
# loop dies. Feed it a real FIFO held open by a writer that never sends data.
FIFO="$XDG_RUNTIME_DIR/monado_stdin"
rm -f "$FIFO"; mkfifo "$FIFO"
sleep 100000 >"$FIFO" &
HOLD=$!
echo "== starting monado-service (headless, NULL compositor) =="
monado-service <"$FIFO" >"$OUT/monado.log" 2>&1 &
SVC=$!
trap 'kill $SVC $HOLD 2>/dev/null; wait $SVC 2>/dev/null; rm -f "$FIFO"' EXIT

# wait up to ~10s for the IPC socket
for _ in $(seq 1 100); do [ -S "$SOCK" ] && break; kill -0 $SVC 2>/dev/null || { echo "monado-service died:"; cat "$OUT/monado.log"; exit 1; }; sleep 0.1; done
[ -S "$SOCK" ] || { echo "monado-service IPC socket never appeared:"; tail -20 "$OUT/monado.log"; exit 1; }
echo "monado-service up (pid $SVC)"

echo "== running harness ($FRAMES frames) =="
XRRLOG_STDERR=1 "$OUT/harness" "$FRAMES" 2>&1 | tee "$OUT/harness.log"
rc=${PIPESTATUS[0]}
echo "== harness exit: $rc =="
exit $rc
