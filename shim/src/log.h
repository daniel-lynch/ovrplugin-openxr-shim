/* log.h — XRRLOG goes to logcat (tag "xrr") on Android, stderr on host.
 * grep it on-device:  adb logcat | grep -i xrr   */
#ifndef XRR_LOG_H
#define XRR_LOG_H
#ifdef __ANDROID__
#include <android/log.h>
#define XRRLOG(...) __android_log_print(ANDROID_LOG_INFO,  "xrr", __VA_ARGS__)
#define XRRERR(...) __android_log_print(ANDROID_LOG_ERROR, "xrr", __VA_ARGS__)
#else
#include <stdio.h>
#define XRRLOG(...) do { fprintf(stderr, "[xrr] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define XRRERR(...) do { fprintf(stderr, "[xrr] ERR " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif
#endif
