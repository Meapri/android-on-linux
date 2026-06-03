/* alr-angle-vk.c — the ANGLE-on-Vulkan INIT+CLEAR+PRESENT proof client.
 *
 * Unlike alr-gles-cube (which uses eglGetDisplay(EGL_DEFAULT_DISPLAY) → ANGLE picks
 * its X11/GLX default backend → "Could not open the default X display" with no X
 * server), this client EXPLICITLY drives ANGLE's **Vulkan** backend via the
 * EGL_EXT_platform_angle extension and renders OFF-SCREEN into a pbuffer, so no
 * window / X / Wayland display is needed. The path is:
 *
 *     eglGetProcAddress("eglGetPlatformDisplayEXT")
 *     eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY,
 *                              { EGL_PLATFORM_ANGLE_TYPE_ANGLE =
 *                                    EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE })
 *     eglInitialize → eglChooseConfig(EGL_PBUFFER_BIT) → eglCreatePbufferSurface
 *     eglCreateContext(GLES2) → eglMakeCurrent
 *     glClearColor(magenta) → glClear → glFinish → glReadPixels (verify the cleared
 *                              pixel came back) → swap.
 *
 * ANGLE's Vulkan backend then dlopen()s libvulkan.so.1 (our guest VK ICD, staged in
 * /usr/lib/androlinux next on LD_LIBRARY_PATH) and marshals every vk* call to the
 * host Mali. GL_RENDERER reports ANGLE's Vulkan renderer string (e.g.
 * "ANGLE (... Mali-G615 ... Vulkan ...)") — NOT software/SwiftShader.
 *
 * Pure C / glibc, NO system GL/EGL headers — every constant + prototype it needs is
 * declared inline below (ANGLE exports the standard EGL/GLES2 ABI). It links
 * -lEGL -lGLESv2 (DT_NEEDED libEGL.so/libGLESv2.so, resolved to ANGLE at runtime).
 *
 * Build: zig cc -target aarch64-linux-gnu.2.34 ... -lEGL -lGLESv2 ; staged at
 * /usr/bin/alr-angle-vk by tools/build_angle_overlay.py (--vk-client). Run via the
 * loader under ALR_ANGLE=1 + ALR_VK_ICD=1 (MainActivity.launchAngleGlesProbe).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- minimal EGL/GLES2 ABI (only what this client calls; ANGLE exports these) ---- */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef int   EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef intptr_t EGLAttrib;

typedef unsigned int  GLenum;
typedef unsigned char GLubyte;
typedef int           GLint;
typedef int           GLsizei;
typedef float         GLfloat;
typedef unsigned int  GLbitfield;

#define EGL_NO_DISPLAY      ((EGLDisplay)0)
#define EGL_NO_SURFACE      ((EGLSurface)0)
#define EGL_NO_CONTEXT      ((EGLContext)0)
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_FALSE           0
#define EGL_TRUE            1
#define EGL_NONE            0x3038
#define EGL_SUCCESS         0x3000

#define EGL_RED_SIZE        0x3024
#define EGL_GREEN_SIZE      0x3023
#define EGL_BLUE_SIZE       0x3022
#define EGL_ALPHA_SIZE      0x3021
#define EGL_DEPTH_SIZE      0x3025
#define EGL_SURFACE_TYPE    0x3033
#define EGL_PBUFFER_BIT     0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT  0x0004
#define EGL_WIDTH           0x3057
#define EGL_HEIGHT          0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

/* EGL_EXT_platform_angle (the well-known token values; ANGLE recognizes these). */
#define EGL_PLATFORM_ANGLE_ANGLE                 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE            0x3203
#define EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE     0x3450
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE     0x3209
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE 0x320A

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_RENDERER         0x1F01
#define GL_VERSION          0x1F02
#define GL_VENDOR           0x1F00
#define GL_RGBA             0x1908
#define GL_UNSIGNED_BYTE    0x1401

extern EGLDisplay eglGetDisplay(void *display_id);
extern EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
extern EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                  EGLConfig *configs, EGLint config_size, EGLint *num_config);
extern EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                          const EGLint *attrib_list);
extern EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                                   EGLContext share_context, const EGLint *attrib_list);
extern EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                                 EGLContext ctx);
extern EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);
extern EGLBoolean eglTerminate(EGLDisplay dpy);
extern EGLint     eglGetError(void);
extern const char *eglQueryString(EGLDisplay dpy, EGLint name);
extern void (*eglGetProcAddress(const char *procname))(void);

extern void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
extern void glClear(GLbitfield mask);
extern void glFinish(void);
extern void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
extern void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt,
                         GLenum type, void *pixels);
