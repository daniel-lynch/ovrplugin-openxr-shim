# RE4 VR Shim — Next Session Handoff: Motion Reprojection / Latency

**Date locked:** 2026-06-23. **State:** RE4 VR is **playable** on Quest 2 via the
OVRPlugin→OpenXR shim. One known issue remains: **ghosting/duplication during head
movement** (and the title/menu "duping" — same root cause). This doc is the plan to
fix it.

Deploy any change with: `./shim/build_android.sh && ./packaging/repack.sh && adb install -r packaging/out/re4vr-shim.apk`
Logs: `adb logcat -d -s xrr`. Device = Quest 2 over wireless adb (`adb connect <redacted-ip>:5555`).

---

## What works (do not regress)
- Rendering is correct & stable when the head is still; stereo/IPD/FOV/poses all verified.
- **Tile-memory flush barrier** (`src/vk_session.c xrr_vk_flush_image`) — THE fix for the
  black/tearing. Quest's Adreno is a tiler; UE's eye render must be flushed to main
  memory + made visible before the compositor reads. We submit a `VkImageMemoryBarrier`
  on UE's queue in `xrr_end_frame` before `xrReleaseSwapchainImage`.
- **Floor tracking** = `XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR` when the game requests
  FloorLevel (gun sits on hip). Eye-level sinks you into the floor; STAGE reverses movement.
- Menus placed at the game's real submitted pose/size (respect `ovrpLayerSubmitFlag_HeadLocked`).
- `DestroyLayer` implemented (+ slot reuse); finite swapchain-wait timeout (no hangs).

## The remaining problem (precisely)
- Symptom: scene/menu **renders correctly but duplicates in two places during head
  movement**; offset tracks head-motion direction; absent when still. Also shows in-game
  as **black flashes on movement**. Same root cause.
- **Proven NOT the cause** (ruled out by logging + a pulled headset video whose mono
  recorded frames are CLEAN): content rendering, stereo/IPD (65-68mm), FOV, swapchain
  stage-vs-acquired index (always matched), session cycling (gone, ~72fps steady),
  eye poses (flags=0xf), pixel count (capping supersample 1.2x→1.0 did NOT help).
- ~~Root cause = submit latency from the mandatory flush-wait.~~ **REFUTED 2026-06-24
  by on-device measurement — see below.**

## UPDATE 2026-06-24: latency theory DISPROVEN; render-ahead abandoned
Measured on device (synchronous build, instrumented `xrr_end_frame`):
- **`submit-vs-predictedDisplayTime = −17 to −18 ms`** every frame (in menu AND gameplay).
  We hand `xrEndFrame` to the compositor ~1.5 frames (at 90 Hz) **BEFORE** the intended
  display moment. We are NOT late — there is healthy headroom. The whole "xrEndFrame
  lands after predictedDisplayTime" premise is false.
- flush-wait blocks the render thread ~1 ms in menus, up to ~8–9 ms in gameplay, but
  never enough to miss the deadline (still −17 ms).
- **Title screen dupes too, where flush-wait is only ~1 ms** → a bug that persists with
  near-zero latency cannot be a latency bug.
