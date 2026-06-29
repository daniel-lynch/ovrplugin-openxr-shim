#!/usr/bin/env bash
# Create a debug keystore for re-signing the repacked APK (one-time).
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_env.sh"
KS="$PKG/debug.keystore"
[ -f "$KS" ] && { echo "keystore exists: $KS"; exit 0; }
"$KEYTOOL" -genkeypair -v -keystore "$KS" -alias re4vrshim \
  -keyalg RSA -keysize 2048 -validity 10000 \
  -storepass android -keypass android \
  -dname "CN=RE4VR Shim, OU=Dev, O=Homebrew, C=US"
echo "created $KS  (storepass/keypass: android)"
