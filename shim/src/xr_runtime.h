/* xr_runtime.h — OpenXR session + frame-loop engine behind the ovrp_* frame fns.
 * Core OpenXR only (no graphics-API headers); the Vulkan graphics binding +
 * swapchains are handled separately once the ovrp_Initialize5 handshake is
 * reversed. This module owns the instance/system/session/spaces + frame pacing. */
#ifndef XR_RUNTIME_H
#define XR_RUNTIME_H

#include <openxr/openxr.h>
#include "ovrplugin_shim.h"

#define XRR_MAX_LAYERS  16
#define XRR_MAX_IMAGES  8

typedef struct {
    XrSwapchain swapchain;        /* color                              */
    XrSwapchain depthSwapchain;   /* optional (SetupLayerDepth)         */
    uint32_t    imageCount;       /* = stage count                      */
    uint32_t    width, height, arraySize;
    int64_t     colorFormat;
    int64_t     depthFormat;      /* 0 = no depth swapchain             */
    uint64_t    colorImages[XRR_MAX_IMAGES];  /* VkImage handles -> app  */
    uint64_t    depthImages[XRR_MAX_IMAGES];
    /* copy-ring: shim color images UE renders into (decoupled from OpenXR swapchain) */
    uint64_t    shimImages[XRR_MAX_IMAGES];
    uint64_t    shimMem[XRR_MAX_IMAGES];
    int         shimCount;        /* >0 = copy-ring active for this layer */
    uint32_t    acquiredIndex;    /* this frame's acquired (render) image */
    uint32_t    depthAcquiredIndex;
    int         depthAcquired;    /* paired depth image held this frame   */
    int         imageAcquired;    /* this frame's render image is held    */
    /* render-ahead pipeline: the image rendered LAST frame is held one extra
     * frame so its tile-memory flush completes off the critical path. */
    int         presentPending;   /* holding last frame's image to present */
    int         presentToken;     /* its flush fence token (xrr_vk_flush_*) */
    uint32_t    presentIndex;     /* its swapchain index (diagnostics)      */
    /* present-on-submit (debug.re4vr.submithook=2): image held from this frame's
     * acquire until UE's eye-render vkQueueSubmit fires, then resolved+released. */
    int         deferColor;       /* color image awaiting deferred present  */
    int         deferDepth;       /* depth image awaiting deferred present  */
    uint32_t    deferColorIndex;  /* held color swapchain index             */
    uint32_t    deferDepthIndex;  /* held depth swapchain index             */
    int         active;
    int         isEyeFov;         /* projection vs overlay              */
    int         layout;           /* ovrpLayout (Array vs side-by-side) */
} XrLayer;

typedef struct {
    XrInstance   instance;
    XrSystemId   systemId;
    XrSession    session;
    XrSpace      appSpace;      /* LOCAL or STAGE — tracking origin */
    XrSpace      viewSpace;     /* VIEW — head pose                 */
    XrSessionState sessionState;
    XrViewConfigurationType viewConfigType;

    /* per-frame state from the last xrWaitFrame */
    XrFrameState frameState;
    XrView       views[2];      /* located eye views                */
    uint32_t     viewCount;
    int          running;       /* xrBeginSession done               */
    int          inFrame;       /* between BeginFrame and EndFrame   */

    XrLayer      layers[XRR_MAX_LAYERS];
    int          layerCount;
} XrRuntime;

extern XrRuntime g_xr;

/* lifecycle (ovrp_PreInitialize3 / ovrp_Initialize5 / ovrp_Shutdown2) */
ovrpResult xrr_pre_init(void);   /* create instance + pick system        */
/* create session from the app's Vulkan handles (Initialize5 args 5-8) + spaces */
ovrpResult xrr_init(void *vkInstance, void *vkPhysicalDevice, void *vkDevice,
                    unsigned int queueFamilyIndex);
void       xrr_shutdown(void);

/* defined in vk_session.c (Vulkan-typed, kept out of this core-only header) */
int        xrr_create_session_vulkan(void *vkInstance, void *vkPhysicalDevice,
                                      void *vkDevice, unsigned int queueFamilyIndex);

