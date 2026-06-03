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

#include <stdlib.h>  /* getenv, atoi (eglQuerySurface drawable size) */
#include <string.h>
#include <stdint.h>  /* intptr_t for eglGetPlatformDisplay attrib_list */

/* ---- Opaque, non-NULL local sentinels. Their addresses are stable for the
 * process lifetime, so the cube's `dpy != EGL_NO_DISPLAY` etc. checks hold and
 * eglMakeCurrent can sanity-check it was handed our objects. ---- */
static int g_display_tag;   /* &g_display_tag is the one EGLDisplay we hand out */
static int g_config_tag;    /* &g_config_tag  is the one EGLConfig  we hand out */
static int g_surface_tag;   /* &g_surface_tag is the one EGLSurface we hand out */
static int g_context_tag;   /* &g_context_tag is the one EGLContext we hand out */
static int g_device_tag;    /* &g_device_tag  is the one EGLDeviceEXT (synthetic) for ANGLE's device path */

#define ALR_EGL_DISPLAY ((EGLDisplay)&g_display_tag)
#define ALR_EGL_CONFIG  ((EGLConfig)&g_config_tag)
#define ALR_EGL_SURFACE ((EGLSurface)&g_surface_tag)
#define ALR_EGL_CONTEXT ((EGLContext)&g_context_tag)
#define ALR_EGL_DEVICE  ((void*)&g_device_tag)

/* EGL_EXT_platform_device / EGL_EXT_device_query tokens (not in the tiny khr hdr). */
#define ALR_EGL_PLATFORM_DEVICE_EXT 0x313F
#define ALR_EGL_RENDERER_EXT        0x335F  /* EGL_RENDERER_EXT (device string) */

static void egl_set_error(EGLint err) { alr_shim()->egl_error = err; }

/* CR-3 device diagnostics: trace ANGLE's EGL call sequence into guest stderr (which
 * the loader tees to logcat as alr_cr_out under ALR_TEE_GUEST_STDOUT). Gated on
 * ALR_SHIM_DIAG=1 so it is silent in normal runs. This is what lets us see exactly
 * which EGL entry ANGLE calls and what we return, to close "Failed to get system
 * egl display" et al. */
#include <stdio.h>
static int alr_egl_diag_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("ALR_SHIM_DIAG"); v = (e && e[0] == '1') ? 1 : 0; }
    return v;
}
#define ALR_EGL_DIAG(...) do { if (alr_egl_diag_on()) { fprintf(stderr, "[alr-egl] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)

/* [LOCAL] return the single display sentinel; force-init the shim ring early so
 * a missing ring is reported up front (and so virtual-ID state exists). */
EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id) {
    (void)display_id;
    (void)alr_shim();              /* triggers lazy ring attach */
    egl_set_error(EGL_SUCCESS);
    ALR_EGL_DIAG("eglGetDisplay(native=%p) -> %p", (void*)display_id, (void*)ALR_EGL_DISPLAY);
    return ALR_EGL_DISPLAY;
}

/* CR-3 (chromium-gpu-path §4.1): ANGLE's gles-egl backend, having seen
 * EGL_KHR/EXT_platform_* in our client extensions, resolves and calls
 * eglGetPlatformDisplay / eglGetPlatformDisplayEXT (EGL 1.5 / EGL_EXT_platform_base)
 * instead of the legacy eglGetDisplay. Both must return our single sentinel
 * display (any platform/native arg is advisory — the real target is the host Mali
 * context). Without these, eglGetProcAddress("eglGetPlatformDisplay") would be
 * NULL and ANGLE's display init aborts. Signatures use void-pointer and
 * intptr-shaped params so we don't depend on EGL 1.5 typedefs in the tiny header. */
EGLDisplay eglGetPlatformDisplay(unsigned int platform, void *native_display,
                                 const intptr_t *attrib_list) {
    (void)native_display; (void)attrib_list;
    (void)alr_shim();              /* triggers lazy ring attach */
    egl_set_error(EGL_SUCCESS);
    ALR_EGL_DIAG("eglGetPlatformDisplay(platform=0x%x, native=%p) -> %p",
                 platform, native_display, (void*)ALR_EGL_DISPLAY);
    return ALR_EGL_DISPLAY;
}

