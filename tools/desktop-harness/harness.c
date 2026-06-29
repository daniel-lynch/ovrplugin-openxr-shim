/* harness.c — desktop OpenXR harness for the OVRPlugin->OpenXR shim.
 *
 * Stands in for Resident Evil 4 VR (UE4 + OculusHMD) on a Linux desktop: it creates
 * a Vulkan instance/device the way UE's VulkanRHI does, then drives the shim's ovrp_*
 * entry points in the exact order UE calls them — PreInitialize3 -> Get*ExtensionsVk
 * -> Initialize5 -> (per frame) Update3/WaitToBeginFrame/BeginFrame4/EndFrame4 -> a
 * one-time CalculateEyeLayerDesc2/SetupLayer -> Shutdown2. The shim creates the real
 * XrInstance/session/swapchains against whatever OpenXR runtime the loader selects
 * (here: Monado's simulated HMD, headless via XRT_COMPOSITOR_NULL).
 *
 * This exercises the whole non-Android OpenXR path of the shim on a PC — no Quest, no
 * libUE4 — so the Steam Frame / Monado / Lepton bring-up can be iterated on a laptop.
 *
 * It is NOT the game and ships nothing from Capcom/Epic/Meta: it only calls our own
 * public ovrp_* ABI. Build: shim/build_host.sh. Run: tools/desktop-harness/run.sh. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <vulkan/vulkan.h>
#include "ovrplugin_shim.h"   /* shim public types/enums (ovrpLayerDesc, ovrpLayout, ...) */

/* ovrp_* the shim exports but doesn't declare in the public header — declare here so we
 * call them with the verified ABI without depending on header completeness. */
extern ovrpResult ovrp_GetInstanceExtensionsVk(const char **outArray, int *inoutCount);
extern ovrpResult ovrp_GetDeviceExtensionsVk(const char **outArray, int *inoutCount);
extern ovrpResult ovrp_CalculateEyeLayerDesc2(ovrpLayout layout, float textureScale,
        int mipLevels, int sampleCount, ovrpTextureFormat colorFormat,
        ovrpTextureFormat depthFormat, int layerFlags, ovrpLayerDesc *out);
extern ovrpResult ovrp_SetupLayer(void *device, ovrpLayerDesc *desc, int *outLayerId);
extern ovrpResult ovrp_GetLayerTextureStageCount(int layerId, int *outCount);
extern ovrpResult ovrp_GetLayerTexture2(int layerId, int stage, int eyeId,
        uint64_t *outColorTex, uint64_t *outDepthTex);

#define VKOK(call) do { VkResult _r = (call); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "[harness] FAIL %s = %d\n", #call, _r); exit(1); } } while (0)
#define LOG(...) do { fprintf(stderr, "[harness] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static VkInstance       g_inst;
static VkPhysicalDevice g_phys;
static VkDevice         g_dev;
static VkQueue          g_queue;
static uint32_t         g_gfxFamily;
static VkCommandPool    g_cmdPool;

/* CPU-side staging for the scene renderer (host-visible; both eye layers, RGBA8). */
static VkBuffer  g_stageBuf;
static VkDeviceMemory g_stageMem;
static void     *g_stagePtr;
static uint32_t  g_stageW, g_stageH;

/* The ovrp_*ExtensionsVk getters report a count then fill a caller array of char*. */
static const char **query_exts(int forDevice, int *outCount) {
    int n = 0;
    ovrpResult r = forDevice ? ovrp_GetDeviceExtensionsVk(NULL, &n)
                             : ovrp_GetInstanceExtensionsVk(NULL, &n);
    if (!OVRP_SUCCESS(r) || n <= 0) { *outCount = 0; return NULL; }
    const char **arr = calloc((size_t)n, sizeof(char *));
    int cap = n;
    r = forDevice ? ovrp_GetDeviceExtensionsVk(arr, &cap)
                  : ovrp_GetInstanceExtensionsVk(arr, &cap);
    if (!OVRP_SUCCESS(r)) { free(arr); *outCount = 0; return NULL; }
    *outCount = n;
    LOG("%s extensions required by runtime (%d):", forDevice ? "device" : "instance", n);
    for (int i = 0; i < n; i++) LOG("    %s", arr[i]);
    return arr;
}

static void make_vk_instance(void) {
    int n = 0;
    const char **exts = query_exts(0, &n);   /* needs the XrInstance (PreInitialize3 done) */
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "re4vr-shim-harness";
    ai.apiVersion = VK_API_VERSION_1_1;      /* UE/Quest Vulkan baseline */
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = (uint32_t)n;
    ci.ppEnabledExtensionNames = exts;
    VKOK(vkCreateInstance(&ci, NULL, &g_inst));
    free(exts);
    LOG("VkInstance created");
}

