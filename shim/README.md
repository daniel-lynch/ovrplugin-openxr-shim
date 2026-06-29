# libOVRPlugin.so shim (OVRPlugin v1.51 -> OpenXR)

Drop-in replacement for RE4 VR's libOVRPlugin.so that re-exports the ovrp_* symbols
backed by OpenXR (Monado) instead of Meta's libvrapi.so. See ../HOST.md, ../SHIM-SCOPE.md.

## Layout
- include/ovrplugin_shim.h  — OVRPlugin v1.51 C ABI (types + core protos; self-checking
  _Static_asserts on binary-verified struct sizes).
- gen_stubs.sh              — generates src/stubs.c from ../analysis/shim_surface.txt.
- src/stubs.c  (generated)  — 224 stubs: 101 Unsupported(-1004), 32 no-op Success(0),
  91 NotYetImplemented(-1005).
- src/core.c               — the 15 header-prototyped core fns (frame loop/init/poses/
  controller/system), currently TODO stubs returning -1005 with outputs zeroed.

## Build (host, for validation)
```
cc -std=c11 -Wall -shared -fPIC -fvisibility=hidden -Iinclude src/stubs.c src/core.c \
   -o build/libOVRPlugin.so
```
Status: builds; exports exactly 239 ovrp_ symbols (1:1 with shim_surface.txt, 0
missing/extra); dlopen+dlsym verified. This is the SKELETON — it loads and resolves,
it does not yet drive VR (core fns are TODO).

## Real target build (deployable) — TODO
Must be aarch64 / Android (bionic) since it runs inside Lepton. Use the Android NDK:
```
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang \
   -std=c11 -shared -fPIC -fvisibility=hidden -Iinclude src/stubs.c src/core.c \
   -o build/arm64/libOVRPlugin.so
```
(host build only proves the C + symbol coverage; NDK build is the artifact that
replaces the real lib in the APK.)

## Modules (current)
- include/ovrplugin_shim.h  — OVRPlugin v1.51 ABI (types, structs, core protos).
- src/stubs.c (generated)   — 221 stubs (unsupported/no-op/TODO).
- src/core.c                — lifecycle + frame loop + poses (-> xr_runtime).
- src/xr_runtime.{h,c}      — OpenXR engine: session, frame loop, swapchains.
- src/vk_session.c          — Vulkan-typed: xrCreateSession binding + image enum.
- src/layers.c              — ovrp_SetupLayer / GetLayerTexture2 / StageCount.
- tests/harness.c           — Path B smoke test (see ../TESTING.md).
Build adds: -Ithird_party/openxr -Ithird_party/vulkan, and src/{core,xr_runtime,
vk_session,layers,stubs}.c. 20 OpenXR fns used; 239/239 ovrp_ exported.

## Done so far
Session lifecycle, frame loop (xrWaitFrame/Begin/EndFrame), poses (xrLocateViews/
Space), swapchains (xrCreateSwapchain from ovrpLayerDesc, acquire/wait/release,
XrCompositionLayerProjection submit). Vulkan binding from Initialize5 args [VERIFIED].

## Next (see ../TESTING.md for the test plan)
1. NDK arm64 build (host build is validation only).
2. [ANDROID-TODO] JavaVM/activity -> XrInstanceCreateInfoAndroidKHR + xrInitializeLoaderKHR.
3. Input action sets (controllers/hands); depth layer; GetVulkan*ExtensionsKHR mapping.
4. Repack APK (real entitlement — you own it); test on Quest 2.
