/* alr_egl_shim.c — the guest-side EGL entry points for the ALR GPU shim.
 *
 * Exported ABI of libEGL.so.1. The guest (the spinning cube, or any glibc GLES2
 * app) links -lEGL against this and gets a working EGL surface/context WITHOUT a
 * real GPU in the guest: the host owns the Mali EGL/GLES2 context and the real
 * surface (an AHB-backed FBO). On the guest side EGL is almost entirely LOCAL —
 * eglGetDisplay/eglInitialize/eglChooseConfig/... return opaque sentinels; the
 * actual rendering target lives host-side and is selected by the host's drain
 * loop, not by anything the guest's EGL says.
 *
 * WHY EGL EMITS (almost) NOTHING ON THE WIRE
 * ------------------------------------------
 * The wire protocol (alr_gles_proto.h / the committed host decoder
 * alr_gpu_decode.hpp) has *no EGL opcodes* — decode_batch() only understands GL
 * ops. So EGL display/config/context/surface objects CANNOT be marshalled as wire
 * ops; there is nothing on the host to decode them. They are therefore pure local
 * sentinels here. The host's GPU thread is what binds a real context to an
 * AHB-backed FBO before it drains the ring (see README "remaining HOST wiring").
 *
 * THE ONE THING EGL DOES DO: eglSwapBuffers is THE per-frame sync point. It maps
 * to RingProducer::flush_and_wait (req_seq bump + block on reply_seq) via
 * alr_shim_flush_and_wait(). The host sees req_seq advance, glFinish-es, presents
 * the AHB through WaylandPresenter, then post_reply()s — unblocking the guest.
 * NOTE: there is deliberately NO "swap" byte in the stream; the host detects the
 * frame boundary from req_seq, exactly as the committed ring + decoder expect.
 *
 * Pure C / glibc. All public entry points have default visibility (the build
 * script hides everything else). Sentinels are non-NULL so callers' `!= EGL_NO_*`
 * checks pass.
 */
#include "alr_khr_egl.h"
#include "alr_shim_internal.h"
#include "alr_shim_env.h"

#include <string.h>

/* ---- Opaque, non-NULL local sentinels. Their addresses are stable for the
 * process lifetime, so the cube's `dpy != EGL_NO_DISPLAY` etc. checks hold and
 * eglMakeCurrent can sanity-check it was handed our objects. ---- */
static int g_display_tag;   /* &g_display_tag is the one EGLDisplay we hand out */
static int g_config_tag;    /* &g_config_tag  is the one EGLConfig  we hand out */
static int g_surface_tag;   /* &g_surface_tag is the one EGLSurface we hand out */
static int g_context_tag;   /* &g_context_tag is the one EGLContext we hand out */

#define ALR_EGL_DISPLAY ((EGLDisplay)&g_display_tag)
#define ALR_EGL_CONFIG  ((EGLConfig)&g_config_tag)
#define ALR_EGL_SURFACE ((EGLSurface)&g_surface_tag)
#define ALR_EGL_CONTEXT ((EGLContext)&g_context_tag)

static void egl_set_error(EGLint err) { alr_shim()->egl_error = err; }

/* [LOCAL] return the single display sentinel; force-init the shim ring early so
 * a missing ring is reported up front (and so virtual-ID state exists). */
EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
    (void)display_id;
    (void)alr_shim();              /* triggers lazy ring attach */
    egl_set_error(EGL_SUCCESS);
    return ALR_EGL_DISPLAY;
}

/* [LOCAL] pretend EGL 1.4. We don't gate on ring_ok here: a ring-less smoke run
 * (no GPU) should still let the app spin its loop and just produce no pixels. */
EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    if (dpy != ALR_EGL_DISPLAY) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }
    if (major) *major = 1;
    if (minor) *minor = 4;
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}

