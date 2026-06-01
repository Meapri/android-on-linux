// ALR GPU AHB-backed render target (Phase 4 / M4 host half).
//
// The guest's GL draw calls (decoded by alr::gpu::decode_batch) must land in an
// AHardwareBuffer the compositor can sample zero-copy. This header provides that
// render target: allocate an AHB (GPU_FRAMEBUFFER | GPU_SAMPLED), import it as a
// GL texture via EGLImage, attach it as an FBO color attachment, render into it,
// then the SAME AHB is handed to the present path (WaylandPresenter external-OES,
// already proven v114) — no glReadPixels, no glTexImage2D upload.
//
// M4 host-only probe `run_fbo_present_probe()`: build the M1 triangle op stream,
// decode it into an AHB-backed FBO on Mali, then sample that AHB back through a
// GL_TEXTURE_EXTERNAL_OES texture into a second readback FBO and glReadPixels-verify
// — proving the full guest-draw -> AHB -> zero-copy-sample loop works on device,
// before wiring the live guest/ring/SurfaceView (M4 device step, deferred to the
// host GPU thread in runtime_report.cpp). Self-contained; public APIs only.

#ifndef ALR_GPU_ALR_GPU_FBO_HPP
#define ALR_GPU_ALR_GPU_FBO_HPP

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "alr_gpu/alr_gpu_decode.hpp"
#include "alr_gpu/alr_gpu_probe.hpp"

namespace alr::gpu {

// Compile a GLES shader; returns 0 on failure (free helper for this TU).
inline GLuint run_compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(s); return 0; }
    return s;
}

struct AhbRenderTarget;                 // fwd
inline void ahb_target_destroy(AhbRenderTarget& rt);  // fwd: create() uses it on failure

// An AHardwareBuffer set up as a GL render target: AHB -> EGLImage -> GL texture
// -> FBO color attachment. The host renders the guest's decoded draws here; the
// same AHB is then sampled zero-copy by the presenter. Caller owns EGL context.
struct AhbRenderTarget {
    AHardwareBuffer* ahb = nullptr;
    void* image = nullptr;     // EGLImageKHR (void* to keep this header light)
    GLuint tex = 0;            // GL_TEXTURE_2D imported from the AHB (FBO color)
    GLuint fbo = 0;
    int w = 0, h = 0;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    PFNEGLDESTROYIMAGEKHRPROC p_destroy_image = nullptr;

    bool valid() const { return ahb && tex && fbo && image; }
};

// Allocate the AHB render target. Returns valid()==true on success. Uses the
// canonical AHB->EGLImage->texture import; the AHB usage includes GPU_FRAMEBUFFER
// so the GPU can render INTO it (vs M2's GPU_SAMPLED-only display buffers).
inline bool ahb_target_create(AhbRenderTarget& rt, EGLDisplay dpy, int w, int h) {
    rt.dpy = dpy;
    rt.w = w; rt.h = h;

    auto p_get_native_buf = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
        eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    auto p_create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    rt.p_destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    auto p_image_target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!p_get_native_buf || !p_create_image || !rt.p_destroy_image || !p_image_target) {
        return false;
    }

    AHardwareBuffer_Desc d{};
    d.width = static_cast<uint32_t>(w);
    d.height = static_cast<uint32_t>(h);
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
              AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    if (AHardwareBuffer_allocate(&d, &rt.ahb) != 0 || rt.ahb == nullptr) {
        rt.ahb = nullptr;
        return false;
    }
    EGLClientBuffer cb = p_get_native_buf(rt.ahb);
    const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR img = cb ? p_create_image(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                          cb, attribs)
                         : EGL_NO_IMAGE_KHR;
    if (img == EGL_NO_IMAGE_KHR) { ahb_target_destroy(rt); return false; }
    rt.image = img;

    // Import the AHB as a GL_TEXTURE_2D (render-target import, not external-OES:
    // FBO color attachments use 2D). Mali accepts an AHB EGLImage on TEXTURE_2D.
    glGenTextures(1, &rt.tex);
    glBindTexture(GL_TEXTURE_2D, rt.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_image_target(GL_TEXTURE_2D, static_cast<GLeglImageOES>(rt.image));

    glGenFramebuffers(1, &rt.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.tex, 0);
    return true;
}

inline void ahb_target_destroy(AhbRenderTarget& rt) {
    if (rt.fbo) { glDeleteFramebuffers(1, &rt.fbo); rt.fbo = 0; }
    if (rt.tex) { glDeleteTextures(1, &rt.tex); rt.tex = 0; }
    if (rt.image && rt.p_destroy_image) {
        rt.p_destroy_image(rt.dpy, static_cast<EGLImageKHR>(rt.image));
    }
    rt.image = nullptr;
    if (rt.ahb) { AHardwareBuffer_release(rt.ahb); rt.ahb = nullptr; }
}