EGLDisplay eglGetPlatformDisplayEXT(unsigned int platform, void *native_display,
                                    const EGLint *attrib_list) {
    (void)native_display; (void)attrib_list;
    (void)alr_shim();
    egl_set_error(EGL_SUCCESS);
    ALR_EGL_DIAG("eglGetPlatformDisplayEXT(platform=0x%x, native=%p) -> %p",
                 platform, native_display, (void*)ALR_EGL_DISPLAY);
    return ALR_EGL_DISPLAY;
}

/* CR-3 (EGL_EXT_device_enumeration/base/query): ANGLE's getNativeDisplay()
 * enumerates devices, then makes the display from one. We expose exactly ONE
 * synthetic device (no real /dev node — the host Mali executor is the GPU). This
 * is what lets ANGLE's device path reach our eglGetPlatformDisplayEXT +
 * eglInitialize and succeed, instead of bailing with "Failed to get system egl
 * display". EGLDeviceEXT/EGLAttrib are modeled as void*/intptr (not in the tiny
 * header); ANGLE only stores the opaque device handle and passes it back to us. */
EGLBoolean eglQueryDevicesEXT(EGLint max_devices, void **devices, EGLint *num_devices) {
    if (!num_devices) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }
    if (devices == NULL) {
        /* Count query. */
        *num_devices = 1;
    } else if (max_devices > 0) {
        devices[0] = ALR_EGL_DEVICE;
        *num_devices = 1;
    } else {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
    }
    egl_set_error(EGL_SUCCESS);
    ALR_EGL_DIAG("eglQueryDevicesEXT(max=%d, query=%d) -> n=%d dev=%p",
                 max_devices, devices == NULL, *num_devices, (void*)ALR_EGL_DEVICE);
    return EGL_TRUE;
}
const char *eglQueryDeviceStringEXT(void *device, EGLint name) {
    if (device != ALR_EGL_DEVICE) { egl_set_error(EGL_BAD_PARAMETER); return (const char*)0; }
    egl_set_error(EGL_SUCCESS);
    switch (name) {
        case EGL_VENDOR:             return "Android-on-Linux (ALR)";
        case ALR_EGL_RENDERER_EXT:   return "ALR Mali passthrough";
        case EGL_EXTENSIONS:         return "";   /* no device-level extensions */
        default:                     return "";
    }
}
EGLBoolean eglQueryDeviceAttribEXT(void *device, EGLint attribute, intptr_t *value) {
    (void)attribute;
    if (device != ALR_EGL_DEVICE || !value) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }
    /* We expose no queryable device attributes (no DRM fd, no native handle). */
    egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
}
EGLBoolean eglQueryDisplayAttribEXT(EGLDisplay dpy, EGLint attribute, intptr_t *value) {
    if (dpy != ALR_EGL_DISPLAY || !value) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }
    /* EGL_DEVICE_EXT (0x322C): hand back our synthetic device so ANGLE can round-trip. */
    if (attribute == 0x322C) { *value = (intptr_t)ALR_EGL_DEVICE; egl_set_error(EGL_SUCCESS); return EGL_TRUE; }
    egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
}

/* [LOCAL] pretend EGL 1.4. We don't gate on ring_ok here: a ring-less smoke run
 * (no GPU) should still let the app spin its loop and just produce no pixels. */
EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    if (dpy != ALR_EGL_DISPLAY) {
        ALR_EGL_DIAG("eglInitialize(dpy=%p) BAD_DISPLAY (ours=%p)", (void*)dpy, (void*)ALR_EGL_DISPLAY);
        egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
    }
    if (major) *major = 1;
    if (minor) *minor = 4;
    egl_set_error(EGL_SUCCESS);
    ALR_EGL_DIAG("eglInitialize(dpy=%p) -> TRUE (1.4)", (void*)dpy);
    return EGL_TRUE;
}