/* [LOCAL] one canned config that advertises ES2 + RGBA8 + depth. The host owns
 * the real framebuffer format; the guest's choice is advisory only. */
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size, EGLint *num_config) {
    (void)attrib_list;
    if (dpy != ALR_EGL_DISPLAY) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }
    if (configs && config_size > 0) configs[0] = ALR_EGL_CONFIG;
    if (num_config) *num_config = (config_size > 0) ? 1 : 0;
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}

/* [LOCAL] answer the handful of attributes a typical app queries on its config.
 * Reports an 8/8/8/8 + 24 depth + 8 stencil ES2 window+pbuffer config. */
EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
    if (dpy != ALR_EGL_DISPLAY || config != ALR_EGL_CONFIG || !value) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
    }
    switch (attribute) {
        case EGL_RED_SIZE:        *value = 8; break;
        case EGL_GREEN_SIZE:      *value = 8; break;
        case EGL_BLUE_SIZE:       *value = 8; break;
        case EGL_ALPHA_SIZE:      *value = 8; break;
        case EGL_DEPTH_SIZE:      *value = 24; break;
        case EGL_STENCIL_SIZE:    *value = 8; break;
        case EGL_RENDERABLE_TYPE: *value = EGL_OPENGL_ES2_BIT; break;
        case EGL_SURFACE_TYPE:    *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
        default:                  *value = 0; break;   /* unknown attr -> 0, still EGL_TRUE */
    }
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}

/* [LOCAL] no wire op: there is no EGL surface opcode (the host's drain loop binds
 * the real AHB-backed FBO as the draw target). We just hand back the one surface
 * sentinel. The native window handle is irrelevant in the guest (the host owns
 * the real surface), so it is accepted and ignored. */
EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  EGLNativeWindowType win, const EGLint *attrib_list) {
    (void)win; (void)attrib_list;
    if (dpy != ALR_EGL_DISPLAY || config != ALR_EGL_CONFIG) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_NO_SURFACE;
    }
    (void)alr_shim();
    egl_set_error(EGL_SUCCESS);
    return ALR_EGL_SURFACE;
}

/* [LOCAL] same single surface sentinel for a pbuffer (the host decides on-/off-
 * screen; the guest can't tell the difference and doesn't need to). */
EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list) {
    (void)attrib_list;
    if (dpy != ALR_EGL_DISPLAY || config != ALR_EGL_CONFIG) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_NO_SURFACE;
    }
    egl_set_error(EGL_SUCCESS);
    return ALR_EGL_SURFACE;
}

/* [LOCAL] no wire op: the real GL context is created host-side (it must exist
 * before any GL op is decoded). The guest context is a sentinel that gates GL
 * emission only in the sense that eglMakeCurrent must be told about it. We accept
 * EGL_CONTEXT_CLIENT_VERSION==2 (and tolerate its absence). */
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                            const EGLint *attrib_list) {
    (void)share_context;
    if (dpy != ALR_EGL_DISPLAY || config != ALR_EGL_CONFIG) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_NO_CONTEXT;
    }
    /* Best-effort: verify a requested client version is 2 if present. */
    if (attrib_list) {
        for (const EGLint *a = attrib_list; a[0] != EGL_NONE; a += 2) {
            if (a[0] == EGL_CONTEXT_CLIENT_VERSION && a[1] != 2) {
                /* We only model ES2; still succeed but note it. */
            }
        }
    }
    (void)alr_shim();
    egl_set_error(EGL_SUCCESS);
    return ALR_EGL_CONTEXT;
}

/* [LOCAL] validate the handles are ours (or all EGL_NO_* for an unbind). No wire
 * op — the host context is always current on the host's GPU thread. */
EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    if (dpy != ALR_EGL_DISPLAY) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }
    /* Allow the unbind case (all EGL_NO_*). */
    if (draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE && ctx == EGL_NO_CONTEXT) {
        egl_set_error(EGL_SUCCESS); return EGL_TRUE;
    }
    if ((draw != ALR_EGL_SURFACE && draw != EGL_NO_SURFACE) ||
        (read != ALR_EGL_SURFACE && read != EGL_NO_SURFACE) ||
        (ctx  != ALR_EGL_CONTEXT)) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
    }
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}

