/* alr-angle-vk.c — the ANGLE-on-Vulkan INIT + SURFACELESS-FBO RENDER proof client.
 *
 * Unlike alr-gles-cube (which uses eglGetDisplay(EGL_DEFAULT_DISPLAY) → ANGLE picks
 * its X11/GLX default backend → "Could not open the default X display" with no X
 * server), this client EXPLICITLY drives ANGLE's **Vulkan** backend via the
 * EGL_EXT_platform_angle extension and renders OFF-SCREEN — with NO window, NO X,
 * NO Wayland AND, crucially, NO eglCreatePbufferSurface — by binding a SURFACELESS
 * context (EGL_KHR_surfaceless_context) and drawing into a client-side FBO. The path:
 *
 *     eglGetProcAddress("eglGetPlatformDisplayEXT")
 *     eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY,
 *                              { TYPE = VULKAN_ANGLE, DEVICE_TYPE = HARDWARE,
 *                                NATIVE_PLATFORM_TYPE = <pinned, see below> })
 *     eglInitialize → eglChooseConfig(EGL_PBUFFER_BIT, just to get a valid config id)
 *     eglCreateContext(GLES2)
 *     eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)   ← surfaceless!
 *     glGenFramebuffers/glGenTextures → an offscreen RGBA8 FBO (glViewport)
 *     glClearColor(magenta) → glClear                            (clear proof)
 *     compile VS+FS → link program → VBO triangle → glDrawArrays (draw proof)
 *     glReadPixels(center) == triangle color, glReadPixels(corner) == clear color.
 *
 * WHY SURFACELESS, NOT PBUFFER (the load-bearing fix): on the shipped chromium-147
 * ANGLE the DisplayVkOffscreen (true headless) backend is NOT compiled — only
 * DisplayVkXcb + DisplayVkWayland are. So eglInitialize picks DisplayVkXcb, which
 * reaches our Mali Vulkan ICD fine (vkCreateInstance → vkCreateDevice), BUT
 * eglCreatePbufferSurface on DisplayVkXcb fails with EGL_BAD_ATTRIBUTE (0x3004): an
 * X11 DisplayVk can only back surfaces that map to an X drawable, and a pbuffer with
 * no X window has no such drawable. The fix is to NEVER ask for a pbuffer surface;
 * instead bind the context with EGL_NO_SURFACE (EGL_KHR_surfaceless_context, which
 * ANGLE always advertises on the Vulkan backend) and render to a GL FBO whose color
 * attachment is a texture ANGLE allocates as a plain VkImage on Mali — no WSI surface
 * anywhere. This is strictly BETTER than a pbuffer for proving the GLES→Vulkan→our-ICD
 * →Mali render path, and it is the canonical headless GLES idiom.
 *
 * DISPLAY SELECTION: ANGLE reaches its Vulkan renderer only after a DisplayVk subclass
 * initializes, and which subclass it picks decides whether it needs a window system.
 * We pin the choice with an explicit EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE
 * attribute and try, in order: (1) EGL_PLATFORM_SURFACELESS_MESA → DisplayVkOffscreen
 * (ideal, used only if ANGLE compiled the offscreen backend); (2) EGL_PLATFORM_X11_EXT
 * → DisplayVkXcb, which with $DISPLAY unset (the loader omits it under ALR_ANGLE) SKIPS
 * xcb_connect and proceeds straight to vkCreateInstance. The first config whose
 * eglInitialize() succeeds wins; on the shipped ANGLE rung (2) wins. EITHER WAY the
 * surfaceless-context render below works (DisplayVkXcb backs FBO images on Mali).
 *
 * ANGLE's Vulkan backend then dlopen()s libvulkan.so.1 (our guest VK ICD, staged in
 * /usr/lib/androlinux next on LD_LIBRARY_PATH) and marshals every vk* call to the
 * host Mali. The draw FORCES the create-resource entrypoints to fire on Mali:
 * compiling GLSL → ANGLE emits vkCreateShaderModule; binding the FBO → vkCreateImage/
 * vkCreateImageView/vkCreateRenderPass/vkCreateFramebuffer; glDrawArrays →
 * vkCreateGraphicsPipelines + vkCmdBeginRenderPass/vkCmdDraw; glReadPixels/glFinish →
 * vkQueueSubmit. GL_RENDERER reports ANGLE's Vulkan renderer string (e.g.
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
#include <stddef.h>
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
typedef char          GLchar;
typedef int           GLint;
typedef unsigned int  GLuint;
typedef int           GLsizei;
typedef float         GLfloat;
typedef unsigned int  GLbitfield;
typedef unsigned char GLboolean;
typedef ptrdiff_t     GLsizeiptr;
typedef ptrdiff_t     GLintptr;

#define EGL_NO_DISPLAY      ((EGLDisplay)0)
#define EGL_NO_SURFACE      ((EGLSurface)0)
#define EGL_NO_CONTEXT      ((EGLContext)0)
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_FALSE           0
#define EGL_TRUE            1
#define EGL_NONE            0x3038
#define EGL_SUCCESS         0x3000
#define EGL_EXTENSIONS      0x3055

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
/* EGL_ANGLE_platform_angle: native-window-system selector. ANGLE's Vulkan backend
 * picks its DisplayVk WSI sub-backend (DisplayVkXcb / DisplayVkWayland / the
 * offscreen DisplayVkOffscreen) from this attribute; when it is 0/absent ANGLE
 * instead consults the environment (WAYLAND_DISPLAY → Wayland, else X11), which is
 * exactly the non-deterministic path we must avoid. Setting it pins the WSI choice. */