- App runs **90/90 fps → ASW/motion-smoothing is NOT engaging** (would show 45/90).
- The pulled-video mono frames are clean → the doubling is introduced **at display time,
  in stereo** (a mono capture can't show stereo divergence).

**Conclusion: the dupe is a stereo/compositor presentation issue, NOT latency.** The
render-ahead pipeline (below) was therefore the wrong fix AND broke rendering (see
"Why render-ahead failed"). It is now OFF by default and gated behind
`debug.re4vr.pipeline` purely for reference. Do not pursue it.

### Ruled out on 2026-06-24 (with the exact data)
- Layer layout: `layout=3` = `ovrpLayout_Array`, `arraySize=2`, we submit
  `imageArrayIndex=eye` — correct, not a side-by-side/double-wide mismatch.
- Per-eye FOV: properly asymmetric & mirrored (L eye `right=+0.785`, R eye
  `left=−0.785`, toed toward the nose), `up/down` symmetric. Correct.
- IPD ~0.068 m. Eye positions sane (`L≈(-0.037,1.088,0.005) R≈(0.031,1.090,0.007)`).

### Head-vs-eye camera mismatch — ALSO RULED OUT (2026-06-24)
`HEADvsEYE:` capture: `head.pos == eyeMid` exactly and `head.q == e0.q == e1.q`
exactly on every frame. UE's Head-derived cameras and our submitted `xrLocateViews`
eye poses are geometrically identical (parallel rig, midpoint = head). Not it.

### Visual A/B probes (debug.re4vr.diag) — localized the dupe INTO the per-eye image
Added a no-rebuild toggle: `adb shell setprop debug.re4vr.diag N; relaunch`.
- `diag=1` projection only (drop quads), `diag=2` force mono (both eyes sample
  array layer 0). Applied in `build_composition`.
- **diag=2 (force mono): dupe STILL present** — a fixed offset down-and-right,
  visible with the head still, small gap. → NOT stereo divergence (both eyes get
  the identical image yet still doubled) and NOT a motion/reprojection ghost (those
  vanish when still).
- **diag=1 (projection only): dupe STILL present** → NOT the quad/menu overlapping.
- `SUBMITLIST:` capture confirms only ONE eye-fov layer (LayerId=1) is submitted
  (most frames `nLayers=1`); occasionally + one quad (LayerId=0). Eye layer flags
  `0x4` (InverseAlpha-ish) / `0x14`; we don't act on them (compositing hints, not
  layout — unlikely to cause a positional dupe).

### Where it stands: the duplicate is INSIDE one eye's image
With both eyes fed the identical array-layer-0 image and still doubled, each per-eye
image itself contains two offset copies. So the doubling is introduced either (a) in
UE's render INTO the eye texture, or (b) in the compositor's per-eye display path —
NOT in our stereo/layer pairing, poses, FOV, layout, or timing (all verified correct).

### ROOT CAUSE FOUND (2026-06-24): the dupe is a QUAD overlapping the projection
Texture readback + visual A/B settled it:
- Dumped both eye array layers + the quad to PPM (`xrr_vk_dump_image`, gated by
  `debug.re4vr.dump`). **Both eye textures are CLEAN single images** (the eye
  projection is a clean RE4 castle scene; pre-title eye is pure black; the quad is a
  clean "armature" studio logo). So nothing we render is doubled.
- **Closing one eye: still doubled** → the second copy is within each eye's image
  (NOT stereo divergence — earlier "converges at a head pose" was the world-locked
  quad parallaxing against the projection).
- **`diag=1` (drop quad layers): the dupe DISAPPEARS** (logo becomes single).
→ **The duplicate IS the quad.** The game's UI/logo is present in the eye projection
  AND re-submitted as a separate world-locked quad layer; the compositor shows both,
  offset, so they parallax with head motion. Worst on title/menus (heavy UI quads),
  mild in-game (few quads), flashes (quad submitted only on some frames per SUBMITLIST),
  persists one-eyed (two real copies). NOT latency, stereo, FOV, layout, or timing.

### REFINED 2026-06-24 (via Quest recordings + frame blends): TWO artifacts
Pulled Quest spectator recordings (`/sdcard/Oculus/VideoShots/`) and blended head-
motion frame pairs (`ffmpeg fps=30` + ImageMagick `-average`). Single spectator frames
are always clean; blending two frames ~0.1-0.2s apart during a head turn exposes
differential motion. Findings:
- **Artifact A — logo duplication (diag=0):** the blend shows TWO "resident evil 4"
  logos offset vertically while the background is nearly aligned → a second logo copy
  that OVER-moves vs the scene. With `diag=1` (quads dropped) the over-moving copy is
  gone. BUT the logo is STILL present in `diag=1` → the logo also lives in the eye
  projection (the game renders it into the eye buffer) AND is re-submitted as a quad.
  The quad copy is the visible dupe. Fix = handle/suppress the duplicate quad.