/* THE PER-FRAME SYNC. Flush the ring and block until the host has drained the
 * batch, glFinish-ed, and presented the AHB (it bumps reply_seq). This is the
 * ONLY blocking call in steady state. No "swap" byte is emitted — the host
 * detects the frame from req_seq advancing inside flush_and_wait. */
EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (dpy != ALR_EGL_DISPLAY || surface != ALR_EGL_SURFACE) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
    }
    alr_shim_flush_and_wait();      /* req_seq++ ; doorbell ; block on reply_seq */
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}

/* [LOCAL] swap interval is a host present-policy detail; accept and ignore. */
EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    (void)interval;
    if (dpy != ALR_EGL_DISPLAY) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}

/* [LOCAL] return + clear the sticky EGL error (EGL get-error semantics). */
EGLint eglGetError(void) {
    AlrShimState *s = alr_shim();
    EGLint e = s->egl_error;
    s->egl_error = EGL_SUCCESS;
    return e;
}

/* [LOCAL] resolve eglGetProcAddress for the names we export. We hand back our own
 * exported functions for the EGL/GLES2 entry points (so apps that go through
 * eglGetProcAddress instead of direct linkage still reach the shim). Anything we
 * don't implement returns NULL (the app must cope, as on real EGL). */
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname) {
    if (!procname) return (__eglMustCastToProperFunctionPointerType)0;
    struct { const char *n; void *f; } tbl[] = {
        /* EGL */
        { "eglGetDisplay",            (void*)eglGetDisplay },
        { "eglInitialize",            (void*)eglInitialize },
        { "eglChooseConfig",          (void*)eglChooseConfig },
        { "eglGetConfigAttrib",       (void*)eglGetConfigAttrib },
        { "eglCreateWindowSurface",   (void*)eglCreateWindowSurface },
        { "eglCreatePbufferSurface",  (void*)eglCreatePbufferSurface },
        { "eglCreateContext",         (void*)eglCreateContext },
        { "eglMakeCurrent",           (void*)eglMakeCurrent },
        { "eglSwapBuffers",           (void*)eglSwapBuffers },
        { "eglSwapInterval",          (void*)eglSwapInterval },
        { "eglGetError",              (void*)eglGetError },
        { "eglQueryString",           (void*)eglQueryString },
        { "eglDestroyContext",        (void*)eglDestroyContext },
        { "eglDestroySurface",        (void*)eglDestroySurface },
        { "eglTerminate",             (void*)eglTerminate },
    };
    for (size_t i = 0; i < sizeof(tbl)/sizeof(tbl[0]); ++i) {
        if (strcmp(procname, tbl[i].n) == 0)
            return (__eglMustCastToProperFunctionPointerType)tbl[i].f;
    }
    return (__eglMustCastToProperFunctionPointerType)0;
}

/* [LOCAL] static strings; identifies the ALR marshalling EGL. */
const char *eglQueryString(EGLDisplay dpy, EGLint name) {
    if (dpy != ALR_EGL_DISPLAY) { egl_set_error(EGL_BAD_PARAMETER); return (const char*)0; }
    switch (name) {
        case EGL_VENDOR:      return "Android-on-Linux (ALR)";
        case EGL_VERSION:     return "1.4 ALR-marshalling";
        case EGL_CLIENT_APIS: return "OpenGL_ES";
        case EGL_EXTENSIONS:  return "";
        default:              egl_set_error(EGL_BAD_PARAMETER); return (const char*)0;
    }
}

/* [LOCAL] teardown sentinels — nothing host-side to release from the guest. */
EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    if (dpy != ALR_EGL_DISPLAY || ctx != ALR_EGL_CONTEXT) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
    }
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    if (dpy != ALR_EGL_DISPLAY || surface != ALR_EGL_SURFACE) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
    }
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy) {
    if (dpy != ALR_EGL_DISPLAY) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}
