/* passthru.c — P4 native-parity forwarding. See passthru.h. Loads the real, SONAME-
 * patched libOVRPlugin_real.so (deps incl. libvrapi ride along in the APK) and hands
 * the whole OVRPlugin session to it when debug.re4vr.passthru=1, so we can capture
 * native's per-eye poses/FOV/submit for the ghost diff. */
#include "ovrplugin_shim.h"
#include "passthru.h"
#include "xr_runtime.h"
#include "log.h"
#include <dlfcn.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#include <jni.h>
#endif

/* The asm trampolines read this flag directly; hidden visibility keeps the load a
 * direct adrp/ldr (no GOT, no interposition). */
__attribute__((visibility("hidden"))) int g_pt_active = 0;
static void *g_pt_handle = 0;

/* ---- signature-agnostic stub forwarders --------------------------------------------
 * A few exports the game calls live only as no-arg () stubs in stubs.c (two aren't even
 * in OVR_Plugin.h, so we have no C signature to forward through). A naked arm64 tail-call
 * trampoline forwards them verbatim: it never touches the arg registers (x0-x7/v0-v7/x8),
 * so it works for ANY signature. Passthru off (or symbol missing) -> falls through to the
 * SAME return constant the original autogen stub used (non-passthru behavior unchanged). */
#if defined(__aarch64__)
#define PT_STUB(N, CONST)                                                        \
    __attribute__((visibility("hidden"))) void *g_pt_real_##N = 0;               \
    __attribute__((naked, visibility("default"))) ovrpResult ovrp_##N(void) {    \
        __asm__ volatile(                                                        \
            "adrp x16, g_pt_active\n\t"                                          \
            "ldr  w17, [x16, :lo12:g_pt_active]\n\t"                             \
            "cbz  w17, 1f\n\t"                                                   \
            "adrp x16, g_pt_real_" #N "\n\t"                                     \
            "ldr  x16, [x16, :lo12:g_pt_real_" #N "]\n\t"                        \
            "cbz  x16, 1f\n\t"                                                   \
            "br   x16\n\t"                                                       \
            "1:\n\t"                                                             \
            "mov  w0, #" #CONST "\n\t"                                           \
            "ret\n\t");                                                          \
    }
#else /* host validation build: plain stub, no forwarding */
#define PT_STUB(N, CONST)                                                        \
    void *g_pt_real_##N = 0;                                                     \
    ovrpResult ovrp_##N(void) { return (ovrpResult)(CONST); }
#endif

/* The game-called () stubs (confirmed in device log). CONST = the constant the original
 * autogen stub returned -> non-passthru path is byte-identical. These names are removed
 * from stubs.c via gen_stubs.sh HEADER_FNS so there's no duplicate symbol. */
PT_STUB(DestroyDistortionWindow2,           0)      /* ovrpSuccess */
PT_STUB(SetupDisplayObjects2,               -1005)  /* ovrpFailure_NotYetImplemented */
PT_STUB(SetReorientHMDOnControllerRecenter, 0)
PT_STUB(SetClientColorDesc,                 0)
PT_STUB(SetAppEngineInfo2,                  0)
PT_STUB(SetAppCPUPriority2,                 0)
PT_STUB(InitializeMixedReality,             -1005)
PT_STUB(GetViewportStencil,                 -1005)
PT_STUB(GetSystemRecommendedMSAALevel2,     -1005)
PT_STUB(GetLocalTrackingSpaceRecenterCount, -1005)
PT_STUB(GetLayerTextureFoveation,           -1005)
PT_STUB(GetControllerHapticsDesc2,          -1005)

static void pt_first_use(void) {
    static int done = 0;
    if (done) return;
    done = 1;
#ifdef __ANDROID__
    char s[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.re4vr.passthru", s) <= 0 || s[0] != '1') return;
    void *h = dlopen("libOVRPlugin_real.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { XRRLOG("PASSTHRU: dlopen real libOVRPlugin FAILED: %s", dlerror()); return; }
    g_pt_handle = h;

    /* ART only calls JNI_OnLoad for System.loadLibrary, not a native dlopen — so the
     * real lib's cached JavaVM is null and its VrApi PreInitialize derefs it (SIGSEGV in
     * CompositorVRAPI::PreInitialize). Replicate the loadLibrary handshake by hand. */
    jint (*real_jni_onload)(JavaVM *, void *) =
        (jint (*)(JavaVM *, void *))dlsym(h, "JNI_OnLoad");
    JavaVM *vm = (JavaVM *)xrr_android_get_vm();
    if (real_jni_onload && vm) {
        jint v = real_jni_onload(vm, 0);
        XRRLOG("PASSTHRU: primed real JNI_OnLoad(vm=%p) -> 0x%x", (void *)vm, v);
    } else {
        XRRLOG("PASSTHRU: WARN no real JNI_OnLoad/vm (onload=%p vm=%p) — PreInitialize may crash",
               (void *)real_jni_onload, (void *)vm);
    }
#define PT_RESOLVE(N) g_pt_real_##N = dlsym(h, "ovrp_" #N)
    PT_RESOLVE(DestroyDistortionWindow2);
    PT_RESOLVE(SetupDisplayObjects2);
    PT_RESOLVE(SetReorientHMDOnControllerRecenter);
    PT_RESOLVE(SetClientColorDesc);
    PT_RESOLVE(SetAppEngineInfo2);
    PT_RESOLVE(SetAppCPUPriority2);
    PT_RESOLVE(InitializeMixedReality);
    PT_RESOLVE(GetViewportStencil);
    PT_RESOLVE(GetSystemRecommendedMSAALevel2);
    PT_RESOLVE(GetLocalTrackingSpaceRecenterCount);
    PT_RESOLVE(GetLayerTextureFoveation);
    PT_RESOLVE(GetControllerHapticsDesc2);
#undef PT_RESOLVE
    g_pt_active = 1;   /* set last: trampolines must not forward until the table is built */
    XRRLOG("PASSTHRU: ACTIVE — real libOVRPlugin owns the session (h=%p). Shim OpenXR path disabled.", h);
#endif
}

int pt_active(void) { pt_first_use(); return g_pt_active; }
void *pt_real(const char *name) { return g_pt_handle ? dlsym(g_pt_handle, name) : 0; }

/* Rate-limited native call census. Keyed by the string-literal pointer (each PT_FWD call
 * site passes a stable literal), so per-function. Logs the first 3 calls then every 1200th
 * — enough to capture the full set of game-called exports + native's return for each,
 * without flooding on the per-frame getters. Diff `PTC` lines vs our shim's returns. */
void pt_log_call(const char *name, long ret) {
    enum { MAXN = 320 };
    static const char *seen[MAXN];
    static unsigned     cnt[MAXN];
    static int          n = 0;
    int i;
    for (i = 0; i < n; i++) if (seen[i] == name) break;
    if (i == n) {
        if (n >= MAXN) return;
        seen[n] = name; cnt[n] = 0; i = n; n++;
    }
    unsigned c = ++cnt[i];
    if (c <= 3 || (c % 1200) == 0)
        XRRLOG("PTC %s -> %ld (#%u)", name, ret, c);
}
