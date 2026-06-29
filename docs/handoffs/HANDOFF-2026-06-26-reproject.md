# RE4 VR Shim — Handoff (2026-06-26, Lever 2 reproject session)

Continuation of `HANDOFF-2026-06-26-skipblack.md`. Branch `latency-perffix-copyring`.
This session **built and shipped the working in-game black fix (Lever 2 reproject)** and hit
the tuning wall that points to a render-ahead pivot. Detail in auto-memory (MEMORY.md index):
`luma-gate-works-bad-vs-benign-black`, `reproject-works-playable`,
`reproject-tuning-and-renderahead-pivot`, `submit-count-cannot-detect-black`,
`skipblack-blacks-are-sustained-onset-only`.

## TL;DR — the in-game black is FIXED (reproject), "1000x better than black, playable"
The whole project's black-hunt is functionally solved. On a truncation-black frame we blit the
held last-good eye image back over it and present with the SAVED pose; the compositor timewarps
it to the current head pose instead of flashing black. Committed through `3522e5c`.

## What was built this session (commits a45362c..3522e5c)
1. **Post-hitch detector + barcode validation tool** (a45362c, c2a6c29): instrument-only black
   flag; on-screen frameIndex barcode that turns red on a flagged frame for frame-exact video
   correlation (`debug.re4vr.barcode`).
2. **Submit-count ruled out** (508f55a): UE issues a flat 3 vkQueueSubmits/frame on black AND
   normal frames — truncation is fewer DRAWS, not fewer submits. Dead end, don't retry.
3. **Per-frame luma black gate** (571aa00): `xrr_vk_frame_luma` reads max luma of a few rows of
   the resolved eye image; covers the whole sustained-black TAIL (post-hitch only caught the
   onset). `debug.re4vr.lumagate`, `lumathr` (default 12). **Bad black = luma<thr AND
   appLayers==1** (appLayers>=2 = a menu's black eye-fov behind a UI quad — benign).
4. **Reproject** (7c0882b): `debug.re4vr.reproject=1`. Copy-ring hold (`L->holdImage`, no
   cross-frame swapchain hold so no pipeline=1 instability). Save acquired->hold on good frames
   (with pose), restore hold->acquired on black frames, submit with saved pose
   (`g_lastGoodViews`, used in `build_composition`).
5. **Tuning** (e3bbe85, 3522e5c): `savethr` (default 40, only hold clearly-lit frames — fixes
   dim reprojected frames); `abruptdrop` (default 0 = reproject all; raise to skip gradual
   fades); `holdevery` (default 4, throttle the save copy for perf).

## State of play (device-tested)
- **Black flashes GONE, playable.** Stable, no crashes over multiple soaks.
- **Perf ~17ms (~59fps).** Breakdown measured: resolve-wait ~7ms + luma ~3.5ms + save ~0.6ms.
  Folding luma into the resolve cmd buffer = WASH (luma's cost is a forced Adreno tile-resolve
  from the TRANSFER_SRC transition, not shareable wait overhead — tried + reverted). 72Hz needs
  attacking the 7ms resolve-wait = render-ahead.
- **Remaining artifacts (all inherent to single-frame reprojection):** (a) reproject OVERRIDES
  the game's intentional fades (comfort/teleport/scene/load) — they're black too; abruptdrop
  helps but can't cleanly separate fade from shallow truncation. (b) long ~1s holds during
  loads are jarring. (c) head-rotation during a hold shows timewarp edge-black; depth-hold (not
  done) would add positional reprojection.

## NEXT — revisit RENDER-AHEAD (user's strategic call), but verify the premise first
Why now: render-ahead adds 1 frame latency -> UE gets more GPU budget/frame -> FEWER/shorter
truncations -> reproject (and its artifacts/long-holds) needed far less; and reproject+luma are
now a SAFETY NET for render-ahead's old instability (detect+hide any black instead of flashing).
- **GATE (do this first):** resolve the contradiction — memory `black-is-not-submit-timing`
  says UE renders empty regardless of timing, but the user observed black DROPPED when the luma
  readback (a per-frame GPU sync) turned on. Experiment: use the luma gate to COUNT
  appLayers==1 truncation-black frames per minute at different added-latency/sync levels (e.g.
  baseline vs lumagate-on vs an added sync). If latency reduces truncation frequency ->
  render-ahead is justified; if not -> stay with reproject + tuning + maybe depth-hold.
- If justified: build render-ahead informed by the luma signal (present held frame only when
  needed, not blindly like the old `pipeline=1`), with reproject as the fallback. See
  `deferred-flush-unstable-abandoned` for the old failure modes (acquire/release imbalance).

## Game perf RE (done this session) — one new lever, several myths busted
Static RE of the GAME binary `dump/apk_libs/lib/arm64-v8a/libUE4.so` via capstone. Full report:
`analysis/game-perf-RE.md`; memory `game-perf-RE-findings`. This REPLACES the behavioral
inferences in `game-doesnt-drive-dynamic-perf-from-feed` with code proof:
- **No game-side dynamic-FFR loop exists** — `SetTiledMultiRes{Level,Dynamic}` args are config
  bytes (`SetTiledMultiResDynamic(Settings.byte[0x11d])`), never compared to GPU time/metrics. So
  shim-side dynamic FFR is the ONLY path (nothing we return makes the game loop). Boot `Dynamic(0)`
  is just that config value.
- **Perf-metrics gate hypothesis REFUTED** — game never gates FFR on `IsPerfMetricsSupported`;
  perf-metrics feed only a stats/HUD collector (0x5c34678). Commit 68a7093 unlocked no behavior.
- **AppSW dead** (`GetLayerTextureSpaceWarp` not even a string in the binary).
- **`GetSystemRecommendedMSAALevel2` = hazard** (over-report -> more MSAA -> more load). Keep at 1.

### NEW game-driven lever — implement `ovrp_GetAdaptiveGpuPerformanceScale2` (do next session)
Call site `0x5c3cca0`: game does `resolution = baseDensity * sqrt(scale)` where `scale` comes from
this API (table slot g+0x130). We return Unsupported -> scale=1.0 -> "viewport always full under
load". **If the shim returns `scale < 1.0` under GPU pressure (derive from `g_gpuFrameMs` vs the
13.9ms budget), the game self-downscales resolution — NO game patch.** Gated on `Settings.byte[0x19]
bit4` (adaptive-res enabled) — VERIFY on device. CAVEAT: this is a GPU-LOAD (pixel) lever; the
truncation is DRAW-bound (quarter-res still blacks), so it's a throughput/comfort/headroom win that
*might* indirectly slacken the render thread — NOT a guaranteed truncation fix. Easy + game-native;
worth stacking, measure truncation effect with the luma counter. (Find the API's stub in
`shim/src/stubs.c`; we already own `g_gpuFrameMs` + apply-foveation path.)

