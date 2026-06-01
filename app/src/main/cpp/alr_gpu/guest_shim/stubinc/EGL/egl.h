/* stubinc/EGL/egl.h — host-only EGL placeholder for the wire-format check harness.
 *
 * alr_gpu_decode.hpp does #include <EGL/egl.h> but uses NO egl* symbol — the
 * include is just for symmetry with the device build. This stub satisfies that
 * include on a host with no Khronos headers. Never shipped to the device.
 */
#ifndef ALR_WIRECHECK_EGL_EGL_H
#define ALR_WIRECHECK_EGL_EGL_H

#include <stdint.h>

typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLConfig;
typedef unsigned int EGLBoolean;
typedef int32_t EGLint;

#endif /* ALR_WIRECHECK_EGL_EGL_H */
