/* core.c — the ovrp_* exports prototyped with real signatures in ovrplugin_shim.h:
 * lifecycle, frame loop, poses, tracking/perf config — all wired to OpenXR via xr_runtime.c.
 * (Session + Vulkan graphics binding are created in ovrp_Initialize5 -> xrr_init.)
 * Each export forwards to the real libOVRPlugin first when passthru is active (PT_FWD).
 */
#include "ovrplugin_shim.h"
#include "xr_runtime.h"
#include "passthru.h"
#include "log.h"
#include <string.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define TODO_RET ovrpFailure_NotYetImplemented

/* --- lifecycle ---------------------------------------------------------- */
OVRP_EXPORT ovrpResult ovrp_PreInitialize3(void *garbage) {
    PT_FWD(ovrp_PreInitialize3, garbage);   /* first pt_active() lazily loads the real lib */
    (void)garbage;
    XRRLOG("ovrp_PreInitialize3 called");
    ovrpResult r = xrr_pre_init();
    XRRLOG("ovrp_PreInitialize3 -> %d", r);
    return r;
}
OVRP_EXPORT ovrpResult ovrp_Initialize5(
        ovrpRenderAPIType apiType, ovrpLogCallback logCallback, void *activity,
        void *vkInstance, void *vkPhysicalDevice, void *vkDevice, void *queue,
        unsigned int flags, const void *version) {   /* version = const ovrpVersion& (ptr) */
    PT_FWD(ovrp_Initialize5, apiType, logCallback, activity, vkInstance,
           vkPhysicalDevice, vkDevice, queue, flags, version);
    (void)apiType; (void)logCallback; (void)flags; (void)version;
    XRRLOG("ovrp_Initialize5: api=%d act=%p vkInst=%p vkPhys=%p vkDev=%p queue=%p fl=%u",
           apiType, activity, vkInstance, vkPhysicalDevice, vkDevice, queue, flags);
    xrr_set_android_activity(activity);
    xrr_vk_set_handles(vkDevice, queue, 0);   /* for the end-of-frame flush barrier */
    /* OpenXR wants queueFamilyIndex+queueIndex; OVRPlugin gives a VkQueue we can't
     * decompose -> default family 0/index 0 (UE Vulkan on Quest uses graphics fam 0). */
    ovrpResult r = xrr_init(vkInstance, vkPhysicalDevice, vkDevice, 0);
    XRRLOG("ovrp_Initialize5 -> %d", r);
    return r;
}
OVRP_EXPORT ovrpResult ovrp_Shutdown2(void) {
    PT_FWD(ovrp_Shutdown2); xrr_shutdown(); return ovrpSuccess; }

