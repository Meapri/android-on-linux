/* alr_khr_egl.h — a MINIMAL, self-contained EGL 1.4 header for the ALR shim.
 *
 * Just the EGL types, tokens, and the entry points the cube uses, with the
 * STANDARD Khronos values (public EGL registry). Vendored so the shim
 * (libEGL.so.1) and the cube build without system Khronos headers and so the
 * guest rootfs can rebuild them. ABI-compatible with <EGL/egl.h>. Pure C.
 *
 * NOTE on EGLNativeWindowType: on real Android EGL this is `ANativeWindow*`.
 * In the ALR guest there is no ANativeWindow — the cube passes whatever the
 * shim's eglCreateWindowSurface accepts; the shim treats the window handle as
 * an opaque value (the host owns the real surface/AHB). We typedef it to void*
 * for guest portability.
 */
#ifndef ALR_KHR_EGL_H
#define ALR_KHR_EGL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- EGL types (match <EGL/egl.h>) ---- */
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef int32_t      EGLint;
typedef void        *EGLDisplay;
typedef void        *EGLConfig;
typedef void        *EGLSurface;
typedef void        *EGLContext;
typedef void        *EGLNativeDisplayType;
typedef void        *EGLNativeWindowType;
typedef void       (*__eglMustCastToProperFunctionPointerType)(void);

/* ---- tokens (standard registry values) ---- */
#define EGL_FALSE                  0
#define EGL_TRUE                   1

#define EGL_DEFAULT_DISPLAY        ((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY             ((EGLDisplay)0)
#define EGL_NO_SURFACE            ((EGLSurface)0)
#define EGL_NO_CONTEXT            ((EGLContext)0)

#define EGL_SUCCESS                0x3000
#define EGL_NOT_INITIALIZED        0x3001
#define EGL_BAD_ALLOC              0x3003
#define EGL_BAD_ATTRIBUTE          0x3004
#define EGL_BAD_PARAMETER          0x300C

#define EGL_ALPHA_SIZE             0x3021
#define EGL_BLUE_SIZE              0x3022
#define EGL_GREEN_SIZE             0x3023
#define EGL_RED_SIZE               0x3024
#define EGL_DEPTH_SIZE             0x3025
#define EGL_STENCIL_SIZE           0x3026
#define EGL_SURFACE_TYPE           0x3033
#define EGL_NONE                   0x3038
#define EGL_RENDERABLE_TYPE        0x3040
#define EGL_VENDOR                 0x3053
#define EGL_VERSION                0x3054
#define EGL_EXTENSIONS             0x3055
#define EGL_HEIGHT                 0x3056   /* registry: EGL_HEIGHT=0x3056, EGL_WIDTH=0x3057 */
#define EGL_WIDTH                  0x3057
#define EGL_CLIENT_APIS            0x308D
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_CONFIG_ID              0x3028
#define EGL_CONTEXT_CLIENT_TYPE    0x3097
#define EGL_BACK_BUFFER            0x3084
#define EGL_SINGLE_BUFFER          0x3085
#define EGL_RENDER_BUFFER          0x3086
#define EGL_OPENGL_ES_API          0x30A0

/* full config-attribute set (eglGetConfigAttrib — glmark2 reads these to score a config) */
#define EGL_BUFFER_SIZE            0x3020
#define EGL_CONFIG_CAVEAT          0x3027
#define EGL_LEVEL                  0x3029
#define EGL_MAX_PBUFFER_HEIGHT     0x302A
#define EGL_MAX_PBUFFER_PIXELS     0x302B
#define EGL_MAX_PBUFFER_WIDTH      0x302C
#define EGL_NATIVE_RENDERABLE      0x302D
#define EGL_NATIVE_VISUAL_ID       0x302E
#define EGL_NATIVE_VISUAL_TYPE     0x302F
#define EGL_SAMPLES                0x3031
#define EGL_SAMPLE_BUFFERS         0x3032
#define EGL_TRANSPARENT_TYPE       0x3034
#define EGL_LUMINANCE_SIZE         0x303D
#define EGL_ALPHA_MASK_SIZE        0x303E
#define EGL_MIN_SWAP_INTERVAL      0x303B
#define EGL_MAX_SWAP_INTERVAL      0x303C
#define EGL_COLOR_BUFFER_TYPE      0x303F
#define EGL_CONFORMANT             0x3042
#define EGL_RGB_BUFFER             0x308E

#define EGL_PBUFFER_BIT            0x0001
#define EGL_WINDOW_BIT             0x0004
#define EGL_OPENGL_ES2_BIT         0x0004

/* ---- entry points the cube/shim use (the exported ABI of libEGL.so.1) ---- */
EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id);
EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size, EGLint *num_config);
EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value);
EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativeWindowType win, const EGLint *attrib_list);
EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list);
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                            const EGLint *attrib_list);
EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLBoolean eglBindAPI(EGLenum api);
EGLDisplay eglGetCurrentDisplay(void);
EGLContext eglGetCurrentContext(void);
EGLSurface eglGetCurrentSurface(EGLint readdraw);
EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);
EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval);
EGLint     eglGetError(void);
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname);
const char *eglQueryString(EGLDisplay dpy, EGLint name);
EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLBoolean eglTerminate(EGLDisplay dpy);
EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value);
EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value);
EGLenum    eglQueryAPI(void);
EGLBoolean eglWaitClient(void);
EGLBoolean eglWaitGL(void);
EGLBoolean eglWaitNative(EGLint engine);
EGLBoolean eglReleaseThread(void);
EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value);
EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig *configs, EGLint config_size, EGLint *num_config);

#ifdef __cplusplus
}
#endif

#endif /* ALR_KHR_EGL_H */