extern const GLubyte *glGetString(GLenum name);
extern GLenum glGetError(void);

typedef EGLDisplay (*PFNEGLGETPLATFORMDISPLAYEXTPROC)(EGLenum platform,
                                                      void *native_display,
                                                      const EGLint *attrib_list);

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const int W = 64, H = 64;

    /* ANGLE advertises EGL_EXT_platform_angle on the NO_DISPLAY client-extension
     * string; resolve the platform-display entrypoint through eglGetProcAddress so
     * we never need the EXT symbol at link time. */
    const char *client_exts = eglQueryString(EGL_NO_DISPLAY, 0x3053 /*EGL_EXTENSIONS*/);
    printf("alr-angle-vk: client EGL extensions = %s\n",
           client_exts ? client_exts : "(null)");

    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!getPlatformDisplay) {
        fprintf(stderr, "alr-angle-vk: eglGetPlatformDisplayEXT not found "
                        "(ANGLE without EGL_EXT_platform_angle?)\n");
        return 2;
    }

    /* Force ANGLE's VULKAN backend on hardware (NOT SwiftShader). */
    const EGLint disp_attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,        EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
        EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE,
        EGL_NONE
    };
    EGLDisplay dpy = getPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE,
                                        EGL_DEFAULT_DISPLAY, disp_attrs);
    if (dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "alr-angle-vk: eglGetPlatformDisplayEXT(VULKAN) returned "
                        "NO_DISPLAY (0x%x)\n", eglGetError());
        return 3;
    }

    EGLint egl_major = 0, egl_minor = 0;
    if (!eglInitialize(dpy, &egl_major, &egl_minor)) {
        fprintf(stderr, "alr-angle-vk: eglInitialize(VULKAN) failed (0x%x)\n",
                eglGetError());
        return 4;
    }
    printf("alr-angle-vk: ANGLE Vulkan display initialized, EGL %d.%d\n",
           (int)egl_major, (int)egl_minor);
    const char *dpy_vendor = eglQueryString(dpy, 0x3053 /*EGL_EXTENSIONS*/);
    printf("alr-angle-vk: display EGL extensions = %.200s\n",
           dpy_vendor ? dpy_vendor : "(null)");

    const EGLint cfg_attrs[] = {
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig config = NULL;
    EGLint num_config = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &config, 1, &num_config) || num_config < 1) {
        fprintf(stderr, "alr-angle-vk: eglChooseConfig failed (0x%x, n=%d)\n",
                eglGetError(), (int)num_config);
        return 5;
    }

    const EGLint pbuf_attrs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(dpy, config, pbuf_attrs);
    if (surface == EGL_NO_SURFACE) {
        fprintf(stderr, "alr-angle-vk: eglCreatePbufferSurface failed (0x%x)\n",
                eglGetError());
        return 6;
    }

    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "alr-angle-vk: eglCreateContext failed (0x%x)\n", eglGetError());
        return 7;
    }

    if (!eglMakeCurrent(dpy, surface, surface, ctx)) {
        fprintf(stderr, "alr-angle-vk: eglMakeCurrent failed (0x%x)\n", eglGetError());
        return 8;
    }

    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *version  = glGetString(GL_VERSION);
    const GLubyte *vendor   = glGetString(GL_VENDOR);
    printf("alr-angle-vk: GL_VENDOR=\"%s\"\n", vendor ? (const char *)vendor : "?");
    printf("alr-angle-vk: GL_RENDERER=\"%s\"\n", renderer ? (const char *)renderer : "?");
    printf("alr-angle-vk: GL_VERSION=\"%s\"\n", version ? (const char *)version : "?");

    /* INIT done; now CLEAR a distinctive magenta and read one pixel back to prove
     * the GLES2→Vulkan→Mali clear actually executed (not a no-op). */
    glViewport(0, 0, W, H);
    glClearColor(242.0f / 255.0f, 26.0f / 255.0f, 204.0f / 255.0f, 1.0f);  /* magenta */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glFinish();

    unsigned char px[4] = {0, 0, 0, 0};
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum gerr = glGetError();
    printf("alr-angle-vk: center pixel = %d,%d,%d,%d (expect ~242,26,204,255) glErr=0x%x\n",
           px[0], px[1], px[2], px[3], (unsigned)gerr);

    eglSwapBuffers(dpy, surface);   /* PRESENT (pbuffer swap is a host sync point) */

    int ok = (px[0] > 200 && px[1] < 80 && px[2] > 150);
    printf("alr-angle-vk: CLEAR %s\n", ok ? "MATCH" : "MISMATCH");

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(dpy);

    printf("alr-angle-vk: done (result=%s)\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
