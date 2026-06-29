/* ovrplugin_shim.h — OVRPlugin (v1.51) C ABI for the RE4 VR -> OpenXR shim.
 *
 * Goal: a drop-in replacement libOVRPlugin.so that re-exports the ovrp_* symbols
 * RE4 VR (com.Armature.VR4, OVRPlugin 1.51 / pkg 19.0.0.449.531) calls, backed by
 * OpenXR (Monado) on Steam Frame instead of Meta's libvrapi.so.
 *
 * Provenance of each declaration:
 *   [VERIFIED]   struct size / arg shape confirmed from Ghidra decompilation of the
 *                actual binary (see analysis/ovrp_decomp.txt, RE-NOTES.md).
 *   [HEADER]     taken from public OVRPlugin.cs @ v1.51; layout trusted because the
 *                VERIFIED structs matched it byte-for-byte, but not independently
 *                re-confirmed against this binary yet.
 *   [TODO]       signature not yet finalized — placeholder, do not trust arg list.
 *
 * Status: scaffold. Base types + the fully-confirmed functions are real; the rest
 * of the ~239-function surface (analysis/shim_surface.txt) is still to be filled.
 */
#ifndef OVRPLUGIN_SHIM_H
#define OVRPLUGIN_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The real lib's exported ovrp_* are thin thunks; we just need matching symbols
 * with default visibility so the game's dlsym resolves to us. */
#define OVRP_EXPORT __attribute__((visibility("default")))

/* ------------------------------------------------------------------ result --- */
/* ovrpResult is a signed 32-bit int; success >= 0, failure < 0.
 * Codes below VERIFIED from return constants in the decompiled binary:
 *   -1001 (0xfffffc17), -1002 (0xfffffc16), -1003 (0xfffffc15). */
typedef int32_t ovrpResult;
#define ovrpSuccess                       0
#define ovrpSuccess_EventUnavailable      1
#define ovrpFailure                      -1000
#define ovrpFailure_InvalidParameter     -1001  /* [VERIFIED] */
#define ovrpFailure_NotInitialized       -1002  /* [VERIFIED] */
#define ovrpFailure_InvalidOperation     -1003  /* [VERIFIED] */
#define ovrpFailure_Unsupported          -1004
#define ovrpFailure_NotYetImplemented    -1005
#define ovrpFailure_OperationFailed      -1006
#define ovrpFailure_InsufficientSize     -1007
#define ovrpFailure_DataIsInvalid        -1008
#define ovrpFailure_DeprecatedOperation  -1009
#define OVRP_SUCCESS(r) ((r) >= 0)

typedef enum { ovrpBool_False = 0, ovrpBool_True = 1 } ovrpBool;

/* -------------------------------------------------------------- math types --- */
typedef struct { float x, y;          } ovrpVector2f;
typedef struct { int32_t x, y;        } ovrpVector2i;
typedef struct { int32_t w, h;        } ovrpSizei;
typedef struct { ovrpVector2i Pos; ovrpSizei Size; } ovrpRecti;
typedef struct { float x, y, z;       } ovrpVector3f;
typedef struct { float x, y, z, w;    } ovrpVector4f;
typedef struct { float x, y, z, w;    } ovrpQuatf;
typedef struct { float w, h;          } ovrpSizef;
typedef struct { ovrpVector2f Pos; ovrpSizef Size; } ovrpRectf;
typedef struct { float UpTan, DownTan, LeftTan, RightTan; } ovrpFovf;
typedef struct { float zNear, zFar; ovrpFovf Fov; } ovrpFrustum2f;

typedef struct { ovrpQuatf Orientation; ovrpVector3f Position; } ovrpPosef; /* 28 */

typedef struct {
    ovrpPosef    Pose;
    ovrpVector3f Velocity;
    ovrpVector3f Acceleration;
    ovrpVector3f AngularVelocity;
    ovrpVector3f AngularAcceleration;
    double       Time;
} ovrpPoseStatef;                                                          /* 88 */