- **Artifact B — scene ghosting on head motion (persists with diag=1):** user still
  sees heavy ghosting of the whole scene during head turns with quads dropped, yet
  single spectator frames are clean → it's PER-EYE reprojection ghosting at display
  (mono spectator can't show it). Textbook symptom of a **projection layer submitted
  WITHOUT depth** → compositor can't do positional reprojection → head translation
  uncompensated → ghosting. (`xrr_setup_layer_depth` currently returns Unsupported.)

### Depth layer (XR_KHR_composition_layer_depth) — IMPLEMENTED + TESTED → NEGATIVE
Implemented behind `debug.re4vr.depth` (default off): enable the ext in pre_init,
create a D32_FLOAT depth swapchain per eye layer (`setup_layer`), hand UE the depth
images via GetLayerTexture2, acquire/wait/flush(depth-aspect barrier)/release in
lockstep with color, chain `XrCompositionLayerDepthInfoKHR` on each projection view.
Tunable: `debug.re4vr.depth_nearz_mm`, `debug.re4vr.depth_revz`. Confined to the
synchronous path (`!g_pipelineActive`).
On device: depth swapchain created cleanly (fmt=126, 3 images, matches color), NO
xrEndFrame/validation errors, both reverse-Z and standard-Z tried. **Result: zero
improvement to the ghosting**, and the Quest spectator recording went **fully black**
with depth on (headset still rendered). → Meta's runtime accepts but does NOT use
plain KHR depth for reprojection (it uses Application SpaceWarp / motion vectors).
**Depth is not the fix here.** Left gated behind the prop (off); do not enable.

Recommendation (original, now amended): depth did NOT fix B.

### ROOT CAUSE OF ARTIFACT B FOUND (2026-06-24): frame drops from flush-wait serialization
Meta `VrApi` perf log during head motion (depth OFF, normal build):
- Calm: `FPS=72/72` steady. During motion: `FPS=30-64/72` (and `30-55/90`) with
  `Stale=40-70` → the app misses display rate, compositor shows stale/reprojected
  frames → the ghosting. Settles when still (app catches up).
- `App` GPU time is only 2-6ms (rendering is cheap!) but `CPU&GPU` total is 13-36ms
  → CPU↔GPU are SERIALIZED, inflating frame time. The synchronous flush-wait
  (`xrr_vk_flush_wait` in end_frame, ~8ms/frame measured) blocks the render thread on
  UE's GPU work each frame, killing CPU/GPU pipelining. Depth=1 doubled it (color +
  depth flush-wait) → `30/90, 31ms`, which is why depth made ghosting WORSE.
**Artifact B = frame drops caused by the flush-wait serializing CPU and GPU.** Not
latency-submit, not depth, not stereo. The fix is to get the flush-wait off the
critical path so CPU/GPU pipeline again.