## Secondary follow-ups (the user "likes them all")
- **Depth-hold** (artifact reduction): enable depth (works on device, gameplay renders), hold
  +restore depth alongside color, chain it (`build_composition` already chains depth) -> proper
  positional reprojection. Helps the head-rotation swim/edge-black.
- **Verify the ghosting-fixed-by-readback** observation independently.

## Build / deploy / config
```
./shim/build_android.sh && ./packaging/repack.sh   # repack bundles LAST build — check timestamp
adb -s <redacted-serial> install -r packaging/out/re4vr-shim.apk
adb -s <redacted-serial> logcat | grep -iE "xrr|REPROJECT"
```
Clean device state set this session: `reproject=1 holdevery=4 savethr=40 abruptdrop=0 depth=0
trace=0 barcode=0`. Launch blocked by controllers-asleep dialog — wake controllers first.

## Key reproject toggles (system props, re-read each frame unless noted)
- `debug.re4vr.reproject 1` — the fix (implies luma readback).
- `debug.re4vr.lumathr N` (12) — black threshold; `savethr N` (40) — min luma to HOLD a frame.
- `debug.re4vr.abruptdrop N` (0=reproject all; ~25 skips fades but misses shallow truncations).
- `debug.re4vr.holdevery N` (4) — save last-good every Nth frame (perf throttle).
- `debug.re4vr.barcode 1` — on-screen frameIndex barcode (red = reprojected frame), validation.
- `debug.re4vr.depth 1` — experimental depth swapchain (renders OK; not yet held for reproject).
