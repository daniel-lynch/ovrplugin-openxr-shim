# RE4 VR Shim — Handoff (2026-06-27, native-parity / P4 passthru)

Branch `latency-perffix-copyring`. Goal this arc: **get the shim to native (stock VrApi) parity.**
Stock RE4 VR is flawless on this Quest 2; our OpenXR shim had black logos + eye-layer ghosting.
Detailed findings in auto-memory (MEMORY.md): `native-parity-diagnosis`, `logo-black-is-layer-zorder`,
`eye-ghost-submission-path-clean-need-p4`, `three-perf-levers-built`.

## SHIPPED + VERIFIED this session
**Logo black FIXED — it was a layer z-order bug.** RE'd it: the 3 intro logos are Oculus splash
layers (`OculusHMD::FSplash`); the game submits `[logo-quad, eye-fov]` in that order, and OpenXR
composites strictly in array order (last = on top), so our opaque eye-fov drew ON TOP of the logo →
black. VrApi treats eye-fov as the base regardless of order. Fix: `build_composition`
(`shim/src/xr_runtime.c`) stable-partitions so projection layers go bottom, quads on top. Gated
`debug.re4vr.eyebottom` (default ON). **Device-verified: logos now render.** A/B: `eyebottom 0`.

## STILL OPEN: eye-layer ghosting (hand deforms / "three hands", close objects, SEATED mode)
Standing mode = black+reproject instead (load-gated, see `standing-vs-sitting-blackflash`); seated =
clean ghost repro (no reproject confound). **Every shim submission metric is CLEAN** (all
device-verified, see `eye-ghost-submission-path-clean-need-p4`): depth on didn't fix it; submit is
18ms EARLY not late; VrApi `Stale=0`; pose render==submit (`g_xr.views`); FOV render==submit; ffr
ruled out; mono cast shows a normal hand in sharp frames → it's a per-eye/stereo artifact we can't
see from our side. **Hence P4.**

## NEXT TASK — build the full P4 passthru forwarding (DE-RISKED, viable)
Run the REAL OVRPlugin inside our app and LOG native's actual per-eye poses/FOV/layer submission, to
diff against our clean-but-ghosting values. Feasibility PROVEN: `PASSTHRU-PROBE: dlopen OK` — real
lib + libvrapi load in our unofficial app, entry points resolve (probe in `core.c` `passthru_probe()`
at PreInitialize3, gated `debug.re4vr.passthru_probe`).

**Already in place:** SONAME-patched real lib at `packaging/libs/arm64/libOVRPlugin_real.so`
(`patchelf --set-soname libOVRPlugin_real.so`); `repack.sh` auto-bundles it when present (verified in
the APK). `libvrapi.so` already rides along from the original APK.

**The build (all-or-nothing — real lib must own the whole session to run EndFrame4):**
1. New `shim/src/passthru.c`: `dlopen("libOVRPlugin_real.so", RTLD_NOW|RTLD_LOCAL)` once; build a
   table of real fn pointers (dlsym each `ovrp_*` the game uses); expose `pt_active()` +
   `pt_<fn>()` accessors (or a single dispatch). Gate: `debug.re4vr.passthru` (default 0).
2. In EVERY game-called export (the ~30 in `core.c`/`layers.c` + the ~12 stubs the game hits — see
   below), add at the top: `if (pt_active()) { ...optional log...; return real_ovrp_X(args); }`.
   Partial forwarding CRASHES mid-frame, so forward the COMPLETE called set. The game-called set =
   everything currently implemented in core.c/layers.c PLUS these stubs it invokes (from device log):
   `DestroyDistortionWindow2, SetupDisplayObjects2, SetReorientHMDOnControllerRecenter,
   SetClientColorDesc, SetAppEngineInfo2, SetAppCPUPriority2, InitializeMixedReality,
   GetViewportStencil, GetSystemRecommendedMSAALevel2, GetLocalTrackingSpaceRecenterCount,
   GetLayerTextureFoveation, GetControllerHapticsDesc2`. (Confirm the live set by grepping a passthru
   boot for any of OUR `stub ovrp`/impl logs that still fire — those are unforwarded calls.)
3. LOG the ghost-relevant ones: `EndFrame4` (per-layer pose+fov+swapchain+flags),
   `GetNodePoseState3`(Eye L/R return), `GetNodeFrustum2`(return), `CalculateEyeLayerDesc2`,
   `WaitToBeginFrame`/`EndFrame4` timing. Forward these to real, log args/returns.
