# RE4 VR (Quest 2) — Dump & Recon Checklist

Goal of this phase: **non-destructively dump your own legally-owned copy of RE4 VR
off the Quest 2 and answer the one question that decides the whole project** —
is the VR runtime OpenXR (shimmable) or VrApi (proprietary, must reimplement)?

Target app: `com.Armature.VR4` (Armature Studio / Capcom / Oculus, UE 4.25.3)

Legal posture: dump-your-own only. We extract from hardware *you own* running a
copy *you own*. **Nothing here gets redistributed** — only patches/shims you
author, applied by people who dump their own copy. Same model as ReXGlue.

> **Editor's note (2026-06):** this is a historical recon document. It explores an
> entitlement-**stub** approach in a few places (§4, the wiring summary, the decision tree)
> that was **not** carried into the project — entitlement handling is out of scope and the
> shim ships **no circumvention code** (see the README's scope section). Those passages are
> kept as a record of the original investigation, not as instructions.

---

## 0. Prereqs (do while the Quest charges)

- [ ] Install Android platform-tools (adb): `sudo apt install android-tools-adb`
      or grab Google's platform-tools zip.
- [ ] Verify: `adb version`
- [ ] Install analysis tooling:
  - [ ] Ghidra (native .so disassembly/patching)
  - [ ] `patchelf`, `binutils` (`readelf`, `nm`, `objdump`), `file`, `unzip`
  - [ ] Python 3 + `pip install lief` (scripted ELF inspection/patching)
  - [ ] FModel **or** umodel/UModel (UE 4.25 .pak browsing) — optional this phase
  - [ ] FluffyQuack's UnrealPak tools — optional this phase
- [ ] Enable **Developer Mode** on the Quest (Meta Quest mobile app →
      Devices → Developer Mode → on; requires a registered dev org — free).
- [ ] Plug Quest into PC via USB-C, put on headset, **Allow USB debugging**
      when prompted (and check "always allow from this computer").

---

## 1. Confirm the device + app are visible

```bash
adb devices                 # should list one device, state "device" not "unauthorized"
adb shell pm list packages | grep -i armature   # expect: com.Armature.VR4
adb shell dumpsys package com.Armature.VR4 | grep -i versionName
```

- [ ] Device shows as `device`
- [ ] `com.Armature.VR4` present
- [ ] Note the versionName here: `____________`

---

## 2. Locate and pull the APK(s)

Split APKs are common, so grab every path.

```bash
adb shell pm path com.Armature.VR4          # prints one or more base/split apk paths
mkdir -p ~/dev/re4vr-port/dump && cd ~/dev/re4vr-port/dump
# pull each path the command above printed, e.g.:
adb pull /data/app/~~xxxx/com.Armature.VR4-yyyy/base.apk .
# repeat for any split_*.apk lines
```

- [ ] `base.apk` pulled
- [ ] Any `split_*.apk` pulled
- [ ] Record sizes: `ls -lh *.apk`

---

## 3. Pull the OBB (game data / .pak files)

```bash
adb shell ls -la /sdcard/Android/obb/com.Armature.VR4/
adb pull /sdcard/Android/obb/com.Armature.VR4/ .
```

- [ ] OBB pulled (e.g. `main.NNN.com.Armature.VR4.obb`)
- [ ] Note the version number NNN in the OBB filename: `______`
      (you'll need it if you ever repack)

---

## 4. ⭐ THE DECISIVE CHECK — OpenXR vs VrApi

This is the whole reason we're here. Inspect the native libs in the APK.

```bash
cd ~/dev/re4vr-port/dump
unzip -l base.apk | grep -iE 'lib/arm64-v8a/.*\.so'        # list native libs
# the money grep:
unzip -l base.apk | grep -iE 'arm64.*(vrapi|openxr|ovrplatform|oculus|UE4)'
```

Interpret the result:

| Lib found in `lib/arm64-v8a/`     | Meaning                                              | Difficulty |
|-----------------------------------|-----------------------------------------------------|------------|
| `libopenxr_loader.so`             | ✅ Standard OpenXR — Steam Frame provides a runtime; you translate vendor extensions. | Tractable |
| `libvrapi.so`                     | ⚠️ Proprietary Meta VrApi — must reimplement/shim the runtime. | Hard |
| `libovrplatformloader.so`         | Present either way — this is the **entitlement check** to NOP/stub. | Required patch |
| `libUE4.so` (or split into modules)| The Unreal runtime itself — the host you'll be hooking. | n/a |

- [x] **RESULT — runtime is:**  ☑ **VrApi** (legacy OVRPlugin path). Confirmed
      2026-06-23 on app v2.3 (versionCode 203). `libvrapi.so` present,
      `libopenxr_loader.so` ABSENT. `libOVRPlugin.so` NEEDs `libvrapi.so` and
      imports 114 `vrapi_*` symbols; 0 OpenXR symbols anywhere.
- [x] `libovrplatformloader.so` present?  ☑ yes (hard-NEEDED by libUE4.so)

**REFINED WIRING (the seam that matters):**
```
libUE4.so --(ovrp_* C API, 239 refs)--> libOVRPlugin.so --(114 vrapi_)--> libvrapi.so --> Horizon OS
libUE4.so --(NEEDED + ovr_* Platform SDK, 132 refs)--> libovrplatformloader.so  (entitlement)
```
- libUE4.so has **0 vrapi_ refs** — game speaks **OVRPlugin's ovrp_* C API**, not
  VrApi directly. libvrapi is just OVRPlugin's backend.
- => **PORT SEAM = reimplement libOVRPlugin.so (ovrp_* on OpenXR), drop libvrapi.**
  ovrp_* is the documented Unity-shared API (OVR_Plugin.h); modern Meta OVRPlugin
  has an OpenXR backend = reference impl / prior art.
- => **Stub the ovr_* Platform SDK** (libovrplatformloader.so) — entitlement/account,
  just return OK; don't reimplement.

> If you want to double-check beyond filename presence, extract and inspect
> imports of `libUE4.so` (it may dynamically link the runtime):
> ```bash
> unzip base.apk 'lib/arm64-v8a/*' -d apk_libs
> readelf -d apk_libs/lib/arm64-v8a/libUE4.so | grep -i NEEDED
> nm -D --defined-only apk_libs/lib/arm64-v8a/libopenxr_loader.so 2>/dev/null | grep -i xr | head
> nm -D apk_libs/lib/arm64-v8a/libUE4.so | grep -iE 'xrCreate|vrapi_' | head
> ```
> `xrCreate*`/`xr*` symbols → OpenXR codepath. `vrapi_*` symbols → VrApi codepath.
> A binary can ship both libs but only *call* one — the symbol check tells you
> which is actually wired up.

---

## 5. Manifest & build recon

```bash
# Needs apktool (sudo apt install apktool) OR aapt from build-tools
aapt dump badging base.apk | grep -iE 'sdkVersion|native-code|package'
apktool d -s base.apk -o apk_decoded     # -s = don't decode .dex, faster
grep -iE 'oculus|vr|xr|entitlement|permission' apk_decoded/AndroidManifest.xml
```

- [ ] minSdk / targetSdk noted: `______`
- [ ] `native-code` ABI (expect `arm64-v8a`): `______`
- [ ] Any Oculus/entitlement metadata flags in manifest noted below.

---

## 6. Confirm .pak accessibility (asset layer)

The community reports these are unencrypted — verify so you know the asset
layer is open if you ever need it.

```bash
# unzip the OBB (it's a zip), find Content/Paks/*.pak, then:
# try opening in FModel/umodel as UE 4.25.3, no AES key
```

- [x] .pak opens with no AES key (confirms community finding) ☑ yes
      Verified 2026-06-23: pakchunk9 footer bEncryptedIndex=0, EncryptionKeyGuid
      all-zero, pak version 9 (FrozenIndex / UE4.25-26). Plaintext index — 53
      readable asset paths incl. /Game/Levels/BIO4/... Asset layer fully open.
- OBB layout: store (uncompressed) zip. main.203 (4.0GB) + patch.203 (4.0GB,
  the v2.3 update layer) + a stashed VR4-Android-Shipping-arm64.apk (== base.apk).
  Paks at VR4/Content/Paks/pakchunk{0..9}[optional]-Android_ETC2.pak (ETC2 =
  Android texture compression; 'optional' = hi-res texture chunks).
  Bink cutscenes at VR4/Content/Movies/*.bk2.

---

## 7. Record findings → decide path

Fill this in, then we branch:

```
versionName:        2.3 (versionCode 203, minSdk 25, targetSdk 29)
runtime:            VrApi  (libvrapi via libOVRPlugin; NO openxr)  [CONFIRMED 2026-06-23]
ovrplatform loader: present (hard-NEEDED by libUE4.so)
UE version:         4.25.3 (engine = libUE4.so, stripped, arm64)
ABIs:               arm64-v8a (single base.apk, no splits)
paks encrypted:     NO — unencrypted, no AES key (CONFIRMED 2026-06-23, pak v9)
device:             Quest 2 serial <redacted-serial> (codename hollywood)
```

**Decision tree:**
- **OpenXR** → next phase: map which Meta OpenXR vendor extensions the binary
  requests, plan the OpenXR→Steam-Frame-runtime shim + entitlement stub.
  This is the "weekend-of-shimming" branch.
- **VrApi** → next phase: scope a VrApi reimplementation/translation shim
  (much larger). Reassess whether the project is worth it vs. waiting/UEVR.
- **Either way** → `libovrplatformloader.so` entitlement bypass is a required
  Ghidra patch (legal for your own copy).

---

## Notes / scratch

(paste command output, symbol dumps, and decisions here as you go)
