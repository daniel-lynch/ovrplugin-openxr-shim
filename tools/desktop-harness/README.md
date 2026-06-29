# Desktop harness — drive the shim against Monado on a PC

The shim is normally exercised only on a Quest (inside RE4's APK). This harness lets you
run its **OpenXR path on a Linux desktop**, headless, against
[Monado](https://monado.freedesktop.org/)'s *simulated HMD* — no headset, no game, no
`libUE4`. It's the fast iteration loop for the Steam Frame / Monado / Lepton bring-up.

## What it is

`harness.c` stands in for the game (UE4 + `OculusHMD`). It creates a Vulkan
instance/device the way UE's VulkanRHI does, then calls our `ovrp_*` exports **in UE's
order**:

```
PreInitialize3 -> Get{Instance,Device}ExtensionsVk -> Initialize5
  -> CalculateEyeLayerDesc2 -> SetupLayer            (once)
  -> per frame: Update3, WaitToBeginFrame, BeginFrame4, GetLayerTexture2,
                (clear the eye image), EndFrame4
  -> Shutdown2
```

The shim does the real OpenXR work underneath — `xrCreateInstance`, `xrGetSystem`,
`xrCreateSession` (Vulkan binding), `xrCreateSwapchain`, the `xrWaitFrame/Begin/EndFrame`
loop — against whatever runtime the OpenXR loader selects. Here that's Monado's simulated
HMD with the **NULL compositor** (renders nowhere), so it runs over SSH / in CI.

It ships nothing from Capcom/Epic/Meta — it only calls our own public `ovrp_*` ABI.

## Prereqs (Debian/Ubuntu)

```sh
sudo apt-get install monado-service libopenxr1-monado libopenxr-loader1 libopenxr-dev \
                     libvulkan-dev
```

## Build & run

```sh
shim/build_host.sh            # builds build/host/libOVRPlugin.so + build/host/harness
tools/desktop-harness/run.sh  # brings up monado-service headless, runs 300 frames
tools/desktop-harness/run.sh 1000   # custom frame count
```

Logs land in `build/host/monado.log` and `build/host/harness.log`.

### The scene (pose→view validation)

Each frame the harness queries the shim's per-eye pose (`ovrp_GetNodePoseState3` for
`EyeLeft`/`EyeRight`, with true IPD separation) and FOV, builds per-pixel world rays, and
renders a **world-locked procedural scene** — checkerboard floor 1.6 m below the eye, sky
gradient, and an orbiting sun — into the acquired eye image. This exercises the shim's
pose/FOV math: the two eyes show correct stereo parallax, and the world counter-moves as the
head pose changes (Monado's simulated HMD sways, so there's real motion). The first few
frames log per-eye pose + FOV. CPU-rendered (fine at the sim's 128×128; it's a test tool, not
a fast path).

### Watch it (windowed)

`VISIBLE=1` uses Monado's main compositor (mirror window) + the imgui debug GUI instead of
the NULL compositor, so you can watch the scene (floor grid, horizon, orbiting sun, stereo
parallax) and inspect swapchains. Needs a display — run it from the physical desktop session,
not over SSH, and give it a big frame count:

```sh
VISIBLE=1 tools/desktop-harness/run.sh 3600
```

## What "pass" looks like

`harness.log` should show the lifecycle succeed and frames present:

```
[harness] PreInitialize3 OK (XrInstance + system up)
[harness] VkInstance created
[harness] VkDevice + graphics queue (family 0) created
[harness] Initialize5 OK (XrSession created)
[harness] SetupLayer OK layerId=0 swapchainStages=3
[harness] ...
[harness] loop done: NNN/NNN frames presented
[harness] Shutdown2 OK — clean exit
```

Exit code 0 = frames presented; 2 = ran but presented nothing (session never reached the
running state — check `monado.log`); 1 = a hard failure.

## Limits / notes

- This validates the **OpenXR + Vulkan binding + frame loop + swapchain** path. It does
  *not* reproduce the game-thread/render-thread pacing or GPU load that drove the on-Quest
  "ghost"; those are device-side behaviours. It's for ABI/path correctness and porting to
  new runtimes, not perf tuning.
- The Android session path (`XR_KHR_android_create_instance`, `xrInitializeLoaderKHR`, the
  JavaVM/Activity chain) is `#ifdef __ANDROID__`-guarded in `xr_runtime.c` /
  `android_init.c`, so the same sources serve both targets.
- `passthru.c` (P4 native forwarding) is Android-only; on host its arm64 trampolines fall
  back to plain stubs and passthru stays inactive.