4. Boot test: with `passthru 1`, does the game run NATIVE through our shim (looks like stock, no
   ghost)? If yes → capture native EndFrame4 per-eye poses/fov, **diff against our values** (ours are
   in the STEREO/HEADvsEYE logs). The delta is the ghost cause. If the game won't init native (Init5
   entitlement/session), fall back to static RE of the projection path.
   CAVEAT: in passthru our shim must NOT also init OpenXR — make `xrr_init`/frame-loop no-op when
   pt_active (real lib owns the session). Watch for double-init of VrApi/OpenXR.

## The 3 perf levers (built earlier this session — all DEAD ENDS, gated off)
- **Adaptive-res** (`ovrp_GetAdaptiveGpuPerformanceScale2`, core.c): INERT — game's `Settings.byte
  [0x19] bit4` clear, never downscales. `debug.re4vr.adaptivescale`.
- **Render-ahead** (`debug.re4vr.renderahead`): stable now (old pipeline=1 instability fixed) but
  does NOT reduce black (still 66-76% under load — black is draw/streaming-bound, not latency).
  BLACKCOUNT counter (`debug.re4vr.blackcount`) is the measurement tool.
- **Depth-hold** (`debug.re4vr.depthhold`, needs `depth 1`): built, untested, ADDS memory pressure.
- **Reproject remains the actual in-game black fix.** Corridor black = engine streaming/memory stall
  (lmkd thrash, Graphics 1.69GB), not GPU — confirmed; GPU knobs don't help. Streaming-throttle CVar
  is a documented-but-not-built future lever (config-injection DEAD — Shipping ignores external
  UE4CommandLine.txt; would need CVar memory-patch via `IConsoleManager`).

## Code state (UNCOMMITTED on branch) — RECOMMEND COMMIT before further work
`git status`: M `core.c stubs.c vk_session.c xr_runtime.c xr_runtime.h packaging/repack.sh`; new
`HANDOFF-2026-06-27*.md`, `save_backup/`, `packaging/libs/arm64/libOVRPlugin_real.so` (986KB binary —
needed for passthru; confirm it's committed or staged). +383/-88 in shim. Contains: eyebottom fix
(keep — verified), 3 perf levers (gated off), QUADLUMA + per-layer luma diag, passthru probe,
depth-aware vk helpers (`xrr_vk_alloc_images_ex`/`xrr_vk_copy_submit_ex`).

## Build / deploy / device workflow
```
./shim/build_android.sh && ./packaging/repack.sh           # repack auto-bundles libOVRPlugin_real.so
adb -s <redacted-serial> install -r packaging/out/re4vr-shim.apk   # -r over SAME-signed shim PRESERVES data (no OBB/save dance)
adb -s <redacted-serial> logcat -G 16M                         # big buffer so brief windows don't rotate
adb -s <redacted-serial> logcat | grep -iE "xrr|PASSTHRU"
```
- **Every cold relaunch needs a manual dismiss** of the UnOfficialApp dialog (debug-signed) and the
  ControllerRequired dialog if controllers asleep → autonomous cold-launch capture is blocked; ask
  the user to dismiss + boot. Logos play on cold start only.
- **SAVE DISCIPLINE:** `adb pull /sdcard/Android/data/com.Armature.VR4/files/savegame00.sav` BEFORE
  any uninstall (uninstall wipes app data; cloud doesn't restore unofficial-app saves — lost one this
  session). Only stock<->shim swaps need uninstall; shim->shim is `install -r` (preserves data). OBB
  preserve trick: `adb shell mv /sdcard/Android/obb/com.Armature.VR4 /sdcard/obb_bak` before uninstall.
- Stock original APK (pristine, real 907KB OVRPlugin) for A/B: `dump/obb/VR4-Android-Shipping-arm64.apk`.
- Current device props: `eyebottom 1 reproject 1 depth 1 depthhold 0 renderahead 0 blackcount 1
  adaptivescale 1 passthru_probe 1` (set `passthru 1` for the new path once built).

## RE tooling (works)
capstone 5.0.7 in python (NOT pyelftools — absent; parse ELF phdrs manually, see scratchpad
disasm.py pattern). `nm -DC` for libUE4 dynsym. PluginWrapper offset→name mapping was UNRELIABLE;
trust call-site offsets + the dynamic `stub ovrp` log signal instead.
