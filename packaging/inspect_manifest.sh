#!/usr/bin/env bash
# Decode the APK manifest and report whether the VR/OpenXR declarations Meta's
# runtime expects are present. For RE4 VR (already a shipping Quest VR app) these
# are almost certainly already there, so this is usually a no-op confirmation.
# If something IS missing, edit the decoded manifest and rebuild with apktool.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_env.sh"
APKTOOL=("${JAVA_HOME:+$JAVA_HOME/bin/}java" -jar "$APKTOOL_JAR")
ORIG="${1:-$ROOT/dump/base.apk}"
WORK="$ROOT/packaging/work/decoded"

rm -rf "$WORK"
"${APKTOOL[@]}" d -f -s -o "$WORK" "$ORIG" >/dev/null
M="$WORK/AndroidManifest.xml"
echo "decoded manifest: $M"; echo

check() { grep -q "$2" "$M" && echo "  [present] $1" || echo "  [MISSING] $1  -> $2"; }
echo "VR / OpenXR declarations:"
check "headtracking feature"   'android.hardware.vr.headtracking'
check "VR intent category"     'com.oculus.intent.category.VR'
check "Samsung vr_only mode"   'com.samsung.android.vr.application.mode'
check "supportedDevices meta"  'com.oculus.supportedDevices'
check "handtracking permission" 'com.oculus.permission.HAND_TRACKING'
echo
echo "If any are MISSING, edit $M then rebuild:"
echo "  ${APKTOOL[*]} b -o repacked.apk $WORK   # then zipalign + apksigner (see repack.sh)"
echo "Note: switching vrapi->OpenXR usually needs NO manifest change; the loader"
echo "      resolves the runtime. This is a confirmation step."
