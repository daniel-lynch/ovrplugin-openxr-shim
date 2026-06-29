# OVRPlugin -> OpenXR shim — scope

Derived 2026-06-23 from RE4 VR v2.3 (`com.Armature.VR4`). Method: extracted the
distinct `ovrp_*` names `libUE4.so` references (dlsym targets) and intersected
with `libOVRPlugin.so`'s exports. Raw lists in `analysis/`.

## Headline numbers
- OVRPlugin exports **438** `ovrp_*` entry points.
- The game actually references **239** of them. That's the shim surface.
- But **~150 of the 239 are stub-to-constant / no-op** for a basic port.
  Realistically **~85-90 functions need real implementation**, of which the
  genuinely hard core is **~40** (frame loop + layer/swapchain + the display
  interop).

## Render API: VULKAN (confirmed)
`libUE4.so` has VulkanRHI compiled in and calls `ovrp_Get{Instance,Device}ExtensionsVk`.
libGLESv2/EGL are NEEDED but that's the standard Android baseline; the active
renderer is Vulkan. => swapchain path is the standard **OpenXR `XR_KHR_vulkan_enable2`**
(import the app's VkImages into `XrSwapchain`). Well-trodden, not exotic.

## STUB to no-op/constant (~131 counted, +misc ≈ 150)
| Group | count | how to stub |
|---|---|---|
| Mixed Reality Capture (`ovrp_Media_*`) | 37 | return not-initialized / no-op |
| Camera device (passthrough/depth) | 21 | report unavailable |
| External camera (MRC) | 13 | report 0 cameras |
| Perf/GPU/CPU/ASW/foveation tuning | 31 | accept+ignore, return safe defaults |
| Boundary / Guardian | 7 | "not configured" (or map to XR play bounds later) |
| Hand tracking | 7 | disabled (RE4 VR is controller-only) |
| System-info getters | 15 | return plausible constants (headset type, region…) |

Stubbing these = ~150 functions for near-free. None affect core gameplay.

## MUST implement (~88 counted; the project's real work)
| Group | count | notes |
|---|---|---|
| Tracking / poses | 32 | mostly mechanical: map OpenXR `xrLocateSpace`/views to `ovrp` pose structs |
| Display / layer / swapchain | 20 | THE HARD PART — projection layers, eye FOV, foveation, swapchain stages |
| Input / controllers | 11 | map Touch controller -> OpenXR action set; mechanical but fiddly |
| Eye / user params | 10 | IPD, eye height, pixels-per-tan-angle -> from XR view config |
| Init / shutdown | 8 | session create/begin/end, instance+system setup |
| Frame loop | 6 | `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame` <-> `ovrp_*Frame4`, predicted display time |

## Verdict
Single-developer-feasible, meaty. The "239 functions" headline collapses to
**~40 hard + ~45 mechanical + ~150 stubs.** The hard 40 are the standard guts of
any OpenXR app (frame loop, projection layers, Vulkan swapchain, pose/input
mapping). **Modern Meta OVRPlugin already ships an OpenXR backend** — that's the
reference for exact `ovrp_*` semantics and struct layouts, which removes most of
the guesswork.

Biggest unknowns / next probes:
1. Exact `ovrp_*` struct layouts/ABI (need OVR_Plugin.h matching this OVRPlugin
   version, or RE them in Ghidra). ABI mismatch = crashes.
2. How the Java side loads libOVRPlugin (System.loadLibrary) — confirms the
   replacement mechanism (drop-in shim .so with same SONAME).
3. Entitlement: stub `ovr_*` in libovrplatformloader path (separate from this).
4. Whether Steam Frame exposes an Android-app OpenXR runtime at all (the OTHER
   big external unknown — the shim is moot if the APK can't load there).