/* binary-verified sizes — header is wrong if these fail */
_Static_assert(sizeof(ovrpPosef) == 28,      "ovrpPosef must be 28 bytes");
_Static_assert(sizeof(ovrpPoseStatef) == 88, "ovrpPoseStatef must be 0x58=88 [VERIFIED]");

/* --------------------------------------------------------------- enums ------- */
typedef enum {
    ovrpNode_None = -1, ovrpNode_EyeLeft = 0, ovrpNode_EyeRight = 1,
    ovrpNode_EyeCenter = 2, ovrpNode_HandLeft = 3, ovrpNode_HandRight = 4,
    ovrpNode_TrackerZero = 5, ovrpNode_TrackerOne = 6, ovrpNode_TrackerTwo = 7,
    ovrpNode_TrackerThree = 8, ovrpNode_Head = 9, ovrpNode_DeviceObjectZero = 10,
    ovrpNode_EnumSize = 0x7fffffff
} ovrpNode;

typedef enum {
    ovrpStep_Render = -1, ovrpStep_Physics = 0, ovrpStep_EnumSize = 0x7fffffff
} ovrpStep;

typedef enum {
    ovrpController_None = 0, ovrpController_LTouch = 0x01, ovrpController_RTouch = 0x02,
    ovrpController_Touch = 0x03, ovrpController_Remote = 0x04, ovrpController_Gamepad = 0x10,
    ovrpController_LTrackedRemote = 0x01000000, ovrpController_RTrackedRemote = 0x02000000,
    ovrpController_Touchpad = 0x08000000, ovrpController_Active = 0x80000000u,
    ovrpController_EnumSize = 0x7fffffff
} ovrpController;

typedef enum {
    ovrpSystemHeadset_None = 0,
    /* mobile */
    ovrpSystemHeadset_Oculus_Quest = 8, ovrpSystemHeadset_Oculus_Quest_2 = 9,
    ovrpSystemHeadset_EnumSize = 0x7fffffff
} ovrpSystemHeadset;

typedef enum {
    ovrpTrackingOrigin_EyeLevel = 0, ovrpTrackingOrigin_FloorLevel = 1,
    ovrpTrackingOrigin_Stage = 2, ovrpTrackingOrigin_EnumSize = 0x7fffffff
} ovrpTrackingOrigin;

/* ----------------------------------------------------------- controller ------ */
typedef struct {
    uint32_t ConnectedControllers;
    uint32_t Buttons;
    uint32_t Touches;
    uint32_t NearTouches;
    float    LIndexTrigger, RIndexTrigger;
    float    LHandTrigger,  RHandTrigger;
    ovrpVector2f LThumbstick, RThumbstick;
    ovrpVector2f LTouchpad,   RTouchpad;
    uint8_t  LBatteryPercentRemaining, RBatteryPercentRemaining;
    uint8_t  LRecenterCount, RRecenterCount;
    uint8_t  Reserved[28];
} ovrpControllerState4;                                                    /* 96 */
_Static_assert(sizeof(ovrpControllerState4) == 96,
               "ovrpControllerState4 must be 0x60=96 [VERIFIED]");

/* ----------------------------------------------------------- layers ---------- */
/* ovrpShape: Compositor::ImportLayerDesc switches on field +0x00. Cases
 * 0,1,2,4,5 verified as overlay shapes; case 3 = EyeFov (projection). [VERIFIED] */
