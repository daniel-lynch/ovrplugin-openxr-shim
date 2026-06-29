/* vk_session.c — Vulkan-typed half of the OpenXR handshake. Uses XR_KHR_vulkan_enable
 * (v1) because OVRPlugin's model is "the app creates the VkInstance/Device" (handles
 * arrive via ovrp_Initialize5 args 5-8) and asks us which extensions to enable via
 * ovrp_GetInstance/DeviceExtensionsVk — that's exactly the v1 get-extensions flow.
 */
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>
#include "xr_runtime.h"
#include "passthru.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>

/* fetch a KHR_vulkan_enable extension entry point by name */
static PFN_xrVoidFunction get_xr(const char *name) {
    PFN_xrVoidFunction fn = NULL;
    if (g_xr.instance != XR_NULL_HANDLE)
        xrGetInstanceProcAddr(g_xr.instance, name, &fn);
    return fn;
}

/* UE's Vulkan handles (from ovrp_Initialize5), used by both the graphics binding
 * and the end-of-frame flush barrier. */
static VkDevice  s_dev;
static VkQueue   s_queue;
static uint32_t  s_qfam;
static VkPhysicalDevice s_phys;   /* for memory-type selection (texture readback) */
static VkInstance s_inst;         /* to load instance-level entry points          */

void xrr_vk_set_handles(void *device, void *queue, unsigned int family) {
    s_dev = (VkDevice)device; s_queue = (VkQueue)queue; s_qfam = family;
}

/* libvulkan + its proc-addr loaders, opened ONCE. Previously every Vulkan helper dlopen'd
 * libvulkan.so per call (xrr_vk_frame_luma did it per frame), leaking a dl handle each time.
 * Returns 1 if vkGetDeviceProcAddr is available; fills gdpa/gipa (either may be NULL-arg). */
static void *s_vklib;
static PFN_vkGetDeviceProcAddr   s_gdpa;
static PFN_vkGetInstanceProcAddr s_gipa;
static int vk_loaders(PFN_vkGetDeviceProcAddr *gdpa, PFN_vkGetInstanceProcAddr *gipa) {
    if (!s_vklib) {
        s_vklib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (s_vklib) {
            s_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(s_vklib, "vkGetDeviceProcAddr");
            s_gipa = (PFN_vkGetInstanceProcAddr)dlsym(s_vklib, "vkGetInstanceProcAddr");
        }
    }
    if (gdpa) *gdpa = s_gdpa;
    if (gipa) *gipa = s_gipa;
    return s_gdpa != NULL;
}

/* The OpenXR runtime synchronizes its compositor against the queue named in the
 * graphics binding. We only have UE's VkQueue handle, so scan (family,index) to
 * find which one it is — a wrong queueIndex means the runtime waits on an idle
 * queue and composites before UE finishes -> black unless we hard-wait ourselves. */
static PFN_vkGetDeviceQueue load_get_device_queue(VkDevice dev) {
    PFN_vkGetDeviceProcAddr gdpa; vk_loaders(&gdpa, NULL);
    return gdpa ? (PFN_vkGetDeviceQueue)gdpa(dev, "vkGetDeviceQueue") : NULL;
}
static void detect_ue_queue(VkDevice dev, uint32_t *family, uint32_t *index) {
    *family = 0; *index = 0;
    if (!s_queue) return;
    PFN_vkGetDeviceQueue gdq = load_get_device_queue(dev);
    if (!gdq) { XRRLOG("queue detect: no vkGetDeviceQueue"); return; }
    for (uint32_t f = 0; f < 4; f++)
        for (uint32_t i = 0; i < 4; i++) {
            VkQueue q = VK_NULL_HANDLE;
            gdq(dev, f, i, &q);
            if (q == s_queue) {
                *family = f; *index = i;
                XRRLOG("queue detect: UE queue is family=%u index=%u", f, i);
                return;
            }
        }
    XRRLOG("queue detect: UE queue not matched, defaulting 0/0");
}

/* ----------------------------------------------- app-side extension queries -- */
/* [VERIFIED from real Compositor::GetInstanceExtensionsVk] ABI is:
 *   ovrp_Get*ExtensionsVk(const char** outArray, int* inoutCount)
 * outArray = caller's array of char*, filled with `count` POINTERS to extension
 * name strings (memcpy count<<3 bytes); inoutCount IN=capacity(entries), OUT=count;
 * returns -1007 if capacity<count; NULL outArray = size query. UE iterates the
 * result as char*[] (my single-buffer version made it deref chars as ptrs ->
 * strcmp segfault). We split xrGetVulkan*ExtensionsKHR's space list into ptrs. */
#define XRR_MAX_EXTS 64
static char        s_extBuf[2][4096];      /* [0]=instance [1]=device  */
static const char *s_extPtrs[2][XRR_MAX_EXTS];
static int         s_extCount[2] = { -1, -1 };

static int build_ext_list(int forDevice) {
    if (s_extCount[forDevice] >= 0) return s_extCount[forDevice];  /* cached */
    const char *fname = forDevice ? "xrGetVulkanDeviceExtensionsKHR"
                                  : "xrGetVulkanInstanceExtensionsKHR";
    union { PFN_xrGetVulkanInstanceExtensionsKHR i;
            PFN_xrGetVulkanDeviceExtensionsKHR   d;
            PFN_xrVoidFunction v; } fn;
    fn.v = get_xr(fname);
    if (!fn.v) return -1;
    char *buf = s_extBuf[forDevice];
    uint32_t got = 0;
    XrResult r = forDevice
        ? fn.d(g_xr.instance, g_xr.systemId, 4096, &got, buf)
        : fn.i(g_xr.instance, g_xr.systemId, 4096, &got, buf);
    if (XR_FAILED(r) || got == 0) { XRRERR("%s failed: %d", fname, (int)r); return -1; }
    buf[got < 4096 ? got : 4095] = '\0';
    /* split the space-separated string in place into pointers */
    int n = 0;
    char *p = buf;
    while (*p && n < XRR_MAX_EXTS) {
        s_extPtrs[forDevice][n++] = p;
        char *sp = p;
        while (*sp && *sp != ' ') sp++;
        if (*sp == ' ') { *sp = '\0'; p = sp + 1; } else break;
    }
    s_extCount[forDevice] = n;
    return n;
}