/* frame loop */
void       xrr_poll_events(void);                 /* session state machine    */
ovrpResult xrr_wait_frame(int frameIndex);        /* xrWaitFrame + locate views */
ovrpResult xrr_begin_frame(int frameIndex);       /* xrBeginFrame             */
ovrpResult xrr_end_frame(int frameIndex,
               const ovrpLayerSubmit *const *layers, int layerCount);
double     xrr_predicted_display_time_s(void);    /* seconds                  */

/* pose query (ovrp_GetNodePoseState3) */
ovrpResult xrr_get_node_pose(ovrpNode node, ovrpPoseStatef *out);
void       xrr_eye_fov_tangents(int eye, float *up, float *down, float *left, float *right);

/* layers / swapchains (ovrp_SetupLayer / GetLayerTextureStageCount / GetLayerTexture2) */
ovrpResult xrr_setup_layer(const ovrpLayerDesc *desc, int *outLayerId);
void       xrr_destroy_layer(int layerId);
ovrpResult xrr_setup_layer_depth(int layerId, const ovrpLayerDesc *depthDesc);
int        xrr_layer_stage_count(int layerId);
ovrpResult xrr_get_layer_texture(int layerId, int stage, int eye,
                                 uint64_t *outColor, uint64_t *outDepth);
/* defined in vk_session.c — enumerate swapchain VkImages as uint64 handles */
uint32_t   xrr_vk_enumerate_images(XrSwapchain sc, uint64_t *out, uint32_t max);

/* recommended per-eye render size from the view configuration */
void       xrr_recommended_eye_size(uint32_t *w, uint32_t *h);

/* tracking origin (xr_runtime.c) — 1 = floor (STAGE), 0 = eye level (LOCAL) */
void       xrr_set_tracking_origin(int floor);
int        xrr_get_tracking_origin(void);
/* apply the game's CPU/GPU perf-level request via XR_EXT_performance_settings */
void       xrr_set_perf_level(int isGpu, int level);

/* game dynamic-perf bridge (xr_runtime.c <- core.c ovrp_*TiledMultiRes* / GPUFrameTime).
 * Forwards RE4's own FFR scaling onto XR_FB_foveation and feeds it a GPU-time estimate. */
int        xrr_foveation_supported(void);              /* GetTiledMultiResSupported  */
void       xrr_set_tiled_multires_level(int ovrpLevel);/* SetTiledMultiResLevel (0..4)*/
int        xrr_get_tiled_multires_level(void);         /* GetTiledMultiResLevel       */
void       xrr_set_tiled_multires_dynamic(int on);     /* SetTiledMultiResDynamic     */
int        xrr_get_tiled_multires_dynamic(void);       /* GetTiledMultiResDynamic     */
float      xrr_gpu_frame_time_ms(void);                /* GetGPUFrameTime source (ms) */
float      xrr_adaptive_gpu_scale(void);               /* GetAdaptiveGpuPerformanceScale2 (Lever A) */
/* perf metrics (ovrp_IsPerfMetricsSupported / GetPerfMetrics{Float,Int}); metric =
 * ovrpPerfMetrics id. *_supported returns 1/0; getters return 1+write *out, else 0. */
int        xrr_perf_metric_supported(int metric);
int        xrr_perf_metric_float(int metric, float *out);
int        xrr_perf_metric_int(int metric, int *out);

/* render-submit race fix (debug.re4vr.submithook). xrr_install_submit_hook patches UE's
 * global VulkanDynamicAPI::vkQueueSubmit to a trampoline (vk_session.c); the trampoline
 * calls xrr_on_ue_submit after each UE submit so present can be ordered after UE's
 * eye-render submit. isRenderQueue = UE submitted to the shim's graphics queue. */
int        xrr_install_submit_hook(void);
void       xrr_on_ue_submit(uint64_t queue, uint64_t fence, int isRenderQueue);

/* Vulkan barrier infra (vk_session.c) — flush UE's tile-memory render to main
 * memory before the OpenXR compositor reads the swapchain image. */
void       xrr_vk_set_handles(void *device, void *queue, unsigned int family);
void       xrr_vk_teardown(void);   /* reset shim Vulkan state for clean re-init (called from shutdown) */
/* render-ahead flush: submit the barrier without blocking (returns a ring token,
 * or -1 if Vulkan isn't ready), wait it a frame later, ready() probes the ring. */