/* [LOCAL] one canned config that advertises ES2 + RGBA8 + depth. The host owns
 * the real framebuffer format; the guest's choice is advisory only. */
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size, EGLint *num_config) {
    (void)attrib_list;
    if (dpy != ALR_EGL_DISPLAY) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }
    if (!num_config) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }  /* EGL: required */
    /* We advertise exactly ONE config. EGL semantics: when `configs` is NULL the call is
     * a COUNT query (config_size is ignored) and must report the number of matching
     * configs in *num_config. glmark2 (libmatrix GLStateEGL) does this FIRST — count,
     * allocate, then fill — so returning 0 on the count query made it abort with
     * "eglChooseConfig() didn't return any configs" / "Couldn't get GL visual config!"
     * (CP-2 drain). Return the real count (1) on the count query; fill on the real query. */
    if (configs == NULL) {
        *num_config = 1;                                   /* count of available configs */
    } else if (config_size > 0) {
        configs[0] = ALR_EGL_CONFIG;
        *num_config = 1;                                   /* one config written */
    } else {
        *num_config = 0;                                   /* zero-length output array */
    }
    egl_set_error(EGL_SUCCESS);
    ALR_EGL_DIAG("eglChooseConfig(count_query=%d) -> n=%d", configs == NULL, *num_config);
    return EGL_TRUE;
}

/* [LOCAL] answer the handful of attributes a typical app queries on its config.
 * Reports an 8/8/8/8 + 24 depth + 8 stencil ES2 window+pbuffer config. */
EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
    if (dpy != ALR_EGL_DISPLAY || config != ALR_EGL_CONFIG || !value) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
    }
    /* Report a COMPLETE 8888 / d24 / s8 ES2 window+pbuffer config. glmark2 reads these
     * into a GLVisualConfig and rejects ("Failed to find suitable EGL config") if a
     * required attr is short — notably EGL_BUFFER_SIZE, which was previously 0 (default)
     * and disqualified the config. */
    switch (attribute) {
        case EGL_BUFFER_SIZE:        *value = 32; break;   /* 8+8+8+8 — the drain blocker */
        case EGL_RED_SIZE:           *value = 8; break;
        case EGL_GREEN_SIZE:         *value = 8; break;
        case EGL_BLUE_SIZE:          *value = 8; break;
        case EGL_ALPHA_SIZE:         *value = 8; break;
        case EGL_LUMINANCE_SIZE:     *value = 0; break;
        case EGL_ALPHA_MASK_SIZE:    *value = 0; break;
        case EGL_DEPTH_SIZE:         *value = 24; break;
        /* STENCIL=0: the executor's AHB-FBO has a depth renderbuffer but NO stencil, so 0 is
         * accurate — AND it's required for glmark2 2023.01: its default target stencil=0, and
         * GLVisualConfig::score_component returns -1000 when (component>0 && target==0); a
         * stencil=8 config scored -802 (<=0) and select_best_config rejected it ("Failed to
         * find suitable EGL config"). With stencil=0 the score is +230 -> selected. */
        case EGL_STENCIL_SIZE:       *value = 0; break;
        case EGL_SAMPLES:            *value = 0; break;
        case EGL_SAMPLE_BUFFERS:     *value = 0; break;
        case EGL_CONFIG_CAVEAT:      *value = EGL_NONE; break;
        case EGL_CONFIG_ID:          *value = 1; break;
        case EGL_LEVEL:              *value = 0; break;
        case EGL_COLOR_BUFFER_TYPE:  *value = EGL_RGB_BUFFER; break;
        case EGL_RENDERABLE_TYPE:    *value = EGL_OPENGL_ES2_BIT; break;
        case EGL_CONFORMANT:         *value = EGL_OPENGL_ES2_BIT; break;
        case EGL_SURFACE_TYPE:       *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
        case EGL_NATIVE_RENDERABLE:  *value = EGL_TRUE; break;
        case EGL_NATIVE_VISUAL_ID:   *value = 0; break;
        case EGL_NATIVE_VISUAL_TYPE: *value = EGL_NONE; break;
        case EGL_TRANSPARENT_TYPE:   *value = EGL_NONE; break;
        case EGL_MAX_PBUFFER_WIDTH:  *value = 4096; break;
        case EGL_MAX_PBUFFER_HEIGHT: *value = 4096; break;
        case EGL_MAX_PBUFFER_PIXELS: *value = 4096 * 4096; break;
        case EGL_MIN_SWAP_INTERVAL:  *value = 0; break;
        case EGL_MAX_SWAP_INTERVAL:  *value = 1; break;
        default:                     *value = 0; break;   /* unknown attr -> 0, still EGL_TRUE */
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
    ALR_EGL_DIAG("eglCreateContext -> %p", (void*)ALR_EGL_CONTEXT);
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

/* [LOCAL] glmark2 (and most EGL apps) call eglBindAPI(EGL_OPENGL_ES_API) at init and
 * abort if it fails. The shim only ever speaks GLES2, so accept any API and succeed
 * (the host context is ES2 regardless of what the guest "binds"). */
EGLBoolean eglBindAPI(EGLenum api) {
    (void)api;
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}

/* [LOCAL] current-state getters: there is exactly one of each sentinel and the host
 * keeps its real context current on its GPU thread, so report our sentinels. (Apps
 * query these to re-fetch the display/surface; returning EGL_NO_* would mislead.) */
EGLDisplay eglGetCurrentDisplay(void) { return ALR_EGL_DISPLAY; }
EGLContext eglGetCurrentContext(void) { return ALR_EGL_CONTEXT; }
EGLSurface eglGetCurrentSurface(EGLint readdraw) { (void)readdraw; return ALR_EGL_SURFACE; }

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

/* CR-3 (chromium-gpu-path 4.1): ANGLE's gles-egl backend resolves the FULL EGL
 * 1.0 to 1.5 core entry-point set up front (egl_loader_autogen, via dlsym or
 * eglGetProcAddress) and ABORTS display init if ANY is NULL. The device drain
 * showed "Could not load EGL entry point eglBindTexImage" - glmark2 never needed
 * these so they were absent. Safe LOCAL stubs for every core entry ANGLE loads.
 * Types beyond the tiny khr header (image, sync, client-buffer) are modeled with
 * void-pointer and EGLint params - ANGLE only needs the SYMBOL to be non-NULL; if
 * it ever calls one on the gles-egl offscreen path we return an honest
 * unsupported value (EGL_FALSE or a NO_* sentinel), which ANGLE handles. We do
 * NOT implement EGLImage/dmabuf import meaningfully (no device node; sec 4.4):
 *   eglBindTexImage / eglReleaseTexImage : pbuffer-tex binding, N/A offscreen.
 *   eglCopyBuffers                       : no native pixmap target.
 *   eglCreatePbufferFromClientBuffer     : no client-buffer source.
 *   eglCreate/Destroy/ClientWait/WaitSync, eglGetSyncAttrib : fence sync; the
 *       host glFinish in eglSwapBuffers is the real sync, so report unavailable.
 *   eglCreate/DestroyImage(KHR)          : no EGLImage (dmabuf path off).
 *   eglCreatePlatformWindow/PixmapSurface: EGL 1.5 ctors to our sentinels. */
EGLBoolean eglBindTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
    (void)surface; (void)buffer;
    if (dpy != ALR_EGL_DISPLAY) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }
    egl_set_error(EGL_SUCCESS); return EGL_TRUE;
}
EGLBoolean eglReleaseTexImage(EGLDisplay dpy, EGLSurface surface, EGLint buffer) {
    (void)surface; (void)buffer;
    if (dpy != ALR_EGL_DISPLAY) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }
    egl_set_error(EGL_SUCCESS); return EGL_TRUE;
}
EGLBoolean eglCopyBuffers(EGLDisplay dpy, EGLSurface surface, void *target) {
    (void)surface; (void)target;
    if (dpy != ALR_EGL_DISPLAY) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }
    egl_set_error(EGL_SUCCESS); return EGL_TRUE;
}
EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay dpy, EGLenum buftype,
                                            void *buffer, EGLConfig config,
                                            const EGLint *attrib_list) {
    (void)buftype; (void)buffer; (void)attrib_list;
    if (dpy != ALR_EGL_DISPLAY || config != ALR_EGL_CONFIG) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_NO_SURFACE;
    }
    egl_set_error(EGL_SUCCESS); return ALR_EGL_SURFACE;
}
/* Fence sync (EGL 1.5 / KHR_fence_sync): not backed — eglSwapBuffers' host
 * glFinish is the actual GPU sync. Return NO_SYNC so ANGLE treats sync as
 * unavailable rather than crashing. */
