/* layers.c — ovrp_* layer/swapchain functions, backed by xr_runtime swapchains.
 * SetupLayer creates an XrSwapchain from the (reversed) ovrpLayerDesc; the app
 * gets the per-stage VkImage handles via GetLayerTexture2 and wraps them in its
 * RHI. Depth swapchains + foveation (FFR) are created in xr_runtime's setup_layer.
 */
#include "ovrplugin_shim.h"
#include "xr_runtime.h"
#include "passthru.h"
#include "log.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* [VERIFIED arg order from UE OculusHMD.cpp]: (layout, textureScale, mipLevels,
 * sampleCount, colorFormat, depthFormat, layerFlags, out). Fills the EyeFov layer
 * desc UE uses to create the eye swapchain (-> ovrp_SetupLayer). */
OVRP_EXPORT ovrpResult ovrp_CalculateEyeLayerDesc2(
        ovrpLayout layout, float textureScale, int mipLevels, int sampleCount,
        ovrpTextureFormat colorFormat, ovrpTextureFormat depthFormat,
        int layerFlags, ovrpLayerDesc *out) {
    if (pt_active()) {
        static __typeof__(&ovrp_CalculateEyeLayerDesc2) _r; static int _g;
        if (!_g) { _r = (__typeof__(&ovrp_CalculateEyeLayerDesc2))pt_real("ovrp_CalculateEyeLayerDesc2"); _g = 1; }
        if (_r) {
            ovrpResult rr = _r(layout, textureScale, mipLevels, sampleCount,
                               colorFormat, depthFormat, layerFlags, out);
            if (out) { static int po = 0;
                if (po++ < 4)
                    XRRLOG("PT EyeLayerDesc2 %dx%d FovL(U%.3f D%.3f L%.3f R%.3f) FovR(U%.3f D%.3f L%.3f R%.3f)",
                           out->TextureSize.w, out->TextureSize.h,
                           out->Fov[0].UpTan, out->Fov[0].DownTan, out->Fov[0].LeftTan, out->Fov[0].RightTan,
                           out->Fov[1].UpTan, out->Fov[1].DownTan, out->Fov[1].LeftTan, out->Fov[1].RightTan);
            }
            return rr;
        }
    }
    if (!out) return ovrpFailure_InvalidParameter;
    uint32_t w, h;
    xrr_recommended_eye_size(&w, &h);
    /* UE asks ~1.2x supersample (1728x1900). debug.re4vr.sscap=1 caps it to 1.0x to
     * free GPU/bandwidth (the copy-ring copies fewer pixels too); the ghosting is a
     * frame-drop/serialization issue not pixel count, but this is a cheap lever to
     * stack with the perf/copy-ring fixes. Default off (full res for sharpness). */
    if (textureScale > 1.0f) {
        static int cap = -1;
        if (cap < 0) {
#ifdef __ANDROID__
            char s[92] = {0};
            extern int __system_property_get(const char*, char*);
            cap = (__system_property_get("debug.re4vr.sscap", s) > 0 && s[0] == '1') ? 1 : 0;
#else
            cap = 0;
#endif
            XRRLOG("supersample cap: %s (debug.re4vr.sscap), UE asked scale=%.2f", cap ? "1.0x" : "off", textureScale);
        }
        if (cap) textureScale = 1.0f;
    }
    if (textureScale > 0.0f) { w = (uint32_t)(w * textureScale); h = (uint32_t)(h * textureScale); }

    /* Aggressive resolution lever (debug.re4vr.resscale = percent of the size above;
     * default 100). Below 100 shrinks the eye buffer further to keep UE under GPU budget
     * so it stops TRUNCATING frames -> black (the load-gated UE frame-drop; RenderDoc:
     * 28 draws vs ~198 normal, resolved eye = pure black). Read once at layer setup. */
    {
        static int rs = -1;
        if (rs < 0) {
#ifdef __ANDROID__
            char s[92] = {0};
            extern int __system_property_get(const char*, char*);
            rs = (__system_property_get("debug.re4vr.resscale", s) > 0) ? atoi(s) : 100;
#else
            rs = 100;
#endif
            if (rs < 25)  rs = 25;
            if (rs > 200) rs = 200;
            XRRLOG("resscale: %d%% (debug.re4vr.resscale)", rs);
        }
        if (rs != 100) { w = (uint32_t)((uint64_t)w * rs / 100); h = (uint32_t)((uint64_t)h * rs / 100); }
    }

    memset(out, 0, sizeof(*out));
    out->Shape       = ovrpShape_EyeFov;
    out->Layout      = layout;
    out->TextureSize.w = (int)w;
    out->TextureSize.h = (int)h;
    out->MipLevels   = mipLevels > 0 ? mipLevels : 1;
    out->SampleCount = sampleCount > 0 ? sampleCount : 1;
    out->Format      = colorFormat;
    out->LayerFlags  = layerFlags;
    for (int e = 0; e < 2; e++) {
        /* real per-eye FOV so the layer desc matches GetNodeFrustum2 + our submit */
        xrr_eye_fov_tangents(e, &out->Fov[e].UpTan, &out->Fov[e].DownTan,
                             &out->Fov[e].LeftTan, &out->Fov[e].RightTan);
        out->VisibleRect[e].Pos.x = 0.0f; out->VisibleRect[e].Pos.y = 0.0f;
        out->VisibleRect[e].Size.w = (float)w; out->VisibleRect[e].Size.h = (float)h;
    }
    out->MaxViewportSize.w = (int)w;
    out->MaxViewportSize.h = (int)h;
    out->DepthFormat = depthFormat;
    {   /* log only when the parameters change — called every frame otherwise */
        static int pw, ph, pl = -1, pf = -1;
        if ((int)layout != pl || (int)w != pw || (int)h != ph || (int)colorFormat != pf) {
            XRRLOG("CalculateEyeLayerDesc2: layout=%d %ux%u fmt=%d depth=%d flags=%d",
                   layout, w, h, colorFormat, depthFormat, layerFlags);
            pl = layout; pw = (int)w; ph = (int)h; pf = colorFormat;
        }
    }
    return ovrpSuccess;
}