#define EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE 0x348F
/* EGL_MESA_platform_surfaceless: the window-system-independent OFFSCREEN platform.
 * As an ANGLE native-platform-type it routes to CreateVulkanOffscreenDisplay
 * (DisplayVkOffscreen) — no X, no Wayland, no surface. Used as the FIRST display rung
 * so, on any ANGLE built with the offscreen backend, we render fully headless. */
#define EGL_PLATFORM_SURFACELESS_MESA            0x31DD

/* GLES2 enums for the offscreen FBO + draw. */
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_RENDERER         0x1F01
#define GL_VERSION          0x1F02
#define GL_VENDOR           0x1F00
#define GL_EXTENSIONS       0x1F03
#define GL_RGBA             0x1908
#define GL_UNSIGNED_BYTE    0x1401
#define GL_FLOAT            0x1406
#define GL_TRIANGLES        0x0004

#define GL_FRAMEBUFFER          0x8D40
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE_2D           0x0DE1
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_NEAREST              0x2600

#define GL_ARRAY_BUFFER     0x8892
#define GL_STATIC_DRAW      0x88E4
#define GL_VERTEX_SHADER    0x8B31
#define GL_FRAGMENT_SHADER  0x8B30
#define GL_COMPILE_STATUS   0x8B81
#define GL_LINK_STATUS      0x8B82

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
extern void glFlush(void);
extern void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
extern void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt,
                         GLenum type, void *pixels);
extern const GLubyte *glGetString(GLenum name);
extern GLenum glGetError(void);

/* FBO + texture color attachment */
extern void glGenFramebuffers(GLsizei n, GLuint *ids);
extern void glBindFramebuffer(GLenum target, GLuint fb);
extern void glFramebufferTexture2D(GLenum target, GLenum att, GLenum textarget,
                                   GLuint tex, GLint level);
extern GLenum glCheckFramebufferStatus(GLenum target);
extern void glGenTextures(GLsizei n, GLuint *ids);
extern void glBindTexture(GLenum target, GLuint tex);
extern void glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h,
                         GLint border, GLenum fmt, GLenum type, const void *px);
extern void glTexParameteri(GLenum target, GLenum pname, GLint param);

/* shaders / program / VBO */
extern GLuint glCreateShader(GLenum type);
extern void glShaderSource(GLuint sh, GLsizei count, const GLchar *const *str,
                           const GLint *len);
