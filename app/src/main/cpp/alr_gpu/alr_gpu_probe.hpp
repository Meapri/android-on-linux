// ALR GPU-native app probe (Phase 4 / M1): host-only proof that the GLES
// command-stream decoder renders a REAL shaded + textured triangle on the Mali
// GPU. No guest, no ring, no IPC — a direct extension of the already-proven
// build_gpu_marshalling_probe (which did clear/scissor only). This exercises the
// full shader + VBO + texture + draw path AND the virtual-ID translation
// (alr::gpu::HostState), then glReadPixels-verifies on real hardware.
//
// Self-contained: include this from runtime_report.cpp and call
// alr_gpu_run_draw_probe(). It owns its own EGL pbuffer + GLES2 context so it does
// not depend on anything in runtime_report.cpp (keeping the GPU work in its own TU
// while another session edits that file).

#ifndef ALR_GPU_ALR_GPU_PROBE_HPP
#define ALR_GPU_ALR_GPU_PROBE_HPP

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <cstdint>
#include <sstream>
#include <string>

#include "alr_gpu/alr_gpu_decode.hpp"

namespace alr::gpu {

inline std::string egl_err_hex_local() {
    std::ostringstream o;
    o << "0x" << std::hex << eglGetError();
    return o.str();
}

inline bool renderer_software_local(const std::string& vendor, const std::string& renderer) {
    auto has = [](const std::string& s, const char* k) {
        std::string lo = s;
        for (auto& c : lo) c = static_cast<char>(::tolower(c));
        return lo.find(k) != std::string::npos;
    };
    return has(renderer, "swiftshader") || has(renderer, "software") ||
           has(renderer, "llvmpipe") || has(vendor, "google (swiftshader") ||
           has(renderer, "softpipe");
}

// Build the op stream for: VS+FS -> program; a VBO of 3 verts (pos.xy + uv.xy);
// a 2x2 checker texture; then a frame that draws GL_TRIANGLES sampling the texture
// tinted by a uniform. Returns the encoded bytes. The triangle covers the center;
// the corners stay clear-color.
inline std::vector<uint8_t> build_triangle_stream(int fbw, int fbh) {
    Encoder e;

    // Virtual IDs (the guest would allocate these monotonically; here we pick them).
    const uint32_t VS = 1, FS = 2, PROG = 1, VBO = 1, TEX = 1;

    const char* vsrc =
        "attribute vec2 aPos; attribute vec2 aUv; varying vec2 vUv;"
        "uniform mat4 uMVP;"
        "void main(){ vUv = aUv; gl_Position = uMVP * vec4(aPos, 0.0, 1.0); }";
    const char* fsrc =
        "precision mediump float; varying vec2 vUv; uniform sampler2D uTex;"
        "void main(){ gl_FragColor = texture2D(uTex, vUv); }";

    // --- shaders + program ---
    e.u8(OP_CREATE_SHADER); e.u32(VS); e.u32(GL_VERTEX_SHADER);
    e.u8(OP_SHADER_SOURCE); e.u32(VS); e.str(vsrc);
    e.u8(OP_COMPILE_SHADER); e.u32(VS);
    e.u8(OP_CREATE_SHADER); e.u32(FS); e.u32(GL_FRAGMENT_SHADER);
    e.u8(OP_SHADER_SOURCE); e.u32(FS); e.str(fsrc);
    e.u8(OP_COMPILE_SHADER); e.u32(FS);
    e.u8(OP_CREATE_PROGRAM); e.u32(PROG);
    e.u8(OP_ATTACH_SHADER); e.u32(PROG); e.u32(VS);
    e.u8(OP_ATTACH_SHADER); e.u32(PROG); e.u32(FS);
    e.u8(OP_BIND_ATTRIB_LOCATION); e.u32(PROG); e.u32(0); e.str("aPos");
    e.u8(OP_BIND_ATTRIB_LOCATION); e.u32(PROG); e.u32(1); e.str("aUv");
    e.u8(OP_LINK_PROGRAM); e.u32(PROG);

    // --- VBO: 3 vertices, each (posx,posy,u,v) as float ---
    const float verts[] = {
        // a centered triangle in NDC; UVs across the texture
        -0.6f, -0.6f, 0.0f, 0.0f,
         0.6f, -0.6f, 1.0f, 0.0f,
         0.0f,  0.6f, 0.5f, 1.0f,
    };
    e.u8(OP_GEN_BUFFER); e.u32(VBO);
    e.u8(OP_BIND_BUFFER); e.u32(GL_ARRAY_BUFFER); e.u32(VBO);
    e.u8(OP_BUFFER_DATA); e.u32(GL_ARRAY_BUFFER);
    e.blob(verts, sizeof(verts)); e.u32(GL_STATIC_DRAW);

    // --- 2x2 checker texture: solid green so the verify is unambiguous ---
    const uint8_t tex[2 * 2 * 4] = {
        0, 220, 0, 255,  0, 220, 0, 255,
        0, 220, 0, 255,  0, 220, 0, 255,
    };
    e.u8(OP_GEN_TEXTURE); e.u32(TEX);
    e.u8(OP_ACTIVE_TEXTURE); e.u32(GL_TEXTURE0);
    e.u8(OP_BIND_TEXTURE); e.u32(GL_TEXTURE_2D); e.u32(TEX);
    e.u8(OP_TEX_PARAMETERI); e.u32(GL_TEXTURE_2D); e.u32(GL_TEXTURE_MIN_FILTER); e.i32(GL_NEAREST);
    e.u8(OP_TEX_PARAMETERI); e.u32(GL_TEXTURE_2D); e.u32(GL_TEXTURE_MAG_FILTER); e.i32(GL_NEAREST);
    e.u8(OP_TEX_IMAGE_2D); e.u32(GL_TEXTURE_2D); e.i32(0); e.u32(GL_RGBA);
    e.i32(2); e.i32(2); e.u32(GL_RGBA); e.u32(GL_UNSIGNED_BYTE);
    e.blob(tex, sizeof(tex));

    // --- a frame ---
    e.u8(OP_VIEWPORT); e.i32(0); e.i32(0); e.i32(fbw); e.i32(fbh);
    e.u8(OP_CLEARCOLOR); e.f32(0.10f); e.f32(0.10f); e.f32(0.40f); e.f32(1.0f);
    e.u8(OP_CLEAR);
    e.u8(OP_USE_PROGRAM); e.u32(PROG);
    const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    e.u8(OP_UNIFORM_MATRIX4FV); e.u32(PROG); e.str("uMVP");
    for (float f : identity) e.f32(f);
    e.u8(OP_UNIFORM1I); e.u32(PROG); e.str("uTex"); e.i32(0);
    e.u8(OP_ENABLE_VAA); e.u32(0);
    e.u8(OP_VERTEX_ATTRIB_POINTER); e.u32(0); e.i32(2); e.u32(GL_FLOAT);
    e.u8(0); e.i32(4 * sizeof(float)); e.u32(0);                 // aPos: offset 0
    e.u8(OP_ENABLE_VAA); e.u32(1);
    e.u8(OP_VERTEX_ATTRIB_POINTER); e.u32(1); e.i32(2); e.u32(GL_FLOAT);
    e.u8(0); e.i32(4 * sizeof(float)); e.u32(2 * sizeof(float)); // aUv: offset 8
    e.u8(OP_DRAW_ARRAYS); e.u32(GL_TRIANGLES); e.i32(0); e.i32(3);
    e.u8(OP_END);
    return e.bytes();
}

// M1 probe: set up an EGL pbuffer + GLES2 context, decode the triangle stream on
// real Mali, glReadPixels-verify, report PASS/FAIL in the project's probe style.
inline std::string run_draw_probe() {
    std::ostringstream out;
    out << "alr gpu draw probe=marshalling-shader-vbo-texture-draw";
    constexpr int W = 64, H = 64;

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || eglInitialize(dpy, nullptr, nullptr) != EGL_TRUE) {
        out << "\nALR GPU DRAW DECODE+EXECUTE: FAIL\nALR GPU DRAW HARDWARE RENDER: FAIL"
            << "\nalr gpu draw error=egl-init " << egl_err_hex_local();
        return out.str();
    }
    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16, EGL_NONE,
    };
    EGLConfig cfg = nullptr; EGLint n = 0;
    if (eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &n) != EGL_TRUE || n < 1) {
        out << "\nALR GPU DRAW DECODE+EXECUTE: FAIL\nALR GPU DRAW HARDWARE RENDER: FAIL"
            << "\nalr gpu draw error=choose-config " << egl_err_hex_local();
        eglTerminate(dpy);
        return out.str();
    }
    const EGLint pb[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
        eglMakeCurrent(dpy, surf, surf, ctx) != EGL_TRUE) {
        out << "\nALR GPU DRAW DECODE+EXECUTE: FAIL\nALR GPU DRAW HARDWARE RENDER: FAIL"
            << "\nalr gpu draw error=make-current " << egl_err_hex_local();
        if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
        if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
        eglTerminate(dpy);
        return out.str();
    }

    // Decode the hand-built op stream on the real GPU.
    const std::vector<uint8_t> stream = build_triangle_stream(W, H);
    HostState st;
    const bool decode_ok = decode_batch(stream.data(), stream.size(), st);
    glFinish();
    const GLenum gl_error = glGetError();

    // Verify: center pixel is the texture's green (drawn triangle), a corner is the
    // blue clear color (outside the triangle).
    unsigned char center[4] = {0, 0, 0, 0};
    unsigned char corner[4] = {0, 0, 0, 0};
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
    glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
    const bool center_green = center[1] > 150 && center[0] < 100 && center[2] < 100;
    const bool corner_blue = corner[2] > 80 && corner[1] < 90 && corner[0] < 90;

    std::string vendor, renderer;
    if (const auto* v = reinterpret_cast<const char*>(glGetString(GL_VENDOR))) vendor = v;
    if (const auto* rr = reinterpret_cast<const char*>(glGetString(GL_RENDERER))) renderer = rr;
    const bool software = renderer_software_local(vendor, renderer);

    const bool decoded_ok = decode_ok && center_green && corner_blue;
    const bool hw_ok = decoded_ok && !software && gl_error == GL_NO_ERROR;

    out << "\nALR GPU DRAW DECODE+EXECUTE: " << (decoded_ok ? "PASS" : "FAIL");
    out << "\nALR GPU DRAW HARDWARE RENDER: " << (hw_ok ? "PASS" : "FAIL");
    out << "\nalr gpu draw ops decoded=" << st.decoded;
    out << "\nalr gpu draw stream bytes=" << stream.size();
    out << "\nalr gpu draw center pixel=" << static_cast<int>(center[0]) << ","
        << static_cast<int>(center[1]) << "," << static_cast<int>(center[2])
        << " (expect ~0,220,0 textured triangle)";
    out << "\nalr gpu draw corner pixel=" << static_cast<int>(corner[0]) << ","
        << static_cast<int>(corner[1]) << "," << static_cast<int>(corner[2])
        << " (expect ~25,25,102 clear)";
    out << "\nalr gpu draw renderer=" << renderer;
    out << "\nalr gpu draw gl error=0x" << std::hex << gl_error << std::dec;
    out << "\nalr gpu draw software renderer=" << (software ? "true" : "false");

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
    eglTerminate(dpy);
    return out.str();
}

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_PROBE_HPP