typedef enum {
    ovrpShape_Quad = 0, ovrpShape_Cylinder = 1, ovrpShape_Cubemap = 2,
    ovrpShape_EyeFov = 3, ovrpShape_OffcenterCubemap = 4, ovrpShape_Equirect = 5,
    ovrpShape_ReconstructionPassthrough = 7, ovrpShape_SurfaceProjectedPassthrough = 8,
    ovrpShape_Fisheye = 9, ovrpShape_EnumSize = 0xF
} ovrpShape;
typedef enum {
    ovrpLayout_Stereo = 0, ovrpLayout_Mono = 1, ovrpLayout_DoubleWide = 2,
    ovrpLayout_Array = 3, ovrpLayout_EnumSize = 0xF
} ovrpLayout;
typedef enum {
    ovrpTextureFormat_R8G8B8A8_sRGB = 0, ovrpTextureFormat_R8G8B8A8 = 1,
    ovrpTextureFormat_R16G16B16A16_FP = 2, ovrpTextureFormat_R11G11B10_FP = 3,
    ovrpTextureFormat_B8G8R8A8_sRGB = 4, ovrpTextureFormat_B8G8R8A8 = 5,
    ovrpTextureFormat_R5G6B5 = 11, ovrpTextureFormat_EnumSize = 0x7fffffff
} ovrpTextureFormat;

/* ovrpLayerDesc — VERIFIED: Compositor::ImportLayerDesc memset's 0x7c=124 and
 * copies 0x68/0x6c/0x7c by version (base / +DepthFormat / +MotionVector). The
 * field layout below reproduces those three sizes exactly. */
typedef struct {
    ovrpShape         Shape;            /* +0x00  switch field [VERIFIED]      */
    ovrpLayout        Layout;           /* +0x04                               */
    ovrpSizei         TextureSize;      /* +0x08                               */
    int               MipLevels;        /* +0x10                               */
    int               SampleCount;      /* +0x14                               */
    ovrpTextureFormat Format;           /* +0x18                               */
    int               LayerFlags;       /* +0x1c  (common header ends +0x20)   */
    ovrpFovf          Fov[2];           /* +0x20                               */
    ovrpRectf         VisibleRect[2];   /* +0x40                               */
    ovrpSizei         MaxViewportSize;  /* +0x60                               */
    ovrpTextureFormat DepthFormat;      /* +0x68  (-> 0x6c = UE 4.25 EyeFov)   */
    /* NOTE: the binary's internal union also supports a 124-byte variant with
     * MotionVector* fields, but UE 4.25's ovrpLayerDesc_EyeFov ends here (108).
     * We match UE's size so we never write past the caller's buffer. */
} ovrpLayerDesc;                                                         /* 108 */
typedef ovrpLayerDesc ovrpLayerDesc_EyeFov;
_Static_assert(sizeof(ovrpLayerDesc) == 0x6c,
               "ovrpLayerDesc must be 0x6c=108 [UE 4.25 ovrpLayerDesc_EyeFov]");

/* ovrpLayerSubmit — VERIFIED 0x130=304 bytes (EndFrame4 allocs count*0x130).
 * Header fields are the public layout [HEADER]; the per-shape union tail is not
 * yet broken out -> reserved bytes to reach 304. [TODO] reverse ShapeData. */
typedef struct {
    int          LayerId;          /* +0x00 */
    int          TextureStage;     /* +0x04 */
    ovrpRecti    ViewportRect[2];  /* +0x08 */
    ovrpPosef    Pose;             /* +0x28 */
    int          LayerSubmitFlags; /* +0x44  (ovrpLayerSubmitFlag_HeadLocked=1<<0) */
    ovrpVector4f ColorScale;       /* +0x48  (added 1.31) */
    ovrpVector4f ColorOffset;      /* +0x58 */
    int          OverrideTextureRectMatrix;          /* +0x68 (ovrpBool=int, added 1.34) */
    float        TextureRectMatrix[16];              /* +0x6C (ovrpTextureRectMatrixf=64B) */
    int          OverridePerLayerColorScaleAndOffset;/* +0xAC */
    ovrpSizef    QuadSize;         /* +0xB0  (ovrpLayerSubmit_Quad tail; world meters) */
    unsigned char _pad[0x130 - 0xB8];                /* reach the 304B union stride     */
} ovrpLayerSubmit;                                                       /* 304 */
enum { ovrpLayerSubmitFlag_HeadLocked = (1 << 0) };
_Static_assert(sizeof(ovrpLayerSubmit) == 0x130,
               "ovrpLayerSubmit must be 0x130=304 [VERIFIED EndFrame4 stride]");