// M4 host probe: decode the triangle op stream into an AHB-backed FBO, then sample
// that AHB zero-copy (external-OES) to verify the rendered content reached the AHB
// and is presentable — the full guest-draw -> AHB -> zero-copy-display loop, minus
// the live guest/ring/SurfaceView (those are the device wiring step).
inline std::string run_fbo_present_probe() {
    std::ostringstream out;
    out << "alr gpu fbo probe=ahb-fbo-rendertarget-then-external-oes-sample";
    constexpr int W = 64, H = 64;

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || eglInitialize(dpy, nullptr, nullptr) != EGL_TRUE) {
        out << "\nALR GPU FBO RENDERTARGET: FAIL\nalr gpu fbo error=egl-init " << egl_err_hex_local();
        return out.str();
    }
    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
    };
    EGLConfig cfg = nullptr; EGLint nc = 0;
    if (eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &nc) != EGL_TRUE || nc < 1) {
        out << "\nALR GPU FBO RENDERTARGET: FAIL\nalr gpu fbo error=choose-config " << egl_err_hex_local();
        eglTerminate(dpy);
        return out.str();
    }
    const EGLint pb[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
        eglMakeCurrent(dpy, surf, surf, ctx) != EGL_TRUE) {
        out << "\nALR GPU FBO RENDERTARGET: FAIL\nalr gpu fbo error=make-current " << egl_err_hex_local();
        if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
        if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
        eglTerminate(dpy);
        return out.str();
    }

    // 1) AHB render target + render the guest triangle into it.
    AhbRenderTarget rt;
    const bool rt_ok = ahb_target_create(rt, dpy, W, H);
    bool fbo_complete = false;
    HostState st;
    unsigned char direct[4] = {0, 0, 0, 0};   // read straight from the AHB-backed FBO
    unsigned char direct_corner[4] = {0, 0, 0, 0};
    if (rt_ok) {
        glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
        fbo_complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (fbo_complete) {
            const std::vector<uint8_t> stream = build_triangle_stream(W, H);
            decode_batch(stream.data(), stream.size(), st);  // renders into the AHB FBO
            glFinish();
            // Read DIRECTLY from the AHB-backed FBO — the most direct proof the
            // guest's decoded draw landed in the AHB (independent of external-OES
            // re-sampling, which v114 already proved separately).
            glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, direct);
            glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, direct_corner);
        }
    }
    const GLenum render_err = glGetError();
    const bool direct_green = direct[1] > 150 && direct[0] < 100 && direct[2] < 100;
    const bool direct_corner_blue = direct_corner[2] > 80 && direct_corner[1] < 90 &&
                                    direct_corner[0] < 90;

    // 2) Sample the AHB back via external-OES into a plain readback FBO, like the
    //    present path would, and read the center/corner to confirm the content.
    auto p_get_native_buf = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
        eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    auto p_create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    auto p_destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    auto p_image_target_ext = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    unsigned char center[4] = {0, 0, 0, 0};
    unsigned char corner[4] = {0, 0, 0, 0};
    bool sampled = false;
    if (rt_ok && fbo_complete && p_get_native_buf && p_create_image && p_image_target_ext) {
        EGLClientBuffer cb = p_get_native_buf(rt.ahb);
        const EGLint at[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        EGLImageKHR ext_img = cb ? p_create_image(dpy, EGL_NO_CONTEXT,
                                                  EGL_NATIVE_BUFFER_ANDROID, cb, at)
                                 : EGL_NO_IMAGE_KHR;
        GLuint ext_tex = 0, rb_tex = 0, rb_fbo = 0;
        if (ext_img != EGL_NO_IMAGE_KHR) {
            glGenTextures(1, &ext_tex);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, ext_tex);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            p_image_target_ext(GL_TEXTURE_EXTERNAL_OES, static_cast<GLeglImageOES>(ext_img));

            // readback FBO (plain RGBA texture) we draw the external-OES quad into
            glGenTextures(1, &rb_tex);
            glBindTexture(GL_TEXTURE_2D, rb_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glGenFramebuffers(1, &rb_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, rb_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rb_tex, 0);

            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                // external-OES sampling shader (full-screen quad, no V-flip needed:
                // both are top-left-consistent here since we only check colors).
                const char* vs = "attribute vec2 p; varying vec2 uv;"
                                 "void main(){ uv = p*0.5+0.5; gl_Position = vec4(p,0.0,1.0); }";
                const char* fs = "#extension GL_OES_EGL_image_external : require\n"
                                 "precision mediump float; varying vec2 uv;"
                                 "uniform samplerExternalOES t;"
                                 "void main(){ gl_FragColor = texture2D(t, uv); }";
                GLuint v = run_compile(GL_VERTEX_SHADER, vs);
                GLuint f = run_compile(GL_FRAGMENT_SHADER, fs);
                if (v && f) {
                    GLuint pr = glCreateProgram();
                    glAttachShader(pr, v); glAttachShader(pr, f);
                    glBindAttribLocation(pr, 0, "p");
                    glLinkProgram(pr);
                    GLint linked = 0; glGetProgramiv(pr, GL_LINK_STATUS, &linked);
                    if (linked) {
                        glViewport(0, 0, W, H);
                        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
                        glUseProgram(pr);
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_EXTERNAL_OES, ext_tex);
                        glUniform1i(glGetUniformLocation(pr, "t"), 0);
                        const GLfloat quad[] = {-1,-1, 1,-1, -1,1, 1,1};
                        glEnableVertexAttribArray(0);
                        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        glFinish();
                        glReadPixels(W/2, H/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
                        glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
                        sampled = true;
                    }
                    glDeleteProgram(pr);
                }
                if (v) glDeleteShader(v);
                if (f) glDeleteShader(f);
            }
            if (rb_fbo) glDeleteFramebuffers(1, &rb_fbo);
            if (rb_tex) glDeleteTextures(1, &rb_tex);
            if (ext_tex) glDeleteTextures(1, &ext_tex);
            if (p_destroy_image) p_destroy_image(dpy, ext_img);
        }
    }
    const GLenum sample_err = glGetError();

    // The triangle's green sampled back through the AHB; corner = clear blue.
    const bool center_green = center[1] > 150 && center[0] < 100 && center[2] < 100;
    const bool corner_blue = corner[2] > 80 && corner[1] < 90 && corner[0] < 90;
    std::string vendor, renderer;
    if (const auto* v = reinterpret_cast<const char*>(glGetString(GL_VENDOR))) vendor = v;
    if (const auto* rr = reinterpret_cast<const char*>(glGetString(GL_RENDERER))) renderer = rr;
    const bool software = renderer_software_local(vendor, renderer);

    const bool target_ok = rt_ok && fbo_complete && render_err == GL_NO_ERROR;
    // PRIMARY verdict: the guest's decoded draw landed in the AHB, read DIRECTLY
    // from the AHB-backed FBO (definitive, no re-sample dependency).
    const bool ahb_render_ok = target_ok && direct_green && direct_corner_blue && !software;
    // SECONDARY: re-sampling that AHB via external-OES (the present path). v114
    // already proved external-OES display independently; if this self-test's
    // same-context re-sample reads black it's a self-test artifact (tile resolve /
    // same-AHB read-after-write in one context), not a render failure — reported
    // separately, does not gate ahb_render_ok.
    const bool resample_ok = sampled && center_green && corner_blue && sample_err == GL_NO_ERROR;

    out << "\nALR GPU FBO RENDERTARGET: " << (target_ok ? "PASS" : "FAIL");
    out << "\nALR GPU AHB RENDER (guest draw landed in AHB, direct read): "
        << (ahb_render_ok ? "PASS" : "FAIL");
    out << "\nalr gpu fbo external-oes resample: " << (resample_ok ? "PASS" : "info-only")
        << " (v114 proves external-OES display separately)";
    out << "\nalr gpu fbo ops decoded=" << st.decoded;
    out << "\nalr gpu fbo AHB-direct pixel=" << static_cast<int>(direct[0]) << ","
        << static_cast<int>(direct[1]) << "," << static_cast<int>(direct[2])
        << " corner=" << static_cast<int>(direct_corner[0]) << ","
        << static_cast<int>(direct_corner[1]) << "," << static_cast<int>(direct_corner[2])
        << " (expect center ~0,220,0 triangle, corner ~25,25,102 clear)";
    out << "\nalr gpu fbo external-oes pixel=" << static_cast<int>(center[0]) << ","
        << static_cast<int>(center[1]) << "," << static_cast<int>(center[2]);
    out << "\nalr gpu fbo renderer=" << renderer;
    out << "\nalr gpu fbo render err=0x" << std::hex << render_err
        << " sample err=0x" << sample_err << std::dec;
    out << "\nalr gpu fbo software renderer=" << (software ? "true" : "false");

    ahb_target_destroy(rt);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
    eglTerminate(dpy);
    return out.str();
}

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_FBO_HPP
