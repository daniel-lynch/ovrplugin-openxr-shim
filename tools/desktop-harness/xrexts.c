/* xrexts.c — list the OpenXR instance extensions the active runtime advertises.
 * Recon for the foveation bring-up: tells us which foveation / FDM / eye-tracking
 * extensions THIS runtime (Monado/Lepton) exposes, so we know what to wire into the
 * shim's apply_foveation() extension point. No Vulkan, no session — just enumerate.
 *   cc xrexts.c -lopenxr_loader -o xrexts && XR_RUNTIME_JSON=.../openxr_monado.json ./xrexts */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openxr/openxr.h>

int main(void) {
    uint32_t n = 0;
    if (xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL) != XR_SUCCESS || !n) {
        fprintf(stderr, "xrEnumerateInstanceExtensionProperties failed (runtime selected? service up?)\n");
        return 1;
    }
    XrExtensionProperties *p = calloc(n, sizeof *p);
    for (uint32_t i = 0; i < n; i++) p[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    if (xrEnumerateInstanceExtensionProperties(NULL, n, &n, p) != XR_SUCCESS) return 1;

    printf("runtime advertises %u instance extensions:\n", n);
    for (uint32_t i = 0; i < n; i++) {
        const char *e = p[i].extensionName;
        int hot = strcasestr(e, "fov") || strcasestr(e, "foveat") || strcasestr(e, "density")
               || strcasestr(e, "fdm") || strcasestr(e, "eye")  || strcasestr(e, "gaze")
               || strcasestr(e, "vrs") || strcasestr(e, "shading_rate") || strcasestr(e, "quad");
        printf("  %s %s (v%u)\n", hot ? "**" : "  ", e, p[i].extensionVersion);
    }
    return 0;
}