int        xrr_vk_flush_submit(uint64_t image, unsigned int arrayLayers);
int        xrr_vk_flush_submit_ex(uint64_t image, unsigned int arrayLayers, int isDepth);
void       xrr_vk_flush_wait(int token);
/* Block until UE's VkQueue is fully idle (vkQueueWaitIdle). Diagnostic probe for the
 * render-submit race: forces all queue work to complete before we resolve/present. */
void       xrr_vk_queue_wait_idle(void);
void       xrr_vk_device_wait_idle(void);
/* copy-ring (vk_session.c): shim images UE renders into + pipelined resolve-copy */
int        xrr_vk_alloc_images(uint64_t *out, uint64_t *outMem, int count,
                               unsigned int w, unsigned int h, unsigned int arraySize, long long vkFormat);
int        xrr_vk_alloc_images_ex(uint64_t *out, uint64_t *outMem, int count,
                               unsigned int w, unsigned int h, unsigned int arraySize, long long vkFormat, int isDepth);
void       xrr_vk_free_images(uint64_t *imgs, uint64_t *mem, int count);
int        xrr_vk_copy_submit(uint64_t srcShim, uint64_t dstXr,
                              unsigned int w, unsigned int h, unsigned int arrayLayers);
int        xrr_vk_copy_submit_ex(uint64_t srcShim, uint64_t dstXr,
                              unsigned int w, unsigned int h, unsigned int arrayLayers, int isDepth);
int        xrr_vk_flush_ready(void);
/* one-shot debug readback of one array layer to a downsampled PPM (debug.re4vr.dump) */
void       xrr_vk_dump_image(uint64_t image, unsigned int w, unsigned int h,
                             unsigned int arrayLayer, const char *path);
/* stamp a black/white binary barcode of `value` (the frameIndex) into the top-left of
 * one array layer, AFTER UE's resolve, so it rides on every frame incl. black ones —
 * frame-exact video<->log correlation (debug.re4vr.barcode). Validation-only. */
void       xrr_vk_stamp_barcode(uint64_t image, unsigned int w, unsigned int h,
                                unsigned int arrayLayer, unsigned int value, int flagged);
/* per-frame black detector: max luminance (0..255) over a few rows of the resolved eye
 * image; ~0 => truncated/black frame. Covers the whole black tail (debug.re4vr.lumagate). */
int        xrr_vk_frame_luma(uint64_t image, unsigned int w, unsigned int h, unsigned int arrayLayer);

/* input (xr_input.c) — OpenXR action sets -> ovrpControllerState4 + hand poses */
int        xrr_input_init(void);
void       xrr_input_sync(void);
void       xrr_get_controller_state(unsigned int mask, ovrpControllerState4 *out);
int        xrr_get_hand_pose(int node, ovrpPoseStatef *out);
int        xrr_node_present(int node);
int        xrr_node_valid(int node);
void       xrr_set_vibration(unsigned int mask, float frequency, float amplitude);

/* Android instance handshake (android_init.c). No-ops on the host build so the
 * same xr_runtime.c serves both. JavaVM is captured via JNI_OnLoad; the activity
 * comes from Initialize5 arg4, with an Application-context reflection fallback. */
int   xrr_android_init_loader(void);    /* xrInitializeLoaderKHR (vm+context)     */
void *xrr_android_instance_next(void);  /* &XrInstanceCreateInfoAndroidKHR | NULL  */
void  xrr_set_android_activity(void *activity);
int   xrr_android_have_real_activity(void);
void *xrr_android_get_vm(void);   /* JavaVM* captured in JNI_OnLoad (NULL on host) */

/* helpers */
static inline void ovrp_pose_from_xr(const XrPosef *in, ovrpPosef *out) {
    out->Orientation.x = in->orientation.x; out->Orientation.y = in->orientation.y;
    out->Orientation.z = in->orientation.z; out->Orientation.w = in->orientation.w;
    out->Position.x = in->position.x; out->Position.y = in->position.y;
    out->Position.z = in->position.z;
}

#endif /* XR_RUNTIME_H */