extern void glCompileShader(GLuint sh);
extern void glGetShaderiv(GLuint sh, GLenum pname, GLint *params);
extern void glGetShaderInfoLog(GLuint sh, GLsizei buf, GLsizei *len, GLchar *log);
extern GLuint glCreateProgram(void);
extern void glAttachShader(GLuint prog, GLuint sh);
extern void glLinkProgram(GLuint prog);
extern void glGetProgramiv(GLuint prog, GLenum pname, GLint *params);
extern void glGetProgramInfoLog(GLuint prog, GLsizei buf, GLsizei *len, GLchar *log);
extern void glUseProgram(GLuint prog);
extern GLint glGetAttribLocation(GLuint prog, const GLchar *name);
extern void glGenBuffers(GLsizei n, GLuint *ids);
extern void glBindBuffer(GLenum target, GLuint buf);
extern void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
extern void glEnableVertexAttribArray(GLuint index);
extern void glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                  GLboolean normalized, GLsizei stride,
                                  const void *ptr);
extern void glDrawArrays(GLenum mode, GLint first, GLsizei count);

typedef EGLDisplay (*PFNEGLGETPLATFORMDISPLAYEXTPROC)(EGLenum platform,
                                                      void *native_display,
                                                      const EGLint *attrib_list);

/* The triangle is BLUE; the clear is MAGENTA. A center readback of BLUE proves the
 * draw executed (GLSL→SPIR-V→Vulkan pipeline ran on Mali); a corner readback of
 * MAGENTA proves the clear executed and the triangle did NOT cover the whole FBO. */
static const GLfloat kClearR = 242.0f / 255.0f, kClearG = 26.0f / 255.0f, kClearB = 204.0f / 255.0f;

static const char *kVS =
    "attribute vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char *kFS =
    "precision mediump float;\n"
    "void main() { gl_FragColor = vec4(0.0, 0.4, 1.0, 1.0); }\n"; /* blue (0,102,255) */