static ovrpResult get_vk_exts(int forDevice, const char **outArray, int *inoutCount) {
    if (!inoutCount) return ovrpFailure_InvalidParameter;
    if (g_xr.instance == XR_NULL_HANDLE || g_xr.systemId == XR_NULL_SYSTEM_ID)
        return ovrpFailure_InvalidOperation;
    int count = build_ext_list(forDevice);
    if (count < 0) return ovrpFailure_Unsupported;
    int cap = *inoutCount;
    *inoutCount = count;                       /* always report the count */
    if (outArray) {
        if (cap < count) return ovrpFailure_InsufficientSize;   /* -1007 */
        memcpy(outArray, s_extPtrs[forDevice], (size_t)count * sizeof(char *));
    }
    return ovrpSuccess;
}

OVRP_EXPORT ovrpResult ovrp_GetInstanceExtensionsVk(const char **outArray, int *inoutCount) {
    PT_FWD(ovrp_GetInstanceExtensionsVk, outArray, inoutCount);  /* real vrapi extensions */
    ovrpResult res = get_vk_exts(0, outArray, inoutCount);
    XRRLOG("GetInstanceExtensionsVk -> %d (count=%d, fill=%d)", res,
           inoutCount ? *inoutCount : -1, outArray != NULL);
    return res;
}
OVRP_EXPORT ovrpResult ovrp_GetDeviceExtensionsVk(const char **outArray, int *inoutCount) {
    PT_FWD(ovrp_GetDeviceExtensionsVk, outArray, inoutCount);    /* real vrapi extensions */
    ovrpResult res = get_vk_exts(1, outArray, inoutCount);
    XRRLOG("GetDeviceExtensionsVk -> %d (count=%d, fill=%d)", res,
           inoutCount ? *inoutCount : -1, outArray != NULL);
    return res;
}

/* --------------------------------------------------------- session creation -- */
int xrr_create_session_vulkan(void *vkInstance, void *vkPhysicalDevice,
                              void *vkDevice, unsigned int queueFamilyIndex) {
    if (g_xr.instance == XR_NULL_HANDLE || g_xr.systemId == XR_NULL_SYSTEM_ID)
        return 0;

    /* required before session creation per XR_KHR_vulkan_enable */
    union { PFN_xrGetVulkanGraphicsRequirementsKHR f; PFN_xrVoidFunction v; } req;
    req.v = get_xr("xrGetVulkanGraphicsRequirementsKHR");
    XRRLOG("vk_session: reqProc=%p", (void *)req.v);
    if (req.f) {
        XrGraphicsRequirementsVulkanKHR gr = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
        XrResult rr = req.f(g_xr.instance, g_xr.systemId, &gr);
        XRRLOG("vk_session: GraphicsRequirements rc=%d minApi=0x%llx maxApi=0x%llx",
               (int)rr, (unsigned long long)gr.minApiVersionSupported,
               (unsigned long long)gr.maxApiVersionSupported);
    }
    /* runtime may require we query the graphics device too */
    union { PFN_xrGetVulkanGraphicsDeviceKHR f; PFN_xrVoidFunction v; } gd;
    gd.v = get_xr("xrGetVulkanGraphicsDeviceKHR");
    if (gd.f) {
        VkPhysicalDevice want = VK_NULL_HANDLE;
        XrResult dr = gd.f(g_xr.instance, g_xr.systemId, (VkInstance)vkInstance, &want);
        XRRLOG("vk_session: GraphicsDevice rc=%d want=%p got=%p match=%d", (int)dr,
               (void *)want, vkPhysicalDevice, want == (VkPhysicalDevice)vkPhysicalDevice);
    }

    s_phys = (VkPhysicalDevice)vkPhysicalDevice;
    s_inst = (VkInstance)vkInstance;
    uint32_t qfam = queueFamilyIndex, qidx = 0;
    detect_ue_queue((VkDevice)vkDevice, &qfam, &qidx);
    s_qfam = qfam;   /* the flush command pool must use the same family */

    XrGraphicsBindingVulkanKHR binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
    binding.instance         = (VkInstance)vkInstance;
    binding.physicalDevice   = (VkPhysicalDevice)vkPhysicalDevice;
    binding.device           = (VkDevice)vkDevice;
    binding.queueFamilyIndex = qfam;
    binding.queueIndex       = qidx;
    XRRLOG("vk_session: binding inst=%p phys=%p dev=%p queue fam=%u idx=%u",
           vkInstance, vkPhysicalDevice, vkDevice, qfam, qidx);

    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next     = &binding;
    sci.systemId = g_xr.systemId;
    XrResult r = xrCreateSession(g_xr.instance, &sci, &g_xr.session);
    if (XR_FAILED(r)) {
        XRRERR("xrCreateSession(Vulkan) failed: %d", (int)r);
        g_xr.session = XR_NULL_HANDLE;
        return 0;
    }
    XRRLOG("vk_session: xrCreateSession OK session=%p", (void *)g_xr.session);
    return 1;
}

/* Enumerate a swapchain's VkImages, returned as opaque uint64 handles. */
/* ---------------------------------------------------- tile-memory flush ----- *
 * On Quest's tiler GPU, UE renders the eye into tile memory and relies on the
 * compositor's submit to resolve it to main memory. We hand the image straight to
 * the OpenXR compositor, so without an explicit barrier the compositor reads stale
 * main-memory contents -> black / tearing / ghosting. This records+submits a
 * pipeline barrier (COLOR_ATTACHMENT_WRITE -> MEMORY_READ) to force the resolve.
 * Submitted on UE's own VkQueue from end_frame (same RHI thread) so there is no
 * concurrent queue access.
 *
 * Split submit/wait: xrr_vk_flush_submit records+submits the barrier and returns a
 * ring token WITHOUT blocking; xrr_vk_flush_wait(token) blocks on that submit's
 * fence. The frame loop submits frame N's flush then waits frame N-1's, so the
 * wait is off the critical path (the GPU finished it during this frame's work) ->
 * xrEndFrame lands on schedule and the compositor stops reprojecting every frame.
 * Ring is sized for >=1 full frame of in-flight tokens (every layer submits per
 * frame, and a token is now waited a frame later). */
#define XRR_FLUSH_RING (XRR_MAX_LAYERS * 2)
static int       s_vkReady, s_vkFailed;
static VkCommandPool   s_pool;
static VkCommandBuffer s_cmd[XRR_FLUSH_RING];
static VkFence         s_fence[XRR_FLUSH_RING];
static int             s_ring;
static PFN_vkCreateCommandPool      p_CreatePool;
static PFN_vkAllocateCommandBuffers p_AllocCmd;
static PFN_vkBeginCommandBuffer     p_BeginCmd;
static PFN_vkCmdPipelineBarrier     p_Barrier;
static PFN_vkEndCommandBuffer       p_EndCmd;
static PFN_vkQueueSubmit            p_Submit;
static PFN_vkResetCommandBuffer     p_ResetCmd;
static PFN_vkCreateFence            p_CreateFence;
static PFN_vkWaitForFences          p_WaitFences;
static PFN_vkResetFences            p_ResetFences;
static PFN_vkGetFenceStatus         p_FenceStatus;
static PFN_vkQueueWaitIdle          p_QueueWaitIdle;
static PFN_vkDeviceWaitIdle         p_DeviceWaitIdle;

