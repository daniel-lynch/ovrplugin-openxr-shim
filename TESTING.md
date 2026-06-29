# Testing the shim — strategy & plan

We do NOT need Steam Frame to validate the hard part. The shim's whole job is
OVRPlugin -> OpenXR, and **the Quest 2 already runs an OpenXR runtime** (Meta's
Horizon OS runtime — the one that replaced VrApi). So the shim can be tested on
hardware we own, today.

## The three paths

| Path | Tests what | Available | Effort |
|------|-----------|-----------|--------|
| **A. Quest 2 + shim swap** | the real shim, real HW, real game, on Meta's OpenXR runtime | now | NDK arm64 build + Android instance handshake + manifest (real entitlement — you own it) |
| **B. Monado-sim harness** | the shim's OpenXR call logic, fast iteration | now | small C harness + Linux arm64 (VM on the Apple-Silicon MacBook) |
| **C. Steam Frame + Lepton** | the actual target (Monado under Lepton) | ~summer 2026 | everything |

Recommended order: **B for fast logic iteration, then A for the real proof.**
A is the thesis-validator; if RE4 renders on the Quest through our OpenXR shim
instead of libvrapi.so, the project is essentially proven.

---

## Path A — Quest 2 (the real test)

Idea: build the shim as an Android arm64 `.so`, drop it into the RE4 APK in place
of the real `libOVRPlugin.so`, sideload, run. Our shim calls the Quest's own
`libopenxr_loader` -> Meta's OpenXR runtime.

Prereqs to do first (these are the currently-open work items):
1. **NDK arm64 build** of the shim — DONE. `shim/build_android.sh` -> NDK r27c ->
   build/arm64/libOVRPlugin.so (aarch64, 438/438 drop-in, NEEDED libopenxr_loader).
2. **Instance handshake** — DONE. src/android_init.c: JNI_OnLoad captures the JavaVM;
   xrr_pre_init calls xrInitializeLoaderKHR + enables XR_KHR_android_create_instance
   + chains XrInstanceCreateInfoAndroidKHR. Activity from Initialize5 arg4 with an
   Application-context reflection fallback. [VERIFY-ON-HW] whether Meta's runtime
   accepts the Application context vs requiring the real Activity, and the
   PreInitialize3-creates-instance-before-activity ordering.
3. **Manifest** — add the OpenXR usage declarations Meta's runtime expects
   (`<uses-feature android:name="android.hardware.vr.headtracking">` already there;
   add OpenXR `<meta-data>`/intent bits per Meta's OpenXR mobile docs).
4. **Entitlement** — on Quest you OWN RE4 and the Quest has the real Meta Horizon
   platform service, so leave the ORIGINAL libovrplatformloader.so untouched and only
   swap libOVRPlugin.so. Logged into the owning account, the real ovr_Entitlement check
   passes legitimately ("you own it"). **CONFIRMED on device 2026-06-29:** a
   debug-re-signed, legit-mode build (original libovrplatformloader.so, no stub) launches
   into the game on a Quest 2 — re-signing does **not** break the entitlement check.
   Entitlement handling on hardware with no Meta backend (e.g. Steam Frame) is out of
   scope for this repo and is the user's responsibility.

Then:
```
# repack (you own the copy; patch-only distribution)
unzip base.apk -d apk/
cp shim/build/arm64/libOVRPlugin.so apk/lib/arm64-v8a/libOVRPlugin.so
# rebuild + zipalign + sign with your own debug key, then:
adb install -r re4vr-shim.apk     # or push OBB + sideload
adb logcat | grep -iE 'xrr|OVRPlugin|openxr'   # watch the [xrr] logs
```
Expected first-run signal: instance/session create succeed in logcat; if the frame
loop spins and the projection layer submits, you get an image (even if poses/input
are rough). Known rough edges on first run: depth (Unsupported), input (controllers
return NotYetImplemented), the [VERIFY-ON-HW] swapchain index lockstep.

## Path B — Monado simulated (fast iteration, works on the Mac)

Monado has a simulated/headless HMD driver — no real headset. Run it in a Linux
arm64 VM (UTM/QEMU on Apple Silicon), point an OpenXR loader at it, and run the
harness (tests/harness.c) which drives the ovrp_* sequence:
  PreInitialize3 -> Initialize5 -> SetupLayer -> [WaitToBeginFrame -> BeginFrame4 ->
  GetNodePoseState3 -> EndFrame4] xN -> Shutdown2
and asserts each returns ovrpSuccess. This exercises the real OpenXR calls without
RE4 or hardware. Build:
```
# inside the Linux arm64 env, with Monado + openxr loader installed:
cc -std=c11 -Iinclude -Ithird_party/openxr tests/harness.c \
   src/*.c -lopenxr_loader -lvulkan -o harness   # needs a Vulkan device/headless
XR_RUNTIME_JSON=/path/to/monado/openxr_monado-dev.json ./harness
```
Note: Initialize5 needs real Vulkan handles; for a pure-logic smoke test the harness
can pass a headless VkInstance/Device (or we add a "no-gfx" build flag that skips
xrCreateSession to test the non-rendering calls first).

## Path C — Steam Frame (the target)

Same arm64 shim `.so`, but the APK runs under **Lepton** (Valve's Waydroid/AOSP) and
the OpenXR runtime is **Monado**. Once Frame ships: NDK build -> repack -> sideload
into Lepton -> the open question is whether Lepton exposes the OpenXR loader +
Vulkan swapchain sharing across the container (see HOST.md unknowns). Path A having
worked makes this mostly a packaging/runtime-plumbing exercise.

---

## Current state (what's ready to test vs not)

WIRED & building (host x86-64, validation only):
- Session lifecycle: instance/system/session create, event-driven state machine.
- Frame loop: xrWaitFrame/Begin/EndFrame, predicted display time.
- Poses: xrLocateViews (eyes) + xrLocateSpace (head).
- Swapchains: xrCreateSwapchain from ovrpLayerDesc, enumerate VkImages, per-frame
  acquire/wait/release, real XrCompositionLayerProjection submit.
- 20 OpenXR functions; 239/239 ovrp_ symbols.

NOT yet (the to-do list before a meaningful Quest run):
- arm64/Android NDK build (host build only so far).
- [ANDROID-TODO] JavaVM/activity -> XrInstanceCreateInfoAndroidKHR + xrInitializeLoaderKHR.
- Input: controller/hand action sets (GetControllerState4 returns NotYetImplemented).
- Depth layer (SetupLayerDepth -> Unsupported).
- Map ovrp_GetInstance/DeviceExtensionsVk -> xrGetVulkan*ExtensionsKHR (so the app
  creates its VkInstance/Device with the runtime's required extensions).
- [VERIFY-ON-HW] swapchain index lockstep assumption.

## Pick-up-tomorrow shortlist
1. Install Android NDK; cross-build the shim to arm64 (proves it builds for target).
2. Stand up the Monado-sim Linux arm64 VM on the Mac + run tests/harness.c (Path B
   smoke test of the non-gfx calls).
3. Then start the [ANDROID-TODO] instance handshake (gates the real Quest run).