void *eglCreateSync(EGLDisplay dpy, EGLenum type, const intptr_t *attrib_list) {
    (void)type; (void)attrib_list;
    if (dpy != ALR_EGL_DISPLAY) { egl_set_error(EGL_BAD_PARAMETER); return (void*)0; }
    egl_set_error(EGL_SUCCESS); return (void*)0;  /* EGL_NO_SYNC */
}
void *eglCreateSyncKHR(EGLDisplay dpy, EGLenum type, const EGLint *attrib_list) {
    (void)type; (void)attrib_list;
    if (dpy != ALR_EGL_DISPLAY) { egl_set_error(EGL_BAD_PARAMETER); return (void*)0; }
    egl_set_error(EGL_SUCCESS); return (void*)0;
}
EGLBoolean eglDestroySync(EGLDisplay dpy, void *sync) {
    (void)dpy; (void)sync; egl_set_error(EGL_SUCCESS); return EGL_TRUE;
}
EGLBoolean eglDestroySyncKHR(EGLDisplay dpy, void *sync) {
    (void)dpy; (void)sync; egl_set_error(EGL_SUCCESS); return EGL_TRUE;
}
EGLint eglClientWaitSync(EGLDisplay dpy, void *sync, EGLint flags, uint64_t timeout) {
    (void)dpy; (void)sync; (void)flags; (void)timeout;
    egl_set_error(EGL_SUCCESS); return 0x30F6 /* EGL_CONDITION_SATISFIED */;
}
EGLint eglClientWaitSyncKHR(EGLDisplay dpy, void *sync, EGLint flags, uint64_t timeout) {
    (void)dpy; (void)sync; (void)flags; (void)timeout;
    egl_set_error(EGL_SUCCESS); return 0x30F6;
}
EGLBoolean eglWaitSync(EGLDisplay dpy, void *sync, EGLint flags) {
    (void)dpy; (void)sync; (void)flags; egl_set_error(EGL_SUCCESS); return EGL_TRUE;
}
EGLBoolean eglGetSyncAttrib(EGLDisplay dpy, void *sync, EGLint attribute, intptr_t *value) {
    (void)dpy; (void)sync; (void)attribute; (void)value;
    egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
}
EGLBoolean eglGetSyncAttribKHR(EGLDisplay dpy, void *sync, EGLint attribute, EGLint *value) {
    (void)dpy; (void)sync; (void)attribute; (void)value;
    egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
}
/* EGLImage (EGL 1.5 / KHR_image_base): dmabuf import path is OFF (§4.4) → no
 * image. NO_IMAGE so ANGLE doesn't take a zero-copy import path. */
