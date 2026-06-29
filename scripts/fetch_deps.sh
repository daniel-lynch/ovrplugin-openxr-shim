#!/usr/bin/env bash
# Fetch build dependencies that aren't committed (permissively-licensed headers +
# the Android NDK). Run once after cloning.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OXR="$ROOT/shim/third_party/openxr/openxr"
VK="$ROOT/shim/third_party/vulkan/vulkan"

echo "== OpenXR headers (Apache-2.0) =="
mkdir -p "$OXR"
OXRBASE="https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK/main/include/openxr"
for h in openxr.h openxr_platform.h openxr_platform_defines.h; do
  curl -fsSL "$OXRBASE/$h" -o "$OXR/$h"
done

echo "== Vulkan headers (Apache-2.0) =="
mkdir -p "$VK/vk_video"
VKBASE="https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/main/include/vulkan"
for h in vulkan_core.h vk_platform.h; do curl -fsSL "$VKBASE/$h" -o "$VK/$h"; done
printf '#ifndef VULKAN_H_\n#define VULKAN_H_\n#include "vk_platform.h"\n#include "vulkan_core.h"\n#endif\n' > "$VK/vulkan.h"
VVBASE="https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/main/include/vk_video"
for f in $(grep -oE 'vk_video/[a-zA-Z0-9_]+\.h' "$VK/vulkan_core.h" | sed 's|vk_video/||' | sort -u); do
  curl -fsSL "$VVBASE/$f" -o "$VK/vk_video/$f"
done

mkdir -p "$ROOT/tools"; cd "$ROOT/tools"

echo "== Android NDK r27c (large ~700MB) =="
[ -d "$ROOT"/tools/android-ndk-* ] 2>/dev/null || {
  curl -L -o ndk.zip "https://dl.google.com/android/repository/android-ndk-r27c-linux.zip"
  unzip -q ndk.zip && rm ndk.zip; }

echo "== JDK (Temurin 21, for keytool/apksigner) =="
ls -d "$ROOT"/tools/jdk-* >/dev/null 2>&1 || {
  curl -L -o jdk.tgz "https://api.adoptium.net/v3/binary/latest/21/ga/linux/x64/jdk/hotspot/normal/eclipse"
  tar xzf jdk.tgz && rm jdk.tgz; }

echo "== Android build-tools (zipalign/apksigner/aapt2) =="
ls -d "$ROOT"/tools/android-[0-9]* >/dev/null 2>&1 || {
  curl -L -o bt.zip "https://dl.google.com/android/repository/build-tools_r34-linux.zip"
  unzip -q bt.zip && rm bt.zip; }

echo "== apktool (manifest tooling) =="
[ -f "$ROOT/tools/apktool.jar" ] || curl -fsSL -o apktool.jar \
  "https://github.com/iBotPeaches/Apktool/releases/download/v2.10.0/apktool_2.10.0.jar"

echo "done. Now: shim/build_android.sh  (and packaging/repack.sh for an installable APK)."
echo "NOTE: URLs are Linux-x86_64; on macOS swap the NDK/JDK/build-tools archives."