/* =================================================================== API ===== */
/* CONFIRMED (arg shape matched in Ghidra) */
OVRP_EXPORT ovrpResult ovrp_GetNodePoseState3(    /* [VERIFIED] 3 ints + out ptr */
    ovrpStep step, int frameIndex, ovrpNode nodeId, ovrpPoseStatef *outState);
OVRP_EXPORT ovrpResult ovrp_GetNodePoseStateRaw(  /* [VERIFIED] same shape, 0x58 */
    ovrpStep step, int frameIndex, ovrpNode nodeId, ovrpPoseStatef *outState);
OVRP_EXPORT ovrpResult ovrp_GetControllerState4(  /* [VERIFIED] mask + out 0x60   */
    ovrpController controllerMask, ovrpControllerState4 *outState);

/* CORE — frame loop / lifecycle  [HEADER: from v1.51, arg list to re-verify] */
/* ovrpRenderAPIType — apiType arg of Initialize5 */
typedef enum {
    ovrpRenderAPI_None = 0, ovrpRenderAPI_OpenGL = 1, ovrpRenderAPI_Android_GLES = 2,
    ovrpRenderAPI_Vulkan = 4, ovrpRenderAPI_EnumSize = 0x7fffffff
} ovrpRenderAPIType;
typedef void (*ovrpLogCallback)(int level, const char *message);

OVRP_EXPORT ovrpResult ovrp_PreInitialize3(void *logCallback);             /* [TODO] */
/* [VERIFIED from UE OculusHMD.cpp InitializeSession()] exact arg order:
 * (apiType, logCallback, activity, VkInstance, VkPhysicalDevice, VkDevice,
 *  VkQueue, flags, version). arg7 is a VkQueue handle, NOT a family index. */
OVRP_EXPORT ovrpResult ovrp_Initialize5(
    ovrpRenderAPIType apiType, ovrpLogCallback logCallback, void *activity,
    void *vkInstance, void *vkPhysicalDevice, void *vkDevice, void *queue,
    unsigned int flags, const void *version);   /* real 9th arg is const ovrpVersion& == a
        64-bit pointer; declaring it as a 32-bit int truncates it (crashes P4 passthru forward). */
OVRP_EXPORT ovrpResult ovrp_Shutdown2(void);
OVRP_EXPORT ovrpResult ovrp_Update3(ovrpStep step, int frameIndex, double predictedTime);
OVRP_EXPORT ovrpResult ovrp_WaitToBeginFrame(int frameIndex);
OVRP_EXPORT ovrpResult ovrp_BeginFrame4(int frameIndex, void *commandQueue);     /* [HEADER] */
OVRP_EXPORT ovrpResult ovrp_EndFrame4(int frameIndex,
    const ovrpLayerSubmit *const *layerSubmitPtr, int layerSubmitCount,
    void *commandQueue);                                                          /* [HEADER] */
OVRP_EXPORT ovrpResult ovrp_GetPredictedDisplayTime(int frameIndex, double *outTime);

/* CORE — system / eye params  [HEADER] */
OVRP_EXPORT ovrpResult ovrp_GetSystemHeadsetType2(ovrpSystemHeadset *outType);
OVRP_EXPORT ovrpResult ovrp_GetTrackingOriginType2(ovrpTrackingOrigin *outOrigin);
OVRP_EXPORT ovrpResult ovrp_SetTrackingOriginType2(ovrpTrackingOrigin origin);
OVRP_EXPORT ovrpResult ovrp_RecenterTrackingOrigin2(unsigned int flags);

/* DYNAMIC PERF — tiled multi-resolution (Oculus name for Fixed Foveated Rendering)
 * + GPU frame time. RE4 drives its own dynamic-perf loop through these; the shim
 * forwards the requested FFR level onto OpenXR XR_FB_foveation (see xr_runtime.c)
 * and answers GetGPUFrameTime with a measured frame-time estimate so the loop has
 * an input. Signatures verified against libOVRPlugin.so (analysis/endframe_impls.txt,
 * analysis/ovrp_decomp.txt): Get* take an out-pointer, Set* take a value. */
