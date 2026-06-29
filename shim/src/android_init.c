/* android_init.c — Android OpenXR instance handshake.
 *
 * On Android the OpenXR loader must be primed with the JavaVM + a Context via
 * xrInitializeLoaderKHR BEFORE xrCreateInstance, and xrCreateInstance needs
 * XR_KHR_android_create_instance with XrInstanceCreateInfoAndroidKHR chained in.
 *
 * JavaVM: captured in JNI_OnLoad (called when the .so loads).
 * Activity/Context: preferred from ovrp_Initialize5 arg4; but PreInitialize3
 *   creates the instance earlier, so we fall back to the Application context via
 *   ActivityThread reflection. [VERIFY-ON-HW] whether Meta's runtime accepts the
 *   Application context or wants the actual Activity.
 *
 * Host build: the #else stubs make this a no-op so xr_runtime.c is portable.
 */
#include "xr_runtime.h"

#ifdef __ANDROID__
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr_platform.h>

static JavaVM *g_vm;
static jobject g_context;   /* global ref: Activity or Application context */
static XrInstanceCreateInfoAndroidKHR g_androidCreate;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;
    return JNI_VERSION_1_6;
}

/* P4 passthru: the real (dlopen'd) libOVRPlugin captures the JavaVM in its own
 * JNI_OnLoad, which ART only invokes for System.loadLibrary — not for a native
 * dlopen. passthru.c hands this VM to the real lib's JNI_OnLoad so its VrApi path
 * doesn't deref a null VM in CompositorVRAPI::PreInitialize. */
void *xrr_android_get_vm(void) { return g_vm; }

static JNIEnv *get_env(void) {
    JNIEnv *env = NULL;
    if (!g_vm) return NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) == JNI_OK) return env;
    return NULL;
}

/* fallback: get the Application context via ActivityThread reflection */
static jobject get_app_context(JNIEnv *env) {
    jclass at = (*env)->FindClass(env, "android/app/ActivityThread");
    if (!at) return NULL;
    jmethodID cur = (*env)->GetStaticMethodID(env, at,
        "currentActivityThread", "()Landroid/app/ActivityThread;");
    jobject atObj = (*env)->CallStaticObjectMethod(env, at, cur);
    if (!atObj) return NULL;
    jmethodID getApp = (*env)->GetMethodID(env, at,
        "getApplication", "()Landroid/app/Application;");
    jobject app = (*env)->CallObjectMethod(env, atObj, getApp);
    return app ? (*env)->NewGlobalRef(env, app) : NULL;
}

static int g_realActivitySet;

void xrr_set_android_activity(void *activity) {
    JNIEnv *env = get_env();
    if (env && activity) {
        g_context = (*env)->NewGlobalRef(env, (jobject)activity);
        g_realActivitySet = 1;     /* the real GameActivity, not the App context */
    }
}
int xrr_android_have_real_activity(void) { return g_realActivitySet; }

static jobject ensure_context(void) {
    if (g_context) return g_context;
    JNIEnv *env = get_env();
    if (env) g_context = get_app_context(env);
    return g_context;
}

int xrr_android_init_loader(void) {
    PFN_xrInitializeLoaderKHR init = NULL;
    if (xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
            (PFN_xrVoidFunction *)&init) != XR_SUCCESS || !init)
        return 0;
    XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
    li.applicationVM      = g_vm;
    li.applicationContext = ensure_context();
    return XR_SUCCEEDED(init((XrLoaderInitInfoBaseHeaderKHR *)&li));
}

void *xrr_android_instance_next(void) {
    g_androidCreate = (XrInstanceCreateInfoAndroidKHR){
        XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    g_androidCreate.applicationVM       = g_vm;
    g_androidCreate.applicationActivity = ensure_context();
    return &g_androidCreate;
}

#else  /* ----- host build: no-ops ----- */
int   xrr_android_init_loader(void)   { return 1; }
void *xrr_android_instance_next(void) { return 0; }
void  xrr_set_android_activity(void *a) { (void)a; }
int   xrr_android_have_real_activity(void) { return 0; }
void *xrr_android_get_vm(void) { return 0; }
#endif