/* [from UE: CalculateEyeViewportRect(EyeLayerDesc, eye, scale, &vpRect)]. desc is
 * passed by value (>16B -> indirect, so a pointer in x0). Array layout: each eye
 * is a full array layer, so the viewport is the full (scaled) texture rect. */
OVRP_EXPORT ovrpResult ovrp_CalculateEyeViewportRect(const ovrpLayerDesc *desc,
        int eye, float scale, ovrpRecti *out) {
    PT_FWD(ovrp_CalculateEyeViewportRect, desc, eye, scale, out);
    (void)eye;
    if (!desc || !out) return ovrpFailure_InvalidParameter;
    out->Pos.x = 0; out->Pos.y = 0;
    out->Size.w = (int)(desc->TextureSize.w * scale);
    out->Size.h = (int)(desc->TextureSize.h * scale);
    return ovrpSuccess;
}

/* [from OVR_Plugin.h] non-eye layer desc (quad/cylinder/etc, e.g. the splash).
 * textureSize is const-ref => a pointer. Fills the common header; UE fills the
 * shape-specific tail. Stubbing this left the desc zeroed -> SetupLayer 0x0 -> fail. */
OVRP_EXPORT ovrpResult ovrp_CalculateLayerDesc(ovrpShape shape, ovrpLayout layout,
        const ovrpSizei *textureSize, int mipLevels, int sampleCount,
        ovrpTextureFormat format, int layerFlags, ovrpLayerDesc *out) {
    PT_FWD(ovrp_CalculateLayerDesc, shape, layout, textureSize, mipLevels, sampleCount, format, layerFlags, out);
    if (!out || !textureSize) return ovrpFailure_InvalidParameter;
    out->Shape       = shape;
    out->Layout      = layout;
    out->TextureSize = *textureSize;
    out->MipLevels   = mipLevels > 0 ? mipLevels : 1;
    out->SampleCount = sampleCount > 0 ? sampleCount : 1;
    out->Format      = format;
    out->LayerFlags  = layerFlags;
    XRRLOG("CalculateLayerDesc: shape=%d layout=%d %dx%d fmt=%d",
           shape, layout, textureSize->w, textureSize->h, format);
    return ovrpSuccess;
}

OVRP_EXPORT ovrpResult ovrp_SetupLayer(void *device, ovrpLayerDesc *desc,
        int *outLayerId) {
    PT_FWD(ovrp_SetupLayer, device, desc, outLayerId);
    (void)device;        /* the Vulkan device is already bound to the XrSession */
    return xrr_setup_layer(desc, outLayerId);
}

/* [VERIFIED from UE OculusHMD.cpp: FOculusHMD::DestroyLayer(uint32 LayerId)] */
OVRP_EXPORT ovrpResult ovrp_DestroyLayer(int layerId) {
    PT_FWD(ovrp_DestroyLayer, layerId);
    xrr_destroy_layer(layerId);
    return ovrpSuccess;
}

OVRP_EXPORT ovrpResult ovrp_GetLayerTextureStageCount(int layerId, int *outCount) {
    PT_FWD(ovrp_GetLayerTextureStageCount, layerId, outCount);
    if (!outCount) return ovrpFailure_InvalidParameter;
    *outCount = xrr_layer_stage_count(layerId);
    return ovrpSuccess;
}

/* (layerId, stage, eyeId, &colorTexHandle, &depthTexHandle); either out may be
 * NULL. Handles are VkImage as uint64 [VERIFIED 5-arg shape from decompile]. */
OVRP_EXPORT ovrpResult ovrp_GetLayerTexture2(int layerId, int stage, int eyeId,
        uint64_t *outColorTex, uint64_t *outDepthTex) {
    PT_FWD(ovrp_GetLayerTexture2, layerId, stage, eyeId, outColorTex, outDepthTex);
    if (!outColorTex && !outDepthTex) return ovrpFailure_InvalidParameter;
    return xrr_get_layer_texture(layerId, stage, eyeId, outColorTex, outDepthTex);
}