/* Compile one shader; log the GLSL compile result (proves ANGLE's translator ran). */
static GLuint compile_shader(GLenum type, const char *src, const char *tag) {
    GLuint sh = glCreateShader(type);
    if (!sh) { fprintf(stderr, "alr-angle-vk: glCreateShader(%s)=0 glErr=0x%x\n", tag, glGetError()); return 0; }
    const GLchar *srcs[1] = { (const GLchar *)src };
    glShaderSource(sh, 1, srcs, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "alr-angle-vk: %s shader compile FAILED: %s\n", tag, log);
        return 0;
    }
    printf("alr-angle-vk: %s shader compiled OK\n", tag);
    return sh;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const int W = 64, H = 64;

    /* Unbuffer stdout so our progress lines (esp. which display config won, the
     * GL_RENDERER, and the readback pixels) reach the loader's stdout tee EVEN IF
     * ANGLE later SIGSEGVs in its own renderer-setup — a block-buffered stdout would
     * otherwise be discarded on the crash, hiding exactly the diagnostics we need. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* ANGLE advertises EGL_EXT_platform_angle on the NO_DISPLAY client-extension
     * string; resolve the platform-display entrypoint through eglGetProcAddress so
     * we never need the EXT symbol at link time. */
    const char *client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("alr-angle-vk: client EGL extensions = %s\n",
           client_exts ? client_exts : "(null)");

    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!getPlatformDisplay) {
        fprintf(stderr, "alr-angle-vk: eglGetPlatformDisplayEXT not found "
                        "(ANGLE without EGL_EXT_platform_angle?)\n");
        return 2;
    }

    /* Display selection — get ANGLE past its WSI/X-display init onto our Mali ICD.
     * Try the configs in order; the first whose eglInitialize() succeeds wins. */
    struct disp_cfg { const char *name; EGLint native_platform; };
    const struct disp_cfg cfgs[] = {
        /* 1) Headless offscreen — no surface at all. Works on any ANGLE that compiled
         *    DisplayVkOffscreen; if not, eglInitialize fails and we fall through. */
        { "surfaceless/offscreen", EGL_PLATFORM_SURFACELESS_MESA },
        /* 2) X11/xcb backend but with $DISPLAY unset → DisplayVkXcb SKIPS xcb_connect
         *    and proceeds to vkCreateInstance. Proven path on the shipped chromium ANGLE.
         *    We render headless on it via a SURFACELESS CONTEXT + FBO (no pbuffer). */
        { "x11/xcb (DISPLAY-unset → skip connect)", 0x31D5 /*EGL_PLATFORM_X11_EXT*/ },
    };

    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLint egl_major = 0, egl_minor = 0;
    const char *won = NULL;
    for (unsigned i = 0; i < sizeof(cfgs) / sizeof(cfgs[0]); ++i) {
        const EGLint disp_attrs[] = {
            EGL_PLATFORM_ANGLE_TYPE_ANGLE,        EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
            EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE,
            EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE, cfgs[i].native_platform,
            EGL_NONE
        };
        EGLDisplay d = getPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE,
                                          EGL_DEFAULT_DISPLAY, disp_attrs);
        if (d == EGL_NO_DISPLAY) {
            printf("alr-angle-vk: display[%s] getPlatformDisplay → NO_DISPLAY (0x%x), "
                   "trying next\n", cfgs[i].name, eglGetError());
            continue;
        }
        EGLint mj = 0, mn = 0;
        if (eglInitialize(d, &mj, &mn)) {
            dpy = d; egl_major = mj; egl_minor = mn; won = cfgs[i].name;
            break;
        }
        printf("alr-angle-vk: display[%s] eglInitialize → fail (0x%x), trying next\n",
               cfgs[i].name, eglGetError());
        eglTerminate(d);   /* release the half-open display before the next attempt */
    }
    if (dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "alr-angle-vk: no ANGLE Vulkan display config initialized "
                        "(0x%x)\n", eglGetError());
        return 4;
    }
    printf("alr-angle-vk: ANGLE Vulkan display initialized via [%s], EGL %d.%d\n",
           won, (int)egl_major, (int)egl_minor);
    const char *dpy_exts = eglQueryString(dpy, EGL_EXTENSIONS);
    printf("alr-angle-vk: display EGL extensions = %.400s\n",
           dpy_exts ? dpy_exts : "(null)");
    int has_surfaceless_ctx =
        dpy_exts && strstr(dpy_exts, "EGL_KHR_surfaceless_context") != NULL;
    printf("alr-angle-vk: EGL_KHR_surfaceless_context = %s\n",
           has_surfaceless_ctx ? "YES" : "no(will-try-anyway)");

    /* Pick a config. We still ask for EGL_PBUFFER_BIT in the surface-type mask so the
     * chosen config is one ANGLE is happy to make a GLES2 context against — but we will
     * NOT create a pbuffer surface from it. The config is only needed for the context. */
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

    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "alr-angle-vk: eglCreateContext failed (0x%x)\n", eglGetError());
        return 7;
    }

    /* SURFACELESS make-current: bind the context with NO draw/read surface. On the
     * Vulkan backend ANGLE supports this (EGL_KHR_surfaceless_context); rendering then
     * targets a client FBO. This is the fix for the DisplayVkXcb pbuffer wall — we
     * never touch eglCreatePbufferSurface (which fails EGL_BAD_ATTRIBUTE on X11). */
    EGLSurface surface = EGL_NO_SURFACE;   /* stays NO_SURFACE on the surfaceless path */
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        EGLint e = eglGetError();
        printf("alr-angle-vk: surfaceless eglMakeCurrent failed (0x%x) — "
               "falling back to pbuffer surface\n", (unsigned)e);
        /* Fallback: a display that genuinely can't do surfaceless. Try a pbuffer
         * (works on DisplayVkOffscreen; on DisplayVkXcb this is the old wall). */
        const EGLint pbuf_attrs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
        surface = eglCreatePbufferSurface(dpy, config, pbuf_attrs);
        if (surface == EGL_NO_SURFACE) {
            fprintf(stderr, "alr-angle-vk: eglCreatePbufferSurface ALSO failed (0x%x) — "
                            "no offscreen target available\n", eglGetError());
            return 6;
        }
        if (!eglMakeCurrent(dpy, surface, surface, ctx)) {
            fprintf(stderr, "alr-angle-vk: pbuffer eglMakeCurrent failed (0x%x)\n", eglGetError());
            return 8;
        }
        printf("alr-angle-vk: bound via PBUFFER surface fallback\n");
    } else {
        printf("alr-angle-vk: bound SURFACELESS context (EGL_NO_SURFACE) — rendering to FBO\n");
    }

    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *version  = glGetString(GL_VERSION);
    const GLubyte *vendor   = glGetString(GL_VENDOR);
    printf("alr-angle-vk: GL_VENDOR=\"%s\"\n", vendor ? (const char *)vendor : "?");
    printf("alr-angle-vk: GL_RENDERER=\"%s\"\n", renderer ? (const char *)renderer : "?");
    printf("alr-angle-vk: GL_VERSION=\"%s\"\n", version ? (const char *)version : "?");

    /* ---- Build an OFFSCREEN FBO (color = texture). ANGLE backs this with a plain
     * VkImage on Mali (vkCreateImage/vkCreateImageView), no WSI surface. ---- */
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum fbstat = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    printf("alr-angle-vk: FBO status = 0x%x (%s) glErr=0x%x\n", (unsigned)fbstat,
           fbstat == GL_FRAMEBUFFER_COMPLETE ? "COMPLETE" : "INCOMPLETE",
           (unsigned)glGetError());
    if (fbstat != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "alr-angle-vk: FBO incomplete — cannot render offscreen\n");
        return 9;
    }
    glViewport(0, 0, W, H);

    /* ---- CLEAR proof: magenta over the whole FBO. ---- */
    glClearColor(kClearR, kClearG, kClearB, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glFinish();
    unsigned char clr[4] = {0, 0, 0, 0};
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, clr);
    printf("alr-angle-vk: after CLEAR center pixel = %d,%d,%d,%d (expect ~242,26,204,255) glErr=0x%x\n",
           clr[0], clr[1], clr[2], clr[3], (unsigned)glGetError());

    /* ---- DRAW proof: compile GLSL → link → VBO triangle covering the center → draw.
     * The triangle is large enough to cover the FBO center but leaves the corners as
     * the magenta clear, so center==blue + corner==magenta proves BOTH clear and draw. */
    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVS, "vertex");
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFS, "fragment");
    int drew = 0;
    if (vs && fs) {
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512] = {0};
            glGetProgramInfoLog(prog, sizeof(log) - 1, NULL, log);
            fprintf(stderr, "alr-angle-vk: program link FAILED: %s\n", log);
        } else {
            printf("alr-angle-vk: program linked OK\n");
            glUseProgram(prog);
            /* A triangle covering the center but not the corners (corners stay magenta). */
            const GLfloat verts[] = {
                -0.8f, -0.8f,
                 0.8f, -0.8f,
                 0.0f,  0.8f,
            };
            GLuint vbo = 0;
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
            GLint loc = glGetAttribLocation(prog, "aPos");
            if (loc < 0) loc = 0;
            glEnableVertexAttribArray((GLuint)loc);
            glVertexAttribPointer((GLuint)loc, 2, GL_FLOAT, 0 /*GL_FALSE*/, 0, (const void *)0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glFinish();
            printf("alr-angle-vk: glDrawArrays issued glErr=0x%x\n", (unsigned)glGetError());
            drew = 1;
        }
    }

    /* ---- Read back center (triangle) and corner (clear) to prove the render path. ---- */
    unsigned char ctr[4] = {0, 0, 0, 0}, cor[4] = {0, 0, 0, 0};
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, ctr);
    glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, cor);
    GLenum gerr = glGetError();
    printf("alr-angle-vk: READBACK center=%d,%d,%d,%d (expect blue ~0,102,255) "
           "corner=%d,%d,%d,%d (expect magenta ~242,26,204) glErr=0x%x\n",
           ctr[0], ctr[1], ctr[2], ctr[3], cor[0], cor[1], cor[2], cor[3], (unsigned)gerr);

    if (surface != EGL_NO_SURFACE)
        eglSwapBuffers(dpy, surface);   /* PRESENT (only meaningful on the pbuffer path) */

    /* Verdicts: CLEAR proven by the magenta corner; DRAW proven by the blue center. */
    int clear_ok = (cor[0] > 200 && cor[1] < 90 && cor[2] > 150);
    int draw_ok  = (drew && ctr[2] > 150 && ctr[0] < 90 && ctr[1] < 160);
    printf("alr-angle-vk: CLEAR %s, DRAW %s\n",
           clear_ok ? "MATCH" : "MISMATCH", draw_ok ? "MATCH" : "MISMATCH");

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(dpy);

    int ok = clear_ok && draw_ok;
    printf("alr-angle-vk: done (result=%s)\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
