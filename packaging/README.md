# Packaging — repack the RE4 VR APK with the shim

Produces an installable, re-signed APK with our OpenXR `libOVRPlugin.so` swapped in. The
original `libovrplatformloader.so` is kept untouched, so the platform's real entitlement
check runs unchanged — on Quest you own the title. Dump-your-own only; nothing here is
redistributed. Entitlement handling on hardware with no Meta backend is **out of scope** for
this project and is the user's responsibility. See ../TESTING.md for the on-device plan.

## One-time setup
```
./build_openxr_loader.sh     # builds libopenxr_loader.so (arm64) -> libs/arm64/
./make_debug_keystore.sh     # debug.keystore for re-signing (repack.sh auto-runs it)
```
Also build the shim libs first: `../shim/build_android.sh`.

## Repack
```
./repack.sh [input.apk]
# example:
./repack.sh                          # ../dump/base.apk
```
Swaps only `libOVRPlugin.so` + bundles `libopenxr_loader.so`, and keeps the original
`libovrplatformloader.so` so the platform's real entitlement check runs unchanged.

Output: `out/re4vr-shim.apk` (zipaligned, v1+v2+v3 signed with the debug key).

## Off-Quest port (Pico / Steam Frame / Monado-on-Android)
```
./steamframe_patches.sh [in.apk] [out.apk]   # default: out/re4vr-shim.apk -> out/re4vr-steamframe.apk
```
Run AFTER `repack.sh` (which swaps in our shim). Needs `apktool`. Applies the Java/manifest
fixes a non-Quest target needs but a real Quest doesn't: spoofs `Build.MANUFACTURER`/`MODEL`
so UE takes the Oculus HMD path (else our shim is never called), neuters
`AndroidThunkJava_ForceQuit`, and strips Meta-only `<uses-(native-)library>` manifest entries
(keeps `libopenxr.google.so`). Rationale + the patches we deliberately skip:
`../docs/research/related-work.md`. Prepared for the bring-up; not yet hardware-validated.

⚠️ **Do NOT install the output on a Quest** — the manifest strip removes Meta libs the
Quest needs. The script warns + prompts for confirmation; pass `NOT_QUEST=1` to bypass the
prompt in automation. On a Quest, install the plain `repack.sh` output (`out/re4vr-shim.apk`).

## Install + run (Quest, dev mode)
```
adb install -r out/re4vr-shim.apk
# push the OBB you dumped (same versionCode 203):
adb push ../dump/obb/main.203.com.Armature.VR4.obb  /sdcard/Android/obb/com.Armature.VR4/
adb push ../dump/obb/patch.203.com.Armature.VR4.obb /sdcard/Android/obb/com.Armature.VR4/
adb logcat | grep -iE 'xrr|openxr|OVRPlugin|Armature'   # watch [xrr] logs
```

## Manifest
```
./inspect_manifest.sh [input.apk]    # confirms VR/OpenXR declarations are present
```
RE4 VR is already a shipping Quest VR app, so its manifest almost certainly already
has the headtracking feature + VR intent category; switching vrapi->OpenXR usually
needs no manifest change. inspect_manifest.sh reports any gaps; edit the decoded
manifest + rebuild with apktool only if something's missing.

## Notes
- Re-signing with our own key is unavoidable (we modify a lib). That's what risks
  the legit entitlement on Quest — see ../TESTING.md.
- Libs are added stored (-0) and the APK is `zipalign -p 4`'d so native libs stay
  page-aligned (extractNativeLibs=false convention).
- Tools used: tools/android-14 (build-tools 34: zipalign/apksigner), tools/apktool.jar,
  tools/jdk-21 (keytool), tools/android-ndk-r27c (loader build).