static int vk_lazy_init(void) {
    if (s_vkReady)  return 1;
    if (s_vkFailed) return 0;
    if (!s_dev || !s_queue) return 0;
    PFN_vkGetDeviceProcAddr gdpa; vk_loaders(&gdpa, NULL);
    if (!gdpa) { s_vkFailed = 1; XRRERR("vk flush: no vkGetDeviceProcAddr"); return 0; }
    #define LOAD(p, n) p = (PFN_##n)gdpa(s_dev, #n); if (!p) { s_vkFailed = 1; XRRERR("vk flush: missing " #n); return 0; }
    LOAD(p_CreatePool, vkCreateCommandPool)
    LOAD(p_AllocCmd,   vkAllocateCommandBuffers)
    LOAD(p_BeginCmd,   vkBeginCommandBuffer)
    LOAD(p_Barrier,    vkCmdPipelineBarrier)
    LOAD(p_EndCmd,     vkEndCommandBuffer)
    LOAD(p_Submit,     vkQueueSubmit)
    LOAD(p_ResetCmd,   vkResetCommandBuffer)
    LOAD(p_CreateFence, vkCreateFence)
    LOAD(p_WaitFences,  vkWaitForFences)
    LOAD(p_ResetFences, vkResetFences)
    LOAD(p_FenceStatus, vkGetFenceStatus)
    LOAD(p_QueueWaitIdle, vkQueueWaitIdle)
    LOAD(p_DeviceWaitIdle, vkDeviceWaitIdle)
    #undef LOAD
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = s_qfam;
    if (p_CreatePool(s_dev, &pci, NULL, &s_pool) != VK_SUCCESS) { s_vkFailed = 1; return 0; }
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = XRR_FLUSH_RING;
    if (p_AllocCmd(s_dev, &ai, s_cmd) != VK_SUCCESS) { s_vkFailed = 1; return 0; }
    for (int i = 0; i < XRR_FLUSH_RING; i++) {
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;   /* first use sees it ready */
        if (p_CreateFence(s_dev, &fi, NULL, &s_fence[i]) != VK_SUCCESS) { s_vkFailed = 1; return 0; }
    }
    s_vkReady = 1;
    XRRLOG("vk flush: barrier ring ready (qfam=%u)", s_qfam);
    return 1;
}

/* Is the barrier ring up (handles bound + entry points loaded)? Lets the frame
 * loop fall back to a synchronous present until Vulkan is ready, then engage the
 * pipeline. Idempotent (vk_lazy_init only runs the setup once). */
int xrr_vk_flush_ready(void) { return vk_lazy_init(); }

/* Record + submit the tile-memory flush barrier for `image` WITHOUT waiting.
 * Returns a ring token to pass to xrr_vk_flush_wait, or -1 if Vulkan isn't ready.
 * isDepth selects the depth-stencil aspect/layout/access (depth swapchains resolve
 * the same way but a color barrier on a depth image is invalid). */
