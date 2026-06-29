# Lever 2 handoff — detect dropped/black frames and reproject instead of presenting black

Entry point for implementing the elegant fix to the in-game black. Read alongside the
auto-memory: `black-is-not-submit-timing-ue-renders-empty`, `ingame-black-render-content`,
`gameplay-eye-image-is-pure-black-confirmed`, `standing-vs-sitting-blackflash`.

## The settled root cause (do not re-litigate)
The in-game black is a **UE-internal frame-drop under GPU load**, NOT a shim bug:
- RenderDoc (`~/renderdoc-captures/RE4/work_frame.rdc`): under load UE renders only **~28
  draws into its eye target vs ~198 in a normal frame** (a truncated frame), and the
  **resolved eye = pure black** (MEAN [0,0,0], reliable ms=1 read).
- Ruled out conclusively: submit-timing (present-on-submit incl. full one-frame defer
  still blacks — `debug.re4vr.submithook 2`/`defern 99`), image targeting (LAYER MISMATCH
  count = 0, stage==acquiredIndex), empty-frame submission (SUBMIT-BLACK/COMPOSE-EMPTY = 0).
- Load-gated: bridge (sparse) never blacks; house/dense geometry blacks; "especially while
  casting" (extra load). The game does NOT self-scale from our GPU-time feed.

## The idea
When UE hands us a truncated/black frame, **don't present it** — re-present the **last
good** eye image with the current frame's pose, so the OpenXR compositor **timewarps/
reprojects** the last good content to the new head pose. A reprojected (slightly stale)
frame is far better than a black flash. This is exactly the "app didn't produce a new
frame" path that compositors are built for; our black frames currently defeat it.

## Two hard parts

### A. A cheap "this frame is bad" signal (the crux)
Per-frame GPU readback to detect black is too costly + unreliable (observer effect; see
`texture-dump-is-unreliable-probe`). Candidate signals, cheapest first:
1. **Post-hitch heuristic:** the truncated frame is the RECOVERY frame after a stall
   (RenderDoc: "recovery frame was 26 draws"). We already detect HITCH (dt>20ms in the
   FRAME trace). Try: skip+repeat the 1–2 frames following a detected hitch. Coarse but
   zero new cost; test first.
2. **Submit/command-buffer count via the vkQueueSubmit hook (already built,
   `debug.re4vr.submithook 1`):** a truncated frame issues fewer submits / command buffers.
   Instrument submits-per-frame (between END markers) and correlate with perceived black.
   Under load we saw ~6 submits/frame normal — a dropped frame may show fewer. Needs a
   black ground-truth to calibrate (hard without readback; use the post-hitch frames as a
   proxy, or a one-off RenderDoc cross-check).
3. **GPU frame time over budget:** `g_gpuFrameMs` already tracked; but it's the PREVIOUS
   frame's dt (lagging), so use it to predict the next frame is at-risk, not to gate the
   current one.
Recommendation: start with (1) post-hitch repeat — simplest, no new signal — and measure.
If it helps but is too coarse, add (2) via the hook.

### B. Re-presenting the last good frame
OpenXR requires xrEndFrame every frame after xrBeginFrame; you can't simply skip present.
To reproject, present the LAST GOOD eye image again with the current `predictedDisplayTime`
+ located views (the runtime timewarps it). Mechanics:
- Keep a reference to the last-good eye image. Two options: (a) DON'T release frame N-1's
  swapchain image and re-submit it (risky — holding across frames is what made
  `pipeline=1` unstable; see `deferred-flush-unstable-abandoned`), or (b) **copy** the last
  good eye image into a shim-owned VkImage (the copy-ring infra already exists:
  `xrr_vk_alloc_images`, `shimImages[]`, the copy path in end_frame) and present a normal
  fresh swapchain image blitted from the held copy. (b) avoids the cross-frame-hold
  instability.
- On a bad frame: skip UE's (black) image, blit last-good copy → the acquired swapchain
  image, submit the previous composition's layer with the CURRENT pose/displayTime.
- Gate the whole thing behind a new `debug.re4vr.skipblack` prop (default 0), like the
  other levers, so the known-good path is untouched.

## Key code locations (shim/src/xr_runtime.c unless noted)
- `xrr_end_frame` sync branch (~1053): where present happens; add the skip+repeat here.
- FRAME/HITCH trace (~1123): the hitch signal (dt>20ms) for heuristic (1).
- `build_composition` (~620): the composition we'd re-submit with updated pose.
- Copy-ring infra: `xrr_vk_alloc_images` / `shimImages[]` (vk_session.c) + the copy-ring
  path in end_frame (~922) — reuse for holding/blitting the last-good image.
- vkQueueSubmit hook + `xrr_on_ue_submit` (~820): submits-per-frame instrumentation for
  signal (2). `debug.re4vr.submithook 1` = instrument.
- Pose update for reprojection: `xrr_eye_fov_tangents` + the located views in `g_xr.views`.

## Validation
Device: Quest 2 `<redacted-serial>` (USB). Build/deploy:
`./shim/build_android.sh && ./packaging/repack.sh && adb -s <redacted-serial> install -r packaging/out/re4vr-shim.apk`
(NOTE: repack silently bundles the LAST successful build — always confirm the build had no
errors and check `shim/build/arm64/libOVRPlugin.so` timestamp before repack.)
Test: Standing, walk off the bridge toward the house (reliable black trigger), `trace=1`.
Success = black flashes replaced by (at worst) brief reprojection judder, not black.

## Prerequisite vs Lever 1
If Lever 1 (force lower GPU load so UE completes frames: `ffr=3` + `debug.re4vr.resscale`
< 100 + `sscap=1`) sufficiently stops the drops at acceptable quality, Lever 2 may be
unnecessary or only needed for the worst spikes. Decide after the Lever 1 result.
```
debug.re4vr.resscale = percent of eye size (default 100; e.g. 75, 50). New this session.
```