static void pick_physical_and_device(void) {
    uint32_t pc = 0;
    VKOK(vkEnumeratePhysicalDevices(g_inst, &pc, NULL));
    if (!pc) { LOG("no Vulkan physical devices"); exit(1); }
    VkPhysicalDevice *pd = calloc(pc, sizeof(*pd));
    VKOK(vkEnumeratePhysicalDevices(g_inst, &pc, pd));
    g_phys = pd[0];   /* shim picks the runtime's preferred device internally; smoke test = [0] */
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_phys, &props);
    LOG("physical device: %s", props.deviceName);
    free(pd);

    uint32_t qf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qf, NULL);
    VkQueueFamilyProperties *qp = calloc(qf, sizeof(*qp));
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qf, qp);
    g_gfxFamily = UINT32_MAX;
    for (uint32_t i = 0; i < qf; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_gfxFamily = i; break; }
    free(qp);
    if (g_gfxFamily == UINT32_MAX) { LOG("no graphics queue family"); exit(1); }

    int n = 0;
    const char **exts = query_exts(1, &n);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = g_gfxFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)n;
    dci.ppEnabledExtensionNames = exts;
    VKOK(vkCreateDevice(g_phys, &dci, NULL, &g_dev));
    free(exts);
    vkGetDeviceQueue(g_dev, g_gfxFamily, 0, &g_queue);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_gfxFamily;
    VKOK(vkCreateCommandPool(g_dev, &pci, NULL, &g_cmdPool));
    LOG("VkDevice + graphics queue (family %u) created", g_gfxFamily);
}

static uint32_t find_mem(uint32_t typeBits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* Host-visible staging buffer big enough for both eye layers (RGBA8). Persistently mapped. */
static int make_staging(uint32_t w, uint32_t h) {
    VkDeviceSize sz = (VkDeviceSize)w * h * 4u * 2u;   /* 2 array layers */
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = sz; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_dev, &bci, NULL, &g_stageBuf) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_dev, g_stageBuf, &mr);
    uint32_t mt = find_mem(mr.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) return 0;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
    if (vkAllocateMemory(g_dev, &mai, NULL, &g_stageMem) != VK_SUCCESS) return 0;
    vkBindBufferMemory(g_dev, g_stageBuf, g_stageMem, 0);
    if (vkMapMemory(g_dev, g_stageMem, 0, sz, 0, &g_stagePtr) != VK_SUCCESS) return 0;
    g_stageW = w; g_stageH = h;
    return 1;
}

/* rotate vector v by quaternion q (x,y,z,w): v + 2*qw*(qv x v) + 2*(qv x (qv x v)) */
static void qrot(float qx, float qy, float qz, float qw,
                 float vx, float vy, float vz, float *ox, float *oy, float *oz) {
    float tx = 2.0f * (qy * vz - qz * vy);
    float ty = 2.0f * (qz * vx - qx * vz);
    float tz = 2.0f * (qx * vy - qy * vx);
    *ox = vx + qw * tx + (qy * tz - qz * ty);
    *oy = vy + qw * ty + (qz * tx - qx * tz);
    *oz = vz + qw * tz + (qx * ty - qy * tx);
}

/* Procedural world-locked scene along a world-space ray: checkerboard floor 1.6m below the
 * eye, sky gradient, and an orbiting sun (the motion). Writes linear RGB into r/g/b. */
static void shade(float ox, float oy, float oz, float dx, float dy, float dz, float t,
                  float *r, float *g, float *b) {
    if (dy < -1e-3f) {
        float floorY = oy - 1.6f;
        float tt = (floorY - oy) / dy;          /* = 1.6 / -dy > 0 */
        if (tt > 0.0f) {
            float hx = ox + dx * tt, hz = oz + dz * tt;
            int chk = (((int)floorf(hx)) + ((int)floorf(hz))) & 1;
            float base = chk ? 0.85f : 0.25f;
            float fog = 1.0f / (1.0f + tt * 0.04f);  /* fade distant floor into sky */
            *r = base * fog + 0.55f * (1.0f - fog);
            *g = base * fog + 0.65f * (1.0f - fog);
            *b = base * fog + 0.85f * (1.0f - fog);
            return;
        }
    }
    float up = dy * 0.5f + 0.5f;                 /* sky gradient */
    *r = 0.30f + 0.20f * up; *g = 0.50f + 0.30f * up; *b = 0.70f + 0.30f * up;
    float sx = cosf(t), sy = 0.40f, sz = sinf(t);     /* orbiting sun */
    float sl = 1.0f / sqrtf(sx * sx + sy * sy + sz * sz); sx *= sl; sy *= sl; sz *= sl;
    if (dx * sx + dy * sy + dz * sz > 0.995f) { *r = 1.0f; *g = 0.95f; *b = 0.70f; }
}