int xrr_vk_flush_submit_ex(uint64_t image, unsigned int arrayLayers, int isDepth) {
    if (!vk_lazy_init()) return -1;
    int idx = s_ring++ % XRR_FLUSH_RING;
    /* this slot's previous submit must be done before we re-record it (its fence
     * was already waited at present time a frame ago, so this is ~instant) */
    p_WaitFences(s_dev, 1, &s_fence[idx], VK_TRUE, 100000000 /*100ms*/);
    p_ResetFences(s_dev, 1, &s_fence[idx]);
    p_ResetCmd(s_cmd[idx], 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_BeginCmd(s_cmd[idx], &bi);
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)(uintptr_t)image;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = arrayLayers ? arrayLayers : 1;
    VkPipelineStageFlags srcStage;
    if (isDepth) {
        b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        srcStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    } else {
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    p_Barrier(s_cmd[idx], srcStage,
              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    p_EndCmd(s_cmd[idx]);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd[idx];
    if (p_Submit(s_queue, 1, &si, s_fence[idx]) != VK_SUCCESS) return -1;
    return idx;
}
/* color-image convenience wrapper (the common case) */
int xrr_vk_flush_submit(uint64_t image, unsigned int arrayLayers) {
    return xrr_vk_flush_submit_ex(image, arrayLayers, 0);
}

/* Block until the flush submitted under `token` has completed. The Meta runtime
 * does NOT synchronize its compositor against our submit, so the image must be
 * fully resolved before we release it to the compositor (else it reads
 * unresolved/invisible writes = black). Called a frame after submit, so the GPU
 * has already finished -> this returns immediately and never stalls the submit. */
void xrr_vk_flush_wait(int token) {
    if (token < 0 || !s_vkReady) return;
    p_WaitFences(s_dev, 1, &s_fence[token], VK_TRUE, 100000000);
}

/* Probe (debug.re4vr.qwait): drain UE's VkQueue so any submitted render completes
 * before we resolve/present. Distinguishes "UE submitted-but-incomplete" (this turns
 * the black image correct) from "UE hadn't submitted yet" (still black -> ordering
 * bug). A hard stall; diagnostic only. */
void xrr_vk_queue_wait_idle(void) {
    if (!vk_lazy_init() || !p_QueueWaitIdle || !s_queue) return;
    p_QueueWaitIdle(s_queue);
}

/* Full GPU completion across ALL queues. UE submits the eye render on its own queue(s)
 * (two seen: s_queue + a second), so draining s_queue alone misses it. Used at the
 * deferred present (which already runs AFTER UE's eye submit) to guarantee the render is
 * COMPLETE before we release+composite — the production form of what the dump's fence-wait
 * did. Heavier than a targeted fence wait; refine to the eye-render fence once confirmed. */
void xrr_vk_device_wait_idle(void) {
    if (!vk_lazy_init() || !p_DeviceWaitIdle || !s_dev) return;
    p_DeviceWaitIdle(s_dev);
}

/* ---- UE vkQueueSubmit hook (render-submit race fix) ----------------------- *
 * RE finding: UE 4.25 submits the eye render through the
 * FVulkan RHI's global PFN VulkanDynamicAPI::vkQueueSubmit AFTER ovrp_EndFrame4 on the
 * RHI thread (proven by the qwait no-op). We patch that global to a trampoline so the
 * shim observes the exact submit and can order present after it (present-on-submit, in
 * xr_runtime.c). The symbol is an EXPORTED BSS global, so dlsym gives its real runtime
 * address (robust to load bias) and the slot is writable (no mprotect). Our own resolve
 * barrier uses p_Submit (a distinct driver pointer), so it never re-enters this hook. */
static PFN_vkQueueSubmit s_realQueueSubmit;
static void           **s_ueSubmitSlot;
static int              s_hookInstalled;

static VKAPI_ATTR VkResult VKAPI_CALL ue_submit_trampoline(
        VkQueue queue, uint32_t count, const VkSubmitInfo *pSubmits, VkFence fence) {
    VkResult r = s_realQueueSubmit(queue, count, pSubmits, fence);   /* real submit first */
    /* notify after it's on the queue, so a same-queue barrier orders FIFO after it */
    xrr_on_ue_submit((uint64_t)(uintptr_t)queue, (uint64_t)(uintptr_t)fence, queue == s_queue);
    return r;
}

int xrr_install_submit_hook(void) {
    if (s_hookInstalled) return 1;
    void *h = dlopen("libUE4.so", RTLD_NOLOAD | RTLD_NOW);
    if (!h) { XRRERR("submithook: libUE4.so not loaded"); return 0; }
    void **slot = (void **)dlsym(h, "_ZN16VulkanDynamicAPI13vkQueueSubmitE");
    if (!slot || !*slot) {
        XRRERR("submithook: global vkQueueSubmit slot=%p val=%p (RHI not up yet?)",
               (void *)slot, slot ? *slot : NULL);
        return 0;
    }
    /* sanity: the live value must look like a real vkQueueSubmit, not garbage */
    Dl_info di; const char *snm = "?", *fnm = "?";
    if (dladdr(*slot, &di)) { if (di.dli_sname) snm = di.dli_sname; if (di.dli_fname) fnm = di.dli_fname; }
    XRRLOG("submithook: UE vkQueueSubmit slot=%p -> %p (%s in %s)", (void *)slot, *slot, snm, fnm);
    if (*slot == (void *)&ue_submit_trampoline) { s_hookInstalled = 1; return 1; }  /* already ours */
    s_realQueueSubmit = (PFN_vkQueueSubmit)*slot;
    s_ueSubmitSlot    = slot;
    *slot = (void *)&ue_submit_trampoline;        /* patch the global */
    s_hookInstalled = 1;
    XRRLOG("submithook: INSTALLED (real=%p tramp=%p, s_queue=%p)",
           (void *)s_realQueueSubmit, (void *)&ue_submit_trampoline, (void *)s_queue);
    return 1;
}

/* ----------------------------------------------- one-shot texture readback -- *
 * Copy one array layer of an eye swapchain image to a host buffer and dump a
 * downsampled PPM, so we can SEE whether UE's rendered eye texture is itself
 * doubled (=> game/UE render) or clean (=> the compositor adds the dupe). Slow &
 * synchronous — gated behind debug.re4vr.dump, fired once. */
static PFN_vkGetPhysicalDeviceMemoryProperties p_MemProps;
static PFN_vkCreateBuffer        p_CreateBuf;
static PFN_vkGetBufferMemoryRequirements p_BufReq;
static PFN_vkAllocateMemory      p_AllocMem;
static PFN_vkBindBufferMemory    p_BindBuf;
static PFN_vkMapMemory           p_MapMem;
static PFN_vkCmdCopyImageToBuffer p_Copy2Buf;
static PFN_vkDestroyBuffer       p_DestroyBuf;
static PFN_vkFreeMemory          p_FreeMem;

static int dump_lazy(PFN_vkGetDeviceProcAddr gdpa) {
    #define DL(p,n) if(!p){ p=(PFN_##n)gdpa(s_dev,#n); if(!p){XRRERR("dump: missing " #n); return 0;} }
    DL(p_CreateBuf, vkCreateBuffer) DL(p_BufReq, vkGetBufferMemoryRequirements)
    DL(p_AllocMem, vkAllocateMemory) DL(p_BindBuf, vkBindBufferMemory)
    DL(p_MapMem, vkMapMemory) DL(p_Copy2Buf, vkCmdCopyImageToBuffer)
    DL(p_DestroyBuf, vkDestroyBuffer) DL(p_FreeMem, vkFreeMemory)
    #undef DL
    return 1;
}

void xrr_vk_dump_image(uint64_t image, unsigned int w, unsigned int h,
                       unsigned int arrayLayer, const char *path) {
    if (!vk_lazy_init() || !s_phys) { XRRERR("dump: not ready"); return; }
    PFN_vkGetDeviceProcAddr gdpa; PFN_vkGetInstanceProcAddr gipa; vk_loaders(&gdpa, &gipa);
    if (!gdpa || !dump_lazy(gdpa)) return;
    if (!p_MemProps) p_MemProps = (gipa && s_inst) ? (PFN_vkGetPhysicalDeviceMemoryProperties)
                                          gipa(s_inst, "vkGetPhysicalDeviceMemoryProperties") : NULL;
    if (!p_MemProps) { XRRERR("dump: no MemProps (inst=%p)", (void*)s_inst); return; }

    VkDeviceSize sz = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = sz; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (p_CreateBuf(s_dev, &bci, NULL, &buf) != VK_SUCCESS) { XRRERR("dump: CreateBuffer"); return; }
    VkMemoryRequirements mr; p_BufReq(s_dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; p_MemProps(s_phys, &mp);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u<<i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; }
    if (mt == UINT32_MAX) { XRRERR("dump: no host-visible mem"); p_DestroyBuf(s_dev,buf,NULL); return; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (p_AllocMem(s_dev, &mai, NULL, &mem) != VK_SUCCESS) { XRRERR("dump: AllocMem"); p_DestroyBuf(s_dev,buf,NULL); return; }
    p_BindBuf(s_dev, buf, mem, 0);

    int idx = s_ring++ % XRR_FLUSH_RING;
    p_WaitFences(s_dev, 1, &s_fence[idx], VK_TRUE, 100000000);
    p_ResetFences(s_dev, 1, &s_fence[idx]);
    p_ResetCmd(s_cmd[idx], 0);
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_BeginCmd(s_cmd[idx], &cbi);
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)(uintptr_t)image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.baseArrayLayer = arrayLayer;
    b.subresourceRange.layerCount = 1;
    p_Barrier(s_cmd[idx], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy rgn; memset(&rgn, 0, sizeof(rgn));
    rgn.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rgn.imageSubresource.mipLevel = 0;
    rgn.imageSubresource.baseArrayLayer = arrayLayer;
    rgn.imageSubresource.layerCount = 1;
    rgn.imageExtent.width = w; rgn.imageExtent.height = h; rgn.imageExtent.depth = 1;
    p_Copy2Buf(s_cmd[idx], (VkImage)(uintptr_t)image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rgn);
    /* restore layout so the compositor/UE is unaffected */
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    p_Barrier(s_cmd[idx], VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    p_EndCmd(s_cmd[idx]);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd[idx];
    if (p_Submit(s_queue, 1, &si, s_fence[idx]) != VK_SUCCESS) { XRRERR("dump: submit"); goto cleanup; }
    p_WaitFences(s_dev, 1, &s_fence[idx], VK_TRUE, 1000000000);

    {   /* downsample to <=256 wide PPM (P6). Format is RGBA8; take RGB. */
        uint8_t *px = NULL;
        if (p_MapMem(s_dev, mem, 0, sz, 0, (void**)&px) != VK_SUCCESS || !px) { XRRERR("dump: map"); goto cleanup; }
        unsigned step = (w > 256) ? (w / 256) : 1;
        unsigned ow = w / step, oh = h / step;
        FILE *f = fopen(path, "wb");
        if (!f) { XRRERR("dump: fopen %s failed (errno path?)", path); goto cleanup; }
        fprintf(f, "P6\n%u %u\n255\n", ow, oh);
        for (unsigned y = 0; y < oh; y++)
            for (unsigned x = 0; x < ow; x++) {
                uint8_t *p = px + ((size_t)(y*step)*w + (x*step)) * 4;
                fwrite(p, 1, 3, f);
            }
        fclose(f);
        XRRLOG("dump: wrote %s (%ux%u from %ux%u layer=%u)", path, ow, oh, w, h, arrayLayer);
    }
cleanup:
    p_FreeMem(s_dev, mem, NULL);
    p_DestroyBuf(s_dev, buf, NULL);
}

/* ---- frame-counter barcode overlay (debug.re4vr.barcode) -------------------
 * Stamp a black/white binary barcode of `value` (the frameIndex we already log) into
 * the top-left of the eye image AFTER UE's render is resolved, so it appears on EVERY
 * frame including truncated/black ones. Lets a Meta Cast recording be aligned
 * frame-exactly to the FRAME/SKIPBLACK logs: read the bits on a black flash -> frameIndex
 * -> grep the log. Black/white survives cast-video compression; subtle color encoding
 * does not. Validation-only; gated off by default. A persistent host-visible staging
 * buffer is filled on the CPU each frame and copied in (no per-frame alloc). */
#define BC_BITS    16                   /* frameIndex low 16 bits (~15 min @72Hz before wrap) */
#define BC_CELLS   (BC_BITS + 1)        /* +1 leading anchor cell (always white = strip locator) */
#define BC_CELL_W  24
#define BC_CELL_H  28
#define BC_GUTTER  2                    /* black gap framing each cell so bits never merge */
#define BC_W       (BC_CELLS * BC_CELL_W)
#define BC_H       (BC_CELL_H + 2 * BC_GUTTER)

static PFN_vkCmdCopyBufferToImage p_Copy2Img;
static VkBuffer       s_bcBuf;
static VkDeviceMemory s_bcMem;
static uint8_t       *s_bcPx;           /* persistent mapping of the staging buffer */

/* lazily create + map the persistent staging buffer and load the copy PFN. */
static int barcode_lazy(PFN_vkGetDeviceProcAddr gdpa) {
    #define BL(p,n) if(!p){ p=(PFN_##n)gdpa(s_dev,#n); if(!p){XRRERR("barcode: missing " #n); return 0;} }
    BL(p_CreateBuf, vkCreateBuffer) BL(p_BufReq, vkGetBufferMemoryRequirements)
    BL(p_AllocMem, vkAllocateMemory) BL(p_BindBuf, vkBindBufferMemory)
    BL(p_MapMem, vkMapMemory) BL(p_Copy2Img, vkCmdCopyBufferToImage)
    #undef BL
    if (s_bcBuf) return 1;
    VkDeviceSize sz = (VkDeviceSize)BC_W * BC_H * 4;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = sz; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (p_CreateBuf(s_dev, &bci, NULL, &s_bcBuf) != VK_SUCCESS) { XRRERR("barcode: CreateBuffer"); return 0; }
    VkMemoryRequirements mr; p_BufReq(s_dev, s_bcBuf, &mr);
    VkPhysicalDeviceMemoryProperties mp; p_MemProps(s_phys, &mp);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u<<i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; }
    if (mt == UINT32_MAX) { XRRERR("barcode: no host-visible mem"); return 0; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
    if (p_AllocMem(s_dev, &mai, NULL, &s_bcMem) != VK_SUCCESS) { XRRERR("barcode: AllocMem"); return 0; }
    p_BindBuf(s_dev, s_bcBuf, s_bcMem, 0);
    if (p_MapMem(s_dev, s_bcMem, 0, sz, 0, (void**)&s_bcPx) != VK_SUCCESS || !s_bcPx) {
        XRRERR("barcode: map"); s_bcPx = NULL; return 0;
    }
    return 1;
}

/* ---- cheap per-frame black detector (debug.re4vr.lumagate) -----------------
 * Sample a few full-width rows of the ALREADY-RESOLVED eye image and return the max
 * luminance (0..255). A truncated/black frame reads ~0 across all rows; any lit scene has
 * bright pixels somewhere on a sampled row -> high max. Unlike the onset-only post-hitch
 * flag, this is PER-FRAME so it covers the whole sustained-black tail. The dump's
 * observer-effect concern doesn't apply: the sync path already flush_waits the resolve, and
 * the black is real content (UE renders empty) — we just read what's there. Call AFTER the
 * resolve wait and BEFORE the barcode stamp (so the strip's white cells don't pollute it). */
#define LUMA_ROWS 5
static VkBuffer       s_luBuf;
static VkDeviceMemory s_luMem;
static uint8_t       *s_luPx;
static unsigned       s_luCap;     /* bytes allocated */

int xrr_vk_frame_luma(uint64_t image, unsigned int w, unsigned int h, unsigned int arrayLayer) {
    if (!vk_lazy_init() || !s_phys) return -1;
    PFN_vkGetDeviceProcAddr gdpa; PFN_vkGetInstanceProcAddr gipa; vk_loaders(&gdpa, &gipa);
    if (!gdpa || !dump_lazy(gdpa)) return -1;     /* dump_lazy loads CreateBuf/Copy2Buf/Map/etc */
    if (!p_MemProps) p_MemProps = (gipa && s_inst) ? (PFN_vkGetPhysicalDeviceMemoryProperties)
                                          gipa(s_inst, "vkGetPhysicalDeviceMemoryProperties") : NULL;
    if (!p_MemProps) return -1;

    unsigned need = LUMA_ROWS * w * 4;
    if (!s_luBuf || need > s_luCap) {
        if (s_luBuf) { p_DestroyBuf(s_dev, s_luBuf, NULL); p_FreeMem(s_dev, s_luMem, NULL); s_luBuf = VK_NULL_HANDLE; s_luPx = NULL; }
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = need; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (p_CreateBuf(s_dev, &bci, NULL, &s_luBuf) != VK_SUCCESS) { XRRERR("luma: CreateBuffer"); return -1; }
        VkMemoryRequirements mr; p_BufReq(s_dev, s_luBuf, &mr);
        VkPhysicalDeviceMemoryProperties mp; p_MemProps(s_phys, &mp);
        uint32_t mt = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((mr.memoryTypeBits & (1u<<i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; }
        if (mt == UINT32_MAX) { XRRERR("luma: no host-visible mem"); return -1; }
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
        if (p_AllocMem(s_dev, &mai, NULL, &s_luMem) != VK_SUCCESS) { XRRERR("luma: AllocMem"); return -1; }
        p_BindBuf(s_dev, s_luBuf, s_luMem, 0);
        if (p_MapMem(s_dev, s_luMem, 0, need, 0, (void**)&s_luPx) != VK_SUCCESS || !s_luPx) { XRRERR("luma: map"); s_luPx = NULL; return -1; }
        s_luCap = need;
    }

    int idx = s_ring++ % XRR_FLUSH_RING;
    p_WaitFences(s_dev, 1, &s_fence[idx], VK_TRUE, 100000000);
    p_ResetFences(s_dev, 1, &s_fence[idx]);
    p_ResetCmd(s_cmd[idx], 0);
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_BeginCmd(s_cmd[idx], &cbi);
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)(uintptr_t)image; b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.baseArrayLayer = arrayLayer; b.subresourceRange.layerCount = 1;
    p_Barrier(s_cmd[idx], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,0,NULL,0,NULL,1,&b);
    /* sample LUMA_ROWS rows spread down the frame (skip y~0.30h where the barcode sits). */
    VkBufferImageCopy rgn[LUMA_ROWS]; memset(rgn, 0, sizeof(rgn));
    const float fy[LUMA_ROWS] = { 0.15f, 0.40f, 0.55f, 0.70f, 0.85f };
    for (int i = 0; i < LUMA_ROWS; i++) {
        rgn[i].bufferOffset = (VkDeviceSize)i * w * 4;
        rgn[i].bufferRowLength = w; rgn[i].bufferImageHeight = 1;
        rgn[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rgn[i].imageSubresource.baseArrayLayer = arrayLayer; rgn[i].imageSubresource.layerCount = 1;
        rgn[i].imageOffset.y = (int)(h * fy[i]);
        rgn[i].imageExtent.width = w; rgn[i].imageExtent.height = 1; rgn[i].imageExtent.depth = 1;
    }
    p_Copy2Buf(s_cmd[idx], (VkImage)(uintptr_t)image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_luBuf, LUMA_ROWS, rgn);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    p_Barrier(s_cmd[idx], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,0,NULL,0,NULL,1,&b);
    p_EndCmd(s_cmd[idx]);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd[idx];
    if (p_Submit(s_queue, 1, &si, s_fence[idx]) != VK_SUCCESS) { XRRERR("luma: submit"); return -1; }
    p_WaitFences(s_dev, 1, &s_fence[idx], VK_TRUE, 100000000);
    /* max luminance over the sampled pixels (RGBA8; max channel is enough for "any light"). */
    int mx = 0; unsigned npx = LUMA_ROWS * w;
    for (unsigned i = 0; i < npx; i++) {
        uint8_t *p = s_luPx + (size_t)i * 4;
        int m = p[0]; if (p[1] > m) m = p[1]; if (p[2] > m) m = p[2];
        if (m > mx) mx = m;
    }
    return mx;
}

void xrr_vk_stamp_barcode(uint64_t image, unsigned int w, unsigned int h,
                          unsigned int arrayLayer, unsigned int value, int flagged) {
    if (!vk_lazy_init() || !s_phys) return;
    PFN_vkGetDeviceProcAddr gdpa; PFN_vkGetInstanceProcAddr gipa; vk_loaders(&gdpa, &gipa);
    if (!gdpa) return;
    if (!p_MemProps) p_MemProps = (gipa && s_inst) ? (PFN_vkGetPhysicalDeviceMemoryProperties)
                                          gipa(s_inst, "vkGetPhysicalDeviceMemoryProperties") : NULL;
    if (!p_MemProps) { XRRERR("barcode: no MemProps"); return; }
    if (!barcode_lazy(gdpa) || !s_bcPx) return;

    /* paint the strip on the CPU as a GRAY ruler (so all 17 cell positions stay visible on
     * ANY frame background, incl. pure black) with each cell BLACK(0) or WHITE(1) on top,
     * gray gutters framing them. anchor cell c=0 = always white (strip locator); cells 1..16
     * = bits of `value`, LSB at cell 1. Three levels (black/gray/white) survive cast-video
     * compression. RGBA8 — these grays are channel-equal so RGBA/BGRA/sRGB don't matter. */
    const uint32_t BLACK = 0xFF000000u, WHITE = 0xFFFFFFFFu, GRAY = 0xFF808080u;
    /* RED when this frame is a flagged LIKELY-TRUNCATED candidate, else gray. (If the
     * swapchain is BGRA the red reads as blue — still clearly != gray, so the flag is
     * unambiguous either way.) Lets the recording show flag-vs-black by eye. */
    const uint32_t REDBG = 0xFF0000FFu;
    uint32_t bg = flagged ? REDBG : GRAY;
    uint32_t *px = (uint32_t*)s_bcPx;
    for (unsigned i = 0; i < (unsigned)(BC_W * BC_H); i++) px[i] = bg;
    for (unsigned c = 0; c < BC_CELLS; c++) {
        int on = (c == 0) ? 1 : (int)((value >> (c - 1)) & 1u);
        uint32_t col = on ? WHITE : BLACK;
        unsigned x0 = c * BC_CELL_W + BC_GUTTER, x1 = (c + 1) * BC_CELL_W - BC_GUTTER;
        for (unsigned y = BC_GUTTER; y < BC_GUTTER + BC_CELL_H; y++)
            for (unsigned x = x0; x < x1; x++) px[y * BC_W + x] = col;
    }

    int idx = s_ring++ % XRR_FLUSH_RING;
    p_WaitFences(s_dev, 1, &s_fence[idx], VK_TRUE, 100000000);
    p_ResetFences(s_dev, 1, &s_fence[idx]);
    p_ResetCmd(s_cmd[idx], 0);
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_BeginCmd(s_cmd[idx], &cbi);
    /* eye color images live in COLOR_ATTACHMENT_OPTIMAL here (same assumption as the
     * resolve path); flip the layer to TRANSFER_DST, copy the strip, flip back. */
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)(uintptr_t)image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.baseArrayLayer = arrayLayer;
    b.subresourceRange.layerCount = 1;
    p_Barrier(s_cmd[idx], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy rgn; memset(&rgn, 0, sizeof(rgn));
    rgn.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rgn.imageSubresource.mipLevel = 0;
    rgn.imageSubresource.baseArrayLayer = arrayLayer;
    rgn.imageSubresource.layerCount = 1;
    /* upper-center: the Meta Cast crops the eye render to 16:9 (top/bottom letterboxed
     * off), and the extreme FOV corner is unviewable in-headset — so (0,0) is invisible.
     * Center horizontally; place ~30% down, inside the cast's visible band. */
    rgn.imageOffset.x = (w > BC_W) ? (int)((w - BC_W) / 2) : 0;
    rgn.imageOffset.y = (h > BC_H) ? (int)(h * 30 / 100) : 0;
    rgn.imageExtent.width = BC_W; rgn.imageExtent.height = BC_H; rgn.imageExtent.depth = 1;
    p_Copy2Img(s_cmd[idx], s_bcBuf, (VkImage)(uintptr_t)image,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    p_Barrier(s_cmd[idx], VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    p_EndCmd(s_cmd[idx]);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd[idx];
    if (p_Submit(s_queue, 1, &si, s_fence[idx]) != VK_SUCCESS) { XRRERR("barcode: submit"); return; }
    p_WaitFences(s_dev, 1, &s_fence[idx], VK_TRUE, 100000000);   /* tiny copy; done before release */
}

/* ------------------------------------------------- shim-owned copy ring ----- *
 * UE renders into these shim images (handed to it via GetLayerTexture2) on its own
 * stage cadence; each frame we copy shimImages[TextureStage] into the freshly
 * acquired OpenXR image. The copy both resolves tile memory AND lets the frame loop
 * pipeline it (wait the PREVIOUS frame's copy, already done) so the CPU never blocks
 * on the current frame's GPU -> breaks the flush-wait serialization. Decoupled from
 * UE's stage so no stage-coupling break. Gated by debug.re4vr.copyring (off default). */
static PFN_vkCreateImage              p_CreateImage;
static PFN_vkGetImageMemoryRequirements p_ImgReq;
static PFN_vkBindImageMemory          p_BindImg;
static PFN_vkDestroyImage             p_DestroyImage;
static PFN_vkCmdCopyImage             p_CopyImg;

static int copyring_lazy(PFN_vkGetDeviceProcAddr gdpa) {
    #define CL(p,n) if(!p){ p=(PFN_##n)gdpa(s_dev,#n); if(!p){XRRERR("copyring: missing " #n); return 0;} }
    CL(p_CreateImage, vkCreateImage) CL(p_ImgReq, vkGetImageMemoryRequirements)
    CL(p_BindImg, vkBindImageMemory) CL(p_DestroyImage, vkDestroyImage)
    CL(p_CopyImg, vkCmdCopyImage)
    /* memory PFNs reused from dump path */
    CL(p_AllocMem, vkAllocateMemory) CL(p_FreeMem, vkFreeMemory)
    #undef CL
    return 1;
}

/* Allocate `count` device-local color images (matching the OpenXR eye swapchain) for
 * UE to render into. Returns image handles in out[], backing memory in outMem[]. */
int xrr_vk_alloc_images_ex(uint64_t *out, uint64_t *outMem, int count,
                        unsigned int w, unsigned int h, unsigned int arraySize,
                        long long vkFormat, int isDepth) {
    if (!vk_lazy_init() || !s_phys) { XRRERR("copyring: vk not ready"); return 0; }
    PFN_vkGetDeviceProcAddr gdpa; PFN_vkGetInstanceProcAddr gipa; vk_loaders(&gdpa, &gipa);
    if (!gdpa || !copyring_lazy(gdpa)) return 0;
    if (!p_MemProps) p_MemProps = (gipa && s_inst) ? (PFN_vkGetPhysicalDeviceMemoryProperties)
                                  gipa(s_inst, "vkGetPhysicalDeviceMemoryProperties") : NULL;
    if (!p_MemProps) { XRRERR("copyring: no MemProps"); return 0; }
    VkPhysicalDeviceMemoryProperties mp; p_MemProps(s_phys, &mp);
    for (int i = 0; i < count; i++) {
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        /* MUTABLE_FORMAT: UE renders the eye target through a linear (UNORM) image view
         * but samples/displays it as sRGB — that needs format-mutable views, which
         * OpenXR runtimes put on their swapchain images. Without it UE's gameplay
         * render path (linear view) fails -> black, while the title path works. */
        ici.flags = isDepth ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = (VkFormat)vkFormat;
        ici.extent.width = w; ici.extent.height = h; ici.extent.depth = 1;
        ici.mipLevels = 1; ici.arrayLayers = arraySize ? arraySize : 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        /* Match the broad usage Meta gives its own swapchain images — UE's mobile
         * gameplay path uses the eye target as an INPUT_ATTACHMENT (subpass resolve)
         * and may clear it via transfer; a narrow usage makes gameplay render black
         * while the simpler title path still works. Depth hold-images (Lever C) use the
         * depth-stencil attachment usage instead so the layout transitions are valid. */
        ici.usage = isDepth
            ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
            : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkImage img = VK_NULL_HANDLE;
        if (p_CreateImage(s_dev, &ici, NULL, &img) != VK_SUCCESS) { XRRERR("copyring: CreateImage %d", i); return 0; }
        VkMemoryRequirements mr; p_ImgReq(s_dev, img, &mr);
        uint32_t mt = UINT32_MAX;
        for (uint32_t k = 0; k < mp.memoryTypeCount; k++)
            if ((mr.memoryTypeBits & (1u<<k)) &&
                (mp.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = k; break; }
        if (mt == UINT32_MAX) { XRRERR("copyring: no device-local mem"); p_DestroyImage(s_dev,img,NULL); return 0; }
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size; mai.memoryTypeIndex = mt;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (p_AllocMem(s_dev, &mai, NULL, &mem) != VK_SUCCESS) { XRRERR("copyring: AllocMem %d", i); p_DestroyImage(s_dev,img,NULL); return 0; }
        p_BindImg(s_dev, img, mem, 0);
        out[i] = (uint64_t)(uintptr_t)img;
        outMem[i] = (uint64_t)(uintptr_t)mem;
    }
    XRRLOG("copyring: allocated %d shim images %ux%u array=%u fmt=%lld depth=%d", count, w, h, arraySize, vkFormat, isDepth);
    return 1;
}
/* color-image convenience wrapper (the common case) */
int xrr_vk_alloc_images(uint64_t *out, uint64_t *outMem, int count,
                        unsigned int w, unsigned int h, unsigned int arraySize,
                        long long vkFormat) {
    return xrr_vk_alloc_images_ex(out, outMem, count, w, h, arraySize, vkFormat, 0);
}

void xrr_vk_free_images(uint64_t *imgs, uint64_t *mem, int count) {
    if (!s_vkReady || !p_DestroyImage) return;
    for (int i = 0; i < count; i++) {
        if (imgs[i]) p_DestroyImage(s_dev, (VkImage)(uintptr_t)imgs[i], NULL);
        if (mem[i])  p_FreeMem(s_dev, (VkDeviceMemory)(uintptr_t)mem[i], NULL);
        imgs[i] = mem[i] = 0;
    }
}

/* Copy image src -> dst (all array layers), resolving tile memory, WITHOUT waiting.
 * Returns a ring token for xrr_vk_flush_wait. Used bidirectionally (swapchain<->shim hold
 * image) for reproject save/restore. isDepth selects the depth aspect + depth-stencil
 * attachment layouts (Lever C depth-hold); both endpoints rest in *_ATTACHMENT_OPTIMAL so
 * the same barriers work either direction (dst content is discarded via UNDEFINED). */
int xrr_vk_copy_submit_ex(uint64_t srcShim, uint64_t dstXr,
                       unsigned int w, unsigned int h, unsigned int arrayLayers, int isDepth) {
    if (!vk_lazy_init() || !p_CopyImg) return -1;
    int idx = s_ring++ % XRR_FLUSH_RING;
    p_WaitFences(s_dev, 1, &s_fence[idx], VK_TRUE, 100000000);
    p_ResetFences(s_dev, 1, &s_fence[idx]);
    p_ResetCmd(s_cmd[idx], 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_BeginCmd(s_cmd[idx], &bi);
    uint32_t layers = arrayLayers ? arrayLayers : 1;
    VkImage src = (VkImage)(uintptr_t)srcShim, dst = (VkImage)(uintptr_t)dstXr;
    VkImageAspectFlags aspect = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout attachLayout = isDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                         : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAccessFlags attachWrite = isDepth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                        : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkPipelineStageFlags attachStage = isDepth
        ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
        : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkImageMemoryBarrier b[2]; memset(b, 0, sizeof(b));
    for (int i = 0; i < 2; i++) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].subresourceRange.aspectMask = aspect;
        b[i].subresourceRange.levelCount = 1;
        b[i].subresourceRange.layerCount = layers;
    }
    /* src: *_ATTACHMENT -> TRANSFER_SRC ; dst: UNDEFINED -> TRANSFER_DST */
    b[0].srcAccessMask = attachWrite; b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].oldLayout = attachLayout; b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].image = src;
    b[1].srcAccessMask = 0; b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].image = dst;
    p_Barrier(s_cmd[idx], attachStage | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b);
    VkImageCopy rgn; memset(&rgn, 0, sizeof(rgn));
    rgn.srcSubresource.aspectMask = aspect; rgn.srcSubresource.layerCount = layers;
    rgn.dstSubresource.aspectMask = aspect; rgn.dstSubresource.layerCount = layers;
    rgn.extent.width = w; rgn.extent.height = h; rgn.extent.depth = 1;
    p_CopyImg(s_cmd[idx], src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    /* dst: TRANSFER_DST -> *_ATTACHMENT (compositor/UE reads) ; src: TRANSFER_SRC -> *_ATTACHMENT (reuse) */
    b[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b[0].newLayout = attachLayout;
    b[0].image = dst;
    b[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b[1].dstAccessMask = attachWrite;
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b[1].newLayout = attachLayout;
    b[1].image = src;
    p_Barrier(s_cmd[idx], VK_PIPELINE_STAGE_TRANSFER_BIT,
              attachStage | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
              0, 0, NULL, 0, NULL, 2, b);
    p_EndCmd(s_cmd[idx]);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd[idx];
    if (p_Submit(s_queue, 1, &si, s_fence[idx]) != VK_SUCCESS) return -1;
    return idx;
}
/* color-image convenience wrapper (the common case) */
int xrr_vk_copy_submit(uint64_t srcShim, uint64_t dstXr,
                       unsigned int w, unsigned int h, unsigned int arrayLayers) {
    return xrr_vk_copy_submit_ex(srcShim, dstXr, w, h, arrayLayers, 0);
}

uint32_t xrr_vk_enumerate_images(XrSwapchain sc, uint64_t *out, uint32_t max) {
    uint32_t n = 0;
    if (xrEnumerateSwapchainImages(sc, 0, &n, NULL) != XR_SUCCESS) return 0;
    if (n > max) n = max;
    XrSwapchainImageVulkanKHR imgs[XRR_MAX_IMAGES];
    for (uint32_t i = 0; i < n; i++) {
        imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        imgs[i].next = NULL;
    }
    uint32_t got = 0;
    if (xrEnumerateSwapchainImages(sc, n, &got,
            (XrSwapchainImageBaseHeader *)imgs) != XR_SUCCESS)
        return 0;
    for (uint32_t i = 0; i < got; i++)
        out[i] = (uint64_t)(uintptr_t)imgs[i].image;
    return got;
}

/* Reset shim Vulkan state so a post-Shutdown2 re-init (new VkDevice from a fresh ovrp_Initialize5)
 * rebuilds the command pool/fences/staging buffers instead of reusing handles from the dead device.
 * Called from xrr_shutdown under g_xrlock. Objects from the old device are left to the driver/process
 * to reclaim — destroying them needs PFNs we don't load, and Shutdown2 is normally process-exit. */
void xrr_vk_teardown(void) {
    s_vkReady = 0; s_vkFailed = 0;
    s_pool = VK_NULL_HANDLE;
    for (int i = 0; i < XRR_FLUSH_RING; i++) { s_cmd[i] = VK_NULL_HANDLE; s_fence[i] = VK_NULL_HANDLE; }
    s_bcBuf = VK_NULL_HANDLE; s_bcMem = VK_NULL_HANDLE; s_bcPx = NULL;
    s_luBuf = VK_NULL_HANDLE; s_luMem = VK_NULL_HANDLE; s_luPx = NULL; s_luCap = 0;
}