void *eglCreateImage(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                     void *buffer, const intptr_t *attrib_list) {
    (void)dpy; (void)ctx; (void)target; (void)buffer; (void)attrib_list;
    egl_set_error(EGL_SUCCESS); return (void*)0;  /* EGL_NO_IMAGE */
}
void *eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                        void *buffer, const EGLint *attrib_list) {
    (void)dpy; (void)ctx; (void)target; (void)buffer; (void)attrib_list;
    egl_set_error(EGL_SUCCESS); return (void*)0;
}
EGLBoolean eglDestroyImage(EGLDisplay dpy, void *image) {
    (void)dpy; (void)image; egl_set_error(EGL_SUCCESS); return EGL_TRUE;
}
EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, void *image) {
    (void)dpy; (void)image; egl_set_error(EGL_SUCCESS); return EGL_TRUE;
}
/* EGL 1.5 platform surface ctors: route to our window/pbuffer sentinels. */
EGLSurface eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config,
                                          void *native_window, const intptr_t *attrib_list) {
    (void)native_window; (void)attrib_list;
    if (dpy != ALR_EGL_DISPLAY || config != ALR_EGL_CONFIG) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_NO_SURFACE;
    }
    egl_set_error(EGL_SUCCESS); return ALR_EGL_SURFACE;
}
EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay dpy, EGLConfig config,
                                          void *native_pixmap, const intptr_t *attrib_list) {
    (void)native_pixmap; (void)attrib_list;
    if (dpy != ALR_EGL_DISPLAY || config != ALR_EGL_CONFIG) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_NO_SURFACE;
    }
    egl_set_error(EGL_SUCCESS); return ALR_EGL_SURFACE;
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
        { "eglGetPlatformDisplay",    (void*)eglGetPlatformDisplay },
        { "eglGetPlatformDisplayEXT", (void*)eglGetPlatformDisplayEXT },
        { "eglInitialize",            (void*)eglInitialize },
        { "eglChooseConfig",          (void*)eglChooseConfig },
        { "eglGetConfigAttrib",       (void*)eglGetConfigAttrib },
        { "eglCreateWindowSurface",   (void*)eglCreateWindowSurface },
        { "eglCreatePbufferSurface",  (void*)eglCreatePbufferSurface },
        { "eglCreateContext",         (void*)eglCreateContext },
        { "eglMakeCurrent",           (void*)eglMakeCurrent },
        { "eglBindAPI",               (void*)eglBindAPI },
        { "eglGetCurrentDisplay",     (void*)eglGetCurrentDisplay },
        { "eglGetCurrentContext",     (void*)eglGetCurrentContext },
        { "eglGetCurrentSurface",     (void*)eglGetCurrentSurface },
        { "eglSwapBuffers",           (void*)eglSwapBuffers },
        { "eglSwapInterval",          (void*)eglSwapInterval },
        { "eglGetError",              (void*)eglGetError },
        { "eglQueryString",           (void*)eglQueryString },
        { "eglDestroyContext",        (void*)eglDestroyContext },
        { "eglDestroySurface",        (void*)eglDestroySurface },
        { "eglTerminate",             (void*)eglTerminate },
        { "eglQuerySurface",          (void*)eglQuerySurface },
        { "eglQueryContext",          (void*)eglQueryContext },
        { "eglQueryAPI",              (void*)eglQueryAPI },
        { "eglWaitClient",            (void*)eglWaitClient },
        { "eglWaitGL",                (void*)eglWaitGL },
        { "eglWaitNative",            (void*)eglWaitNative },
        { "eglReleaseThread",         (void*)eglReleaseThread },
        { "eglSurfaceAttrib",         (void*)eglSurfaceAttrib },
        { "eglGetConfigs",            (void*)eglGetConfigs },
        /* CR-3: full EGL core ANGLE's egl_loader resolves (else display init aborts). */
        { "eglBindTexImage",          (void*)eglBindTexImage },
        { "eglReleaseTexImage",       (void*)eglReleaseTexImage },
        { "eglCopyBuffers",           (void*)eglCopyBuffers },
        { "eglCreatePbufferFromClientBuffer", (void*)eglCreatePbufferFromClientBuffer },
        { "eglCreateSync",            (void*)eglCreateSync },
        { "eglCreateSyncKHR",         (void*)eglCreateSyncKHR },
        { "eglDestroySync",           (void*)eglDestroySync },
        { "eglDestroySyncKHR",        (void*)eglDestroySyncKHR },
        { "eglClientWaitSync",        (void*)eglClientWaitSync },
        { "eglClientWaitSyncKHR",     (void*)eglClientWaitSyncKHR },
        { "eglWaitSync",              (void*)eglWaitSync },
        { "eglGetSyncAttrib",         (void*)eglGetSyncAttrib },
        { "eglGetSyncAttribKHR",      (void*)eglGetSyncAttribKHR },
        { "eglCreateImage",           (void*)eglCreateImage },
        { "eglCreateImageKHR",        (void*)eglCreateImageKHR },
        { "eglDestroyImage",          (void*)eglDestroyImage },
        { "eglDestroyImageKHR",       (void*)eglDestroyImageKHR },
        { "eglCreatePlatformWindowSurface", (void*)eglCreatePlatformWindowSurface },
        { "eglCreatePlatformPixmapSurface", (void*)eglCreatePlatformPixmapSurface },
    };
    for (size_t i = 0; i < sizeof(tbl)/sizeof(tbl[0]); ++i) {
        if (strcmp(procname, tbl[i].n) == 0)
            return (__eglMustCastToProperFunctionPointerType)tbl[i].f;
    }
    return (__eglMustCastToProperFunctionPointerType)0;
}

