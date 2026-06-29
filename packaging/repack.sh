#!/usr/bin/env bash
# Repack a dumped-your-own RE4 VR APK with the OpenXR shim + the OpenXR loader, then
# zipalign + re-sign. The original libovrplatformloader.so is kept untouched, so the
# platform's real entitlement check runs unchanged — on Quest you own the title.
# Entitlement handling on hardware with no Meta backend is out of scope for this repo.
#
#   ./repack.sh [input.apk]
#     input.apk : your dumped base.apk            (default: ../dump/base.apk)
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_env.sh"
SHIM="$SHIM_OUT"
[ -n "${BT:-}" ] && [ -d "$BT" ] || { echo "Android build-tools not found. Run scripts/fetch_deps.sh"; exit 1; }

ORIG="${1:-$ROOT/dump/base.apk}"
OUT="$PKG/out/re4vr-shim.apk"

[ -f "$ORIG" ] || { echo "input APK not found: $ORIG"; exit 1; }
[ -f "$SHIM/libOVRPlugin.so" ] || { echo "build the arm64 shim first: shim/build_android.sh"; exit 1; }

mkdir -p "$PKG/out" "$PKG/work"
WORK="$PKG/work/re4vr.apk"
cp "$ORIG" "$WORK"

# 1) strip old signatures (we re-sign below)
zip -q -d "$WORK" 'META-INF/*.RSA' 'META-INF/*.SF' 'META-INF/*.MF' 'META-INF/*.EC' 2>/dev/null || true

# 2) stage the replacement libs under lib/arm64-v8a/
STAGE="$PKG/work/stage"; rm -rf "$STAGE"; mkdir -p "$STAGE/lib/arm64-v8a"
cp "$SHIM/libOVRPlugin.so" "$STAGE/lib/arm64-v8a/"
if [ -f "$PKG/libs/arm64/libopenxr_loader.so" ]; then
  cp "$PKG/libs/arm64/libopenxr_loader.so" "$STAGE/lib/arm64-v8a/"
  echo "bundling libopenxr_loader.so"
else
  echo "WARN: packaging/libs/arm64/libopenxr_loader.so missing — shim NEEDs it at"
  echo "      runtime. Build it: packaging/build_openxr_loader.sh"
fi
# original libovrplatformloader.so is left untouched -> the real entitlement check runs.
echo "keeping original libovrplatformloader.so (real entitlement; you own the title)"
# P4 passthru RE: bundle the SONAME-patched REAL OVRPlugin so the shim can dlopen + forward
# to it (debug.re4vr.passthru*). Opt-in: only when staged in packaging/libs/arm64/.
if [ -f "$PKG/libs/arm64/libOVRPlugin_real.so" ]; then
  cp "$PKG/libs/arm64/libOVRPlugin_real.so" "$STAGE/lib/arm64-v8a/"
  echo "bundling libOVRPlugin_real.so (P4 passthru)"
fi

# 3) replace/add the libs, stored (-0) so zipalign -p can page-align them
( cd "$STAGE" && zip -q -0 -X "$WORK" lib/arm64-v8a/*.so )

# 4) align, then sign (v1+v2+v3)
"$BT/zipalign" -f -p 4 "$WORK" "$PKG/work/aligned.apk"
[ -f "$PKG/debug.keystore" ] || "$PKG/make_debug_keystore.sh"
"$BT/apksigner" sign --ks "$PKG/debug.keystore" --ks-pass pass:android \
  --out "$OUT" "$PKG/work/aligned.apk"
"$BT/apksigner" verify "$OUT" && echo "signature OK"

echo
echo "built: $OUT"
echo "install: adb install -r \"$OUT\"   (push the OBB too: dump/obb/* -> /sdcard/Android/obb/com.Armature.VR4/)"
