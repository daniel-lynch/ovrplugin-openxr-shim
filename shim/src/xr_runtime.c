/* xr_runtime.c — OpenXR session + frame-loop engine. See xr_runtime.h.
 *
 * Full pipeline is live: instance/session creation, Vulkan graphics binding (from the
 * ovrp_Initialize5 handshake), per-eye swapchains, composition, and the frame loop.
 * Frame pacing runs on the GAME thread (xrWaitFrame in xrr_wait_frame) and hands the
 * frameState to the RENDER thread via the g_fsRing FIFO — see the frame-loop section.
 */
#include "xr_runtime.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

/* CLOCK_MONOTONIC ns — Meta's XrTime on Android is this clock, so we can compare
 * our submit moment directly against frameState.predictedDisplayTime. */
static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

XrRuntime g_xr;

/* Serializes all session/frame OpenXR calls. ovrp_WaitToBeginFrame/Update3 run on
 * the game thread, BeginFrame4/EndFrame4 on the render thread; without this the
 * session teardown (xrEndSession) races the frame loop and crashes the runtime. */
static pthread_mutex_t g_xrlock = PTHREAD_MUTEX_INITIALIZER;

/* Game-thread frame pacing (vrapi-style). UE's loop is pipelined: ovrp_WaitToBeginFrame(N) runs on
 * the GAME thread one frame ahead of ovrp_BeginFrame4/EndFrame4(N-1) on the RENDER thread (proven by
 * the FLOOP trace). OpenXR's recommended pipelined model is: xrWaitFrame on the sim/game thread
 * (it BLOCKS = paces the game, like vrapi), xrBeginFrame/xrEndFrame on the render thread. We hand the
 * xrWaitFrame'd frameState from the game thread to the render thread via this 1:1 FIFO ring. Pacing
 * the game thread (instead of our old no-op + xrWaitFrame-on-render-thread workaround) is what lets
 * UE's internal pipeline run smoothly -> stops the render-thread stalls that drop frames -> judder. */
#define FS_RING 8
static XrFrameState g_fsRing[FS_RING];
static unsigned     g_fsHead, g_fsTail;          /* head=pushed by wait, tail=popped by begin */
static pthread_cond_t g_fsCond = PTHREAD_COND_INITIALIZER;

/* Published per-frame view snapshot for the GAME-thread pose/time getters
 * (xrr_get_node_pose / xrr_eye_fov_tangents / xrr_predicted_display_time_s). The render thread
 * (single writer, begin_frame after xrLocateViews) publishes it; the game-thread getters read it.
 * A SEQLOCK, not a mutex: an A/B soak showed a per-frame mutex on these hot getters cost real
 * smoothness, so readers spin on a version counter instead (no syscall, no contention, no torn
 * read). g_poseSeq is even when stable, odd while the writer is mid-update. */
static volatile unsigned g_poseSeq;
static XrView   g_pubViews[2];
static uint32_t g_pubViewCount;
static XrTime   g_pubDisplayTime;

/* Lock-free snapshot of the published views (seqlock reader). Retries if it catches a write. */
static void pose_snapshot(XrView outViews[2], uint32_t *outCount, XrTime *outTime) {
    for (;;) {
        unsigned s1 = __atomic_load_n(&g_poseSeq, __ATOMIC_ACQUIRE);
        if (s1 & 1u) continue;                       /* writer mid-update -> retry */
        if (outViews) { outViews[0] = g_pubViews[0]; outViews[1] = g_pubViews[1]; }
        if (outCount) *outCount = g_pubViewCount;
        if (outTime)  *outTime  = g_pubDisplayTime;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);     /* data reads complete before re-check */
        if (__atomic_load_n(&g_poseSeq, __ATOMIC_RELAXED) == s1) return;  /* no write intervened */
    }
}

/* ----------------------------------------------- render-ahead pipeline ----- *
 * The tile-memory flush (vk_session.c) must complete before the compositor reads
 * the image, but the Meta runtime won't sync against our submit — so a synchronous
 * flush-wait inside end_frame pushes xrEndFrame past predictedDisplayTime and the
 * compositor reprojects every frame (= ghosting on head motion). Instead we render
 * one frame ahead: submit frame N's flush without waiting, present frame N-1's
 * image (whose flush finished during this frame's work) with its STORED views/time.
 * Net +1 frame latency (absorbed by normal reprojection), but xrEndFrame lands on
 * schedule. g_pending holds the composition built last frame, presented this frame.
 * Single-buffered: xrEndFrame consumes the layer structs synchronously, so we can
 * rebuild g_pending in place right after submitting it. submit[]/proj.views point
 * back into this struct's own (file-static, stable) arrays. */
typedef struct {
    int    valid;
    XrTime displayTime;
    XrCompositionLayerProjection       proj;
    XrCompositionLayerProjectionView   pviews[2];
    XrCompositionLayerDepthInfoKHR     pdepth[2];   /* chained on pviews when present */
    XrCompositionLayerQuad             quads[XRR_MAX_LAYERS];
    const XrCompositionLayerBaseHeader *submit[XRR_MAX_LAYERS];
    int    nLayers;
} PendingFrame;
static PendingFrame g_pending;
static int g_pipelineActive;   /* engaged once flush ring is up + layers double-buffered */
static int g_copyRingEngaged;  /* this frame is using the copy-ring path (drops quads) */
static double g_gpuFrameMs;    /* measured end-to-end frame dt (ms), fed to ovrp_GetGPUFrameTime */
static int g_postHitch;        /* Lever 2: >0 => this frame is a post-hitch LIKELY-TRUNCATED candidate (skipblack) */
static int g_frameLuma = -1;   /* Lever 2: max luma (0..255) of this frame's resolved eye image (lumagate) */
/* black-frame detection (debug.re4vr.blackcount / lumagate diagnostics) */
static int g_frameBlack;       /* this frame is a truncation black (luma<thr + abrupt onset) */
static int g_prevLuma = -1;    /* previous frame's luma (for abrupt-drop onset detection) */
static int g_reprojActive;     /* inside a truncation-black run (abrupt onset, not yet recovered) */
/* Lever B truncation counter: rolling count of bad-black eye frames (luma<thr && appLayers==1),
 * tagged with present mode, so we can MEASURE whether render-ahead reduces black vs sync. */
static long g_blackTotal;      /* bad-black eye frames since boot */
static long g_blackWindow;     /* bad-black eye frames in the current report window */
static long g_frameWindow;     /* eye frames in the current report window */
static int g_cpuPerfLevel = -1, g_gpuPerfLevel = -1;  /* last ovrp clock level the game requested (perf metrics 12/13) */
/* present-on-submit (debug.re4vr.submithook=2): end_frame stashed a composition and is
 * waiting for UE's eye-render vkQueueSubmit (via the hook) before releasing+presenting. */
static int     g_deferPresent;
static int     g_deferSubmits;     /* renderQ submits seen since this frame deferred */
static int64_t g_deferStartNs;
static int64_t g_lastEndFrameNs;   /* for the submit-vs-EndFrame instrumentation */
static long    g_submitSeq;        /* monotonic UE-submit counter (atomic) */
static int     g_frameSubmits;     /* UE vkQueueSubmits in the current end_frame window (atomic) */
static int     g_frameRenderSubmits; /* of those, render-queue submits — truncated frame => fewer */

/* present-on-submit: present the deferred frame after the Nth render-queue submit
 * following end_frame (the eye render is split across ~3 submits that all land before
 * the next END; presenting on the 1st was too early -> still black). Tunable to pin the
 * exact submit; default 3 = wait for the whole render cluster. debug.re4vr.defern. */
static int defer_submit_count(void) {
    int v = 3;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.defern", s) > 0) v = atoi(s);
#endif
    return v < 1 ? 1 : v;
}

/* Render-ahead is OFF by default (the synchronous flush-wait path is known-good and
 * playable; the pipeline regressed to black — Meta appears to mis-composite a
 * swapchain image held acquired across xrEndFrame). Opt in at runtime WITHOUT a
 * rebuild for A/B testing:  adb shell setprop debug.re4vr.pipeline 1  then relaunch.
 * Read once and cached. */
static int pipeline_wanted(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = 0;
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.pipeline", v) > 0 && v[0] == '1') cached = 1;
#endif
    XRRLOG("render-ahead pipeline: %s (debug.re4vr.pipeline)", cached ? "ENABLED" : "off (synchronous)");
    return cached;
}

/* Visual A/B probe for the stereo "dupe" (no rebuild — adb shell setprop
 * debug.re4vr.diag N; relaunch). 0=off (normal), 1=projection only (drop quads,
 * tests layer overlap), 2=force mono (both eyes sample array layer 0; if the dupe
 * disappears it's stereo divergence, if it persists it's a single-eye/temporal
 * ghost). Re-read each call so it can be flipped without relaunch where possible. */
static int diag_mode(void) {
    static int cached = -2;
    int v = cached;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.diag", s) > 0) v = atoi(s); else v = 0;
#else
    v = 0;
#endif
    if (v != cached) { cached = v; XRRLOG("diag_mode -> %d", v); }
    return cached < 0 ? 0 : cached;
}

/* Frame-lifecycle trace verbosity (debug.re4vr.trace, re-read each frame):
 *   0 = off (default; only the always-on starvation/hitch warnings fire)
 *   1 = one FRAME summary line per end_frame (frameIndex, shouldRender, layer counts,
 *       dt, late) — the per-frame pacing trace
 *   2 = + per-call WAIT/BEGIN and a per-layer submit dump (id/stage/flags/pose) — the
 *       "log everything going through the shim" mode for spotting an old-lib workaround
 * Independent of debug.re4vr.diag (visual A/B). */
static int trace_level(void) {
    static int cached = -2;
    int v = cached;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.trace", s) > 0) v = atoi(s); else v = 0;
#else
    v = 0;
#endif
    if (v != cached) { cached = v; XRRLOG("trace_level -> %d", v); }
    return cached < 0 ? 0 : cached;
}

/* Depth-submission toggle + tuning (read once, before any layer is created so the
 * depth swapchain decision is stable). debug.re4vr.depth=1 enables it. We don't
 * have UE's exact depth-projection params (they're in the un-reversed ovrpLayerSubmit
 * tail), so assume Quest-UE convention: reverse-Z, infinite far. Tunable on-device:
 *   debug.re4vr.depth_nearz_mm  (near plane in mm, default 100 = 0.1m)
 *   debug.re4vr.depth_revz      (1 = reverse-Z/infinite far [default], 0 = standard) */
static int g_haveDepthExt = 0;     /* XR_KHR_composition_layer_depth advertised (set in pre_init) */
static int   g_depthOn = -1;
static float g_depthNearZ = 0.1f;
static int   g_depthRevZ = 1;
static int depth_wanted(void) {
    if (g_depthOn >= 0) return g_depthOn;
    g_depthOn = 0;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.depth", s) > 0 && s[0] == '1') g_depthOn = 1;
    if (__system_property_get("debug.re4vr.depth_nearz_mm", s) > 0) {
        int mm = atoi(s); if (mm > 0) g_depthNearZ = (float)mm / 1000.0f;
    }
    if (__system_property_get("debug.re4vr.depth_revz", s) > 0) g_depthRevZ = (s[0] != '0');
#endif
    XRRLOG("depth: %s nearZ=%.3fm reverseZ=%d (ext=%d)",
           g_depthOn ? "ENABLED" : "off", g_depthNearZ, g_depthRevZ, g_haveDepthExt);
    return g_depthOn && g_haveDepthExt;
}

/* Copy-ring toggle (debug.re4vr.copyring=1): UE renders into shim images, we copy ->
 * OpenXR + pipeline the resolve so the CPU never blocks on the current frame's GPU
 * (breaks the flush-wait serialization that drops frames). Read once at layer setup. */
static int g_copyringOn = -1;
static int copyring_wanted(void) {
    if (g_copyringOn >= 0) return g_copyringOn;
    g_copyringOn = 0;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.copyring", s) > 0 && s[0] == '1') g_copyringOn = 1;
#endif
    XRRLOG("copy-ring: %s (debug.re4vr.copyring)", g_copyringOn ? "ENABLED" : "off");
    return g_copyringOn;
}

/* PERF test: skip the flush-wait (debug.re4vr.noflushwait=1). Re-read each frame. */
static int noflushwait_wanted(void) {
    int v = 0;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.noflushwait", s) > 0 && s[0] == '1') v = 1;
#endif
    return v;
}

/* Render-submit race fix (debug.re4vr.submithook, re-read each frame):
 *   0 = off (current synchronous present; default, unchanged path)
 *   1 = install the UE vkQueueSubmit hook + INSTRUMENT only (log submit-vs-EndFrame
 *       order, queue, fence; NO behavior change) — validates the hook + the threading
 *       interleave before we depend on it
 *   2 = present-on-submit: end_frame defers; the hook fires the present after UE's
 *       eye-render submit lands (orders present after render, no fixed latency) */
static int submithook_level(void) {
    int v = 0;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.submithook", s) > 0) v = atoi(s);
#endif
    return v < 0 ? 0 : v;
}

/* Lever 2 — detect dropped/black frames and (eventually) reproject last-good instead of
 * presenting UE's truncated/black frame (debug.re4vr.skipblack, re-read each frame):
 *   0 = off (default, unchanged path)
 *   1 = INSTRUMENT only: flag the recovery frame(s) after a HITCH as LIKELY-TRUNCATED and
 *       log them; NO behavior change. Validate these logs correlate with perceived black
 *       (walk off the bridge into the house, standing) before depending on the signal.
 *   2 = (future) reproject: re-present the last-good eye image on a flagged frame.
 * Root cause is settled: UE renders a truncated (~28 vs ~198 draws) black frame under load;
 * the truncated frame is the RECOVERY frame following a stall, which we already detect as a
 * dt>20ms HITCH. See analysis/lever2-detect-skip-black-handoff.md. */
static int skipblack_level(void) {
    int v = 0;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.skipblack", s) > 0) v = atoi(s);
#endif
    return v < 0 ? 0 : v;
}

/* How many frames after a HITCH to flag as candidate-truncated (debug.re4vr.skipblackn,
 * default 2 — RenderDoc showed the recovery frame is the bad one; 1-2 covers the window). */
static int skipblack_count(void) {
    int v = 2;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.skipblackn", s) > 0) { int n = atoi(s); if (n > 0) v = n; }
#endif
    return v;
}

/* Frame-counter barcode overlay (debug.re4vr.barcode=1): stamp the frameIndex as a
 * black/white barcode into the eye image each frame so a Meta Cast recording aligns
 * frame-exactly to the FRAME/SKIPBLACK logs (read bits on a black flash -> grep f=N).
 * Validation tool for Lever 2; re-read each frame, off by default. */
static int barcode_wanted(void) {
    int v = 0;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.barcode", s) > 0 && s[0] == '1') v = 1;
#endif
    return v;
}

/* Per-frame luminance black gate (debug.re4vr.lumagate=1): after the resolve, sample a few
 * rows of the eye image; max luma ~0 => this frame is truncated/black. Covers the sustained
 * black tail that the onset-only post-hitch flag misses. Re-read each frame; off by default
 * (adds a small readback submit/wait per frame). */