### Perf-level fix — IMPLEMENTED + CONFIRMED WIN (2026-06-24)
We were no-op'ing `ovrp_SetSystemCpuLevel2/GpuLevel2`. The game requests CPU=2
(SUSTAINED_HIGH) and GPU=3 (BOOST); now forwarded via XR_EXT_performance_settings
(`xrr_set_perf_level`, always on). On device: `perf level: CPU ovrp=2->xr=50 rc=0`,
`GPU ovrp=3->xr=75 rc=0`. Clocks ramped GPU 305-490MHz -> **525-587MHz**, CPU steady
2419MHz. FPS improved to mostly 72/72 + some 90/90 (user confirmed "fps higher, logo
better"). Remaining: motion drops to 45-68/72 (flush-wait, see copy ring) + occasional
severe 49ms hitches (likely title asset streaming, not our code). Keep this fix.

### UE-source facts (ue_src/, real OVR_Plugin_Types.h) that pin the copy-ring design
- UE wraps OUR VkImage as its RHI render target (`RHICreateTexture2D[Array]FromResource`
  in CustomPresent_Vulkan) — so handing UE shim images via GetLayerTexture2 works.
- Present flow (`FinishRHIFrame_RHIThread`): for each layer UpdateLayer_RHIThread builds
  the ovrpLayerSubmit (TextureStage = the stage UE rendered into THIS frame) -> EndFrame4
  (our xrr_end_frame) -> on success, `IncrementSwapChainIndex_RHIThread` advances UE's
  stage for next frame. So `submit->TextureStage` reliably names the image UE just drew.
- depthFormat 10 = ovrpTextureFormat_None (D16=6,D24_S8=7,D32_FP=8,D32_S824=9) -> UE does
  NOT render depth. Depth path is moot (mapping corrected).
- ovrpLayerSubmit_EyeFov tail carries ViewportRect[2], DepthNear/Far, Fov[2] (in the
  un-reversed union pad) if ever needed.

THE FIX for the motion drops (validated target): shim-owned copy ring.
Design (eye layers only, behind debug.re4vr.copyring): allocate shim VkImages per eye
layer (color, COLOR_ATTACHMENT|SAMPLED|TRANSFER_SRC); GetLayerTexture2 returns shim
images so UE renders into them on its own stage cadence. OpenXR swapchain gets
+TRANSFER_DST usage. Each end_frame: copy shimImages[submit->TextureStage] ->
openxr[acquiredIndex] (4 barriers: shim COLOR->TRANSFER_SRC, openxr UNDEFINED->
TRANSFER_DST, vkCmdCopyImage all array layers, openxr TRANSFER_DST->COLOR_ATTACHMENT,
shim TRANSFER_SRC->COLOR_ATTACHMENT) submitted NO-wait; present the PREVIOUS frame's
openxr image (wait its copy token = already done -> no CPU stall) with the stored
composition; hold this frame's openxr. The copy both resolves tile memory AND is
pipelined, so the CPU never blocks on the current frame's GPU = breaks the serialization.
UE's stage is decoupled (copy uses TextureStage explicitly), so no stage-coupling break.

### COPY RING — IMPLEMENTED (untested), behind debug.re4vr.copyring (default off)
Code: `xrr_vk_alloc_images`/`xrr_vk_free_images`/`xrr_vk_copy_submit` (vk_session.c),
shim fields on XrLayer, setup_layer allocates shim images + adds TRANSFER_DST to the
eye swapchain, GetLayerTexture2 hands UE the shim images, end_frame has a copy-ring
branch (copy shim[TextureStage]->openxr[acquiredIndex] no-wait, present previous frame,
hold current). Builds clean. **VALIDATION CUT: eye layers only — quads are DROPPED in
copy-ring mode, so MENUS/UI ARE HIDDEN.** It's for measuring whether removing the eye
flush-wait serialization restores framerate; if confirmed, next step is to handle quads
(composite current-frame quads on top of the pipelined eye, or give quads shim+copy too).

### TEST PLAN (run together when ready; perf-level fix is already always-on)
All live except where noted. Relaunch after setting copyring/sscap (decided at setup).
- Baseline (perf fix only): props all 0. Move head, `adb logcat -d | grep VrApi` — note
  the FPS/Stale distribution (expect 72/72 mostly + drops to 45-68/72 on motion).
- Copy ring: `adb shell setprop debug.re4vr.copyring 1` + relaunch. Expect: scene visible
  but NO menus; check VrApi FPS holds 72/72 (or 90/90) on motion with fewer Stale, and
  ghosting reduced. Log: `copy-ring: ENABLED`, `copyring: allocated N shim images`,
  `end_frame copy-ring`. If black/garbage -> a barrier/layout bug in xrr_vk_copy_submit.
- Supersample cap (stackable): `adb shell setprop debug.re4vr.sscap 1` + relaunch
  (eye 1728x1900 -> 1440x1584; frees GPU + shrinks the copy). Log: `supersample cap: 1.0x`.
- Revert: set all back to 0, relaunch.
The 49ms hitches (title asset streaming) are NOT addressed by any of these.

### COPY-RING TEST RESULT (2026-06-24): works on title, BLACK in-game
On-device with copyring=1:
- Engaged cleanly (`copy-ring: ENABLED`, `allocated 3 shim images` for 1440 & 1728 eye
  layers), frames flowed (`end_frame copy-ring #20161+`), one transient `xrEndFrame
  FAILED rc=-23` (XR_ERROR_LAYER_INVALID) at the 1440->1728 layer swap.
- **TITLE: after a moment, ghosting RESOLVED — "everything looks good."** Frame pacing
  hugely improved: long stretches of `FPS=72/72 Stale=0 CPU&GPU=2.9-3.2ms` (vs baseline
  13-36ms) = the CPU/GPU serialization is broken, exactly as intended. **This proves the
  flush-wait serialization is the ghosting root cause.**
- **IN-GAME: mostly BLACK.** From the one in-game snapshot: copy-ring still running,
  app stuck at stage=2 (note: title also runs stage=2 and works, so stage isn't it),
  a `WaitSwapchainImage TIMEOUT layer=1` near startup, no error flood.
- Leading hypothesis: the 1-frame pipeline holds 2 of the 3 OpenXR swapchain images;
  under heavier in-game load the compositor can't return the 3rd in time -> begin_frame
  xrWaitSwapchainImage TIMES OUT -> the present-pending chain breaks and does NOT
  self-recover -> persistent black. (Title is light enough to never time out.)
- NEXT (tractable, not fundamental): make the copy-ring chain robust to a timeout —
  on a missed acquire, present empty (valid) that frame and cleanly re-establish the
  chain next good frame (never submit an invalid/stale layer; keep swapchain
  acquire/release perfectly balanced across the timeout path). Then re-test in-game.
  Also worth: confirm the timeout frequency in-game (capture was flaky — headset must
  be worn for the whole window). Copy-ring left OFF by default; perf fix is the
  shippable win so far.

### COPY-RING RETEST + PIXEL DUMP (2026-06-24): shim redirection breaks gameplay render
Robustness fix (present-empty on broken chain) added; retested in-game = still black.
Live buffer showed copy-ring running fine in-game: ~4000 frames (`copy-ring #2161..6241`),
**0 TIMEOUT, ~2 chain-broken, no FAILED** — so NOT timeouts/chain-break/errors.
Added a pixel dump to the copy-ring path (`debug.re4vr.dump` -> cr_shim_a0.ppm /
cr_xr_a0.ppm). Dumped the shim (UE's render target) and the post-copy openxr image:
- **Both are pure black (mean=0, std=0 — every pixel 0) in gameplay** (`shim stage=0`).
  The copy is faithful (openxr == shim); the SHIM ITSELF is black = UE rendered nothing
  into the shim image in gameplay.
- On the TITLE the copy-ring showed content (castle) -> shim had content there.
Conclusion: **UE renders into our shim image on the title but NOT in gameplay** — its
heavier gameplay render path apparently doesn't write to the resource-wrapped shim
(RHICreateTexture2DArrayFromResource) image we hand it via GetLayerTexture2. Likely a
UE render-target/MSAA/resolve detail specific to the 3D scene path. Needs UE-internals
+ RenderDoc to chase; not resolvable via remote logcat/dump loop.

## RENDERDOC DEEP-DIVE (2026-06-24) — copy-ring gameplay = MSAA resolve interaction
Set up offline RenderDoc replay (huge: no headset needed for analysis):
- App made debuggable via apktool (packaging/work/dbg flow -> re4vr-dbg.apk); RenderDoc
  Android server installed; capture over USB; replay headless with
  `qrenderdoc --python` over `adb://<usb-serial>` + CreateRemoteServerConnection +
  CopyCaptureToRemote + remote.OpenCapture (local replay of an Android capture fails;
  wireless adb drops the replay connection — USB is required). Scripts in
  ~/renderdoc-captures/rd_*.py ; captures RE4/black.rdc (gameplay), RE4/title.rdc.
- FINDING: UE renders the eye with **2x MSAA** into its OWN target (RenderDoc res 11965,
  ms=2), and the render pass **resolve attachment is our copy-ring shim** (ms=1) — i.e.
  UE resolves the MSAA scene INTO the shim we copy. So there is NO stage mismatch; the
  shim IS the resolve target. BUT in the gameplay capture the resolved shim is not the
  scene at our copy point (reads white/garbage), while on the title it lands fine.
- So the copy-ring gameplay black is an **MSAA-resolve-into-our-shim interaction**: our
  externally-created VkImage (MUTABLE_FORMAT + COLOR|SAMPLED|TRANSFER_SRC|DST|INPUT_ATT,
  TILING_OPTIMAL) isn't receiving UE's MSAA resolve correctly in the gameplay path.
  Suspects to chase next: the MUTABLE_FORMAT/sRGB-vs-UNORM view used as resolve target,
  the image's create flags vs what a valid resolve dst needs, or tile-memory MSAA
  specifics on Adreno. (Single-shim made it worse — UE needs distinct per-stage images.)
- Tooling note: GetMinMax(...,CompType.Typeless) gives misleading values; trust
  SaveTexture/visual instead.

## NET STATE (end of 2026-06-24 session)
- SHIPPABLE WIN: perf-level fix (always on) — clocks boost, framerate up, confirmed.
- ROOT CAUSE PROVEN: motion ghosting = frame drops from the flush-wait serializing
  CPU/GPU (copy-ring eliminated it on the title -> CPU&GPU 13-36ms dropped to ~3ms).
- COPY-RING: built + behind debug.re4vr.copyring (default off). Works on title, but the
  shim redirection leaves gameplay black (UE not rendering into the shim in-game).
  Parked pending UE-render-path investigation.
- DEAD ENDS (with evidence): latency-submit theory (we submit early), depth layer (UE
  passes None; Meta ignores plain KHR depth anyway), render-ahead-by-holding-OpenXR-
  images (breaks UE's texture-stage coupling), stereo/FOV/layout (all correct).
- Levers available: debug.re4vr.sscap (supersample cap), and the perf fix is permanent.
- Diagnostic toolkit retained: debug.re4vr.{copyring,depth,pipeline,noflushwait,diag,
  dump,sscap} + the perf/flush-wait probes. UE renders into shim-allocated
VkImages on its own stage cadence (decoupled from the OpenXR swapchain, so no
stage-coupling break like the render-ahead attempt). Each frame: copy the shim image
into a freshly-acquired OpenXR image and pipeline the flush (wait the PREVIOUS frame's
copy, which is already done) → CPU never blocks on the current frame's GPU. Releases
the OpenXR image normally each frame (no holding → stage cadence intact). Cost: one
full-res image copy/frame (~10% bandwidth) + extra VRAM; big but well-scoped. This is
the original "option 2" and is now backed by the VrApi frame-drop data.
Tooling that worked: `debug.re4vr.diag` visual A/B + `debug.re4vr.dump` texture readback
+ Quest recording → frame-blend (the only way to make the artifact objectively visible).

### (Artifact A) figure out the correct quad handling
Open question — why does the same content appear in BOTH the projection and a quad
(real OVRPlugin presumably shows it once)? Avenues:
1. Confirm the overlap: dump eye + quad on the SAME title frame and check the logo is
   in both (double-render) vs the quad being the only intended copy.
2. UI routing: the game may render UI into the eye buffer only because some ovrp_* call
   we stub makes it think it's NOT in a layer-composited VR mode. Audit stubs that gate
   UE's "render UI to a separate layer vs into the eye buffer" decision.
3. Quad placement: we world-lock the quad at the app's submitted pose in appSpace
   (LOCAL_FLOOR). If the app's pose assumes a different space/convention, our quad is
   offset from where the projection shows the same content; correct placement (or
   head-locking) could make them coincide.
Workaround that proves the cause (not a fix): `diag=1` drops quads → dupe gone but
menus/overlays vanish and title head-tracking feels less smooth.

### (superseded) read back the eye texture — DONE, textures are clean
Add a one-shot GPU readback of eye-swapchain array layer 0 (copy VkImage→host buffer,
dump PPM, `adb pull`) gated behind a prop. If the dumped texture is doubled → it's
UE's render (game/UE-side; investigate multiview / the RE4 VR mod's render setup). If
the dumped texture is CLEAN/single → the compositor introduces it at display (per-eye
distortion/reprojection path; pursue Meta-specific settings / frame capture).
Everything cheaper than this has been exhausted. Device left on the clean synchronous
(playable) path; `debug.re4vr.diag`/`debug.re4vr.pipeline` both 0.

## (Dead end, kept for reference) The render-ahead pipeline

## The fix: take the flush-wait OUT of the critical path (pipeline it) — IMPLEMENTED 2026-06-24
Render-ahead by one frame so we never block the submit. **Status: built, compiles
clean, NOT yet tested on device.** Deploy + test per the commands at top.

How it works now (`xr_runtime.c xrr_end_frame`, gated by `g_pipelineActive`):
1. `xrr_begin_frame`: acquires image `A_N` for each layer as before (unchanged).
2. `xrr_end_frame` **(A)**: `xrr_vk_flush_submit(A_N)` — submits the barrier, returns a
   ring token, does **NOT** wait.
3. **(B)**: waits the *previous* frame's flush token (already done → ~free), releases
   `A_{N-1}` (FIFO → releases the older, present-pending image), and `xrEndFrame`s the
   **stored** composition `g_pending` (frame N-1's views + predictedDisplayTime).
4. **(C)** builds frame N's composition into `g_pending`; **(D)** promotes `A_N` to
   `presentPending` (held one more frame; `begin_frame` re-acquires fresh).
- **Why the stage↔acquire invariant survives:** still exactly one acquire + one release
  per frame, just offset by one → the OpenXR FIFO and UE's `TextureStage` stay in
  lockstep (the `LAYER MISMATCH` log will fire if this ever breaks — watch it).
- Net: +1 frame latency (~13ms, absorbed by normal reprojection); CPU never blocks on
  the flush → `xrEndFrame` lands on schedule → ghosting should clear.
- **Engage gate:** pipeline turns on once `xrr_vk_flush_ready()` AND every active layer
  has `imageCount >= 2` (Quest gives 3). Until then it runs the **synchronous fallback**
  (old flush+wait+release path, retained) — so worst case = today's behaviour, not a
  regression. Look for `render-ahead pipeline engaged` in logcat to confirm it switched.
- New split flush API in `vk_session.c`: `xrr_vk_flush_submit`/`_wait`/`_ready`; ring
  grown to `XRR_MAX_LAYERS*2` so a token stays valid a full frame.

### On-device validation checklist
- Confirm `render-ahead pipeline engaged` appears once, early.
- Watch for `LAYER MISMATCH` (should NOT appear) and `xrEndFrame FAILED` (should NOT).
- Heartbeat now prints `pipelined=1`. Framerate should stay ~72fps.
- **First tuning knob if ghosting persists:** `submit_pending()` uses the STORED
  `g_pending.displayTime` (frame N-1's predicted time). If motion judders/over-reprojects,
  try using the *current* `g_xr.frameState.predictedDisplayTime` instead while keeping
  the stored views — one-line change, documented inline. (Views must stay stored.)
- Session teardown: `pipeline_reset()` releases held images + clears `g_pending` on
  STOPPING so a restart doesn't present a stale composition over destroyed swapchains.
- Risk: medium-high (acquire/release pairing, holding an image across frames). If it
  misbehaves, set `g_pipelineActive` permanently 0 to fall back to the synchronous path.

## Other levers to try (cheaper, possibly complementary)
- **Cap supersample**: `src/layers.c ovrp_CalculateEyeLayerDesc2` — `if (textureScale>1) textureScale=1;`
  Tried, didn't fix ghosting alone, but frees GPU headroom; may help combined with pipelining.
- **Submit a depth layer** (`XR_KHR_composition_layer_depth`): better positional reprojection.
  `xrr_setup_layer_depth` currently returns Unsupported; would need a depth swapchain +
  `GetLayerTexture2` depth handles + `XrCompositionLayerDepthInfoKHR` on the projection.
- **Adjust predicted display time**: account for the flush-wait latency when filling
  `xrEndFrame.displayTime` so the compositor reprojects less. Hacky; secondary.

## Key files / functions
- `src/vk_session.c` — `xrr_vk_flush_image` (the flush+wait; pipelining changes the wait),
  `detect_ue_queue` (queue is family0/idx0), `xrr_vk_set_handles`.
- `src/xr_runtime.c` — `xrr_begin_frame`, `xrr_end_frame` (acquire/flush/release/compose),
  `make_app_space` (LOCAL_FLOOR), heartbeat/diag logs.
- `src/layers.c` — `ovrp_CalculateEyeLayerDesc2` (resolution/FOV), quad placement is in
  `xr_runtime.c` end_frame.
- Diagnostic logging still in (rate-limited, harmless): begin/end heartbeats, view poses,
  layer stage-vs-acquired mismatch, quad pose/size/flags, GetNodePose nodes. Strip before
  any public release.

## Verified facts to trust
- App's rendered frames are CLEAN (pulled headset video confirms) — the bug is display-time.
- Frame loop runs ~72fps with the flush-wait; the issue is per-frame reprojection from
  pose/time latency, not dropped framerate.
- Removing the flush-wait → black (runtime won't sync for us). The wait is mandatory in
  its current synchronous form; pipelining is how to keep it without the latency.