typedef enum {
    ovrpTiledMultiResLevel_Off        = 0,
    ovrpTiledMultiResLevel_LMSLow     = 1,
    ovrpTiledMultiResLevel_LMSMedium  = 2,
    ovrpTiledMultiResLevel_LMSHigh    = 3,
    ovrpTiledMultiResLevel_LMSHighTop = 4,
    ovrpTiledMultiResLevel_EnumSize   = 0x7fffffff
} ovrpTiledMultiResLevel;
OVRP_EXPORT ovrpResult ovrp_GetTiledMultiResSupported(ovrpBool *outSupported);
OVRP_EXPORT ovrpResult ovrp_GetTiledMultiResLevel(ovrpTiledMultiResLevel *outLevel);
OVRP_EXPORT ovrpResult ovrp_SetTiledMultiResLevel(ovrpTiledMultiResLevel level);
OVRP_EXPORT ovrpResult ovrp_GetTiledMultiResDynamic(ovrpBool *outDynamic);
OVRP_EXPORT ovrpResult ovrp_SetTiledMultiResDynamic(ovrpBool isDynamic);
OVRP_EXPORT ovrpResult ovrp_GetGPUFrameTime(float *outGpuTimeMs);

/* Perf metrics. The game polls IsPerfMetricsSupported(metric) and, for supported
 * metrics, GetPerfMetrics{Float,Int}(metric, &out). Signatures verified against
 * libOVRPlugin.so (analysis/ovrp_decomp.txt): (uint metric, out-ptr). We answer per
 * metric truthfully (see xr_runtime.c) rather than stubbing the whole call Unsupported. */
typedef enum {
    ovrpPerfMetrics_App_CpuTime_Float                    = 0,
    ovrpPerfMetrics_App_GpuTime_Float                    = 1,
    ovrpPerfMetrics_Compositor_CpuTime_Float             = 3,
    ovrpPerfMetrics_Compositor_GpuTime_Float             = 4,
    ovrpPerfMetrics_Compositor_DroppedFrameCount_Int     = 5,
    ovrpPerfMetrics_System_GpuUtilPercentage_Float       = 7,
    ovrpPerfMetrics_System_CpuUtilAveragePercentage_Float= 8,
    ovrpPerfMetrics_System_CpuUtilWorstPercentage_Float  = 9,
    ovrpPerfMetrics_Device_CpuClockFrequencyInMHz_Float  = 10,
    ovrpPerfMetrics_Device_GpuClockFrequencyInMHz_Float  = 11,
    ovrpPerfMetrics_Device_CpuClockLevel_Int             = 12,
    ovrpPerfMetrics_Device_GpuClockLevel_Int             = 13,
    ovrpPerfMetrics_Count                                = 14,
    ovrpPerfMetrics_EnumSize                             = 0x7fffffff
} ovrpPerfMetrics;
OVRP_EXPORT ovrpResult ovrp_IsPerfMetricsSupported(ovrpPerfMetrics metric, ovrpBool *outSupported);
OVRP_EXPORT ovrpResult ovrp_GetPerfMetricsFloat(ovrpPerfMetrics metric, float *outValue);
OVRP_EXPORT ovrpResult ovrp_GetPerfMetricsInt(ovrpPerfMetrics metric, int *outValue);

/* TODO: remaining ~225 of the 239-function surface; see analysis/shim_surface.txt.
 * Buckets (per SHIM-SCOPE.md): ~150 stub-to-constant (Media_, camera, perf,
 * boundary, handtracking, system-getters), ~45 mechanical (rest of tracking,
 * input, eye params), and the hard layer/swapchain set (SetupLayer,
 * CalculateEyeLayerDesc2, GetLayerTexture2) which need the 128-byte
 * ovrpLayerDesc reversed first. */

#ifdef __cplusplus
}
#endif
#endif /* OVRPLUGIN_SHIM_H */