static int lumagate_wanted(void) {
    int v = 0;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.lumagate", s) > 0 && s[0] == '1') v = 1;
#endif
    return v;
}
/* black threshold for the luma gate (debug.re4vr.lumathr, default 12): max-luma below this
 * = black. Tunable while we calibrate against the recording. */
static int lumagate_thr(void) {
    int v = 12;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.lumathr", s) > 0) { int n = atoi(s); if (n >= 0) v = n; }
#endif
    return v;
}

/* (Lever 2 reproject + Lever C depth-hold were removed — they were a black-frame mitigation,
 * not the eye-ghost fix; the ghost was a frame-pacing bug, fixed in the frame loop. The luma
 * read + blackcount/lumagate black-detection diagnostics below are kept.) */
/* Min one-frame luma drop to treat a black onset as a TRUNCATION (debug.re4vr.abruptdrop).
 * DEFAULT 0 = reproject ALL black (proven no-flash behavior). Raise it (e.g. 25) to skip
 * gradual fades (comfort/scene/load ramp ~5-15/frame): truncation snaps lit->black (drop
 * 50-77) so it still triggers, but the bigger the value the more shallow truncations are
 * missed (black returns) — a fragile balance, hence opt-in. */
static int abruptdrop(void) {
    int v = 0;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.abruptdrop", s) > 0) { int n = atoi(s); if (n >= 0) v = n; }
#endif
    return v;
}
/* Lever B: always-on truncation-black counter (debug.re4vr.blackcount, default 1). Counts the
 * per-frame bad-black verdict so we can compare black/min between sync and render-ahead. Cheap
 * (just needs the luma read, which it forces on if nothing else wants it). */
static int blackcount_wanted(void) {
    int v = 0;   /* default OFF: the per-frame luma readback (GPU copy+fence) is shipping overhead;
                  * opt in for black-frame measurement with debug.re4vr.blackcount=1. */
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.blackcount", s) > 0 && s[0] == '1') v = 1;
#endif
    return v;
}
/* Lever B: engage the render-ahead (deferred-flush) pipeline (debug.re4vr.renderahead, default 0).
 * Kept separate from the legacy debug.re4vr.pipeline escape hatch so the old path stays available
 * for comparison. Gives UE a full extra frame of GPU budget before we resolve -> fewer/shorter
 * truncations; reproject+luma ride along as the safety net for any residual black. */
static int renderahead_wanted(void) {
    int v = 0;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.renderahead", s) > 0 && s[0] == '1') v = 1;
#endif
    return v;
}
/* Layer z-order fix (debug.re4vr.eyebottom, default ON). OVRPlugin/VrApi treat the eye-fov as the
 * BASE world layer regardless of the order the app submits layers in; OpenXR composites STRICTLY in
 * array order (last = on top). RE4's splash/logo path submits the logo QUAD before the eye-fov, so
 * without reordering our opaque eye-fov projection draws ON TOP of the logo quad -> black logos.
 * Reorder so projection layers sit at the bottom, quads on top — matching native. */
static int eyebottom_wanted(void) {
    int v = 1;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.eyebottom", s) > 0 && s[0] == '0') v = 0;
#endif
    return v;
}

/* DIAG probe (debug.re4vr.qwait=1): vkQueueWaitIdle on UE's queue before we resolve/
 * present, to test the render-submit race (black under load). Re-read each frame. */
static int qwait_wanted(void) {
    int v = 0;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.qwait", s) > 0 && s[0] == '1') v = 1;
#endif
    return v;
}

/* Texture readback. Setting debug.re4vr.dump=N captures a BURST of the next N consecutive
 * frames (intermittent/flashing artifacts need many frames to catch). Returns the burst frame
 * index 1..N while a burst is in progress (used to frame-number the dump files), else 0. */
static int dump_pending(void) {
    static int last = 0, remaining = 0, seq = 0; int v = last;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.dump", s) > 0) v = atoi(s); else v = 0;
#endif
    if (v != last && v != 0) { last = v; remaining = v; seq = 0; XRRLOG("dump trigger: burst %d frames", v); }
    else last = v;
    if (remaining > 0) { remaining--; return ++seq; }
    return 0;
}

/* Drop any in-flight pipelined state and release held images (session teardown).
 * Caller must hold g_xrlock and the session must still be valid. */
static void pipeline_reset(void) {
    for (int i = 0; i < g_xr.layerCount; i++) {
        XrLayer *L = &g_xr.layers[i];
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        if (L->presentPending) { xrReleaseSwapchainImage(L->swapchain, &ri); L->presentPending = 0; }
        if (L->imageAcquired)  { xrReleaseSwapchainImage(L->swapchain, &ri); L->imageAcquired = 0; }
    }
    g_pending.valid = 0;
    g_pipelineActive = 0;
}

#define XR_OK(x)  (XR_SUCCEEDED(x))
static int xrfail(const char *what, XrResult r) {
    if (XR_SUCCEEDED(r)) return 0;
    XRRERR("%s failed: XrResult=%d", what, (int)r);
    return 1;
}

/* Tracking origin. FLOOR (LOCAL_FLOOR) gives correct head-height-above-floor so
 * body-anchored inventory lines up; EYE (LOCAL) is head-height origin. Default to
 * eye level (the original working seated behaviour) and let the game switch to
 * floor via SetTrackingOriginType2 when it wants roomscale/standing. */
static int g_floorOrigin = 0;
static int g_haveLocalFloor = 0;   /* set in pre_init from ext_supported() */
static int g_havePerfExt = 0;      /* XR_EXT_performance_settings advertised */
static int g_haveFoveation = 0;    /* XR_FB_foveation (+config+swapchain_update_state) advertised */
static int g_haveEyeTrackedFov = 0;/* XR_META_foveation_eye_tracked advertised (Steam Frame/Quest Pro) */

static void make_app_space(int floor) {
    if (g_xr.appSpace != XR_NULL_HANDLE) { xrDestroySpace(g_xr.appSpace); g_xr.appSpace = XR_NULL_HANDLE; }
    XrReferenceSpaceCreateInfo ci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    ci.poseInReferenceSpace.orientation.w = 1.0f;

    /* Floor (LOCAL_FLOOR) when the game asks (RE4 standing) so head height above
     * the floor is correct; eye level (LOCAL) otherwise. The earlier "floor ->
     * black" was a red herring (a FOV mismatch, since fixed), not the origin. */
    if (floor && g_haveLocalFloor) {
        ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
        if (xrCreateReferenceSpace(g_xr.session, &ci, &g_xr.appSpace) == XR_SUCCESS) {
            XRRLOG("app space: LOCAL_FLOOR (floor, player-centred)");
            g_floorOrigin = floor; return;
        }
        XRRLOG("app space: LOCAL_FLOOR create failed, falling back to LOCAL");
    }
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    xrCreateReferenceSpace(g_xr.session, &ci, &g_xr.appSpace);
    XRRLOG("app space: LOCAL (eye level) [requested floor=%d]", floor);
    g_floorOrigin = floor;
}

void xrr_set_tracking_origin(int floor) {
    pthread_mutex_lock(&g_xrlock);
    if (g_xr.session != XR_NULL_HANDLE) make_app_space(floor);
    else g_floorOrigin = floor;
    pthread_mutex_unlock(&g_xrlock);
}
int xrr_get_tracking_origin(void) { return g_floorOrigin; }

/* Map the game's ovrp CPU/GPU perf level (~0-4) to an OpenXR perf-settings level and
 * apply it via XR_EXT_performance_settings, so the device boosts clocks under load
 * (the game was requesting this; we used to no-op it). isGpu=0 -> CPU domain. */
void xrr_set_perf_level(int isGpu, int level) {
    static union { PFN_xrPerfSettingsSetPerformanceLevelEXT f; PFN_xrVoidFunction v; } set = {0};
    static int loaded = 0, lastCpu = -99, lastGpu = -99;
    /* Record the game's requested clock level (its intent) up front, so perf metrics
     * 12/13 report it even if we can't forward to XR_EXT_performance_settings. */
    if (isGpu) g_gpuPerfLevel = level; else g_cpuPerfLevel = level;
    if (!g_havePerfExt || g_xr.session == XR_NULL_HANDLE) return;
    if (!loaded) {
        loaded = 1;
        xrGetInstanceProcAddr(g_xr.instance, "xrPerfSettingsSetPerformanceLevelEXT", &set.v);
    }
    if (!set.f) return;
    int *last = isGpu ? &lastGpu : &lastCpu;
    if (level == *last) return;          /* only call on change */
    *last = level;
    XrPerfSettingsLevelEXT lvl = (level <= 0) ? XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT
                               : (level == 1) ? XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT
                               : (level == 2) ? XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT
                                              : XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
    XrPerfSettingsDomainEXT dom = isGpu ? XR_PERF_SETTINGS_DOMAIN_GPU_EXT
                                        : XR_PERF_SETTINGS_DOMAIN_CPU_EXT;
    pthread_mutex_lock(&g_xrlock);
    XrResult r = set.f(g_xr.session, dom, lvl);
    pthread_mutex_unlock(&g_xrlock);
    XRRLOG("perf level: %s ovrp=%d -> xr=%d rc=%d", isGpu ? "GPU" : "CPU", level, (int)lvl, (int)r);
}

/* Is an instance extension advertised by the runtime? (query before enabling) */
static int ext_supported(const char *name) {
    uint32_t n = 0;
    if (xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL) != XR_SUCCESS || n == 0) return 0;
    XrExtensionProperties *props = calloc(n, sizeof(*props));
    if (!props) return 0;
    for (uint32_t i = 0; i < n; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    int found = 0;
    if (xrEnumerateInstanceExtensionProperties(NULL, n, &n, props) == XR_SUCCESS)
        for (uint32_t i = 0; i < n; i++)
            if (strcmp(props[i].extensionName, name) == 0) { found = 1; break; }
    free(props);
    return found;
}