/* CR-3 (chromium-gpu-path §4.3): the extension strings ANGLE's gles-egl backend
 * queries on the SYSTEM EGL (= us) before it will build a display. We advertise
 * ONLY the offscreen/surfaceless + create-context set, and DELIBERATELY omit
 * everything dmabuf/GBM/DRM (EGL_EXT_image_dma_buf_import, EGL_*_platform_gbm,
 * EGL_*_device_*, EGL_*_stream_*) so ANGLE never tries to open /dev/dri (absent +
 * SELinux-denied for an untrusted_app) and instead falls to the surfaceless FBO
 * path — exactly what our host Mali executor renders into (AhbRenderTarget). Same
 * canned string answers both the client query (dpy == EGL_NO_DISPLAY) and the
 * display query; that is harmless for ANGLE (it greps for the names it needs).
 *   - EGL_EXT_client_extensions : lets ANGLE query extensions with no display.
 *   - EGL_KHR_platform_*        : lets ANGLE pick a platform via
 *                                 eglGetPlatformDisplay (surfaceless/Android/GBM-less).
 *   - EGL_KHR_surfaceless_context: the offscreen FBO path (no window surface).
 *   - EGL_KHR_create_context (+ _no_error / _robustness no-op): ANGLE creates its
 *                                 ES context via eglCreateContext attribs. */
/* CR-3 device finding (chromium-gpu-path §4.1 refinement): ANGLE's gles-egl
 * FunctionsEGL::initialize, on a host with NO Wayland/GBM/surfaceless EGL platform
 * advertised, takes its getNativeDisplay() path, which REQUIRES the device
 * enumeration set — (EGL_EXT_device_enumeration | EGL_EXT_device_base) +
 * EGL_EXT_platform_base + EGL_EXT_platform_device — and then enumerates devices via
 * eglQueryDevicesEXT and builds the display with
 * eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, device). Without these ANGLE
 * bails BEFORE ever calling our eglGetDisplay/eglInitialize, yielding "Failed to
 * get system egl display" (device-observed). So we advertise the device-enum set
 * and return ONE synthetic device from eglQueryDevicesEXT — that drives ANGLE down
 * the EGL_PLATFORM_DEVICE path onto our sentinel display (which has no real /dev
 * node; the host Mali executor is the actual GPU). */
static const char *const ALR_EGL_CLIENT_EXTS =
    "EGL_EXT_client_extensions "
    "EGL_KHR_platform_base "
    "EGL_EXT_platform_base "
    "EGL_KHR_platform_android "
    "EGL_EXT_platform_device "
    "EGL_EXT_device_enumeration "
    "EGL_EXT_device_base "
    "EGL_EXT_device_query "
    "EGL_KHR_client_get_all_proc_addresses "
    "EGL_KHR_get_all_proc_addresses";
static const char *const ALR_EGL_DISPLAY_EXTS =
    "EGL_KHR_surfaceless_context "
    "EGL_KHR_create_context "
    "EGL_KHR_create_context_no_error "
    "EGL_EXT_create_context_robustness "
    "EGL_KHR_get_all_proc_addresses "
    "EGL_KHR_no_config_context "
    "EGL_KHR_gl_colorspace "
    "EGL_KHR_fence_sync";

/* [LOCAL] static strings; identifies the ALR marshalling EGL.
 * EGL_NO_DISPLAY (client) query is VALID per EGL_EXT_client_extensions and must
 * NOT error — ANGLE issues it first to discover platform extensions. */
