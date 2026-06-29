# Related work — how others run Quest games elsewhere, and why this shim is different

A survey of the projects in the same space, why none of them is what this repo is, and
what (little) we'd adopt from them. Based on public repos + black-box surface inspection
of publicly-distributed binaries — no decompilation of anyone's proprietary internals.

## TL;DR

There is **no public, open-source reimplementation of `libOVRPlugin.so` on OpenXR.** A
whole-of-GitHub search for `ovrplugin` returns three repos, none a shim. The one tool that
*runs* VrApi/OVRPlugin Quest games on other headsets — **Overport** — does it by
**redistributing Meta's own newer OVRPlugin binary** plus a vendor OpenXR loader, not by
reimplementing anything. So this project (a clean-room, from-scratch `ovrp_*` engine on
OpenXR) appears to be the only open implementation of that translation layer.

## Overport (`ovrport/app`)

GPLv3, Kotlin/Compose, ~349★. **Two halves, only one of which is open:**

1. **The patcher (open, in the repo).** A Compose Multiplatform + ARSCLib app/CLI that
   rewrites a Quest APK: strips entitlements (via injected **Frida** scripts + a SKU/asset
   config), fixes the manifest, swaps icons/labels, and applies engine-specific smali
   patches. Its VR-library handling is three tiny patches:
   - `CopyOVRPluginVrApiPatch` — drop a bundled `libOVRPlugin.so` into any game that has
     `libvrapi.so`
   - `RemoveVrApiPatch` — delete the game's `libvrapi.so`
   - `CopyLibrariesPatch` — copy in the rest of the bundle

2. **The translation libraries (closed, NOT in the repo).** The libraries it injects are
   downloaded at patch time from the author's server
   (`ovrp.crx.moe/api/v1/releases/index` -> `files.crx.moe/.../libraries.zip`), version-
   managed separately. Source unpublished.

### What's actually in `libraries.zip` (black-box surface inspection)

The bundle is **Meta's / vendors' proprietary binaries**, not original code:

| File | What it is |
|---|---|
| `libOVRPlugin.so` (4.1 MB, 611 `ovrp_` exports) | **Meta's own OVRPlugin** — internal build paths intact in `.rodata` (`arvr/projects/integrations/OVRPlugin/Src/Util/CompositorOpenXR.cpp`). A *newer* build than RE4VR's bundled one (986 KB, VrApi-era), specifically one with the **OpenXR compositor backend**. |
| `libopenxr_loader_{meta,pico,yvr,generic}.so` | Per-vendor OpenXR loaders; the patcher picks one for the target headset. |
| `libovrplatformloader*.so`, `libpxrplatformloader.so` | Entitlement/platform loaders (the Frida-mocked entitlement piece). |

### Overport's actual strategy (now unambiguous)

For a VrApi-era title it: **replaces the game's old VrApi-routed Meta OVRPlugin with a newer
Meta OVRPlugin that has an OpenXR backend**, deletes `libvrapi.so`, drops in the target
vendor's OpenXR loader, and bypasses entitlement with Frida. The newer Meta OVRPlugin's
`CompositorOpenXR` path then talks to e.g. Pico's OpenXR runtime.

**Consequences:**

- It ships **Meta's (and Pico's/YVR's) proprietary binaries** verbatim. That is a very
  different — and far more exposed — legal posture than a clean-room reimplementation.
- It depends on the **newer OVRPlugin's C ABI still matching what the old game's UE/Unity
  build calls.** For an old VrApi-era UE4 title like RE4VR this is not guaranteed; the
  surface has drifted across OVRPlugin versions.
- There is **no original translation code** to learn from — the hard part is Meta's, and
  we already have RE4VR's real `libOVRPlugin_real.so` for reference.

### Why this matters for us

Our shim is the open, clean-room alternative to the one piece Overport keeps closed (and
which is, in fact, Meta's). It can't be replaced by their blob in a clean open product
because their blob *is* Meta's binary. Their **patcher**, however, is genuinely useful prior
art for the *packaging* layer (entitlement strip, manifest/engine smali patches) — see the
off-Quest patches we adopt below.

## Quake III Arena VR Edition (`GUNNM-VR/...`)

A **source port**, not a shim. Built on the open-source Quake3e engine + baseq3a, recompiled
for Android/Quest with Vulkan, using `#ifdef` to select OpenXR (PCVR) or VRAPI (Quest) at
compile time. This is the easy case and the exact opposite of ours: with engine source you
just compile against whichever runtime. We **can't** recompile RE4VR (closed UE4 binary), so
we must be a binary-compatible `libOVRPlugin.so` instead. Useful only as a contrast.

## Off-Quest patches worth adopting (packaging layer)

For **RE4VR on a real Quest** we need none of these — the shim alone is sufficient (good
confirmation). They become necessary only when porting the APK to a **non-Quest** Android VR
device (Pico / Steam Frame / Monado-on-Android). Reimplemented in our own packaging in
`packaging/steamframe_patches.sh` (see the `steamframe-port` branch):

| Patch (Overport name) | Why off-Quest | Status |
|---|---|---|
| `OculusUnrealPatch` | UE gates the Oculus HMD path on `Build.MANUFACTURER`/`MODEL`; off-Quest it's false so our shim is never called. Spoof them in `GameActivity` smali. | **adopt** |
| `RemoveUsesLibraryPatch` | Strip `<uses-(native-)library>` entries (except `libopenxr.google.so`) that name Meta-only libs and would block install/launch elsewhere. | **adopt** |
| `DisableControllerOffsetPatch` | Touch->other-controller pose offset; our `xr_input` has no offset, so non-Touch controllers may be misplaced. | **shim TODO** (runtime, not packaging) |
| `RemoveUnrealForceQuitPatch` | Strip `System.exit` from `AndroidThunkJava_ForceQuit` so a failed off-Quest check can't hard-kill the app. | adopt (defensive) |
| `FixUnrealCrashPatch` | Creates a stub `UnityPlayer.currentActivity` so an engine-agnostic injected blob can find the Activity. | **skip** — our shim gets the Activity natively from `Initialize5` + JNI. |
| `MetaXRAudioPatch` | Hex-NOPs `libMetaXRAudioWwise/Unity.so`. | **N/A** — RE4VR uses `libovraudio64.so`, not Meta XR Audio. |
| `DisableSpaceWarp` / `ForcePassthrough` | Config toggles. | N/A — RE4VR doesn't use AppSpaceWarp; equivalent to our `debug.re4vr.*` props. |

## Sources

- Overport patcher: <https://github.com/ovrport/app> (GPLv3)
- Overport library index: `https://ovrp.crx.moe/api/v1/releases/index`
- Quake III VR Edition: <https://github.com/GUNNM-VR/Quake-III-Arena-VR-Edition>
- Meta deprecates VrApi / "all-in on OpenXR":
  <https://developers.meta.com/horizon/blog/oculus-all-in-on-openxr-deprecates-proprietary-apis/>
- OVRPlugin vs VRAPI vs LibOVR:
  <https://developers.meta.com/horizon/documentation/unity/os-openxr-vrapi/>
- Allegations Meta's OVRPlugin blocks non-Meta runtimes (Voices of VR #1526):
  <https://voicesofvr.com/1526-allegations-that-metas-ovrplugin-is-undermining-the-spirit-of-openxr-by-blocking-non-meta-headsets-on-pcvr/>
