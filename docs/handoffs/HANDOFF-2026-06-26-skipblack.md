# RE4 VR Shim — Handoff (2026-06-26, skipblack instrument session)

Continuation of `HANDOFF-2026-06-26.md` (morning). Branch `latency-perffix-copyring`.
This session built + ran **Lever 2 step 1 (instrument-only black-frame detector)** and
validated the signal is real but coarse. Detailed finding in auto-memory
`skipblack-posthitch-signal-validated`. Design: `analysis/lever2-detect-skip-black-handoff.md`.

## TL;DR — the post-hitch black signal is REAL but needs a tighter gate + frame-accurate proof
Added `debug.re4vr.skipblack 1` (instrument-only): after a HITCH (dt>20ms), flag the next
`skipblackn` frames as LIKELY-TRUNCATED and log them. NO behavior change at level 1. Ran on
device (standing, walk off bridge into house). 47 HITCHes / 89 flagged frames. Result:

- **The truncated/black frame has a signature: a SHORT (<12ms) frame right after a big hitch**
  — it finishes fast because UE rendered almost nothing into it (matches RenderDoc 28-vs-198
  draws → pure black).
- **Big hitches (≥40ms): 25, of which 52% have that <12ms truncated recovery frame.**
- **Mild hitches (20–40ms): 22, only 5%** — these recover to a full ~14ms frame = a 1-frame
  judder, NOT black. The current heuristic OVER-flags these.
- HITCH magnitudes span 20ms → 3.8s. The multi-hundred-ms / multi-second ones are
  **load/streaming stalls** (level transitions) — a different event from per-frame gameplay
  black drops. Must be separated from the gameplay black.
- Ground truth is still COARSE: user confirmed "black" happened, but not frame-accurate; the
  short-dt=black link rests on the single RenderDoc capture, not per-frame verified.

Raw frame/flag sequence persisted: `analysis/skipblack-2026-06-26/frame-sequence.log`.

## NEXT TASK — do BOTH (user decision this session):
### (3) Tighten the detector, then re-instrument
- **Gate the arm on hitch ≥40ms** (drops the 22 harmless mild hitches; ~halves false flags).
- **Add short-dt confirmation:** only treat a post-hitch frame as truncated if its own
  dt < ~12ms. Combines the two corroborating signals; cuts the ~48% of big hitches whose
  recovery wasn't short (those may be a different failure mode — investigate separately).
- **Handle sustained-overload clusters** (e.g. f=3162–3173: ~10 consecutive hitches). The
  fixed "next 2 frames" model is wrong there — the whole burst is bad. Consider: stay armed
  while consecutive frames keep hitching, not a fixed count.
- Re-run instrument-only and re-inspect (reuse the awk recipes from this session's analysis).
- Code is all in `shim/src/xr_runtime.c`: `skipblack_level()`/`skipblack_count()` (~205) and
  the flag block inside the FRAME/HITCH trace (~1186, look for `SKIPBLACK: LIKELY-TRUNCATED`).

### (2) Frame-accurate ground truth via Meta Cast (do alongside / before reproject)
- In-game recording is broken (empty mp4); display 0 is FLAG_SECURE so scrcpy/screenrecord
  can't see VR. Use **Meta Cast → desktop screen-record** while walking off the bridge.
- Run with `skipblack=1 skipblackn=2 trace=1`, capture logcat with timestamps in parallel.
- Align the video's visible black flashes to the `SKIPBLACK: LIKELY-TRUNCATED` log
  timestamps to PROVE (or refute) flag == perceived black, per frame. This is the
  `verify-before-theory` discipline — don't build the reproject on the coarse signal alone.

### THEN — Lever 2 step 2 (reproject, `debug.re4vr.skipblack 2`)
Only after (2)+(3). Re-present the **last-good** eye image with the current pose so the
OpenXR compositor timewarps it instead of showing black.
- **Use the COPY-ring, NOT a cross-frame swapchain hold** — holding across frames is what
  sank `pipeline=1` (see memory `deferred-flush-unstable-abandoned`). Copy last-good eye →
  shim VkImage (`xrr_vk_alloc_images`/`shimImages[]`, copy path in end_frame ~922), blit it
  into the acquired image on a flagged frame, submit the composition with the CURRENT pose.
- Low harm on false positives: reprojecting a stale frame ≈ what the compositor already does
  during the hitch, so over-flagging mild hitches is tolerable but wasteful — the ≥40ms gate
  keeps it clean.
- Success = black flashes replaced by (at worst) brief reprojection judder, not black.

## Build / deploy / test (unchanged)
```
./shim/build_android.sh && ./packaging/repack.sh      # repack bundles LAST build — check
                                                       # shim/build/arm64/libOVRPlugin.so timestamp
adb -s <redacted-serial> install -r packaging/out/re4vr-shim.apk
adb -s <redacted-serial> logcat | grep -iE "xrr|SKIPBLACK"
```
Launch is BLOCKED by the controllers-asleep system dialog
(`app_launch_blocked_controller_required`) — wake controllers + headset on before
`am start -n com.Armature.VR4/com.epicgames.ue4.GameActivity`.
Logcat is dominated by PerfMetrics polling — filter to `FRAME f=|HITCH|SKIPBLACK` for pacing.

## Props
- `debug.re4vr.skipblack 0|1|2` — 0 off / 1 instrument-only (built) / 2 reproject (TODO).
- `debug.re4vr.skipblackn N` — post-hitch frames to flag (default 2). Will change with the
  cluster-aware rewrite in (3).
- Reset to baseline now: skipblack=0, skipblackn=0, trace=0, ffr=2, resscale=100.

## State
- Code committed? **NO — `skipblack` detector is UNCOMMITTED** in the working tree
  (`shim/src/xr_runtime.c`). Commit it (instrument-only, gated off, safe) before further work.
- Branch `latency-perffix-copyring`, clean except the skipblack edit + the new analysis dir.