/* Best-effort pose-driven render: for each eye, build per-pixel world rays from the shim's
 * located eye pose + FOV, shade the procedural scene, and copy into that array layer. This
 * exercises the shim's pose/FOV math (stereo parallax between eyes; world-locked content
 * counter-moves as the head pose changes). Failures here don't fail the harness. */
static void render_scene(uint64_t image, uint32_t arrayLayers, const ovrpLayerDesc *desc,
                         const ovrpPoseStatef pose[2], float t) {
    if (!image || !g_stagePtr) return;
    uint32_t W = g_stageW, H = g_stageH;
    for (uint32_t eye = 0; eye < arrayLayers; eye++) {
        const ovrpPosef *p = &pose[eye].Pose;
        float ox = p->Position.x, oy = p->Position.y, oz = p->Position.z;
        float lt = desc->Fov[eye].LeftTan, rt = desc->Fov[eye].RightTan;
        float ut = desc->Fov[eye].UpTan,   dt = desc->Fov[eye].DownTan;
        uint8_t *px = (uint8_t *)g_stagePtr + (size_t)eye * W * H * 4u;
        /* ovrpFovf tangents are positive magnitudes: horizontal spans -LeftTan..+RightTan,
         * vertical spans +UpTan (top) ..-DownTan (bottom). */
        for (uint32_t y = 0; y < H; y++) {
            float v = ut - (ut + dt) * ((y + 0.5f) / H);
            for (uint32_t x = 0; x < W; x++) {
                float u = -lt + (rt + lt) * ((x + 0.5f) / W);
                float il = 1.0f / sqrtf(u * u + v * v + 1.0f);
                float ex = u * il, ey = v * il, ez = -1.0f * il;   /* OpenXR: -Z forward */
                float dx, dy, dz;
                qrot(p->Orientation.x, p->Orientation.y, p->Orientation.z, p->Orientation.w,
                     ex, ey, ez, &dx, &dy, &dz);
                float r, g, b; shade(ox, oy, oz, dx, dy, dz, t, &r, &g, &b);
                uint8_t *o = px + ((size_t)y * W + x) * 4u;
                o[0] = (uint8_t)(r * 255.0f); o[1] = (uint8_t)(g * 255.0f);
                o[2] = (uint8_t)(b * 255.0f); o[3] = 255;
            }
        }
    }

    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = g_cmdPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    if (vkAllocateCommandBuffers(g_dev, &ai, &cb) != VK_SUCCESS) return;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, arrayLayers };
    VkImageMemoryBarrier toDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = (VkImage)image; toDst.subresourceRange = range;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &toDst);

    VkBufferImageCopy region[2]; uint32_t nr = 0;
    for (uint32_t eye = 0; eye < arrayLayers; eye++) {
        VkBufferImageCopy c; memset(&c, 0, sizeof c);
        c.bufferOffset = (VkDeviceSize)eye * W * H * 4u;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.mipLevel = 0; c.imageSubresource.baseArrayLayer = eye;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = W; c.imageExtent.height = H; c.imageExtent.depth = 1;
        region[nr++] = c;
    }
    vkCmdCopyBufferToImage(cb, g_stageBuf, (VkImage)image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nr, region);

    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   /* what the compositor reads */
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, NULL, 0, NULL, 1, &toRead);

    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_queue);
    vkFreeCommandBuffers(g_dev, g_cmdPool, 1, &cb);
}

