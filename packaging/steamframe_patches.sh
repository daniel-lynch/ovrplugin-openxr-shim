#!/usr/bin/env bash
# steamframe_patches.sh — off-Quest APK patches for porting RE4 VR (UE4) to a NON-Meta
# Android VR device (Pico / Steam Frame / Monado-on-Android). These are NOT needed on a
# real Quest 2 (the shim alone suffices there); they only matter once Build.MANUFACTURER
# isn't "Oculus" and Meta-only manifest libs aren't present on the target.
#
# Our own implementation of the transforms; the set is informed by Overport's open-source
# patcher (docs/research/related-work.md). It does NOT use any of their binaries — we ship
# our own libOVRPlugin shim via repack.sh; this only fixes the APK's Java/manifest so UE
# takes the Oculus HMD path and the package installs off-Quest.
#
# Pipeline: apktool decode -> manifest + smali patches -> apktool build -> zipalign -> sign.
# Run repack.sh FIRST (it swaps in our shim libs); feed its output here.
#
#   ./steamframe_patches.sh [in.apk] [out.apk]
#     in.apk  : shim-repacked APK            (default: out/re4vr-shim.apk)
#     out.apk : patched output               (default: out/re4vr-steamframe.apk)
#
# Prereqs: apktool (https://apktool.org), plus the Android build-tools used by repack.sh.
# NOTE: prepared for the non-Quest bring-up; not yet validated on Steam Frame hardware.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_env.sh"
[ -n "${BT:-}" ] && [ -d "$BT" ] || { echo "Android build-tools not found. Run scripts/fetch_deps.sh"; exit 1; }
command -v apktool >/dev/null || { echo "apktool not found — install it (https://apktool.org) to run the smali/manifest patches"; exit 1; }

IN="${1:-$PKG/out/re4vr-shim.apk}"
OUT="${2:-$PKG/out/re4vr-steamframe.apk}"
[ -f "$IN" ] || { echo "input APK not found: $IN  (run repack.sh first)"; exit 1; }

# --- DO NOT INSTALL ON QUEST guard ----------------------------------------------------
# This output is for NON-Quest devices. On a Quest the uses-(native-)library strip removes
# Meta libs the runtime relies on -> can break launch/features. The Quest build is the plain
# repack.sh output (out/re4vr-shim.apk). Require explicit acknowledgement so this can't be
# produced/installed against a Quest by accident. Bypass in automation with NOT_QUEST=1.
cat <<'WARN'
========================================================================
  WARNING: NON-QUEST build. DO NOT install the output on a Meta Quest.
  It strips Meta-only manifest libs the Quest needs (can break launch).
  For Quest, install the plain repack.sh output: out/re4vr-shim.apk
========================================================================
WARN
if [ "${NOT_QUEST:-0}" != 1 ]; then
  if [ -t 0 ]; then
    read -r -p "Target is NOT a Quest and I understand this build will break on Quest [y/N] " ack
    case "$ack" in y|Y|yes|YES) ;; *) echo "aborted (not confirmed)"; exit 1 ;; esac
  else
    echo "Refusing to run non-interactively. Re-run with NOT_QUEST=1 to confirm the target is not a Quest."
    exit 1
  fi
fi

WORK="$PKG/work/steamframe"
rm -rf "$WORK"; mkdir -p "$WORK"
DEC="$WORK/dec"

echo "== apktool decode =="
apktool d -f -o "$DEC" "$IN" >/dev/null

# Locate UE's GameActivity smali (ue4 or unreal namespace, across smali_classesN dirs).
mapfile -t GA < <(find "$DEC" -path '*/com/epicgames/ue4/GameActivity.smali' \
                               -o -path '*/com/epicgames/unreal/GameActivity.smali' 2>/dev/null)
[ "${#GA[@]}" -gt 0 ] || echo "WARN: no com/epicgames/{ue4,unreal}/GameActivity.smali found (UE patches skipped)"

# --- 1) Oculus device spoof: UE gates the Oculus HMD path on Build.MANUFACTURER/MODEL.
#        Off-Quest these are wrong, so OculusHMD never inits and our shim is never called.
#        Replace the field reads with constant "Oculus" / "Quest 2" in the same register. ---
spoofed=0
for f in "${GA[@]}"; do
  perl -0777 -pe 's/sget-object (v\d+|p\d+), Landroid\/os\/Build;->MANUFACTURER:Ljava\/lang\/String;/const-string $1, "Oculus"/g' -i "$f"
  perl -0777 -pe 's/sget-object (v\d+|p\d+), Landroid\/os\/Build;->MODEL:Ljava\/lang\/String;/const-string $1, "Quest 2"/g' -i "$f"
  spoofed=1
done
[ "$spoofed" = 1 ] && echo "patched: Build.MANUFACTURER->\"Oculus\", MODEL->\"Quest 2\" (device spoof)"

# --- 2) Neuter AndroidThunkJava_ForceQuit: drop the System.exit(I) call so a failed
#        off-Quest check can't hard-kill the app before we recover. ---
for f in "${GA[@]}"; do
  perl -0777 -pe 's/(\.method public AndroidThunkJava_ForceQuit\(\)V.*?)(invoke-static \{[vp]\d+\}, Ljava\/lang\/System;->exit\(I\)V\n)(.*?\.end method)/$1$3/s' -i "$f" \
    && grep -q 'AndroidThunkJava_ForceQuit' "$f" && echo "patched: removed System.exit in AndroidThunkJava_ForceQuit ($(basename "$(dirname "$f")"))"
done

# --- 3) Manifest: strip <uses-library>/<uses-native-library> that name Meta-only libs
#        (keep libopenxr.google.so) — they'd block install/launch off-Quest. ---
MAN="$DEC/AndroidManifest.xml"
if [ -f "$MAN" ]; then
  before=$(grep -cE 'uses-(native-)?library' "$MAN" || true)
  perl -0777 -pe 's/[ \t]*<uses-(native-)?library[^>]*android:name="(?!libopenxr\.google\.so)[^"]*"[^>]*\/>\n//g' -i "$MAN"
  after=$(grep -cE 'uses-(native-)?library' "$MAN" || true)
  echo "patched: manifest uses-(native-)library entries $before -> $after (kept libopenxr.google.so)"
fi

echo "== apktool build =="
apktool b -o "$WORK/unsigned.apk" "$DEC" >/dev/null

echo "== align + sign =="
"$BT/zipalign" -f -p 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"
[ -f "$PKG/debug.keystore" ] || "$PKG/make_debug_keystore.sh"
"$BT/apksigner" sign --ks "$PKG/debug.keystore" --ks-pass pass:android --out "$OUT" "$WORK/aligned.apk"
"$BT/apksigner" verify "$OUT" && echo "signature OK"

echo
echo "built: $OUT"
echo "NOTE: NON-QUEST build — do NOT install on a Quest (use repack.sh's re4vr-shim.apk there)."
echo "      prepared for the non-Quest bring-up; validate on the target headset."
echo "Still a shim TODO (runtime, not packaging): controller-pose offset for non-Touch"
echo "controllers (Overport's DisableControllerOffset) — handle in shim/src/xr_input.c."
