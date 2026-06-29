#!/usr/bin/env bash
# Cross-build the shim to Android arm64 (the deployable target — runs inside the
# RE4 APK on Quest/Lepton). Host build is validation-only; this is the real artifact.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHIM="$ROOT/shim"
NDK="${ANDROID_NDK:-$(ls -d "$ROOT"/tools/android-ndk-* 2>/dev/null | head -1)}"
[ -n "${NDK:-}" ] && [ -d "$NDK" ] || { echo "NDK not found. Set \$ANDROID_NDK or run scripts/fetch_deps.sh"; exit 1; }
TC="$(ls -d "$NDK"/toolchains/llvm/prebuilt/* 2>/dev/null | head -1)"   # host tag (linux/darwin)
CC="$TC/bin/aarch64-linux-android29-clang"   # API 29 (Quest is Android 10+; app targetSdk=29)

[ -x "$CC" ] || { echo "NDK clang not found at $CC"; exit 1; }
mkdir -p "$SHIM/build/arm64"

"$CC" -std=c11 -Wall -shared -fPIC -fvisibility=hidden \
   -I"$SHIM/include" -I"$SHIM/third_party/openxr" -I"$SHIM/third_party/vulkan" \
   "$SHIM/src/stubs.c" "$SHIM/src/core.c" "$SHIM/src/xr_runtime.c" \
   "$SHIM/src/vk_session.c" "$SHIM/src/layers.c" "$SHIM/src/android_init.c" "$SHIM/src/xr_input.c" \
   "$SHIM/src/passthru.c" \
   -llog \
   -o "$SHIM/build/arm64/libOVRPlugin.so"

# NOTE: entitlement handling is out of scope for this project. On Quest you own the title,
# so the platform's real entitlement check runs unchanged (keep the original
# libovrplatformloader.so). Running on hardware with no Meta backend requires a valid
# entitlement by some other means, which is the user's responsibility and not part of this repo.

# the xr* symbols resolve against libopenxr_loader.so at runtime; declare the dep so
# the dynamic linker loads it (bundle libopenxr_loader.so in the APK lib/arm64-v8a/).
PATCHELF="$(command -v patchelf || true)"
if [ -n "$PATCHELF" ]; then
  "$PATCHELF" --add-needed libopenxr_loader.so "$SHIM/build/arm64/libOVRPlugin.so"
  echo "added NEEDED libopenxr_loader.so"
else
  echo "WARN: patchelf not found — add 'libopenxr_loader.so' as NEEDED before packaging"
fi
echo "built: $SHIM/build/arm64/libOVRPlugin.so"
