# Target host platform — Steam Frame

Researched 2026-06-23. Answers "can the dumped APK run on Steam Frame, and do we
need Android given SteamOS is Linux?"

## Do we need the Android side? YES.
The game is an **Android binary**, not a Linux one — ARM64==ARM64 does NOT bridge:
- `libUE4.so` links **bionic** libc (ABI-incompatible with SteamOS glibc).
- Depends on Android system libs: liblog, libandroid (ANativeActivity,
  AAssetManager, input), libOpenSLES; boots via a Java/JNI NativeActivity.
- Reads OBB assets via Android AssetManager/storage paths.
=> Cannot run the .so bare on SteamOS. Must run inside an Android runtime.
   Only Android-free path = full native source recompile (no source -> not viable).

## The host pieces all exist (and are open)
- **Lepton** = Valve's official Android-on-Linux layer; a **Waydroid/AOSP fork
  built specifically to run Quest APKs on Steam Frame**, with sideloading. APKs
  run **native ARM64, no emulation** (the "Waydroid needs x86" caveat is about
  Waydroid on x86 PCs; Frame is ARM so it doesn't apply). Walkabout Mini Golf
  (Quest title) already cited running on it.
- **Monado** = open OpenXR runtime, runs on **Linux AND Android**, Vulkan
  compositor using VK_KHR_external_memory_fd / external_semaphore_fd (matches our
  Vulkan-renderer finding).

## Architecture
```
Steam Frame (SteamOS / Arch Linux, ARM64)
└─ Lepton (AOSP/Waydroid container, native ARM64)
    └─ RE4 VR APK (unmodified bionic Android binary)
        ├─ libUE4.so → [SHIM libOVRPlugin] → OpenXR → Monado → Frame compositor (Vulkan)
        └─ ovr_* Platform SDK → [STUB] (entitlement)
```

## Why the shim IS the project
Meta ended VrApi support 2022-08-31; OpenXR is the only supported Quest API and
Valve's whole stack is OpenXR (Monado). So:
- OpenXR Quest games  -> Lepton+Monado likely run them with little/no work.
- VrApi games (RE4 VR) -> won't: Lepton/AOSP will never ship Meta's proprietary
  libvrapi.so, so the unmodified game finds no VR runtime. The OVRPlugin->OpenXR
  shim is exactly what bridges a dead-API VrApi game to Frame's OpenXR stack.

## Remaining real unknowns (gated on Frame shipping ~summer 2026)
1. Does Lepton expose an OpenXR loader+runtime to apps INSIDE the container?
   (Almost certainly yes for the OpenXR-Quest-game use case; ride on it.)
2. Can a SIDELOADED app reach the runtime + compositor (perms across the Waydroid
   boundary)?
3. **Likely the real technical crux:** sharing Vulkan swapchain images from inside
   the Lepton container out to the host Monado/Frame compositor
   (VK_KHR_external_memory_fd across the container GPU boundary). May be moot if
   Monado's compositor runs inside the container.
