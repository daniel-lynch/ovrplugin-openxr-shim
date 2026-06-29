# RE4 VR shim — docs

Research trail and session handoffs for the OVRPlugin→OpenXR shim that runs RE4 VR
on non-Meta OpenXR runtimes (Quest 2 today; Steam Frame the goal).

- **`handoffs/`** — chronological session handoffs (the day-by-day journey).
- **`research/`** — reverse-engineering notes, design docs, the ghost-fix writeup, and
  `related-work.md` (how Overport/others run Quest games elsewhere; why this shim is novel).
- `../analysis/` — raw RE dumps (`*.txt`, mostly gitignored; `all_exports.txt` +
  `shim_surface.txt` feed `shim/gen_stubs.sh`).
- Operational docs live at repo root: `README.md`, `TESTING.md`, `HOST.md`.

## The "ghost" — how it was actually solved (the short version)

For weeks the headline bug was a **VR "ghost"**: doubled/tripled hands and watches,
seated-mode deform, standing-mode black flashes. It survived every theory it *looked*
like — stereo geometry, FOV/IPD, depth reprojection, render-submit ordering, GPU load.

The break came from building **P4 passthru** (`research/ghost-fix-2026-06-27.md`):
running the real Meta `libOVRPlugin` *inside* our app proved native is flawless, so the
bug was in **our path**. Then always-on anomaly logging caught the real signature —
**dropped frames** — and a recording + frame-blending showed it was **whole-frame
temporal judder**, not a stereo/geometry artifact. Localizing the stall showed UE's
render thread blocking ~85–150ms during motion at only ~14ms of GPU work.

Root cause: the shim left `ovrp_WaitToBeginFrame` a **no-op** and ran `xrWaitFrame` on
the **render** thread, so the game thread was unpaced and UE's pipelined renderer
desynced → render-thread stalls → dropped frames → the compositor's timewarp filled the
gaps → judder. **Fix: pace the game thread like vrapi** (`xrWaitFrame` in
`xrr_wait_frame`, frameState handed to the render thread via a 1:1 FIFO ring). Result:
render stall 150→3ms, drops 0.79% (~native 0.34%), steady 72Hz — ghost gone.
Commits `79476c3` (fix) + `c0ecff3` (XR_FRAME_DISCARDED guard).

Secondary wins kept as defaults: game-driven FFR (apply the game's foveation), and the
per-frame luma readback off.

## Reading order if you're new

1. `research/ghost-fix-2026-06-27.md` — the full diagnosis chain (start here).
2. `handoffs/` newest → oldest — the journey, including the dead ends (reproject,
   submit-ordering, depth) that were ruled out.
3. `research/RECON.md`, `RE-NOTES.md`, `SHIM-SCOPE.md` — how the shim was reverse-engineered.
4. `research/render-submit-sync-design.md` — the render/submit sync design.
