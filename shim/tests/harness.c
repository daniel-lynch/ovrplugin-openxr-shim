/* harness.c — drives the ovrp_* sequence against a real OpenXR runtime
 * (Path B: Monado simulated/headless). Smoke-tests the shim's OpenXR usage
 * without RE4 or a headset. See ../../TESTING.md.
 *
 * Build (inside a Linux env with an OpenXR loader):
 *   cc -std=c11 -I../include -I../third_party/openxr harness.c \
 *      ../src/stubs.c ../src/core.c ../src/xr_runtime.c ../src/vk_session.c \
 *      ../src/layers.c -lopenxr_loader -o harness
 *   XR_RUNTIME_JSON=/path/openxr_monado-dev.json ./harness
 *
 * NOTE: Initialize5 -> xrCreateSession needs real Vulkan handles. For a first
 * smoke test we stop after PreInitialize3 (instance+system create) unless real
 * VkInstance/Device handles are provided. Set WITH_VK=1 + fill the handles to go
 * further once a headless Vulkan device is available.
 */
#include "ovrplugin_shim.h"
#include <stdio.h>

#define CHECK(expr) do { \
    ovrpResult _r = (expr); \
    printf("  %-34s -> %d %s\n", #expr, _r, OVRP_SUCCESS(_r) ? "OK" : "(fail)"); \
} while (0)

int main(void) {
    printf("== ovrp shim smoke test ==\n");

    printf("[lifecycle]\n");
    CHECK(ovrp_PreInitialize3(NULL));          /* xrCreateInstance + xrGetSystem */

    /* Session creation needs real Vulkan handles; pass NULLs and expect failure
     * until a headless Vulkan device is wired (see TESTING.md Path B note). */
    printf("[init — expect fail without Vulkan handles]\n");
    CHECK(ovrp_Initialize5(ovrpRenderAPI_Vulkan, NULL, NULL, NULL,
                           NULL, NULL, NULL, 0, 0));

    printf("[frame loop — will report not-ready until session exists]\n");
    for (int f = 0; f < 3; f++) {
        ovrp_Update3(ovrpStep_Render, f, 0.0);
        ovrp_WaitToBeginFrame(f);
        ovrp_BeginFrame4(f, NULL);
        ovrpPoseStatef head;
        ovrp_GetNodePoseState3(ovrpStep_Render, f, ovrpNode_Head, &head);
        double t = 0; ovrp_GetPredictedDisplayTime(f, &t);
        ovrp_EndFrame4(f, NULL, 0, NULL);
        printf("  frame %d: head.w=%.3f t=%.6f\n", f, head.Pose.Orientation.w, t);
    }

    printf("[shutdown]\n");
    CHECK(ovrp_Shutdown2());
    printf("== done ==\n");
    return 0;
}