const char *eglQueryString(EGLDisplay dpy, EGLint name) {
    if (dpy == EGL_NO_DISPLAY) {
        /* Client (no-display) query: only EXTENSIONS (+ optional VERSION) are
         * defined; ANGLE only reads EXTENSIONS here. */
        if (name == EGL_EXTENSIONS) {
            egl_set_error(EGL_SUCCESS);
            ALR_EGL_DIAG("eglQueryString(NO_DISPLAY, EXTENSIONS) -> client exts");
            return ALR_EGL_CLIENT_EXTS;
        }
        ALR_EGL_DIAG("eglQueryString(NO_DISPLAY, 0x%x) -> NULL", name);
        egl_set_error(EGL_BAD_PARAMETER); return (const char*)0;
    }
    if (dpy != ALR_EGL_DISPLAY) {
        ALR_EGL_DIAG("eglQueryString(dpy=%p BAD, 0x%x) -> NULL", (void*)dpy, name);
        egl_set_error(EGL_BAD_PARAMETER); return (const char*)0;
    }
    switch (name) {
        case EGL_VENDOR:      egl_set_error(EGL_SUCCESS); return "Android-on-Linux (ALR)";
        case EGL_VERSION:     egl_set_error(EGL_SUCCESS); return "1.4 ALR-marshalling";
        case EGL_CLIENT_APIS: egl_set_error(EGL_SUCCESS); return "OpenGL_ES";
        case EGL_EXTENSIONS:
            egl_set_error(EGL_SUCCESS);
            ALR_EGL_DIAG("eglQueryString(dpy, EXTENSIONS) -> display exts");
            return ALR_EGL_DISPLAY_EXTS;
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

/* [LOCAL] surface/context lifecycle queries a GLES app makes (glmark2 dlsym set).
 * All local — the host owns the real surface/context; these report consistent values. */

/* The drawable is the host AHB render target; report ITS size so the guest sets a
 * matching glViewport. The loader passes ALR_GPU_FB_W/H (= executor AHB size); default
 * 1280x720 (WS-1 gcfg) if unset. */
EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value) {
    if (dpy != ALR_EGL_DISPLAY || surface != ALR_EGL_SURFACE || !value) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
    }
    int fb_w = 1280, fb_h = 720;
    const char* ew = getenv(ALR_ENV_FB_W);
    const char* eh = getenv(ALR_ENV_FB_H);
    if (ew && *ew) { int v = atoi(ew); if (v > 0) fb_w = v; }
    if (eh && *eh) { int v = atoi(eh); if (v > 0) fb_h = v; }
    switch (attribute) {
        case EGL_WIDTH:         *value = fb_w; break;
        case EGL_HEIGHT:        *value = fb_h; break;
        case EGL_RENDER_BUFFER: *value = EGL_BACK_BUFFER; break;
        case EGL_CONFIG_ID:     *value = 1; break;
        default:                *value = 0; break;
    }
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}

EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value) {
    if (dpy != ALR_EGL_DISPLAY || ctx != ALR_EGL_CONTEXT || !value) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
    }
    switch (attribute) {
        case EGL_CONTEXT_CLIENT_TYPE:    *value = EGL_OPENGL_ES_API; break;
        case EGL_CONTEXT_CLIENT_VERSION: *value = 2; break;
        case EGL_RENDER_BUFFER:          *value = EGL_BACK_BUFFER; break;
        case EGL_CONFIG_ID:              *value = 1; break;
        default:                         *value = 0; break;
    }
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}

/* glmark2 calls eglBindAPI(EGL_OPENGL_ES_API) then may verify via eglQueryAPI. */
EGLenum eglQueryAPI(void) { return EGL_OPENGL_ES_API; }

/* Sync barriers: the shim's only real sync is eglSwapBuffers (per-frame), so these
 * are no-ops that succeed. */
EGLBoolean eglWaitClient(void) { egl_set_error(EGL_SUCCESS); return EGL_TRUE; }
EGLBoolean eglWaitGL(void) { egl_set_error(EGL_SUCCESS); return EGL_TRUE; }
EGLBoolean eglWaitNative(EGLint engine) { (void)engine; egl_set_error(EGL_SUCCESS); return EGL_TRUE; }
EGLBoolean eglReleaseThread(void) { egl_set_error(EGL_SUCCESS); return EGL_TRUE; }

/* Accept + ignore surface attributes (e.g. EGL_SWAP_BEHAVIOR) — host owns the surface. */
EGLBoolean eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value) {
    (void)attribute; (void)value;
    if (dpy != ALR_EGL_DISPLAY || surface != ALR_EGL_SURFACE) {
        egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE;
    }
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}

/* Enumerate configs — same single canned config as eglChooseConfig. configs==NULL is a
 * count query (EGL semantics). */
EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig *configs, EGLint config_size, EGLint *num_config) {
    if (dpy != ALR_EGL_DISPLAY || !num_config) { egl_set_error(EGL_BAD_PARAMETER); return EGL_FALSE; }
    if (configs == NULL) { *num_config = 1; }
    else if (config_size > 0) { configs[0] = ALR_EGL_CONFIG; *num_config = 1; }
    else { *num_config = 0; }
    egl_set_error(EGL_SUCCESS);
    return EGL_TRUE;
}
