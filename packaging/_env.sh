# Shared, portable path/tool detection for the packaging scripts. `source` this.
# Derives the repo root from this file's location and auto-detects tool versions
# under tools/ (or env overrides), so the repo builds/repacks anywhere.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG="$ROOT/packaging"
SHIM_OUT="$ROOT/shim/build/arm64"

# JDK: prefer a bundled tools/jdk-*, else an existing JAVA_HOME, else system java.
_bundled_jdk="$(ls -d "$ROOT"/tools/jdk-* 2>/dev/null | head -1 || true)"
if [ -n "${_bundled_jdk:-}" ]; then export JAVA_HOME="$_bundled_jdk"; fi
KEYTOOL="${JAVA_HOME:+$JAVA_HOME/bin/}keytool"

# Android build-tools dir (zipalign / apksigner / aapt2), e.g. tools/android-14.
BT="$(ls -d "$ROOT"/tools/android-[0-9]* 2>/dev/null | head -1 || true)"

# apktool jar (optional, for manifest work).
APKTOOL_JAR="$ROOT/tools/apktool.jar"

# Android NDK (for building the OpenXR loader).
NDK="${ANDROID_NDK:-$(ls -d "$ROOT"/tools/android-ndk-* 2>/dev/null | head -1 || true)}"
