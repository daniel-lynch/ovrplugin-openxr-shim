#!/usr/bin/env bash
# Build the Khronos OpenXR loader (libopenxr_loader.so) for Android arm64 so the
# shim's NEEDED dependency resolves at runtime. Output -> packaging/libs/arm64/.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_env.sh"
[ -n "${NDK:-}" ] && [ -d "$NDK" ] || { echo "NDK not found. Set \$ANDROID_NDK or run scripts/fetch_deps.sh"; exit 1; }
SRC="$ROOT/tools/OpenXR-SDK"

if [ ! -d "$SRC" ]; then
  echo "downloading OpenXR-SDK source..."
  curl -fsSL -o "$ROOT/tools/oxrsdk.tgz" \
    "https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/heads/main.tar.gz"
  tar xzf "$ROOT/tools/oxrsdk.tgz" -C "$ROOT/tools"
  mv "$ROOT/tools/OpenXR-SDK-main" "$SRC"
fi

GEN="Unix Makefiles"; command -v ninja >/dev/null && GEN="Ninja"
cmake -S "$SRC" -B "$SRC/build-android" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DDYNAMIC_LOADER=ON -DBUILD_TESTS=OFF -DBUILD_API_LAYERS=OFF \
  -DBUILD_CONFORMANCE_TESTS=OFF -DBUILD_WITH_SYSTEM_JSONCPP=OFF \
  -G "$GEN" >/dev/null
cmake --build "$SRC/build-android" --target openxr_loader -j4

mkdir -p "$PKG/libs/arm64"
find "$SRC/build-android" -name 'libopenxr_loader.so' -exec cp {} "$PKG/libs/arm64/" \;
echo "loader -> $PKG/libs/arm64/libopenxr_loader.so"
file "$PKG/libs/arm64/libopenxr_loader.so" 2>/dev/null || true
