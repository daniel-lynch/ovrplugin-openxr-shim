# ovrplugin-openxr-shim

A from-scratch reimplementation of Meta's `libOVRPlugin.so` on top of **OpenXR**,
so legacy VrApi/OVRPlugin-based Meta Quest titles can run on standard OpenXR
runtimes (Monado, and eventually Valve's **Steam Frame** under Lepton).

**Status:** *Resident Evil 4 VR* boots and is playable on a Quest 2 through this
shim — stereo rendering, head + controller tracking, buttons, grips, haptics, and
save loading all work. (Developed as a preservation / interoperability experiment.)

## What it is

Quest's `libOVRPlugin.so` is the C shim Unreal/Unity games call to talk to Meta's
VR runtime. Meta deprecated the underlying VrApi in 2022 and the whole modern stack
(incl. Steam Frame's Monado) is OpenXR-only, so VrApi-era titles have no runtime on
non-Meta OpenXR platforms. This project re-exports the `ovrp_*` C API backed by
OpenXR instead, as a **drop-in replacement** `libOVRPlugin.so`:

```
game (libUE4.so) ──ovrp_* C API──> [THIS SHIM] ──OpenXR──> runtime (Monado / Meta / …)
```

It implements the OpenXR instance/session lifecycle, the Vulkan graphics binding,
the frame loop + swapchains, layer compositing, and action-based input — mapping all
of it to the `ovrp_*` ABI the game expects.

## Legal / scope

- This repo contains **only original code**. It does **not** include or redistribute
  any game, the Meta runtime, Meta's headers, or Epic's UnrealEngine source. You
  must build the shim yourself and apply it to a copy of a game **you legally own and
  dump yourself** (patch-only, dump-your-own — like ROM-hack patches).
- Reimplementing an API for interoperability is the goal here; no proprietary binaries
  or decompiled source are published.
- Entitlement/ownership checks are **out of scope**: this project ships no circumvention code
  and circumvents nothing. On Quest the platform's real entitlement check runs unchanged (you
  own the title). Running on hardware with no Meta backend requires a valid entitlement by
  other means — that is the user's responsibility and not provided here.
- Not affiliated with or endorsed by Meta, Capcom, Epic Games, or Valve. All
  trademarks belong to their owners.
- Provided as-is, no warranty. You are responsible for compliance with applicable law and
  the terms of any software you use it with, in your jurisdiction.

## Build

```sh
scripts/fetch_deps.sh          # OpenXR + Vulkan headers (Apache-2.0), Android NDK
shim/build_android.sh          # -> shim/build/arm64/libOVRPlugin.so
```
Needs: Android NDK (r27c), a JDK, and the OpenXR/Vulkan headers (the fetch script
gets them). A host x86-64 build is also supported for compile-validation.

## Use (with your own dumped game)

```sh
packaging/repack.sh /path/to/your/base.apk   # swap the shim in, re-sign
adb install -r packaging/out/<game>-shim.apk
# push your own dumped OBB, then launch on a dev-mode Quest
```
See `packaging/README.md` and `TESTING.md` for the full flow.

## Layout

- `shim/src/` — the implementation: `xr_runtime` (session/frame loop/swapchains),
  `vk_session` (Vulkan binding + ext), `layers`, `xr_input` (action sets), `core`
  (the ovrp_* entry points), `android_init`, generated `stubs`.
- `shim/include/ovrplugin_shim.h` — the `ovrp_*` C ABI (clean-room from observed ABI).
- `packaging/` — repack/sign tooling.
- `docs/` — research notes + session handoffs (`docs/README.md` narrates how the
  frame-pacing "ghost" was solved); `TESTING.md`/`HOST.md` at root.

## Acknowledgements

Built against the [OpenXR](https://www.khronos.org/openxr/) and
[Vulkan](https://www.vulkan.org/) specs and the Khronos OpenXR loader.