/* ----------------------------------------------------------- lifecycle ----- */
ovrpResult xrr_pre_init(void) {
    if (g_xr.instance != XR_NULL_HANDLE) return ovrpSuccess;
    memset(&g_xr, 0, sizeof(g_xr));
    g_xr.viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

    /* Build the extension list. Vulkan binding is what RE4 needs (VulkanRHI);
     * on Android we also prime the loader and chain the Android create info. */
    const char *exts[12];
    uint32_t ne = 0;
    exts[ne++] = "XR_KHR_vulkan_enable";   /* v1: app creates VkInstance/Device */
    void *instNext = NULL;
#ifdef __ANDROID__
    xrr_android_init_loader();                       /* xrInitializeLoaderKHR */
    exts[ne++] = "XR_KHR_android_create_instance";
    instNext = xrr_android_instance_next();          /* JavaVM + activity     */
#endif
    /* LOCAL_FLOOR = player-centred origin (same x/z + facing as you) dropped to
     * the floor. Roomscale games need floor height; unlike STAGE this doesn't
     * reorient to the Guardian, so it won't reverse movement. */
    g_haveLocalFloor = ext_supported("XR_EXT_local_floor");
    if (g_haveLocalFloor) exts[ne++] = "XR_EXT_local_floor";
    /* Submit per-eye depth so the compositor can do POSITIONAL reprojection
     * (XR_KHR_composition_layer_depth). Without it, head translation can't be
     * reprojected -> the scene ghosts during head motion (artifact B). Enabling
     * the ext is harmless; the depth swapchain/submit is gated by debug.re4vr.depth. */
    g_haveDepthExt = ext_supported(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    if (g_haveDepthExt) exts[ne++] = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;
    /* Honor the game's CPU/GPU perf-level requests (it calls ovrp_SetSystemCpu/GpuLevel2,
     * which we used to no-op) so the device boosts clocks under load instead of
     * sagging while CPU-bound -> fewer frame drops. */
    g_havePerfExt = ext_supported(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (g_havePerfExt) exts[ne++] = XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME;
    /* Fixed Foveated Rendering: the game asks for foveation (ovrp_GetLayerTextureFoveation,
     * which we stub) — instead we apply runtime FFR to the eye swapchain, cutting periphery
     * shading load (the GPU-bound black/stutter). Needs FB_foveation + its level profiles +
     * swapchain_update_state to apply. Other HMDs (Steam Frame) will expose different
     * foveation exts; apply_foveation() is the single extension point to add them. */
    g_haveFoveation = ext_supported(XR_FB_FOVEATION_EXTENSION_NAME)
                   && ext_supported(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME)
                   && ext_supported(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
    if (g_haveFoveation) {
        exts[ne++] = XR_FB_FOVEATION_EXTENSION_NAME;
        exts[ne++] = XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME;
        exts[ne++] = XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME;
        /* Eye-tracked foveation (Steam Frame / Quest Pro): extends the FB framework so the
         * runtime moves the high-res region with the user's gaze. Optional upgrade on top of
         * fixed FFR — request only if advertised; without it we still get fixed FFR. The game
         * (RE4) only asks for fixed TiledMultiRes; this transparently upgrades it to gaze-driven
         * where the runtime supports it. (Absent on stock/upstream Monado; present on SteamVR/
         * Lepton and Quest Pro.) */
        g_haveEyeTrackedFov = ext_supported(XR_META_FOVEATION_EYE_TRACKED_EXTENSION_NAME);
        if (g_haveEyeTrackedFov) exts[ne++] = XR_META_FOVEATION_EYE_TRACKED_EXTENSION_NAME;
    }
    XRRLOG("foveation: FB_foveation=%d eye_tracked(META)=%d", g_haveFoveation, g_haveEyeTrackedFov);
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    ici.next = instNext;
    strcpy(ici.applicationInfo.applicationName, "RE4VR-OVRPlugin-shim");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = ne;
    ici.enabledExtensionNames = exts;
    XRRLOG("pre_init: xrCreateInstance (exts=%u, android=%d)", ne, instNext != NULL);
    if (xrfail("xrCreateInstance", xrCreateInstance(&ici, &g_xr.instance)))
        return ovrpFailure_OperationFailed;

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (xrfail("xrGetSystem", xrGetSystem(g_xr.instance, &sgi, &g_xr.systemId)))
        return ovrpFailure_OperationFailed;
    XRRLOG("pre_init OK: instance=%p systemId=%llu", (void *)g_xr.instance,
           (unsigned long long)g_xr.systemId);
    return ovrpSuccess;
}

ovrpResult xrr_init(void *vkInstance, void *vkPhysicalDevice, void *vkDevice,
                    unsigned int queueFamilyIndex) {
    /* The instance was created in PreInitialize3 with the Application context
     * (the real Activity isn't available until here). Meta's runtime needs the
     * real Activity to drive the session IDLE->READY transition, so recreate the
     * instance with it before creating the session. */
    static int s_recreated = 0;
    if (!s_recreated && xrr_android_have_real_activity() &&
        g_xr.instance != XR_NULL_HANDLE && g_xr.session == XR_NULL_HANDLE) {
        s_recreated = 1;
        XRRLOG("init: recreating XrInstance with the real Activity");
        xrDestroyInstance(g_xr.instance);
        g_xr.instance  = XR_NULL_HANDLE;
        g_xr.systemId  = XR_NULL_SYSTEM_ID;
    }
    if (g_xr.instance == XR_NULL_HANDLE) {
        ovrpResult r = xrr_pre_init();
        if (!OVRP_SUCCESS(r)) return r;
    }
    /* Build the Vulkan graphics binding from the app's handles and create the
     * session (vk_session.c). ABI of the handle args is [VERIFIED] from the real
     * ovrp_Initialize5 impl. [ANDROID-TODO] xrCreateInstance also needs the JavaVM
     * + activity (Initialize5 args 2 & 4) via XrInstanceCreateInfoAndroidKHR on the
     * NDK build — captured separately. */
    XRRLOG("init: vkInstance=%p vkPhys=%p vkDevice=%p qfi=%u",
           vkInstance, vkPhysicalDevice, vkDevice, queueFamilyIndex);
    if (g_xr.session == XR_NULL_HANDLE) {
        if (!xrr_create_session_vulkan(vkInstance, vkPhysicalDevice, vkDevice,
                                       queueFamilyIndex))
            return ovrpFailure_OperationFailed;
    }
    XRRLOG("init OK: session=%p", (void *)g_xr.session);

    make_app_space(g_floorOrigin);   /* floor-level (STAGE) by default */
    XrReferenceSpaceCreateInfo view = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    view.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    view.poseInReferenceSpace.orientation.w = 1.0f;
    xrCreateReferenceSpace(g_xr.session, &view, &g_xr.viewSpace);

    xrr_input_init();   /* action set must be attached before first xrSyncActions */
    return ovrpSuccess;
}

void xrr_shutdown(void) {
    /* Tear down under g_xrlock so we don't free the session/instance out from under the render
     * thread (begin/end) or the game-thread getters, which all take the lock. Set running=0 +
     * broadcast first so a render thread parked in the frameState cond-wait exits promptly.
     * (Narrow residual: xrWaitFrame runs lock-free on the game thread for pacing, so a shutdown
     * in its ~ns window after the running check could still race the destroy — acceptable for an
     * app-exit-only path.) */
    /* Graceful exit handshake. xrEndSession is only legal in the STOPPING state, so we
     * can't just call it — we ask the runtime to exit (xrRequestExitSession), then pump
     * events until it transitions RUNNING -> ... -> STOPPING; poll_events() owns the
     * xrEndSession on that transition (and closes any in-flight frame first). Done BEFORE
     * taking g_xrlock for the destroy, since poll_events() takes the lock itself. Bounded
     * (~1s) so a runtime that never transitions can't hang app exit; the destroy then
     * proceeds regardless (xrDestroySession is valid from any state). */
    pthread_mutex_lock(&g_xrlock);
    int running = g_xr.running && g_xr.session != XR_NULL_HANDLE;
    pthread_mutex_unlock(&g_xrlock);
    if (running && !xrfail("xrRequestExitSession", xrRequestExitSession(g_xr.session))) {
        for (int i = 0; i < 200; i++) {                  /* 200 * 5ms = ~1s cap */
            xrr_poll_events();                           /* STOPPING -> xrEndSession + running=0 */
            pthread_mutex_lock(&g_xrlock);
            int stopped = !g_xr.running;
            pthread_mutex_unlock(&g_xrlock);
            if (stopped) break;
            nanosleep(&(struct timespec){ 0, 5 * 1000 * 1000 }, NULL);
        }
    }

    pthread_mutex_lock(&g_xrlock);
    g_xr.running = 0;
    pthread_cond_broadcast(&g_fsCond);
    /* poll_events already issued xrEndSession on STOPPING; destroy is valid from any state. */
    if (g_xr.session)  xrDestroySession(g_xr.session);
    if (g_xr.instance) xrDestroyInstance(g_xr.instance);
    xrr_vk_teardown();   /* free shim-owned Vulkan objects + allow clean re-init */
    memset(&g_xr, 0, sizeof(g_xr));
    pthread_mutex_unlock(&g_xrlock);
}

/* ---------------------------------------------------- session state machine */
void xrr_poll_events(void) {
    if (g_xr.instance == XR_NULL_HANDLE) return;
    pthread_mutex_lock(&g_xrlock);
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(g_xr.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *s = (XrEventDataSessionStateChanged *)&ev;
            g_xr.sessionState = s->state;
            static const char *snames[] = {"UNKNOWN","IDLE","READY","SYNCHRONIZED",
                "VISIBLE","FOCUSED","STOPPING","LOSS_PENDING","EXITING"};
            int si = (int)s->state;
            XRRLOG("session state -> %d (%s)", si,
                   (si >= 0 && si <= 8) ? snames[si] : "?");
            if (s->state == XR_SESSION_STATE_READY && !g_xr.running) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = g_xr.viewConfigType;
                if (!xrfail("xrBeginSession", xrBeginSession(g_xr.session, &bi))) {
                    g_xr.running = 1;
                    XRRLOG("xrBeginSession OK -> running");
                }
            } else if (s->state == XR_SESSION_STATE_STOPPING && g_xr.running) {
                /* close any in-flight frame first — xrEndSession while a frame is
                 * open crashes the runtime. */
                if (g_xr.inFrame) {
                    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
                    ei.displayTime = g_xr.frameState.predictedDisplayTime;
                    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    xrEndFrame(g_xr.session, &ei);
                    g_xr.inFrame = 0;
                }
                pipeline_reset();   /* release held images, drop stale composition */
                xrEndSession(g_xr.session);
                g_xr.running = 0;
            }
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
    pthread_mutex_unlock(&g_xrlock);
}

/* ----------------------------------------------------------- frame loop ---- */
/* Game-thread pacing (OpenXR's recommended pipelined model, like vrapi): xrWaitFrame runs
 * here on the GAME thread (ovrp_WaitToBeginFrame) and BLOCKS to pace it; the frameState is
 * handed to the RENDER thread (ovrp_BeginFrame4/EndFrame4) via the g_fsRing FIFO. The render
 * thread pops it in xrr_begin_frame instead of calling xrWaitFrame. This freed the render
 * thread from pacing back-pressure and fixed the dropped-frame judder. wait/begin stay 1:1. */
ovrpResult xrr_wait_frame(int frameIndex) {
    if (trace_level() >= 2) XRRLOG("WAIT f=%d", frameIndex);
    xrr_poll_events();
    pthread_mutex_lock(&g_xrlock);
    int ready = g_xr.running && g_xr.session != XR_NULL_HANDLE;
    pthread_mutex_unlock(&g_xrlock);
    if (!ready) return ovrpFailure_NotYetImplemented;

    /* xrWaitFrame paces the GAME thread (vrapi model) and is NOT held under g_xrlock, so it never
     * blocks the render thread's begin/end. The runtime synchronizes wait<->begin 1:1; we hand the
     * resulting frameState to the render thread via the FIFO ring. */
    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    if (xrfail("xrWaitFrame", xrWaitFrame(g_xr.session, &fwi, &fs)))
        return ovrpFailure_OperationFailed;

    pthread_mutex_lock(&g_xrlock);
    if (g_fsHead - g_fsTail >= FS_RING) g_fsTail++;   /* drop oldest if render thread fell behind */
    g_fsRing[g_fsHead % FS_RING] = fs;
    g_fsHead++;
    pthread_cond_signal(&g_fsCond);
    pthread_mutex_unlock(&g_xrlock);
    return ovrpSuccess;
}

ovrpResult xrr_begin_frame(int frameIndex) {
    if (trace_level() >= 2) XRRLOG("BEGIN f=%d", frameIndex);
    xrr_poll_events();                       /* locks internally */
    pthread_mutex_lock(&g_xrlock);
    if (!g_xr.running || g_xr.inFrame) {
        static int rj = 0;
        if (rj++ < 60 || (rj % 240) == 0)
            XRRLOG("begin_frame REJECT: running=%d inFrame=%d (count=%d)",
                   g_xr.running, g_xr.inFrame, rj);
        pthread_mutex_unlock(&g_xrlock);
        return ovrpFailure_NotYetImplemented;
    }

    /* Take the frameState from the matching xrWaitFrame done on the GAME thread (FIFO, 1:1). The
     * game leads by ~1 frame so it's normally already queued; wait briefly otherwise. xrWaitFrame is
     * NO LONGER called here — pacing now blocks the game thread (vrapi model), freeing this render
     * thread from pacing back-pressure that was stalling UE's render -> dropped frames -> judder. */
    while (g_fsHead == g_fsTail && g_xr.running) {
        struct timespec to; clock_gettime(CLOCK_REALTIME, &to);
        to.tv_nsec += 20L * 1000000L;
        if (to.tv_nsec >= 1000000000L) { to.tv_sec++; to.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_fsCond, &g_xrlock, &to);
    }
    if (g_fsHead == g_fsTail) {   /* shutting down or game produced no wait */
        pthread_mutex_unlock(&g_xrlock); return ovrpFailure_NotYetImplemented;
    }
    g_xr.frameState = g_fsRing[g_fsTail % FS_RING];
    g_fsTail++;

    XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
    vli.viewConfigurationType = g_xr.viewConfigType;
    vli.displayTime = g_xr.frameState.predictedDisplayTime;
    vli.space = g_xr.appSpace;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    for (int i = 0; i < 2; i++) g_xr.views[i] = (XrView){ XR_TYPE_VIEW };
    xrLocateViews(g_xr.session, &vli, &vs, 2, &g_xr.viewCount, g_xr.views);
    /* publish the view snapshot for the game-thread getters (seqlock writer; single writer = us) */
    {
        unsigned s = g_poseSeq;
        __atomic_store_n(&g_poseSeq, s + 1, __ATOMIC_RELEASE);   /* odd: write in progress */
        g_pubViews[0] = g_xr.views[0]; g_pubViews[1] = g_xr.views[1];
        g_pubViewCount = g_xr.viewCount; g_pubDisplayTime = g_xr.frameState.predictedDisplayTime;
        __atomic_store_n(&g_poseSeq, s + 2, __ATOMIC_RELEASE);   /* even: stable */
    }
    {   /* DIAG: is the head pose UE renders from (xrLocateSpace VIEW, node=Head)
         * consistent with the eye poses we submit (xrLocateViews)? UE builds its
         * cameras from Head; we composite with the raw eye views. If Head != eye
         * midpoint, or eye orientation != head orientation, UE renders one camera
         * and the compositor reprojects to another => ghost/double. */
        static int hv = 0;
        if ((hv++ % 120) == 0 && g_xr.viewCount == 2) {
            XrSpaceLocation h = { XR_TYPE_SPACE_LOCATION };
            xrLocateSpace(g_xr.viewSpace, g_xr.appSpace,
                          g_xr.frameState.predictedDisplayTime, &h);
            XrVector3f e0 = g_xr.views[0].pose.position, e1 = g_xr.views[1].pose.position;
            XrQuaternionf hq = h.pose.orientation, q0 = g_xr.views[0].pose.orientation,
                          q1 = g_xr.views[1].pose.orientation;
            XRRLOG("HEADvsEYE: head.pos=(%.3f,%.3f,%.3f) eyeMid=(%.3f,%.3f,%.3f) "
                   "head.q=(%.3f,%.3f,%.3f,%.3f) e0.q=(%.3f,%.3f,%.3f,%.3f) e1.q=(%.3f,%.3f,%.3f,%.3f)",
                   h.pose.position.x, h.pose.position.y, h.pose.position.z,
                   (e0.x+e1.x)*0.5f, (e0.y+e1.y)*0.5f, (e0.z+e1.z)*0.5f,
                   hq.x,hq.y,hq.z,hq.w, q0.x,q0.y,q0.z,q0.w, q1.x,q1.y,q1.z,q1.w);
        }
    }
    {   /* DIAG: stereo sanity — both eye poses + IPD. Bad IPD/identical eyes ->
         * double vision / "duped at a different spot". */
        static int vc = 0;
        if ((vc++ % 240) == 0) {
            XrPosef *l = &g_xr.views[0].pose, *r = &g_xr.views[1].pose;
            float ipd = sqrtf((l->position.x-r->position.x)*(l->position.x-r->position.x)
                            + (l->position.y-r->position.y)*(l->position.y-r->position.y)
                            + (l->position.z-r->position.z)*(l->position.z-r->position.z));
            XRRLOG("views flags=0x%x L=(%.3f,%.3f,%.3f) R=(%.3f,%.3f,%.3f) IPD=%.3f viewCount=%u",
                   (unsigned)vs.viewStateFlags, l->position.x,l->position.y,l->position.z,
                   r->position.x,r->position.y,r->position.z, ipd, g_xr.viewCount);
        }
    }

    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrResult bfr = xrBeginFrame(g_xr.session, &bi);
    if (bfr == XR_FRAME_DISCARDED) {   /* our wait/begin pairing slipped — count it (should be 0) */
        static long d = 0; XRRLOG("OUR xrBeginFrame XR_FRAME_DISCARDED (#%ld)", ++d);
    } else if (xrfail("xrBeginFrame", bfr)) {
        pthread_mutex_unlock(&g_xrlock); return ovrpFailure_OperationFailed;
    }
    g_xr.inFrame = 1;

    xrr_input_sync();   /* refresh controller state + hand poses for this frame */

    /* acquire+wait this frame's image for each layer */
    for (int i = 0; i < g_xr.layerCount; i++) {
        XrLayer *L = &g_xr.layers[i];
        if (!L->active || L->swapchain == XR_NULL_HANDLE || L->imageAcquired) continue;
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrResult ar = xrAcquireSwapchainImage(L->swapchain, &ai, &L->acquiredIndex);
        {   /* DIAG: index sequence + any acquire failure (e.g. second acquire while
             * the pipeline holds an image -> would explain black). */
            static int ac = 0;
            if (ac++ < 24) XRRLOG("pipe: acquire layer=%d rc=%d acquiredIndex=%u presentPending=%d imgCount=%u",
                                  i, (int)ar, L->acquiredIndex, L->presentPending, L->imageCount);
        }
        if (ar != XR_SUCCESS)
            continue;
        XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wi.timeout = 100000000; /* 100 ms — never block the render thread forever */
        XrResult wr = xrWaitSwapchainImage(L->swapchain, &wi);
        if (wr == XR_TIMEOUT_EXPIRED) {
            /* couldn't get the image in time: release the acquire so we stay
             * balanced and skip this layer this frame rather than deadlock. */
            static int tw = 0;
            if (tw++ < 40) XRRLOG("WaitSwapchainImage TIMEOUT layer=%d idx=%u", i, L->acquiredIndex);
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(L->swapchain, &ri);
            L->imageAcquired = 0;
            continue;
        }
        L->imageAcquired = 1;
        /* acquire+wait the paired depth image (lockstep with color) so UE renders
         * depth into it; released together in end_frame. Depth only runs in the
         * synchronous path (the pipeline holds images and doesn't manage depth). */
        L->depthAcquired = 0;
        if (L->depthSwapchain != XR_NULL_HANDLE && !g_pipelineActive) {
            XrSwapchainImageAcquireInfo dai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            if (xrAcquireSwapchainImage(L->depthSwapchain, &dai, &L->depthAcquiredIndex) == XR_SUCCESS) {
                XrSwapchainImageWaitInfo dwi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                dwi.timeout = 100000000;
                if (xrWaitSwapchainImage(L->depthSwapchain, &dwi) == XR_TIMEOUT_EXPIRED) {
                    XrSwapchainImageReleaseInfo dri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                    xrReleaseSwapchainImage(L->depthSwapchain, &dri);   /* stay balanced */
                } else {
                    L->depthAcquired = 1;
                }
            }
        }
    }
    static int hb = 0;
    if ((hb++ % 240) == 0)
        XRRLOG("begin_frame heartbeat #%d shouldRender=%d layers=%d",
               hb, g_xr.frameState.shouldRender, g_xr.layerCount);
    pthread_mutex_unlock(&g_xrlock);
    return ovrpSuccess;
}

/* Build the composition (projection + quads) the app submitted into `pf`, using
 * the views/time current at call time. Captured at frame N, presented at frame N+1
 * so the stored views/time stay matched to the image they were rendered for (the
 * pipeline must never pair an image with a different frame's poses -> reprojection
 * to the wrong frustum). proj.views/submit[] point into pf's own arrays. */
static void build_composition(PendingFrame *pf,
        const ovrpLayerSubmit *const *layers, int layerCount) {
    int diag = diag_mode();
    pf->nLayers = 0;
    pf->displayTime = g_xr.frameState.predictedDisplayTime;
    pf->valid = 1;
    if (!layers || !g_xr.frameState.shouldRender) {
        /* STARVATION: this frame will composite NOTHING -> compositor shows black
         * (or reprojects). The title black-strobe / menu smear live here. Always
         * log (capped), or every time under trace. */
        static int ce = 0;
        if (ce++ < 300 || trace_level())
            XRRLOG("COMPOSE-EMPTY: layers=%p layerCount=%d shouldRender=%d -> BLACK frame",
                   (const void *)layers, layerCount, g_xr.frameState.shouldRender);
        return;
    }

    for (int k = 0; k < layerCount && pf->nLayers < XRR_MAX_LAYERS; k++) {
        const ovrpLayerSubmit *s = layers[k];
        if (!s || s->LayerId < 0 || s->LayerId >= g_xr.layerCount) continue;
        XrLayer *L = &g_xr.layers[s->LayerId];
        if (!L->active || L->swapchain == XR_NULL_HANDLE) continue;
        /* projection only: drop quads (diag=1, or copy-ring mode which is eye-only) */
        if ((diag == 1 || g_copyRingEngaged) && !L->isEyeFov) continue;

        if (L->isEyeFov) {
            /* DIAG: the game rendered into colorImages[TextureStage]; we acquire one
             * image per frame in lockstep, so acquiredIndex must equal TextureStage.
             * The render-ahead pipeline keeps this 1:1 cadence (one acquire + one
             * release per frame), just offset by a frame. */
            static int mm = 0, ok = 0;
            if ((uint32_t)s->TextureStage != L->acquiredIndex) {
                if (mm++ < 40)
                    XRRLOG("LAYER MISMATCH: id=%d submitStage=%d acquiredIndex=%u imgCount=%u",
                           s->LayerId, s->TextureStage, L->acquiredIndex, L->imageCount);
            } else if (ok++ < 5) {
                XRRLOG("layer ok: id=%d stage==acquired=%u", s->LayerId, L->acquiredIndex);
            }
            {   /* DIAG: does UE submit a sub-full ViewportRect (dynamic resolution)?
                 * If the rendered rect is smaller than the swapchain we currently sample
                 * the FULL texture (imageRect=L->width/height) -> shrunk/letterboxed image.
                 * Log a periodic baseline + ALWAYS log non-full rects (capped). */
                ovrpRecti v0 = s->ViewportRect[0], v1 = s->ViewportRect[1];
                int full0 = (v0.Size.w == (int)L->width && v0.Size.h == (int)L->height);
                int full1 = (v1.Size.w == (int)L->width && v1.Size.h == (int)L->height);
                static int vbase = 0, vsub = 0;
                if (!full0 || !full1) {
                    if (vsub++ < 200)
                        XRRLOG("VIEWPORT SUB: id=%d tex=%ux%u L=(%d,%d %dx%d) R=(%d,%d %dx%d)",
                               s->LayerId, L->width, L->height,
                               v0.Pos.x, v0.Pos.y, v0.Size.w, v0.Size.h,
                               v1.Pos.x, v1.Pos.y, v1.Size.w, v1.Size.h);
                } else if ((vbase++ % 60) == 0) {
                    XRRLOG("VIEWPORT full: id=%d tex=%ux%u rect=%dx%d",
                           s->LayerId, L->width, L->height, v0.Size.w, v0.Size.h);
                }
            }
            for (int eye = 0; eye < 2; eye++) {
                const XrView *vw = &g_xr.views[eye];   /* reproject removed: always live views */
                pf->pviews[eye] = (XrCompositionLayerProjectionView){
                    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                pf->pviews[eye].pose = vw->pose;
                pf->pviews[eye].fov  = vw->fov;
                pf->pviews[eye].subImage.swapchain = L->swapchain;
                /* diag 2 = force mono: both eyes sample array layer 0. If the dupe
                 * vanishes -> stereo divergence; if it persists -> single-eye ghost. */
                pf->pviews[eye].subImage.imageArrayIndex =
                    (diag == 2) ? 0 : ((L->arraySize > 1) ? eye : 0);
                if (diag == 3) {   /* 2x center-zoom: sample only the center quarter.
                    * Dupe gap GROWS => offset is in the texture (UE render); gap
                    * UNCHANGED => compositor adds it after sampling. */
                    pf->pviews[eye].subImage.imageRect.offset.x = (int)(L->width / 4);
                    pf->pviews[eye].subImage.imageRect.offset.y = (int)(L->height / 4);
                    pf->pviews[eye].subImage.imageRect.extent.width  = (int)(L->width / 2);
                    pf->pviews[eye].subImage.imageRect.extent.height = (int)(L->height / 2);
                } else {
                    pf->pviews[eye].subImage.imageRect.extent.width  = (int)L->width;
                    pf->pviews[eye].subImage.imageRect.extent.height = (int)L->height;
                }
                /* Chain per-eye depth so the compositor can positionally reproject
                 * (artifact B). Reverse-Z/infinite-far is UE's Quest convention;
                 * params tunable via debug.re4vr.depth_* . Only in normal mode. */
                if (diag == 0 && !g_pipelineActive && L->depthSwapchain != XR_NULL_HANDLE) {
                    XrCompositionLayerDepthInfoKHR *d = &pf->pdepth[eye];
                    *d = (XrCompositionLayerDepthInfoKHR){ XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR };
                    d->subImage.swapchain = L->depthSwapchain;
                    d->subImage.imageArrayIndex = (L->arraySize > 1) ? eye : 0;
                    d->subImage.imageRect.extent.width  = (int)L->width;
                    d->subImage.imageRect.extent.height = (int)L->height;
                    if (g_depthRevZ) { d->minDepth = 0.0f; d->maxDepth = 1.0f;
                                       d->nearZ = INFINITY; d->farZ = g_depthNearZ; }
                    else            { d->minDepth = 0.0f; d->maxDepth = 1.0f;
                                       d->nearZ = g_depthNearZ; d->farZ = INFINITY; }
                    pf->pviews[eye].next = d;
                }
            }
            pf->proj = (XrCompositionLayerProjection){ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
            pf->proj.space = g_xr.appSpace;
            pf->proj.viewCount = 2;
            pf->proj.views = pf->pviews;
            pf->submit[pf->nLayers++] = (const XrCompositionLayerBaseHeader *)&pf->proj;
        } else {
            /* quad (menu / loading). Place it where the GAME asked: its submitted
             * Pose + world Size, in our tracking space — unless the game flags it
             * HeadLocked, in which case pin it to the view. Head-locking everything
             * (the old hack) made world-anchored menus appear doubled/misplaced. */
            int headLocked = (s->LayerSubmitFlags & ovrpLayerSubmitFlag_HeadLocked) != 0;
            float sw = s->QuadSize.w, sh = s->QuadSize.h;
            int sizeSane = (sw > 0.05f && sw < 50.0f && sh > 0.05f && sh < 50.0f);
            XrCompositionLayerQuad *q = &pf->quads[pf->nLayers];
            *q = (XrCompositionLayerQuad){ XR_TYPE_COMPOSITION_LAYER_QUAD };
            q->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            q->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q->subImage.swapchain = L->swapchain;
            q->subImage.imageRect.extent.width  = (int)L->width;
            q->subImage.imageRect.extent.height = (int)L->height;
            XrPosef ap;   /* ovrpPosef and XrPosef share layout */
            ap.orientation.x = s->Pose.Orientation.x;
            ap.orientation.y = s->Pose.Orientation.y;
            ap.orientation.z = s->Pose.Orientation.z;
            ap.orientation.w = s->Pose.Orientation.w;
            ap.position.x = s->Pose.Position.x;
            ap.position.y = s->Pose.Position.y;
            ap.position.z = s->Pose.Position.z;
            q->pose  = ap;
            q->space = headLocked ? g_xr.viewSpace : g_xr.appSpace;
            if (diag == 4) {   /* TEST: head-lock the quad centered in view. If the
                * dupe collapses, the duplicate was a world-locked quad separating
                * from a head-locked eye-buffer copy of the same UI. */
                q->space = g_xr.viewSpace;
                q->pose = (XrPosef){ {0,0,0,1}, {0,0,-2.0f} };
                float aspect = L->height ? (float)L->width / (float)L->height : 1.0f;
                q->size.width = 2.0f; q->size.height = 2.0f / aspect;
                pf->submit[pf->nLayers++] = (const XrCompositionLayerBaseHeader *)q;
                continue;
            }
            if (sizeSane) { q->size.width = sw; q->size.height = sh; }
            else {                                    /* fall back to old readable hack */
                q->space = g_xr.viewSpace;
                q->pose = (XrPosef){ {0,0,0,1}, {0,0,-1.5f} };
                float aspect = L->height ? (float)L->width / (float)L->height : 1.0f;
                q->size.width = 1.5f; q->size.height = 1.5f / aspect;
            }
            static int qd = 0;
            if (qd++ < 6)
                XRRLOG("quad id=%d tex %ux%u flags=0x%x headLocked=%d app-pose=(%.2f,%.2f,%.2f) size=(%.2f,%.2f) sane=%d",
                       s->LayerId, L->width, L->height, s->LayerSubmitFlags, headLocked,
                       s->Pose.Position.x, s->Pose.Position.y, s->Pose.Position.z, sw, sh, sizeSane);
            pf->submit[pf->nLayers++] = (const XrCompositionLayerBaseHeader *)q;
        }
    }
    /* Layer z-order fix: emit projection (eye-fov, opaque world) layers FIRST (bottom of the
     * OpenXR layer stack), quads/overlays AFTER (top), regardless of the app's submit order.
     * Stable partition (preserves order within each group). Without this, the splash submits
     * [logo-quad, eye-fov] and the opaque eye-fov composites on top -> black logos. See
     * eyebottom_wanted(). No-op for single-layer frames (gameplay/title). */
    if (eyebottom_wanted() && pf->nLayers > 1) {
        const XrCompositionLayerBaseHeader *ord[XRR_MAX_LAYERS]; int n = 0;
        for (int k = 0; k < pf->nLayers; k++)
            if (pf->submit[k]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) ord[n++] = pf->submit[k];
        for (int k = 0; k < pf->nLayers; k++)
            if (pf->submit[k]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) ord[n++] = pf->submit[k];
        for (int k = 0; k < pf->nLayers; k++) pf->submit[k] = ord[k];
    }
    if (pf->nLayers == 0 && layerCount > 0) {
        /* STARVATION (filtered): app DID submit layers but every one was dropped
         * (inactive/no-swapchain/bad id, or diag/copy-ring filter) -> BLACK frame
         * despite app intent. Distinct from COMPOSE-EMPTY (no input / !shouldRender). */
        static int cf = 0;
        if (cf++ < 300 || trace_level())
            XRRLOG("COMPOSE-FILTERED: appLayerCount=%d all dropped -> BLACK (diag=%d copyRing=%d)",
                   layerCount, diag, g_copyRingEngaged);
    }
    {   /* DIAG: exactly what goes to xrEndFrame. >1 projection layer (or a stray
         * eye-fov layer the app still submits) would composite offset -> the
         * in-projection "dupe". Logs the count + each app submit's id/stage/flags. */
        static int sl = 0;
        if ((sl++ % 120) == 0) {
            XRRLOG("SUBMITLIST: nLayers=%d (appLayerCount=%d) projViews=%d",
                   pf->nLayers, layerCount, pf->proj.viewCount);
            for (int k = 0; k < layerCount && layers && k < 6; k++) {
                const ovrpLayerSubmit *s = layers[k];
                if (!s) continue;
                int id = s->LayerId;
                int eye = (id >= 0 && id < g_xr.layerCount) ? g_xr.layers[id].isEyeFov : -1;
                XRRLOG("  app submit[%d]: LayerId=%d isEyeFov=%d stage=%d flags=0x%x",
                       k, id, eye, s->TextureStage, s->LayerSubmitFlags);
            }
        }
    }
}

/* Submit g_pending to the compositor (or an empty frame if there's none). Returns
 * the XrResult of xrEndFrame. Caller holds g_xrlock. */
static XrResult submit_pending(void) {
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (g_pending.valid) {
        ei.displayTime = g_pending.displayTime;
        ei.layerCount  = g_pending.nLayers;
        ei.layers      = g_pending.nLayers ? g_pending.submit : NULL;
    } else {
        ei.displayTime = g_xr.frameState.predictedDisplayTime;
    }
    if (ei.layerCount == 0 || ei.layers == NULL) {
        /* THE black frame, measured at the point it hits the compositor. This is the
         * title strobe / menu starvation. Always log (capped) + every time under trace. */
        static int sb = 0;
        if (sb++ < 400 || trace_level())
            XRRLOG("SUBMIT-BLACK: valid=%d nLayers=%d shouldRender=%d displayTime=%lld",
                   g_pending.valid, g_pending.nLayers, g_xr.frameState.shouldRender,
                   (long long)ei.displayTime);
    }
    XrResult r = xrEndFrame(g_xr.session, &ei);
    if (!XR_SUCCEEDED(r)) {
        static int ef = 0;
        if (ef++ < 40) XRRLOG("xrEndFrame FAILED rc=%d nLayers=%d shouldRender=%d",
                              (int)r, g_pending.nLayers, g_xr.frameState.shouldRender);
    }
    return r;
}

/* Complete a present-on-submit (deferred) frame: UE's eye-render submit has now landed,
 * so the same-queue resolve barrier orders after it and captures real content. Resolve +
 * release the held image(s), then present the stashed composition. Caller holds g_xrlock. */
static void present_deferred_locked(void) {
    for (int i = 0; i < g_xr.layerCount; i++) {
        XrLayer *L = &g_xr.layers[i];
        if (L->deferColor) {
            if (L->deferColorIndex < L->imageCount)
                xrr_vk_flush_wait(xrr_vk_flush_submit(L->colorImages[L->deferColorIndex], L->arraySize));
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(L->swapchain, &ri);
            L->deferColor = 0;
        }
        if (L->deferDepth) {
            if (L->deferDepthIndex < L->imageCount)
                xrr_vk_flush_wait(xrr_vk_flush_submit_ex(L->depthImages[L->deferDepthIndex], L->arraySize, 1));
            XrSwapchainImageReleaseInfo dri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(L->depthSwapchain, &dri);
            L->deferDepth = 0;
        }
    }
    submit_pending();
    g_pending.valid = 0;
}

/* Trampoline callback (vk_session.c) after each UE vkQueueSubmit. Level 1 just logs the
 * submit-vs-EndFrame ordering (validates the hook + interleave). Level 2 fires the
 * deferred present on the first graphics-queue submit after end_frame stashed a frame —
 * i.e., present rides UE's eye-render submit, ordered after it. */
void xrr_on_ue_submit(uint64_t queue, uint64_t fence, int isRenderQueue) {
    int lvl = submithook_level();
    if (lvl < 1) return;
    long seq = __atomic_add_fetch(&g_submitSeq, 1, __ATOMIC_RELAXED);
    /* per-frame submit tally (reset each end_frame): a truncated/black frame issues fewer
     * GPU submits, so this is a candidate PER-FRAME black signal that (unlike the onset-only
     * post-hitch flag) should stay low for the whole sustained-black tail. */
    __atomic_add_fetch(&g_frameSubmits, 1, __ATOMIC_RELAXED);
    if (isRenderQueue) __atomic_add_fetch(&g_frameRenderSubmits, 1, __ATOMIC_RELAXED);
    static int n = 0;
    if (n++ < 600 || trace_level()) {
        int64_t since = g_lastEndFrameNs ? (now_ns() - g_lastEndFrameNs) : 0;
        XRRLOG("UE-SUBMIT seq=%ld q=0x%llx renderQ=%d fence=0x%llx +%.2fms-after-END defer=%d",
               seq, (unsigned long long)queue, isRenderQueue,
               (unsigned long long)fence, (double)since / 1e6, g_deferPresent);
    }
    if (lvl < 2 || !isRenderQueue) return;
    pthread_mutex_lock(&g_xrlock);
    if (g_deferPresent && ++g_deferSubmits >= defer_submit_count()) {
        present_deferred_locked();
        g_deferPresent = 0;
    }
    pthread_mutex_unlock(&g_xrlock);
}

/* blackcount/lumagate diagnostic: read the RESOLVED eye image's luma and set the bad-black verdict
 * g_frameBlack (truncation = abrupt lit->black, not a gradual fade), and tick the blackcount window.
 * No-op unless debug.re4vr.blackcount or .lumagate is set. (The old Lever-2 reproject hold/restore
 * that consumed this verdict was removed — the ghost was a frame-pacing bug, fixed in the loop.) */
static void update_black_verdict(XrLayer *L, uint32_t idx, int layerCount) {
    if (!L->isEyeFov || idx >= L->imageCount) return;
    if (!(blackcount_wanted() || lumagate_wanted())) return;

    g_frameLuma = xrr_vk_frame_luma(L->colorImages[idx], L->width, L->height, 0);
    int isBlack = (g_frameLuma >= 0 && g_frameLuma < lumagate_thr() && layerCount <= 1);
    if (!isBlack) {
        g_reprojActive = 0;
    } else if (!g_reprojActive && g_prevLuma >= 0 &&
               (g_prevLuma - g_frameLuma) >= abruptdrop()) {
        g_reprojActive = 1;   /* abrupt lit->black = truncation onset */
    }
    g_frameBlack = isBlack && g_reprojActive;
    if (g_frameLuma >= 0) g_prevLuma = g_frameLuma;

    if (blackcount_wanted()) {
        g_frameWindow++;
        if (g_frameBlack) { g_blackWindow++; g_blackTotal++; }
        if (g_frameWindow >= 360) {
            XRRLOG("BLACKCOUNT mode=%s black=%ld/%ld (%.1f%%) total=%ld",
                   g_pipelineActive ? "render-ahead" : "sync", g_blackWindow, g_frameWindow,
                   100.0 * (double)g_blackWindow / (double)g_frameWindow, g_blackTotal);
            g_blackWindow = 0; g_frameWindow = 0;
        }
    }
}

ovrpResult xrr_end_frame(int frameIndex,
        const ovrpLayerSubmit *const *layers, int layerCount) {
    pthread_mutex_lock(&g_xrlock);
    if (!g_xr.running) { pthread_mutex_unlock(&g_xrlock); return ovrpFailure_NotYetImplemented; }
    /* only xrEndFrame if we actually began this frame (pairs 1:1 with BeginFrame) */
    if (!g_xr.inFrame) { pthread_mutex_unlock(&g_xrlock); return ovrpSuccess; }

    /* render-submit hook: install once when enabled (UE's RHI is up by first end_frame). */
    { static int tried = 0;
      if (!tried && submithook_level() >= 1) { tried = 1; xrr_install_submit_hook(); } }
    /* present-on-submit safety: if last frame deferred but UE's submit never fired (e.g.
     * a shouldRender=0 frame, or the eye render landed on a non-graphics queue), flush the
     * held frame now so the swapchain image can't stay stuck across frames. */
    if (g_deferPresent) {
        static int sf = 0;
        if (sf++ < 200 || trace_level())
            XRRLOG("submithook: stale deferred frame (no UE submit), flushing (%.2fms held)",
                   (double)(now_ns() - g_deferStartNs) / 1e6);
        present_deferred_locked();
        g_deferPresent = 0;
    }
    g_lastEndFrameNs = now_ns();

    if (trace_level() >= 2) {   /* dump EVERY layer the app handed us this frame —
        * id/stage/flags/pose/quad-size. The "log everything" view for spotting an
        * old-lib workaround (odd cadence, repeated stage, sentinel flags, etc.). */
        XRRLOG("END f=%d appLayerCount=%d shouldRender=%d", frameIndex, layerCount,
               g_xr.frameState.shouldRender);
        for (int k = 0; k < layerCount && layers && k < XRR_MAX_LAYERS; k++) {
            const ovrpLayerSubmit *s = layers[k];
            if (!s) { XRRLOG("  layer[%d]=NULL", k); continue; }
            XRRLOG("  layer[%d] id=%d stage=%d flags=0x%x pose=(%.2f,%.2f,%.2f) quad=(%.2f,%.2f)",
                   k, s->LayerId, s->TextureStage, s->LayerSubmitFlags,
                   s->Pose.Position.x, s->Pose.Position.y, s->Pose.Position.z,
                   s->QuadSize.w, s->QuadSize.h);
        }
    }

    /* ---- Copy-ring path (debug.re4vr.copyring): UE rendered into shim images; copy
     * shim[TextureStage] -> the acquired OpenXR image (resolves tile memory) WITHOUT
     * waiting, then present the PREVIOUS frame's image (its copy finished a frame ago
     * -> no CPU stall) => breaks the flush-wait serialization. Eye layers only; quads
     * are dropped in this mode (menus hidden — validation cut). ------------------- */
    int copyActive = xrr_vk_flush_ready();
    if (copyActive) {
        int anyShim = 0;
        for (int i = 0; i < g_xr.layerCount; i++)
            if (g_xr.layers[i].active && g_xr.layers[i].shimCount > 0) { anyShim = 1; break; }
        copyActive = anyShim;
    }
    if (copyActive) {
        g_copyRingEngaged = 1;
        int tok[XRR_MAX_LAYERS]; for (int i = 0; i < XRR_MAX_LAYERS; i++) tok[i] = -1;
        /* (A) copy this frame's eye shim render -> acquired OpenXR image (no wait) */
        for (int k = 0; k < layerCount && layers; k++) {
            const ovrpLayerSubmit *s = layers[k];
            if (!s || s->LayerId < 0 || s->LayerId >= g_xr.layerCount) continue;
            XrLayer *L = &g_xr.layers[s->LayerId];
            if (!L->isEyeFov || L->shimCount <= 0 || !L->imageAcquired) continue;
            if (L->acquiredIndex >= L->imageCount) continue;
            /* single-shim: UE rendered into shimImages[0] (the only one we hand it) */
            tok[s->LayerId] = xrr_vk_copy_submit(L->shimImages[0],
                                  L->colorImages[L->acquiredIndex], L->width, L->height, L->arraySize);
            if (dump_pending()) {   /* DIAG: dump ALL shim stages — which one did UE
                * actually render into vs the submitStage we copy? (off-by-one check) */
                char p[160];
                for (int st = 0; st < L->shimCount && st < 3; st++) {
                    snprintf(p, sizeof(p), "/sdcard/Android/data/com.Armature.VR4/files/cr_shim%d.ppm", st);
                    xrr_vk_dump_image(L->shimImages[st], L->width, L->height, 0, p);
                }
                XRRLOG("copy-ring dump: app submitStage=%d (we copy this one) acquiredIdx=%u shimCount=%d",
                       s->TextureStage, L->acquiredIndex, L->shimCount);
            }
        }
        /* (B) present the PREVIOUS frame's eye image (held); its copy is already done.
         * Track whether we actually released a fresh backing image — if a prior frame
         * timed out (begin_frame couldn't acquire) the chain has no present-pending,
         * and presenting g_pending then would reference a swapchain with no freshly
         * released image -> XR_ERROR_LAYER_INVALID -> black, no recovery. In that case
         * present EMPTY (valid) so xrEndFrame succeeds and the chain self-heals. */
        int releasedPresent = 0;
        for (int i = 0; i < g_xr.layerCount; i++) {
            XrLayer *L = &g_xr.layers[i];
            if (!L->presentPending) continue;
            xrr_vk_flush_wait(L->presentToken);
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(L->swapchain, &ri);
            L->presentPending = 0;
            releasedPresent++;
        }
        /* If no fresh image was released (acquire failed under image contention), DON'T
         * present black — re-present the last composition. We didn't release a new
         * image, so the runtime re-shows its last-released image (freeze on the last
         * good frame) instead of flashing black. Only force-empty on the very first
         * frame (no composition built yet). */
        if (!releasedPresent && !g_pending.valid) {
            static int cb = 0;
            if (cb++ < 20) XRRLOG("copy-ring: no composition yet -> empty frame");
        } else if (!releasedPresent) {
            static int fr = 0;
            if (fr++ % 120 == 0) XRRLOG("copy-ring: chain broken -> re-present last frame (freeze)");
        }
        XrResult rc = submit_pending();   /* present last composition (fresh or repeated) */
        /* (C) build this frame's eye-only composition; promote eye images, release
         * any acquired non-eye (quad) images we dropped to stay swapchain-balanced. */
        build_composition(&g_pending, layers, layerCount);
        for (int i = 0; i < g_xr.layerCount; i++) {
            XrLayer *L = &g_xr.layers[i];
            if (L->isEyeFov && L->shimCount > 0 && L->imageAcquired) {
                L->presentPending = 1; L->presentToken = tok[i];
                L->presentIndex = L->acquiredIndex; L->imageAcquired = 0;
            } else if (L->imageAcquired) {
                XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(L->swapchain, &ri);
                L->imageAcquired = 0;
            }
        }
        g_xr.inFrame = 0;
        static int cr = 0;
        if ((cr++ % 240) == 0) XRRLOG("end_frame copy-ring #%d nLayers=%d", cr, g_pending.nLayers);
        pthread_mutex_unlock(&g_xrlock);
        return XR_SUCCEEDED(rc) ? ovrpSuccess : ovrpFailure_OperationFailed;
    }
    g_copyRingEngaged = 0;

    /* Engage the render-ahead pipeline once the flush ring is up AND every active
     * layer is at least double-buffered (we hold one image while acquiring the
     * next). Until then run synchronously (flush+wait+release in this frame) — same
     * as before, correct but with the latency that causes ghosting. Lever B exposes
     * it via debug.re4vr.renderahead (the legacy debug.re4vr.pipeline still works). */
    int wantPipeline = (pipeline_wanted() || renderahead_wanted());
    if (!g_pipelineActive && wantPipeline && xrr_vk_flush_ready()) {
        int dbuf = 1;
        for (int i = 0; i < g_xr.layerCount; i++)
            if (g_xr.layers[i].active && g_xr.layers[i].imageCount < 2) { dbuf = 0; break; }
        if (dbuf) { g_pipelineActive = 1; XRRLOG("frame loop: render-ahead pipeline engaged"); }
    } else if (g_pipelineActive && !wantPipeline) {
        /* Clean disengage (Lever B stabilization): drain any image held from last frame so the
         * swapchain can't get stuck acquired across the mode switch, then fall back to the sync
         * path for THIS frame. Costs a one-frame hiccup (the held composition is dropped). */
        for (int i = 0; i < g_xr.layerCount; i++) {
            XrLayer *L = &g_xr.layers[i];
            if (L->presentPending && L->presentIndex < L->imageCount) {
                xrr_vk_flush_wait(xrr_vk_flush_submit(L->colorImages[L->presentIndex], L->arraySize));
                XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(L->swapchain, &ri);
            }
            L->presentPending = 0;
        }
        g_pipelineActive = 0;
        XRRLOG("frame loop: render-ahead pipeline disengaged (drained held images)");
    }

    XrResult r;
    if (g_pipelineActive) {
        /* DEFERRED-FLUSH pipeline (fixes the render-submit race that blacked the old
         * pipeline). Root cause: UE 4.25's RHI submits the eye render AFTER our
         * EndFrame under load, so flushing the just-acquired image at END(N) resolved
         * an un-submitted (black) image. Fix: hold frame N's rendered image one frame
         * and flush+present it at END(N+1) — by then UE has had a full frame to
         * vkQueueSubmit it, so the resolve barrier orders AFTER UE's writes and
         * captures real content. We present frame N-1 each frame (1 frame latency). */
        static int pd = 0;   /* DIAG: trace hold/flush/release across frames */
        int diag = (pd < 24 || (pd % 120) == 0); pd++;
        int doDump = dump_pending();   /* pipeline-path texture probe (debug.re4vr.dump) */
        for (int i = 0; i < g_xr.layerCount; i++) {
            XrLayer *L = &g_xr.layers[i];
            if (!L->presentPending) continue;
            if (L->presentIndex >= L->imageCount) {   /* stabilization guard: never release a bad index */
                XRRLOG("pipe(deferred): WARN presentPending with bad presentIndex=%u (imgCount=%u) -> dropped",
                       L->presentIndex, L->imageCount);
                L->presentPending = 0;
                continue;
            }
            /* (A) flush the HELD image (rendered last frame) NOW — a full frame after
             * UE rendered it, so its vkQueueSubmit has landed and this barrier resolves
             * the completed render — then wait for the resolve and release it. */
            int t = xrr_vk_flush_submit(L->colorImages[L->presentIndex], L->arraySize);
            xrr_vk_flush_wait(t);
            /* blackcount/lumagate verdict on the resolved HELD image (the content we present). */
            update_black_verdict(L, L->presentIndex, layerCount);
            if (doDump && L->isEyeFov) {
                char p[160];
                snprintf(p, sizeof(p), "/sdcard/Android/data/com.Armature.VR4/files/pipe_present_l%d.ppm", i);
                xrr_vk_dump_image(L->colorImages[L->presentIndex], L->width, L->height, 0, p);
                XRRLOG("pipe dump: layer=%d presentIndex=%u (deferred-flush held image)", i, L->presentIndex);
            }
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(L->swapchain, &ri);
            if (diag) XRRLOG("pipe(deferred): release held presentIndex=%u (this-frame acquiredIndex=%u) gpend(valid=%d nLayers=%d)",
                             L->presentIndex, L->acquiredIndex, g_pending.valid, g_pending.nLayers);
            L->presentPending = 0;
        }
        /* (B) present frame N-1's composition (paired with the image just released). */
        r = submit_pending();
        if (diag) XRRLOG("pipe(deferred): submit_pending valid=%d nLayers=%d displayTime=%lld",
                         g_pending.valid, g_pending.nLayers, (long long)g_pending.displayTime);

        /* (C) capture THIS frame's composition for next frame, then (D) HOLD this
         * frame's rendered image (do NOT flush it yet — defer to next frame's (A)). */
        build_composition(&g_pending, layers, layerCount);
        for (int i = 0; i < g_xr.layerCount; i++) {
            XrLayer *L = &g_xr.layers[i];
            if (!L->imageAcquired) continue;
            L->presentPending = 1;
            L->presentIndex   = L->acquiredIndex;
            L->imageAcquired  = 0;
        }
    } else {
        /* present-on-submit (debug.re4vr.submithook=2): instead of presenting now (before
         * UE has submitted the eye render -> the black race), stash the composition and
         * hold the rendered image; xrr_on_ue_submit fires present_deferred_locked once
         * UE's eye-render vkQueueSubmit lands, ordering present after the render. */
        int haveEye = 0;
        for (int i = 0; i < g_xr.layerCount; i++)
            if (g_xr.layers[i].imageAcquired && g_xr.layers[i].isEyeFov) { haveEye = 1; break; }
        if (submithook_level() >= 2 && g_xr.frameState.shouldRender && haveEye) {
            build_composition(&g_pending, layers, layerCount);
            for (int i = 0; i < g_xr.layerCount; i++) {
                XrLayer *L = &g_xr.layers[i];
                if (L->imageAcquired) { L->deferColor = 1; L->deferColorIndex = L->acquiredIndex; L->imageAcquired = 0; }
                if (L->depthAcquired) { L->deferDepth = 1; L->deferDepthIndex = L->depthAcquiredIndex; L->depthAcquired = 0; }
            }
            g_deferPresent = 1; g_deferSubmits = 0; g_deferStartNs = now_ns();
            r = XR_SUCCESS;   /* present happens later in xrr_on_ue_submit */
            static int dl = 0;
            if (dl++ < 200 || trace_level()) XRRLOG("submithook: present deferred (awaiting UE eye submit)");
        } else {
        /* synchronous fallback: flush+wait then release this frame's images, and
         * present the composition immediately (no extra latency, but ghosts). */
        int64_t waitNs = 0;   /* PROBE: time spent blocked on the flush fence */
        int doDump = dump_pending();
        /* PERF VALIDATION (debug.re4vr.noflushwait=1): submit the flush but DON'T wait,
         * to test whether the flush-wait is what serializes CPU/GPU. Rendering will be
         * black/garbage (compositor reads unresolved), but VrApi FPS tells us if the
         * framerate recovers -> confirms the flush-wait is the frame-drop cause. */
        int skipWait = noflushwait_wanted();
        /* PROBE (debug.re4vr.qwait=1): drain UE's queue BEFORE we resolve/release, so
         * any submitted eye render completes first. If this turns the black gameplay
         * image correct, UE's render was submitted-but-incomplete; if still black, it
         * wasn't submitted yet when end_frame ran (ordering bug). See render-submit race. */
        if (qwait_wanted()) {
            int64_t qw0 = now_ns();
            xrr_vk_queue_wait_idle();
            static int ql = 0;
            if (ql++ < 20) XRRLOG("qwait: vkQueueWaitIdle %.2fms", (double)(now_ns() - qw0) / 1e6);
        }
        for (int i = 0; i < g_xr.layerCount; i++) {
            XrLayer *L = &g_xr.layers[i];
            if (!L->imageAcquired) continue;
            if (L->acquiredIndex < L->imageCount) {
                int t0tok = xrr_vk_flush_submit(L->colorImages[L->acquiredIndex], L->arraySize);
                int64_t w0 = now_ns();
                if (!skipWait) xrr_vk_flush_wait(t0tok);
                waitNs += now_ns() - w0;
                /* blackcount/lumagate black verdict for the resolved eye image (diagnostic only).
                 * Skipped under the noflushwait probe — luma of an unresolved image is meaningless. */
                if (!skipWait)
                    update_black_verdict(L, L->acquiredIndex, layerCount);
                /* Diagnostic (trace): log each QUAD layer's luma per frame. The eye-fov luma
                 * probe above doesn't cover overlays, so this tells us whether a submitted quad
                 * (e.g. the intro logos) has real content or is arriving black — disambiguates a
                 * compositing/placement bug (quad has content) from a content/sync bug (black). */
                if (!skipWait && !L->isEyeFov && trace_level() >= 1) {
                    int ql = xrr_vk_frame_luma(L->colorImages[L->acquiredIndex], L->width, L->height, 0);
                    static int qc = 0;
                    if (qc++ < 4000)
                        XRRLOG("QUADLUMA: layer=%d tex=%ux%u luma=%d %s", i, L->width, L->height, ql,
                               (ql <= 0) ? "BLACK" : "HAS-CONTENT");
                }
                if (doDump && L->isEyeFov) {   /* burst-dump the eye array slices (both eyes) */
                    uint64_t img = L->colorImages[L->acquiredIndex];
                    char p[176];
                    for (uint32_t a = 0; a < L->arraySize; a++) {
                        snprintf(p, sizeof(p),
                            "/sdcard/Android/data/com.Armature.VR4/files/eye_a%u_f%03d.ppm",
                            a, doDump);
                        xrr_vk_dump_image(img, L->width, L->height, a, p);
                    }
                }
                /* Lever 2 validation: stamp the frameIndex barcode into the eye image
                 * AFTER the resolve, so it rides on every frame incl. truncated/black
                 * ones — frame-exact video<->log correlation (debug.re4vr.barcode). */
                if (barcode_wanted() && L->isEyeFov) {
                    /* stamp turns red on a flagged frame. With lumagate on, "flagged" = the
                     * luma gate's per-frame black verdict (should stay red through the whole
                     * black tail); otherwise the onset-only post-hitch flag. */
                    int flagged = lumagate_wanted() ? g_frameBlack
                                                    : (skipblack_level() >= 1 && g_postHitch > 0);
                    for (uint32_t a = 0; a < L->arraySize; a++)
                        xrr_vk_stamp_barcode(L->colorImages[L->acquiredIndex],
                                             L->width, L->height, a, (unsigned)frameIndex, flagged);
                }
            }
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(L->swapchain, &ri);
            L->imageAcquired = 0;
            /* flush + release the paired depth image (tile memory must resolve before
             * the compositor samples it for reprojection), in lockstep with color. */
            if (L->depthAcquired && L->depthAcquiredIndex < L->imageCount) {
                xrr_vk_flush_wait(xrr_vk_flush_submit_ex(L->depthImages[L->depthAcquiredIndex], L->arraySize, 1));
                XrSwapchainImageReleaseInfo dri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(L->depthSwapchain, &dri);
                L->depthAcquired = 0;
            }
        }
        build_composition(&g_pending, layers, layerCount);
        /* PROBE: is the latency theory real? Measure (a) how long the flush-wait
         * blocks the render thread, and (b) how late we call xrEndFrame relative to
         * the predictedDisplayTime we located views for (now - predicted; positive
         * => we submit AFTER the intended display moment => the compositor reprojects).
         * Both confirm/deny the premise behind render-ahead before we build it. */
        int64_t lateNs = now_ns() - (int64_t)g_xr.frameState.predictedDisplayTime;
        static int64_t accWait = 0, maxWait = 0; static int pc = 0;
        accWait += waitNs; if (waitNs > maxWait) maxWait = waitNs;
        if (++pc % 120 == 0) {
            XRRLOG("PROBE: flush-wait avg=%.2fms max=%.2fms | submit-vs-display=%.2fms (>0 late) | predicted=%lld now-rel",
                   (double)accWait / pc / 1e6, (double)maxWait / 1e6, (double)lateNs / 1e6,
                   (long long)g_xr.frameState.predictedDisplayTime);
            accWait = 0; maxWait = 0; pc = 0;
        }
        r = submit_pending();
        g_pending.valid = 0;   /* consumed now; nothing carried to next frame */
        }   /* end synchronous-present (non-deferred) path */
    }

    g_xr.inFrame = 0;
    static int hb = 0;
    if ((hb++ % 240) == 0)
        XRRLOG("end_frame heartbeat #%d pipelined=%d nLayers=%d appLayers=%d",
               hb, g_pipelineActive, g_pending.nLayers, layerCount);
    {   /* Per-frame pacing trace + hitch detection. dt = wall time since the last
         * end_frame; >20ms means we missed a 72Hz (13.9ms) cadence -> the compositor
         * had to reproject/hold, the visual signature of the menu smear. The HITCH
         * line is always-on (capped); full per-frame lines only at trace>=1. */
        static int64_t lastEnd = 0; static int hc = 0;
        int64_t tnow = now_ns();
        double dt_ms = lastEnd ? (double)(tnow - lastEnd) / 1e6 : 0.0;
        lastEnd = tnow;
        if (dt_ms > 0.0) g_gpuFrameMs = dt_ms;   /* source for ovrp_GetGPUFrameTime */
        int hitch = (dt_ms > 20.0);
        /* snapshot+reset the per-frame UE submit tally for this end_frame window (atomic
         * exchange; the hook may add concurrently). subs/rsubs let us test whether
         * truncated/black frames issue fewer submits — a per-frame black signal. */
        int subs  = __atomic_exchange_n(&g_frameSubmits, 0, __ATOMIC_RELAXED);
        int rsubs = __atomic_exchange_n(&g_frameRenderSubmits, 0, __ATOMIC_RELAXED);
        int tl = trace_level();
        if (tl >= 1 || (hitch && hc++ < 400))
            XRRLOG("FRAME f=%d shouldRender=%d appLayers=%d nLayers=%d pipelined=%d dt=%.1fms subs=%d rsubs=%d luma=%d%s%s",
                   frameIndex, g_xr.frameState.shouldRender, layerCount, g_pending.nLayers,
                   g_pipelineActive, dt_ms, subs, rsubs, g_frameLuma,
                   g_frameBlack ? " BLACK" : "", hitch ? " HITCH" : "");

        /* Lever 2 (debug.re4vr.skipblack>=1): flag the recovery frame(s) after a HITCH as
         * LIKELY-TRUNCATED. Instrument-only at level 1 — these logs are the ground-truth
         * we validate against perceived black before building the reproject (level 2).
         * The hitch frame ARMS the window; the following skipblack_count() frames are the
         * candidates UE likely rendered truncated/black. */
        int sbl = skipblack_level();
        if (sbl >= 1) {
            if (g_postHitch > 0) {
                XRRLOG("SKIPBLACK: LIKELY-TRUNCATED frame f=%d (post-hitch, %d left) dt=%.1fms shouldRender=%d",
                       frameIndex, g_postHitch, dt_ms, g_xr.frameState.shouldRender);
                g_postHitch--;
            }
            if (hitch) g_postHitch = skipblack_count();   /* (re)arm window for next frames */
        } else {
            g_postHitch = 0;
        }
    }
    {   /* STEREO DIAG (periodic, survives any capture window): the eye layer's
         * layout + the exact per-eye fov/pose we submit. A side-by-side layout
         * (arraySize=1) submitted with full-width per-eye rects => each eye samples
         * the whole double-wide texture => literal "two copies" doubling. */
        static int sd = 0;
        if ((sd++ % 120) == 0) {
            for (int i = 0; i < g_xr.layerCount; i++) {
                XrLayer *L = &g_xr.layers[i];
                if (!L->active || !L->isEyeFov) continue;
                XrFovf f0 = g_xr.views[0].fov, f1 = g_xr.views[1].fov;
                XrVector3f p0 = g_xr.views[0].pose.position, p1 = g_xr.views[1].pose.position;
                float ipd = sqrtf((p0.x-p1.x)*(p0.x-p1.x)+(p0.y-p1.y)*(p0.y-p1.y)+(p0.z-p1.z)*(p0.z-p1.z));
                XRRLOG("STEREO: layer=%d layout=%d arraySize=%u tex=%ux%u imgArrIdx=%s | "
                       "L.fov(l,r,u,d)=(%.3f,%.3f,%.3f,%.3f) R.fov=(%.3f,%.3f,%.3f,%.3f) IPD=%.3fm",
                       i, L->layout, L->arraySize, L->width, L->height,
                       (L->arraySize > 1) ? "eye" : "0(SHARED!)",
                       f0.angleLeft,f0.angleRight,f0.angleUp,f0.angleDown,
                       f1.angleLeft,f1.angleRight,f1.angleUp,f1.angleDown, ipd);
            }
        }
    }
    pthread_mutex_unlock(&g_xrlock);
    return XR_SUCCEEDED(r) ? ovrpSuccess : ovrpFailure_OperationFailed;
}

void xrr_recommended_eye_size(uint32_t *w, uint32_t *h) {
    *w = 1440; *h = 1584;          /* Quest 2 fallback */
    if (g_xr.instance == XR_NULL_HANDLE || g_xr.systemId == XR_NULL_SYSTEM_ID) return;
    uint32_t n = 0;
    if (xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.systemId,
            g_xr.viewConfigType, 0, &n, NULL) != XR_SUCCESS || n == 0) return;
    XrViewConfigurationView v[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW },
                                     { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    if (n > 2) n = 2;
    if (xrEnumerateViewConfigurationViews(g_xr.instance, g_xr.systemId,
            g_xr.viewConfigType, n, &n, v) == XR_SUCCESS && n >= 1) {
        *w = v[0].recommendedImageRectWidth;
        *h = v[0].recommendedImageRectHeight;
    }
}

/* ------------------------------------------------------ layers / swapchains */
static int64_t vkformat_from_ovrp(ovrpTextureFormat f) {
    switch (f) {                                   /* VkFormat numeric values */
        case ovrpTextureFormat_R8G8B8A8_sRGB:     return 43;  /* RGBA8_SRGB   */
        case ovrpTextureFormat_R8G8B8A8:          return 37;  /* RGBA8_UNORM  */
        case ovrpTextureFormat_R16G16B16A16_FP:   return 97;  /* RGBA16_SFLOAT*/
        case ovrpTextureFormat_R11G11B10_FP:      return 122; /* B10G11R11_UF */
        case ovrpTextureFormat_B8G8R8A8_sRGB:     return 50;  /* BGRA8_SRGB   */
        case ovrpTextureFormat_B8G8R8A8:          return 44;  /* BGRA8_UNORM  */
        case ovrpTextureFormat_R5G6B5:            return 4;   /* R5G6B5_PACK16*/
        default:                                  return 43;
    }
}

/* ovrp depth formats -> VkFormat. [CORRECTED from real OVR_Plugin_Types.h]:
 * D16=6, D24_S8=7, D32_FP=8, D32_S824_FP=9, None=10. RE4 passes 10 (None) -> UE does
 * NOT render depth, so the depth layer never engages (earlier 10->D32 was a bug that
 * forced an unwanted depth target and blacked the cast). 0 = none. */
static int64_t vk_depthformat_from_ovrp(int f) {
    switch (f) {
        case 6: return 124;  /* D16          -> VK_FORMAT_D16_UNORM          */
        case 7: return 129;  /* D24_S8       -> VK_FORMAT_D24_UNORM_S8_UINT  */
        case 8: return 126;  /* D32_FP       -> VK_FORMAT_D32_SFLOAT         */
        case 9: return 130;  /* D32_S824_FP  -> VK_FORMAT_D32_SFLOAT_S8_UINT */
        default: return 0;   /* None(10) or unknown -> no depth             */
    }
}

/* Free a layer the app destroyed (DestroyLayer). Must be serialized against the
 * frame loop, which acquires/composites by layer. Releasing any in-flight image
 * first keeps the swapchain balanced before xrDestroySwapchain. */
void xrr_destroy_layer(int layerId) {
    pthread_mutex_lock(&g_xrlock);
    if (layerId >= 0 && layerId < g_xr.layerCount && g_xr.layers[layerId].active) {
        XrLayer *L = &g_xr.layers[layerId];
        /* drain both in-flight images (FIFO releases the older present-pending one
         * first, then this frame's). */
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        if (L->presentPending) {
            xrr_vk_flush_wait(L->presentToken);
            xrReleaseSwapchainImage(L->swapchain, &ri);
            L->presentPending = 0;
        }
        if (L->imageAcquired) {
            xrReleaseSwapchainImage(L->swapchain, &ri);
            L->imageAcquired = 0;
        }
        if (L->swapchain != XR_NULL_HANDLE) xrDestroySwapchain(L->swapchain);
        if (L->depthSwapchain != XR_NULL_HANDLE) xrDestroySwapchain(L->depthSwapchain);
        if (L->shimCount > 0) xrr_vk_free_images(L->shimImages, L->shimMem, L->shimCount);
        memset(L, 0, sizeof(*L));   /* active=0 -> skipped by begin/end frame */
        XRRLOG("destroy_layer: freed id=%d", layerId);
    }
    pthread_mutex_unlock(&g_xrlock);
}

/* --- Game-driven Fixed Foveated Rendering (TiledMultiRes) -------------------
 * RE4 runs its own dynamic-perf loop: it polls GPU frame time and, under load,
 * lowers foveation via ovrp_SetTiledMultiResLevel/Dynamic (the Oculus name for
 * FFR). We map that onto OpenXR XR_FB_foveation. Source of truth for the level
 * actually applied, in priority order:
 *   1. debug.re4vr.ffr prop  — manual test override (-1/unset = let the game drive)
 *   2. g_ffrGameLevel        — the game's last ovrp_SetTiledMultiResLevel request
 *   3. MEDIUM (default)      — sane baseline if neither set
 * ovrpTiledMultiResLevel: 0=Off 1=LMSLow 2=LMSMedium 3=LMSHigh 4=LMSHighTop. */
static int g_ffrGameLevel   = -1;   /* ovrp level the game last Set (-1 = unset) */
static int g_ffrGameDynamic = 1;    /* game's dynamic flag (runtime-dynamic on)  */

/* debug.re4vr.ffr override: returns -1 when unset (let the game drive), else 0..3. */
static int ffr_prop_override(void) {
    int v = -1;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.ffr", s) > 0 && s[0]) v = atoi(s);
#endif
    if (v > 3) v = 3;
    return v;
}

/* ovrpTiledMultiResLevel (0..4) -> our FFR strength (0..3). HighTop folds to high. */
static int ovrp_level_to_ffr(int lvl) {
    if (lvl <= 0) return 0;
    if (lvl >= 4) return 3;
    return lvl;   /* Low/Med/High map 1:1 */
}

/* Resolve the FFR strength (0..3) to apply right now (override > game > default). */
static int ffr_resolve(void) {
    int p = ffr_prop_override();
    if (p >= 0) return p;
    if (g_ffrGameLevel >= 0) return ovrp_level_to_ffr(g_ffrGameLevel);
    return 2;   /* default medium */
}

/* Load the XR_FB_foveation entry points once. Returns 1 if usable. */
static int foveation_entrypoints(PFN_xrCreateFoveationProfileFB *create,
                                 PFN_xrDestroyFoveationProfileFB *destroy,
                                 PFN_xrUpdateSwapchainFB *update) {
    static PFN_xrCreateFoveationProfileFB  createProf;
    static PFN_xrDestroyFoveationProfileFB destroyProf;
    static PFN_xrUpdateSwapchainFB         updateSc;
    static int loaded = 0, loadFail = 0;
    if (loadFail) return 0;
    if (!loaded) {
        union { PFN_xrVoidFunction v; PFN_xrCreateFoveationProfileFB  f; } a = {0};
        union { PFN_xrVoidFunction v; PFN_xrDestroyFoveationProfileFB f; } b = {0};
        union { PFN_xrVoidFunction v; PFN_xrUpdateSwapchainFB         f; } c = {0};
        xrGetInstanceProcAddr(g_xr.instance, "xrCreateFoveationProfileFB",  &a.v);
        xrGetInstanceProcAddr(g_xr.instance, "xrDestroyFoveationProfileFB", &b.v);
        xrGetInstanceProcAddr(g_xr.instance, "xrUpdateSwapchainFB",         &c.v);
        createProf = a.f; destroyProf = b.f; updateSc = c.f;
        if (!createProf || !updateSc) { loadFail = 1; XRRERR("FFR: entry points missing"); return 0; }
        loaded = 1;
    }
    *create = createProf; *destroy = destroyProf; *update = updateSc;
    return 1;
}

/* Eye-tracked foveation toggle: default ON when XR_META_foveation_eye_tracked is available,
 * set debug.re4vr.etfr=0 to force fixed FFR (A/B the gaze-driven upgrade on device). */
static int etfr_enabled(void) {
    static int v = -1;
    if (v < 0) {
#ifdef __ANDROID__
        char s[92] = {0};
        extern int __system_property_get(const char *, char *);
        v = (__system_property_get("debug.re4vr.etfr", s) > 0 && s[0] == '0') ? 0 : 1;  /* default on */
#else
        v = 1;
#endif
    }
    return v;
}

/* Apply FFR strength `ffr` (0..3, 0=disable) to one eye swapchain via XR_FB_foveation.
 * `dynamicOn` lets the runtime ease foveation when there's GPU headroom. This is the
 * single extension point for foveation — add other HMDs' schemes (e.g. Steam Frame /
 * eye-tracked) here behind their own availability flags. */
static void apply_foveation_sc(XrSwapchain sc, int ffr, int dynamicOn) {
    if (!g_haveFoveation || sc == XR_NULL_HANDLE) return;
    PFN_xrCreateFoveationProfileFB  createProf;
    PFN_xrDestroyFoveationProfileFB destroyProf;
    PFN_xrUpdateSwapchainFB         updateSc;
    if (!foveation_entrypoints(&createProf, &destroyProf, &updateSc)) return;

    /* ffr 0 = disable. Apply a real LEVEL_NONE profile rather than a NULL profile:
     * the Meta runtime null-derefs on profile==XR_NULL_HANDLE (crashes in
     * libvrapiimpl) even though the spec says NULL means "default". So we always
     * create a valid profile and map Off -> XR_FOVEATION_LEVEL_NONE_FB. */
    XrFoveationLevelProfileCreateInfoFB lvl = { XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB };
    lvl.level = (ffr <= 0) ? XR_FOVEATION_LEVEL_NONE_FB
              : (ffr == 1) ? XR_FOVEATION_LEVEL_LOW_FB
              : (ffr == 2) ? XR_FOVEATION_LEVEL_MEDIUM_FB
                           : XR_FOVEATION_LEVEL_HIGH_FB;
    lvl.verticalOffset = 0.0f;
    lvl.dynamic = dynamicOn ? XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB
                            : XR_FOVEATION_DYNAMIC_DISABLED_FB;
    /* Eye-tracked upgrade: chain XrFoveationEyeTrackedProfileCreateInfoMETA onto the FB level
     * profile so the runtime centres foveation on gaze (falls back to fixed when gaze is
     * invalid). Only when the ext is advertised, FFR is on, and not disabled via prop. */
    int eyeTracked = g_haveEyeTrackedFov && etfr_enabled() && ffr > 0;
    XrFoveationEyeTrackedProfileCreateInfoMETA et = { XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META };
    if (eyeTracked) lvl.next = &et;
    XrFoveationProfileCreateInfoFB pci = { XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB };
    pci.next = &lvl;
    XrFoveationProfileFB profile = XR_NULL_HANDLE;
    if (xrfail("xrCreateFoveationProfileFB", createProf(g_xr.session, &pci, &profile))) return;

    XrSwapchainStateFoveationFB st = { XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB };
    st.profile = profile;
    XrResult r = updateSc(sc, (XrSwapchainStateBaseHeaderFB *)&st);
    if (destroyProf) destroyProf(profile);   /* safe to free once applied */
    if (xrfail("xrUpdateSwapchainFB", r)) return;
    XRRLOG("FFR: applied level=%d dynamic=%d eyeTracked=%d to eye swapchain", ffr, dynamicOn, eyeTracked);
}

/* Apply the currently-resolved FFR to an eye swapchain (used at layer creation). */
static void apply_foveation(XrSwapchain sc) {
    apply_foveation_sc(sc, ffr_resolve(), g_ffrGameDynamic);
}

/* Re-apply the resolved FFR to the live eye swapchain(s). Caller holds g_xrlock so
 * this can't race the render thread's acquire/release (all under the same lock). */
static void reapply_foveation_locked(void) {
    for (int i = 0; i < g_xr.layerCount; i++) {
        XrLayer *L = &g_xr.layers[i];
        if (L->active && L->isEyeFov && L->swapchain != XR_NULL_HANDLE)
            apply_foveation_sc(L->swapchain, ffr_resolve(), g_ffrGameDynamic);
    }
}

/* ---- ovrp_*TiledMultiRes* / GetGPUFrameTime bridge (called from core.c) ---- */
int xrr_foveation_supported(void) { return g_haveFoveation ? 1 : 0; }

void xrr_set_tiled_multires_level(int ovrpLevel) {
    pthread_mutex_lock(&g_xrlock);
    int changed = (ovrpLevel != g_ffrGameLevel);
    g_ffrGameLevel = ovrpLevel;
    if (changed) {
        XRRLOG("game SetTiledMultiResLevel=%d -> applied ffr=%d", ovrpLevel, ffr_resolve());
        reapply_foveation_locked();
    }
    pthread_mutex_unlock(&g_xrlock);
}

int xrr_get_tiled_multires_level(void) {
    return g_ffrGameLevel < 0 ? 0 : g_ffrGameLevel;   /* report what the game Set */
}

void xrr_set_tiled_multires_dynamic(int on) {
    pthread_mutex_lock(&g_xrlock);
    on = on ? 1 : 0;
    int changed = (on != g_ffrGameDynamic);
    g_ffrGameDynamic = on;
    if (changed) {
        XRRLOG("game SetTiledMultiResDynamic=%d", on);
        reapply_foveation_locked();
    }
    pthread_mutex_unlock(&g_xrlock);
}

int xrr_get_tiled_multires_dynamic(void) { return g_ffrGameDynamic; }

/* GPU frame time (ms) fed to the game's dynamic-perf loop. We have no portable GPU
 * timestamp query, so we approximate with the measured end-to-end frame dt (updated
 * in xrr_end_frame). The handoff accepts an approximate value to engage the scaler.
 * NOTE: unit (ms vs s) and the dt-vs-true-GPU-time proxy both need on-device
 * confirmation — watch whether the game's FFR engages under Standing load. */
float xrr_gpu_frame_time_ms(void) {
    /* Throttled correlation log: the GPU-time we feed the game vs. the foveation it
     * asks for in response. If level stays low while fed time is high (or vice-versa),
     * the frame-dt proxy / unit is the problem. ~1/sec under trace, capped otherwise. */
    static int n = 0;
    if ((n++ % 72) == 0 && (trace_level() >= 1 || n < 720))
        XRRLOG("GPU-TIME fed=%.2fms gameLevel=%d dynamic=%d -> applied ffr=%d",
               g_gpuFrameMs, g_ffrGameLevel, g_ffrGameDynamic, ffr_resolve());
    return (float)g_gpuFrameMs;
}

/* ---- Lever A: dynamic resolution via ovrp_GetAdaptiveGpuPerformanceScale2 ----
 * RE finding: the game presets scale=1.0, calls this API, then
 * sets render resolution = baseDensity * sqrt(scale). Returning <1 under GPU pressure makes
 * the GAME self-downscale its eye buffer — no game patch. Gated game-side on Settings.byte
 * [0x19] bit4 (adaptive-res enabled): if that bit is clear the game ignores us and uses a
 * fixed screen-percentage CVar, so this lever is inert (verify on device by watching whether
 * the viewport shrinks). Caveat: the truncation black is DRAW-bound, so quarter-res still
 * blacks — this is a throughput/headroom win, not a guaranteed black fix (measure with the
 * BLACKCOUNT counter). */
static int adaptivescale_wanted(void) {
    int v = 1;   /* default ON — game ignores it unless its own bit4 is set */
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.adaptivescale", s) > 0 && s[0] == '0') v = 0;
#endif
    return v;
}
/* Lower bound on scale (debug.re4vr.adaptivefloor, percent; default 60 = 0.60). Bounds how far
 * the game may shrink resolution: res ~ sqrt(scale), so floor 0.60 -> ~0.77x linear res. */
static float adaptivefloor(void) {
    int v = 60;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.adaptivefloor", s) > 0) { int n = atoi(s); if (n >= 10 && n <= 100) v = n; }
#endif
    return (float)v / 100.0f;
}

/* Compute the adaptive GPU scale the game multiplies into its resolution. budget = 72Hz frame
 * (13.9ms); an overloaded frame returns budget/gpuMs (<1). EMA-smoothed to avoid resolution
 * thrash (sqrt-quantized to /1024 game-side, but the raw value still oscillates with dt). */
float xrr_adaptive_gpu_scale(void) {
    static float ema = 1.0f;
    if (!adaptivescale_wanted()) { ema = 1.0f; return 1.0f; }
    const float budget = 13.9f;
    float floor = adaptivefloor();
    float inst = 1.0f;
    if (g_gpuFrameMs > 0.1) {
        inst = budget / (float)g_gpuFrameMs;
        if (inst > 1.0f) inst = 1.0f;
        if (inst < floor) inst = floor;
    }
    ema = ema * 0.85f + inst * 0.15f;   /* ~7-frame time constant */
    if (ema < floor) ema = floor;
    if (ema > 1.0f) ema = 1.0f;
    static int n = 0;
    if ((n++ % 72) == 0 && (trace_level() >= 1 || n < 720))
        XRRLOG("ADAPTIVE-SCALE gpuMs=%.2f inst=%.3f smoothed=%.3f floor=%.2f (game res ~sqrt)",
               g_gpuFrameMs, inst, ema, floor);
    return ema;
}

/* ---- ovrp_*PerfMetrics* backend (called from core.c) ----------------------
 * Per-metric perf query (ovrpPerfMetrics ids). The game polls IsPerfMetricsSupported
 * per metric, then GetPerfMetrics{Float,Int} for the supported ones. We answer
 * TRUTHFULLY per metric: supported only where we have a real measurement, and we log
 * every query so we learn exactly which metrics RE4 wants (then we can wire more real
 * sources). Today the one real source is App GPU time (= our measured frame dt). */
enum { OVRP_PM_APP_CPUTIME_F = 0, OVRP_PM_APP_GPUTIME_F = 1,
       OVRP_PM_DEV_CPULEVEL_I = 12, OVRP_PM_DEV_GPULEVEL_I = 13 };

int xrr_perf_metric_supported(int metric) {
    int sup = (metric == OVRP_PM_APP_GPUTIME_F)                          /* measured frame dt */
           || (metric == OVRP_PM_DEV_CPULEVEL_I && g_cpuPerfLevel >= 0)  /* game's CPU clock level */
           || (metric == OVRP_PM_DEV_GPULEVEL_I && g_gpuPerfLevel >= 0); /* game's GPU clock level */
    static int n = 0;
    if (n++ < 120 || trace_level())
        XRRLOG("PerfMetrics IsSupported(metric=%d) -> %d", metric, sup);
    return sup;
}

/* Returns 1 and writes *out if we provide this float metric, else 0 (-> Unsupported). */
int xrr_perf_metric_float(int metric, float *out) {
    float v = 0.0f; int ok = 0;
    if (metric == OVRP_PM_APP_GPUTIME_F) { v = (float)g_gpuFrameMs; ok = 1; }  /* ms */
    if (out) *out = v;
    static int n = 0;
    if (n++ < 240 || trace_level())
        XRRLOG("PerfMetrics GetFloat(metric=%d) -> %.3f ok=%d", metric, v, ok);
    return ok;
}

/* Returns 1 and writes *out if we provide this int metric, else 0 (-> Unsupported). */
int xrr_perf_metric_int(int metric, int *out) {
    int v = 0, ok = 0;
    if (metric == OVRP_PM_DEV_CPULEVEL_I && g_cpuPerfLevel >= 0) { v = g_cpuPerfLevel; ok = 1; }
    if (metric == OVRP_PM_DEV_GPULEVEL_I && g_gpuPerfLevel >= 0) { v = g_gpuPerfLevel; ok = 1; }
    if (out) *out = v;
    static int n = 0;
    if (n++ < 120 || trace_level())
        XRRLOG("PerfMetrics GetInt(metric=%d) -> %d ok=%d", metric, v, ok);
    return ok;
}

ovrpResult xrr_setup_layer(const ovrpLayerDesc *desc, int *outLayerId) {
    if (!desc || !outLayerId) return ovrpFailure_InvalidParameter;
    if (g_xr.session == XR_NULL_HANDLE) return ovrpFailure_NotInitialized;

    /* Hold g_xrlock across slot allocation + the whole build/publish so the render-thread frame
     * loop (which iterates g_xr.layers under the same lock) never sees a half-built layer — and
     * for memory-ordering symmetry with xrr_destroy_layer. setup is infrequent (init / menu layer
     * create), so the brief contention with the frame loop is fine. */
    pthread_mutex_lock(&g_xrlock);
    /* reuse a freed slot if one exists, else grow */
    int slot = -1;
    for (int i = 0; i < g_xr.layerCount; i++)
        if (!g_xr.layers[i].active) { slot = i; break; }
    if (slot < 0) {
        if (g_xr.layerCount >= XRR_MAX_LAYERS) {
            pthread_mutex_unlock(&g_xrlock); return ovrpFailure_InsufficientSize;
        }
        slot = g_xr.layerCount++;
    }

    XrLayer *L = &g_xr.layers[slot];
    memset(L, 0, sizeof(*L));
    L->width       = (uint32_t)desc->TextureSize.w;
    L->height      = (uint32_t)desc->TextureSize.h;
    L->arraySize   = (desc->Layout == ovrpLayout_Array) ? 2 : 1;
    L->layout      = desc->Layout;
    L->isEyeFov    = (desc->Shape == ovrpShape_EyeFov);
    L->colorFormat = vkformat_from_ovrp(desc->Format);

    /* the runtime only accepts formats from its supported list; validate ours and
     * fall back to the first supported (preferring an sRGB color format). */
    int64_t fmts[128]; uint32_t nf = 0;
    xrEnumerateSwapchainFormats(g_xr.session, 128, &nf, fmts);
    int supported = 0; int64_t fallback = nf ? fmts[0] : L->colorFormat;
    for (uint32_t i = 0; i < nf; i++) {
        if (fmts[i] == L->colorFormat) supported = 1;
        if (fmts[i] == 43 || fmts[i] == 50) fallback = fmts[i];  /* RGBA8/BGRA8 sRGB */
    }
    if (!supported) {
        XRRLOG("setup_layer: format %lld unsupported, using %lld (of %u)",
               (long long)L->colorFormat, (long long)fallback, nf);
        L->colorFormat = fallback;
    }

    int useCopyRing = L->isEyeFov && copyring_wanted();
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                     XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     /* copy-ring copies into the OpenXR image -> needs TRANSFER_DST */
                     (useCopyRing ? XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT : 0);
    ci.format      = L->colorFormat;
    ci.sampleCount = desc->SampleCount ? (uint32_t)desc->SampleCount : 1;
    ci.width       = L->width;
    ci.height      = L->height;
    ci.faceCount   = 1;
    ci.arraySize   = L->arraySize;
    ci.mipCount    = desc->MipLevels ? (uint32_t)desc->MipLevels : 1;
    XRRLOG("setup_layer: fmt=%lld %ux%u samples=%u array=%u mips=%u",
           (long long)ci.format, ci.width, ci.height, ci.sampleCount,
           ci.arraySize, ci.mipCount);
    if (xrfail("xrCreateSwapchain", xrCreateSwapchain(g_xr.session, &ci, &L->swapchain))) {
        pthread_mutex_unlock(&g_xrlock); return ovrpFailure_OperationFailed;
    }

    /* Fixed Foveated Rendering on the eye buffer only (foveating menus/quads is
     * pointless and would blur readable UI). The game's foveation request is stubbed;
     * this applies it at the runtime instead. */
    if (L->isEyeFov) apply_foveation(L->swapchain);

    L->imageCount = xrr_vk_enumerate_images(L->swapchain, L->colorImages, XRR_MAX_IMAGES);

    /* copy-ring: allocate shim images UE renders into (same count/format/size as the
     * OpenXR eye swapchain). If allocation fails, fall back to direct rendering. */
    if (useCopyRing && L->imageCount > 0) {
        if (xrr_vk_alloc_images(L->shimImages, L->shimMem, (int)L->imageCount,
                                L->width, L->height, L->arraySize, (long long)L->colorFormat)) {
            L->shimCount = (int)L->imageCount;
            XRRLOG("setup_layer: copy-ring active, %d shim images for layer %d", L->shimCount, slot);
        } else {
            XRRLOG("setup_layer: copy-ring shim alloc FAILED -> direct render");
        }
    }

    /* Depth swapchain for positional reprojection (eye layers only). UE requested a
     * DepthFormat; if depth is enabled and the format is a real depth format the
     * runtime supports, create it + hand UE the depth images via GetLayerTexture2. */
    int64_t dfmt = vk_depthformat_from_ovrp(desc->DepthFormat);
    if (L->isEyeFov && dfmt && depth_wanted()) {
        int dsupported = 0;
        for (uint32_t i = 0; i < nf; i++) if (fmts[i] == dfmt) { dsupported = 1; break; }
        if (!dsupported) {
            XRRLOG("setup_layer: depth fmt %lld unsupported -> no depth layer", (long long)dfmt);
        } else {
            XrSwapchainCreateInfo dci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
            dci.usageFlags  = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            dci.format      = dfmt;
            dci.sampleCount = ci.sampleCount;
            dci.width       = L->width; dci.height = L->height;
            dci.faceCount   = 1; dci.arraySize = L->arraySize;
            dci.mipCount    = 1;
            if (xrfail("xrCreateSwapchain(depth)", xrCreateSwapchain(g_xr.session, &dci, &L->depthSwapchain))) {
                L->depthSwapchain = XR_NULL_HANDLE;
            } else {
                uint32_t dn = xrr_vk_enumerate_images(L->depthSwapchain, L->depthImages, XRR_MAX_IMAGES);
                L->depthFormat = dfmt;
                XRRLOG("setup_layer: DEPTH swapchain fmt=%lld %ux%u array=%u images=%u (color images=%u)",
                       (long long)dfmt, L->width, L->height, L->arraySize, dn, L->imageCount);
                if (dn < L->imageCount) {   /* must stay in lockstep with color */
                    XRRLOG("setup_layer: depth image count %u < color %u -> disabling depth", dn, L->imageCount);
                    xrDestroySwapchain(L->depthSwapchain); L->depthSwapchain = XR_NULL_HANDLE;
                }
            }
        }
    }

    L->active = 1;          /* set LAST: the lock + active-gate publish the fully-built layer */
    *outLayerId = slot;
    pthread_mutex_unlock(&g_xrlock);
    return ovrpSuccess;
}

ovrpResult xrr_setup_layer_depth(int layerId, const ovrpLayerDesc *depthDesc) {
    if (layerId < 0 || layerId >= g_xr.layerCount) return ovrpFailure_InvalidParameter;
    XrLayer *L = &g_xr.layers[layerId];
    if (!L->active) return ovrpFailure_InvalidParameter;
    /* depth swapchain: D24S8/D32 — submitted via XR_KHR_composition_layer_depth.
     * [TODO] create + wire the depth ext; for now report unsupported so the app
     * falls back to no-depth submission. */
    (void)depthDesc;
    return ovrpFailure_Unsupported;
}

int xrr_layer_stage_count(int layerId) {
    pthread_mutex_lock(&g_xrlock);
    int n = (layerId >= 0 && layerId < g_xr.layerCount && g_xr.layers[layerId].active)
            ? (int)g_xr.layers[layerId].imageCount : 0;
    pthread_mutex_unlock(&g_xrlock);
    return n;
}

ovrpResult xrr_get_layer_texture(int layerId, int stage, int eye,
                                 uint64_t *outColor, uint64_t *outDepth) {
    (void)eye;   /* array layout: one image, eye = array layer chosen at submit */
    pthread_mutex_lock(&g_xrlock);   /* read layer state consistently vs setup/destroy/frame loop */
    ovrpResult res = ovrpFailure_InvalidParameter;
    if (layerId >= 0 && layerId < g_xr.layerCount) {
        XrLayer *L = &g_xr.layers[layerId];
        if (L->active && stage >= 0 && (uint32_t)stage < L->imageCount) {
            static int gc = 0;   /* DIAG: how often / with what stage is this called? */
            if (gc++ < 30) XRRLOG("GetLayerTexture: id=%d stage=%d eye=%d (call #%d)",
                                  layerId, stage, eye, gc);
            /* copy-ring: hand UE the per-stage shim image so it renders there (we copy ->
             * OpenXR later). Per-stage (distinct images) is required — collapsing to one
             * image breaks UE's multi-buffering/TAA. */
            if (outColor) *outColor = (L->shimCount > 0) ? L->shimImages[stage] : L->colorImages[stage];
            if (outDepth) *outDepth = L->depthSwapchain ? L->depthImages[stage] : 0;
            res = ovrpSuccess;
        }
    }
    pthread_mutex_unlock(&g_xrlock);
    return res;
}

double xrr_predicted_display_time_s(void) {
    /* OpenXR time is ns (XrTime) -> seconds. Read the published snapshot (seqlock, lock-free). */
    XrTime t; pose_snapshot(NULL, NULL, &t);
    return (double)t * 1e-9;
}

/* ------------------------------------------------------------- poses ------- */
/* Real per-eye FOV as ovrp tangents (positive magnitudes), from the runtime's
 * located views. The game MUST render its projection with these exact tangents or
 * the compositor reprojects to a different frustum -> ghosting/tearing/black. */
void xrr_eye_fov_tangents(int eye, float *up, float *down, float *left, float *right) {
    XrView v[2]; pose_snapshot(v, NULL, NULL);
    XrFovf f = v[eye & 1].fov;
    if (f.angleLeft == 0.0f && f.angleRight == 0.0f) {   /* not located yet */
        *up = *down = *left = *right = 1.19f;            /* ~50deg fallback */
        return;
    }
    *up    = tanf(f.angleUp);
    *down  = tanf(-f.angleDown);
    *left  = tanf(-f.angleLeft);
    *right = tanf(f.angleRight);
}

ovrpResult xrr_get_node_pose(ovrpNode node, ovrpPoseStatef *out) {
    if (!out) return ovrpFailure_InvalidParameter;
    memset(out, 0, sizeof(*out));
    out->Pose.Orientation.w = 1.0f;
    out->Time = xrr_predicted_display_time_s();
    if (!g_xr.running) return ovrpFailure_NotYetImplemented;

    {   /* DIAG: which nodes does UE query to build its view matrices? */
        static int nq = 0;
        if (nq++ < 24) XRRLOG("GetNodePose node=%d", node);
    }
    if (node == ovrpNode_EyeLeft || node == ovrpNode_EyeRight) {
        int i = (node == ovrpNode_EyeRight);
        XrView v[2]; uint32_t vc; pose_snapshot(v, &vc, NULL);
        if (vc > (uint32_t)i)
            ovrp_pose_from_xr(&v[i].pose, &out->Pose);
        return ovrpSuccess;
    }
    if (node == ovrpNode_Head || node == ovrpNode_EyeCenter) {
        XrTime dt; pose_snapshot(NULL, NULL, &dt);
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        if (xrLocateSpace(g_xr.viewSpace, g_xr.appSpace, dt, &loc) == XR_SUCCESS)
            ovrp_pose_from_xr(&loc.pose, &out->Pose);
        return ovrpSuccess;
    }
    if (node == ovrpNode_HandLeft || node == ovrpNode_HandRight) {
        int got = xrr_get_hand_pose(node, out);
        static int hd = 0;
        if (hd < 6) { hd++; XRRLOG("GetNodePose hand node=%d got=%d", node, got); }
        return got ? ovrpSuccess : ovrpFailure_NotYetImplemented;
    }
    return ovrpFailure_NotYetImplemented;
}
