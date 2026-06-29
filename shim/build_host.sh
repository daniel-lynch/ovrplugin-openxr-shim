#!/usr/bin/env bash
# build_host.sh — desktop (x86_64 Linux) build of the shim + the OpenXR test harness.
# Mirrors build_android.sh but targets the host so the OpenXR path can be driven against
# Monado's simulated HMD on a PC (see tools/desktop-harness/). NOT the shipping artifact —
# that's build_android.sh (arm64). Needs: a C compiler, libvulkan, and the OpenXR loader
# (Debian: libvulkan-dev libopenxr-loader1 libopenxr-dev). Run tools/desktop-harness/run.sh
# after this to launch it headless against monado-service.
set -euo pipefail
SHIM="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SHIM/.." && pwd)"
OUT="$ROOT/build/host"
mkdir -p "$OUT"

CC="${CC:-cc}"
# _GNU_SOURCE: glibc gates dladdr/CLOCK_MONOTONIC behind it (the NDK exposes them by default).
INC="-I$SHIM/include -I$SHIM/third_party/openxr -I$SHIM/third_party/vulkan"
CFLAGS="-std=c11 -Wall -D_GNU_SOURCE -fPIC -O2 -g $INC"

echo "== compiling shim sources (host) =="
OBJS=()
for f in "$SHIM"/src/*.c; do
  o="$OUT/$(basename "${f%.c}").o"
  "$CC" $CFLAGS -c "$f" -o "$o"
  OBJS+=("$o")
done

# The shim's xr* calls resolve against the system OpenXR loader; Vulkan against libvulkan.
# Detect the loader via ldconfig, falling back to a direct file probe (ldconfig reads
# /etc/ld.so.cache, which some sandboxes block -> false negative).
have_loader() {
  ldconfig -p 2>/dev/null | grep -q libopenxr_loader && return 0
  for d in /usr/lib /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/local/lib; do
    [ -e "$d/libopenxr_loader.so" ] || [ -e "$d/libopenxr_loader.so.1" ] && return 0
  done
  return 1
}
if ! have_loader; then
  echo "WARN: libopenxr_loader not found — install libopenxr-loader1 libopenxr-dev to link/run."
  echo "      (objects built OK; skipping the .so + harness link.)"
  exit 0
fi

echo "== linking libOVRPlugin.so (host) =="
"$CC" -shared -o "$OUT/libOVRPlugin.so" "${OBJS[@]}" -lopenxr_loader -lvulkan -lpthread -ldl
echo "built: $OUT/libOVRPlugin.so"

echo "== building harness =="
"$CC" -std=c11 -Wall -D_GNU_SOURCE -O2 -g -I"$SHIM/include" -I"$SHIM/third_party/vulkan" \
   "$ROOT/tools/desktop-harness/harness.c" \
   -o "$OUT/harness" \
   -L"$OUT" -lOVRPlugin -lvulkan -lm -Wl,-rpath,"$OUT"
echo "built: $OUT/harness"
echo
echo "run it headless against Monado:  tools/desktop-harness/run.sh"