int main(int argc, char **argv) {
    int frames = (argc > 1) ? atoi(argv[1]) : 300;
    if (frames < 1) frames = 1;
    LOG("starting; %d frames. Runtime via OpenXR loader (XR_RUNTIME_JSON / active_runtime.json).", frames);

    /* 1. lifecycle: PreInitialize3 creates the XrInstance + picks the system */
    if (!OVRP_SUCCESS(ovrp_PreInitialize3(NULL))) { LOG("PreInitialize3 failed"); return 1; }
    LOG("PreInitialize3 OK (XrInstance + system up)");

    /* 2. Vulkan, created with the runtime-required extensions (UE's VulkanRHI order) */
    make_vk_instance();
    pick_physical_and_device();

    /* 3. Initialize5 hands the shim our Vulkan handles -> it creates the XrSession */
    long versionStub[4] = {0};   /* arg9 = const ovrpVersion& — shim ignores the contents */
    ovrpResult ir = ovrp_Initialize5(ovrpRenderAPI_Vulkan, NULL, NULL,
                                     (void *)g_inst, (void *)g_phys, (void *)g_dev,
                                     (void *)g_queue, 0, versionStub);
    if (!OVRP_SUCCESS(ir)) { LOG("Initialize5 failed (%d)", ir); return 1; }
    LOG("Initialize5 OK (XrSession created)");

    /* 4. eye-fov layer (UE: CalculateEyeLayerDesc2 -> SetupLayer once) */
    ovrpLayerDesc desc;
    ovrpResult dr = ovrp_CalculateEyeLayerDesc2(ovrpLayout_Array, 1.0f, 1, 1,
                        ovrpTextureFormat_R8G8B8A8_sRGB, (ovrpTextureFormat)0, 0, &desc);
    if (!OVRP_SUCCESS(dr)) { LOG("CalculateEyeLayerDesc2 failed (%d)", dr); return 1; }
    LOG("EyeLayerDesc %dx%d arraylayout, fmt=%d", desc.TextureSize.w, desc.TextureSize.h, desc.Format);
    int layerId = -1;
    ovrpResult sr = ovrp_SetupLayer((void *)g_dev, &desc, &layerId);
    if (!OVRP_SUCCESS(sr) || layerId < 0) { LOG("SetupLayer failed (%d)", sr); return 1; }
    int stageCount = 0; ovrp_GetLayerTextureStageCount(layerId, &stageCount);
    LOG("SetupLayer OK layerId=%d swapchainStages=%d", layerId, stageCount);
    uint32_t arrayLayers = (desc.Layout == ovrpLayout_Array) ? 2u : 1u;
    if (make_staging((uint32_t)desc.TextureSize.w, (uint32_t)desc.TextureSize.h))
        LOG("scene renderer ready (%dx%d, %u eye layers)", desc.TextureSize.w, desc.TextureSize.h, arrayLayers);
    else
        LOG("WARN: staging buffer alloc failed — frames will be submitted blank");

    /* 5. frame loop. Update3 advances the session state machine (IDLE->READY->FOCUSED);
     * Wait/Begin/EndFrame no-op until the session is running, so early frames are fine. */
    if (stageCount < 1) stageCount = 1;
    int presented = 0, renderStage = 0;   /* stage advances per presented frame, in lockstep
                                            * with the shim's one-acquire-per-running-frame */
    for (int f = 0; f < frames; f++) {
        ovrp_Update3(ovrpStep_Render, f, 0.0);
        ovrp_WaitToBeginFrame(f);
        ovrp_BeginFrame4(f, NULL);
        int stage = renderStage % stageCount;   /* = the image begin_frame just acquired */

        /* per-eye pose from the shim's located views (true IPD separation) + the eye FOV
         * from the layer desc -> render a world-locked scene into the acquired eye image. */
        ovrpPoseStatef eyePose[2]; memset(eyePose, 0, sizeof eyePose);
        eyePose[0].Pose.Orientation.w = eyePose[1].Pose.Orientation.w = 1.0f;
        ovrp_GetNodePoseState3(ovrpStep_Render, f, ovrpNode_EyeLeft,  &eyePose[0]);
        ovrp_GetNodePoseState3(ovrpStep_Render, f, ovrpNode_EyeRight, &eyePose[1]);
        if (f < 3)
            LOG("frame %d eyeL pos=(%.3f %.3f %.3f) eyeR pos=(%.3f %.3f %.3f) fovL(R%.3f L%.3f)",
                f, eyePose[0].Pose.Position.x, eyePose[0].Pose.Position.y, eyePose[0].Pose.Position.z,
                eyePose[1].Pose.Position.x, eyePose[1].Pose.Position.y, eyePose[1].Pose.Position.z,
                desc.Fov[0].RightTan, desc.Fov[0].LeftTan);

        uint64_t color = 0, depthTex = 0;
        if (OVRP_SUCCESS(ovrp_GetLayerTexture2(layerId, stage, 0, &color, &depthTex)) && color)
            render_scene(color, arrayLayers, &desc, eyePose, (float)f * 0.03f);

        ovrpLayerSubmit submit;
        memset(&submit, 0, sizeof submit);
        submit.LayerId = layerId;
        submit.TextureStage = stage;
        submit.Pose.Orientation.w = 1.0f;     /* pose/FOV come from the shim's located views */
        const ovrpLayerSubmit *ptrs[1] = { &submit };
        ovrpResult er = ovrp_EndFrame4(f, ptrs, 1, NULL);
        if (OVRP_SUCCESS(er)) {
            if (presented == 0)   /* views are located now — refresh FOV (setup value was the fallback) */
                ovrp_CalculateEyeLayerDesc2(ovrpLayout_Array, 1.0f, 1, 1,
                        ovrpTextureFormat_R8G8B8A8_sRGB, (ovrpTextureFormat)0, 0, &desc);
            presented++; renderStage++;
        }

        if (f < 5 || (f % 60) == 0)
            LOG("frame %d: end=%d (presented=%d)", f, er, presented);
    }

    LOG("loop done: %d/%d frames presented", presented, frames);
    ovrp_Shutdown2();
    LOG("Shutdown2 OK — clean exit");
    return presented > 0 ? 0 : 2;
}