/* --- frame loop --------------------------------------------------------- */
OVRP_EXPORT ovrpResult ovrp_Update3(ovrpStep step, int frameIndex, double predictedTime) {
    PT_FWD(ovrp_Update3, step, frameIndex, predictedTime);
    (void)step; (void)frameIndex; (void)predictedTime;
    static int o; if (!o) { o = 1; XRRLOG("ovrp_Update3 first call"); }
    xrr_poll_events();                /* advance the session state machine */
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_WaitToBeginFrame(int frameIndex) {
    PT_FWD(ovrp_WaitToBeginFrame, frameIndex);
    ovrpResult r = xrr_wait_frame(frameIndex);
    static int o; if (!o) { o = 1; XRRLOG("ovrp_WaitToBeginFrame first call -> %d", r); }
    return r;
}
OVRP_EXPORT ovrpResult ovrp_BeginFrame4(int frameIndex, void *commandQueue) {
    PT_FWD(ovrp_BeginFrame4, frameIndex, commandQueue);
    (void)commandQueue;
    return xrr_begin_frame(frameIndex);
}
OVRP_EXPORT ovrpResult ovrp_EndFrame4(int frameIndex,
        const ovrpLayerSubmit *const *layerSubmitPtr, int layerSubmitCount,
        void *commandQueue) {
    if (pt_active()) {
        static __typeof__(&ovrp_EndFrame4) _r; static int _g;
        if (!_g) { _r = (__typeof__(&ovrp_EndFrame4))pt_real("ovrp_EndFrame4"); _g = 1; }
        if (_r) {
            /* native per-layer submit: id, head-lock flag, world pose — diff vs ours */
            static int po = 0;
            if (po++ < 24 && layerSubmitPtr) {
                for (int i = 0; i < layerSubmitCount; i++) {
                    const ovrpLayerSubmit *L = layerSubmitPtr[i];
                    if (!L) continue;
                    XRRLOG("PT EndFrame4 f=%d layer[%d/%d] id=%d flags=0x%x pos=(%.4f %.4f %.4f) quat=(%.4f %.4f %.4f %.4f)",
                           frameIndex, i, layerSubmitCount, L->LayerId, L->LayerSubmitFlags,
                           L->Pose.Position.x, L->Pose.Position.y, L->Pose.Position.z,
                           L->Pose.Orientation.x, L->Pose.Orientation.y,
                           L->Pose.Orientation.z, L->Pose.Orientation.w);
                }
            }
            return _r(frameIndex, layerSubmitPtr, layerSubmitCount, commandQueue);
        }
    }
    (void)commandQueue;
    static int o; if (!o) { o = 1; XRRLOG("ovrp_EndFrame4 first call (layers=%d)", layerSubmitCount); }
    return xrr_end_frame(frameIndex, layerSubmitPtr, layerSubmitCount);
}
OVRP_EXPORT ovrpResult ovrp_GetPredictedDisplayTime(int frameIndex, double *outTime) {
    PT_FWD(ovrp_GetPredictedDisplayTime, frameIndex, outTime);
    (void)frameIndex;
    if (!outTime) return ovrpFailure_InvalidParameter;
    *outTime = xrr_predicted_display_time_s();
    return ovrpSuccess;
}

/* --- tracking / input --------------------------------------------------- */
OVRP_EXPORT ovrpResult ovrp_GetNodePoseState3(ovrpStep step, int frameIndex,
        ovrpNode nodeId, ovrpPoseStatef *outState) {
    if (pt_active()) {
        static __typeof__(&ovrp_GetNodePoseState3) _r; static int _g;
        if (!_g) { _r = (__typeof__(&ovrp_GetNodePoseState3))pt_real("ovrp_GetNodePoseState3"); _g = 1; }
        if (_r) {
            ovrpResult rr = _r(step, frameIndex, nodeId, outState);
            /* native eye poses — the ghost diff target (cf. our STEREO/HEADvsEYE logs) */
            if (outState && (nodeId == ovrpNode_EyeLeft || nodeId == ovrpNode_EyeRight)) {
                static int po = 0;
                if (po++ < 16)
                    XRRLOG("PT NodePoseState3 eye=%d pos=(%.4f %.4f %.4f) quat=(%.4f %.4f %.4f %.4f)",
                           (int)nodeId, outState->Pose.Position.x, outState->Pose.Position.y,
                           outState->Pose.Position.z, outState->Pose.Orientation.x,
                           outState->Pose.Orientation.y, outState->Pose.Orientation.z,
                           outState->Pose.Orientation.w);
            }
            return rr;
        }
    }
    (void)step; (void)frameIndex;
    return xrr_get_node_pose(nodeId, outState);
}
OVRP_EXPORT ovrpResult ovrp_GetNodePoseStateRaw(ovrpStep step, int frameIndex,
        ovrpNode nodeId, ovrpPoseStatef *outState) {
    PT_FWD(ovrp_GetNodePoseStateRaw, step, frameIndex, nodeId, outState);
    (void)step; (void)frameIndex;
    return xrr_get_node_pose(nodeId, outState);
}
OVRP_EXPORT ovrpResult ovrp_GetControllerState4(ovrpController controllerMask,
        ovrpControllerState4 *outState) {
    PT_FWD(ovrp_GetControllerState4, controllerMask, outState);
    if (!outState) return ovrpFailure_InvalidParameter;
    xrr_get_controller_state(controllerMask, outState);
    return ovrpSuccess;
}

/* node presence/validity — the game gates controller-pose queries on these */
OVRP_EXPORT ovrpResult ovrp_SetControllerVibration2(ovrpController mask,
        float frequency, float amplitude) {
    PT_FWD(ovrp_SetControllerVibration2, mask, frequency, amplitude);
    xrr_set_vibration(mask, frequency, amplitude);
    return ovrpSuccess;
}

OVRP_EXPORT ovrpResult ovrp_GetNodePresent2(ovrpNode node, ovrpBool *out) {
    PT_FWD(ovrp_GetNodePresent2, node, out);
    if (out) *out = xrr_node_present(node) ? ovrpBool_True : ovrpBool_False;
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_GetNodeOrientationValid(ovrpNode node, ovrpBool *out) {
    PT_FWD(ovrp_GetNodeOrientationValid, node, out);
    if (out) *out = xrr_node_valid(node) ? ovrpBool_True : ovrpBool_False;
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_GetNodePositionValid(ovrpNode node, ovrpBool *out) {
    PT_FWD(ovrp_GetNodePositionValid, node, out);
    if (out) *out = xrr_node_valid(node) ? ovrpBool_True : ovrpBool_False;
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_GetNodeOrientationTracked2(ovrpNode node, ovrpBool *out) {
    PT_FWD(ovrp_GetNodeOrientationTracked2, node, out);
    if (out) *out = xrr_node_valid(node) ? ovrpBool_True : ovrpBool_False;
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_GetNodePositionTracked2(ovrpNode node, ovrpBool *out) {
    PT_FWD(ovrp_GetNodePositionTracked2, node, out);
    if (out) *out = xrr_node_valid(node) ? ovrpBool_True : ovrpBool_False;
    return ovrpSuccess;
}

/* --- system / tracking config ------------------------------------------ */
OVRP_EXPORT ovrpResult ovrp_GetSystemHeadsetType2(ovrpSystemHeadset *outType) {
    PT_FWD(ovrp_GetSystemHeadsetType2, outType);
    if (outType) *outType = ovrpSystemHeadset_Oculus_Quest_2;
    return ovrpSuccess;
}

/* [VERIFIED from UE OculusHMD.cpp + disasm] ovrp_GetInitialized takes NO args and
 * returns ovrpBool DIRECTLY. My earlier (ovrpBool* out) signature wrote into x0
 * (which still held `this`), corrupting FOculusHMD's vtable -> null virtual crash.
 * Return false until the SESSION exists so FOculusHMD::InitDevice proceeds into
 * InitializeSession() (which calls ovrp_Initialize5 to create it). */
OVRP_EXPORT ovrpBool ovrp_GetInitialized(void) {
    PT_FWD(ovrp_GetInitialized);
    /* called every frame, many times — don't log (it rolls the logcat buffer) */
    return (g_xr.session != XR_NULL_HANDLE) ? ovrpBool_True : ovrpBool_False;
}
OVRP_EXPORT ovrpResult ovrp_GetSystemDisplayFrequency2(float *outFreq) {
    PT_FWD(ovrp_GetSystemDisplayFrequency2, outFreq);
    if (outFreq) *outFreq = 72.0f;             /* Quest 2 default refresh */
    return ovrpSuccess;
}

/* per-frame app-state getters the loading loop polls (all (ovrpBool* out)->result) */
OVRP_EXPORT ovrpResult ovrp_GetAppHasVrFocus2(ovrpBool *out) {
    PT_FWD(ovrp_GetAppHasVrFocus2, out);
    if (out) *out = ovrpBool_True;  return ovrpSuccess;    /* we have focus */
}
OVRP_EXPORT ovrpResult ovrp_GetAppShouldQuit2(ovrpBool *out) {
    PT_FWD(ovrp_GetAppShouldQuit2, out);
    if (out) *out = ovrpBool_False; return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_GetUserPresent2(ovrpBool *out) {
    PT_FWD(ovrp_GetUserPresent2, out);
    if (out) *out = ovrpBool_True;  return ovrpSuccess;    /* headset worn */
}
OVRP_EXPORT ovrpResult ovrp_GetAppShouldRecenter2(ovrpBool *out) {
    PT_FWD(ovrp_GetAppShouldRecenter2, out);
    if (out) *out = ovrpBool_False; return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_GetAppShouldRecreateDistortionWindow2(ovrpBool *out) {
    PT_FWD(ovrp_GetAppShouldRecreateDistortionWindow2, out);
    if (out) *out = ovrpBool_False; return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_GetSystemMultiViewSupported2(ovrpBool *out) {
    PT_FWD(ovrp_GetSystemMultiViewSupported2, out);
    if (out) *out = ovrpBool_True;  return ovrpSuccess;    /* Quest supports multiview */
}
OVRP_EXPORT ovrpResult ovrp_GetAppHasInputFocus(ovrpBool *out) {
    PT_FWD(ovrp_GetAppHasInputFocus, out);
    if (out) *out = ovrpBool_True;  return ovrpSuccess;
}
/* the OpenXR session IS our display; nothing extra to set up */
OVRP_EXPORT ovrpResult ovrp_SetupDistortionWindow3(unsigned int flags) {
    PT_FWD(ovrp_SetupDistortionWindow3, flags);
    (void)flags; return ovrpSuccess;
}
/* like GetInitialized, this returns ovrpBool DIRECTLY (no out-param) */
OVRP_EXPORT ovrpBool ovrp_GetMixedRealityInitialized(void) {
    PT_FWD(ovrp_GetMixedRealityInitialized);
    return ovrpBool_False;   /* MR not used */
}
/* [VERIFIED: UE builds the eye projection matrix from this FOV — OculusHMD.cpp
 * line ~2369 uses Fov.{Left,Right,Up,Down}Tan]. Must match the FOV we composite
 * with (g_xr.views[].fov), or the image is reprojected to the wrong frustum. */
OVRP_EXPORT ovrpResult ovrp_GetNodeFrustum2(ovrpNode node, ovrpFrustum2f *out) {
    if (pt_active()) {
        static __typeof__(&ovrp_GetNodeFrustum2) _r; static int _g;
        if (!_g) { _r = (__typeof__(&ovrp_GetNodeFrustum2))pt_real("ovrp_GetNodeFrustum2"); _g = 1; }
        if (_r) {
            ovrpResult rr = _r(node, out);
            if (out) { static int po = 0;
                if (po++ < 8)
                    XRRLOG("PT GetNodeFrustum2 node=%d -> Fov U=%.3f D=%.3f L=%.3f R=%.3f zN=%.3f zF=%.1f",
                           node, out->Fov.UpTan, out->Fov.DownTan, out->Fov.LeftTan,
                           out->Fov.RightTan, out->zNear, out->zFar);
            }
            return rr;
        }
    }
    if (!out) return ovrpFailure_InvalidParameter;
    int eye = (node == ovrpNode_EyeRight) ? 1 : 0;   /* EyeLeft/EyeCenter -> 0 */
    out->zNear = 0.01f;
    out->zFar  = 1000.0f;
    xrr_eye_fov_tangents(eye, &out->Fov.UpTan, &out->Fov.DownTan,
                         &out->Fov.LeftTan, &out->Fov.RightTan);
    static int o = 0;
    if (o++ < 4) XRRLOG("GetNodeFrustum2 node=%d -> Fov U=%.3f D=%.3f L=%.3f R=%.3f",
                        node, out->Fov.UpTan, out->Fov.DownTan, out->Fov.LeftTan, out->Fov.RightTan);
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_GetTrackingOriginType2(ovrpTrackingOrigin *outOrigin) {
    PT_FWD(ovrp_GetTrackingOriginType2, outOrigin);
    if (outOrigin)
        *outOrigin = xrr_get_tracking_origin() ? ovrpTrackingOrigin_FloorLevel
                                                : ovrpTrackingOrigin_EyeLevel;
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_SetTrackingOriginType2(ovrpTrackingOrigin origin) {
    PT_FWD(ovrp_SetTrackingOriginType2, origin);
    /* roomscale games request FloorLevel/Stage; honor it so the floor height (and
     * thus body-anchored inventory) is correct. */
    XRRLOG("ovrp_SetTrackingOriginType2(%d) -> %s", (int)origin,
           origin != ovrpTrackingOrigin_EyeLevel ? "floor" : "eye");
    xrr_set_tracking_origin(origin != ovrpTrackingOrigin_EyeLevel);
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_RecenterTrackingOrigin2(unsigned int flags) {
    PT_FWD(ovrp_RecenterTrackingOrigin2, flags);
    (void)flags; return ovrpSuccess;
}

/* Perf levels: the game asks for CPU/GPU clock levels under load. We used to no-op
 * these, letting the device underclock while CPU-bound -> frame drops. Forward them
 * to XR_EXT_performance_settings so clocks boost. (real sig: one int level) */
OVRP_EXPORT ovrpResult ovrp_SetSystemCpuLevel2(int level) {
    PT_FWD(ovrp_SetSystemCpuLevel2, level);
    xrr_set_perf_level(0, level); return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_SetSystemGpuLevel2(int level) {
    PT_FWD(ovrp_SetSystemGpuLevel2, level);
    xrr_set_perf_level(1, level); return ovrpSuccess;
}

/* Dynamic perf: tiled multi-res (FFR) + GPU frame time. Previously stubbed as
 * Unsupported, which disabled RE4's own GPU-load mitigation. We now report FFR
 * supported, forward the game's Set* requests onto OpenXR foveation (xr_runtime.c),
 * and feed GetGPUFrameTime so the game's scaler has an input. */
OVRP_EXPORT ovrpResult ovrp_GetTiledMultiResSupported(ovrpBool *outSupported) {
    PT_FWD(ovrp_GetTiledMultiResSupported, outSupported);
    if (!outSupported) return ovrpFailure_InvalidParameter;
    *outSupported = xrr_foveation_supported() ? ovrpBool_True : ovrpBool_False;
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_GetTiledMultiResLevel(ovrpTiledMultiResLevel *outLevel) {
    PT_FWD(ovrp_GetTiledMultiResLevel, outLevel);
    if (!outLevel) return ovrpFailure_InvalidParameter;
    *outLevel = (ovrpTiledMultiResLevel)xrr_get_tiled_multires_level();
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_SetTiledMultiResLevel(ovrpTiledMultiResLevel level) {
    PT_FWD(ovrp_SetTiledMultiResLevel, level);
    xrr_set_tiled_multires_level((int)level);
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_GetTiledMultiResDynamic(ovrpBool *outDynamic) {
    PT_FWD(ovrp_GetTiledMultiResDynamic, outDynamic);
    if (!outDynamic) return ovrpFailure_InvalidParameter;
    *outDynamic = xrr_get_tiled_multires_dynamic() ? ovrpBool_True : ovrpBool_False;
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_SetTiledMultiResDynamic(ovrpBool isDynamic) {
    PT_FWD(ovrp_SetTiledMultiResDynamic, isDynamic);
    xrr_set_tiled_multires_dynamic(isDynamic != ovrpBool_False);
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_GetGPUFrameTime(float *outGpuTimeMs) {
    PT_FWD(ovrp_GetGPUFrameTime, outGpuTimeMs);
    if (!outGpuTimeMs) return ovrpFailure_InvalidParameter;
    *outGpuTimeMs = xrr_gpu_frame_time_ms();
    return ovrpSuccess;
}

/* Lever A — dynamic resolution. Previously stubbed Unsupported, leaving the game's preset
 * scale=1.0 (full res always). We return scale<1 under GPU pressure; the game self-downscales
 * its eye buffer (resolution = baseDensity * sqrt(scale)). See xr_runtime.c / game-perf-RE.md. */
OVRP_EXPORT ovrpResult ovrp_GetAdaptiveGpuPerformanceScale2(float *outScale) {
    PT_FWD(ovrp_GetAdaptiveGpuPerformanceScale2, outScale);
    if (!outScale) return ovrpFailure_InvalidParameter;
    *outScale = xrr_adaptive_gpu_scale();
    return ovrpSuccess;
}

/* Perf metrics: per-metric support + read. Previously the whole call returned
 * Unsupported, which the game (correctly) treats as "no perf metrics at all" and may
 * gate its adaptive systems on. We answer per metric truthfully instead. */
OVRP_EXPORT ovrpResult ovrp_IsPerfMetricsSupported(ovrpPerfMetrics metric, ovrpBool *outSupported) {
    PT_FWD(ovrp_IsPerfMetricsSupported, metric, outSupported);
    if (!outSupported) return ovrpFailure_InvalidParameter;
    *outSupported = xrr_perf_metric_supported((int)metric) ? ovrpBool_True : ovrpBool_False;
    return ovrpSuccess;
}
OVRP_EXPORT ovrpResult ovrp_GetPerfMetricsFloat(ovrpPerfMetrics metric, float *outValue) {
    PT_FWD(ovrp_GetPerfMetricsFloat, metric, outValue);
    if (!outValue) return ovrpFailure_InvalidParameter;
    return xrr_perf_metric_float((int)metric, outValue) ? ovrpSuccess : ovrpFailure_Unsupported;
}
OVRP_EXPORT ovrpResult ovrp_GetPerfMetricsInt(ovrpPerfMetrics metric, int *outValue) {
    PT_FWD(ovrp_GetPerfMetricsInt, metric, outValue);
    if (!outValue) return ovrpFailure_InvalidParameter;
    return xrr_perf_metric_int((int)metric, outValue) ? ovrpSuccess : ovrpFailure_Unsupported;
}
