// ALR GPU on-screen demo (Phase 4 / GPU-native Linux app track, M5).
//
// The capstone of the in-process GPU pipeline: a spinning, textured CUBE driven
// end to end by the same machinery the earlier milestones proved — a producer
// emits a GL op stream (alr::gpu::Encoder, the SAME API build_triangle_stream
// uses) through the SPSC command ring (alr_gpu_ring.hpp); the GpuExecutorService's
// GL thread decodes each frame into an AHB-backed FBO on the real Mali GPU
// (alr_gpu_decode.hpp + alr_gpu_fbo.hpp), then presents that AHB zero-copy to an
// ANativeWindow via a DIFFERENT EGL context sampling GL_TEXTURE_EXTERNAL_OES (the
// cross-context present added to GpuExecutorService, which defeats Mali's
// same-context black-AHB tile-resolve hazard).
//
// PURELY in-process: no fork, no rootfs, no shared-memory fd — a plain heap ring
// both the producer thread and the executor's consumer thread see, exactly like
// run_live_integration_probe. This header adds (a) the per-frame cube op-stream
// builder and (b) a self-contained demo entry point run_screen_cube_demo() that
// the JNI/UI layer calls with the SurfaceView's ANativeWindow.
//
// Header-only and self-contained (public EGL/GLES2/ANativeWindow APIs only).

#ifndef ALR_GPU_ALR_GPU_SCREEN_HPP
#define ALR_GPU_ALR_GPU_SCREEN_HPP

#include <android/native_window.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "alr_gpu/alr_gpu_decode.hpp"
#include "alr_gpu/alr_gpu_host_service.hpp"
#include "alr_gpu/alr_gpu_probe.hpp"
#include "alr_gpu/alr_gpu_ring.hpp"

namespace alr::gpu {

// ---------------------------------------------------------------------------
// Hand-rolled 4x4 matrix helpers. COLUMN-MAJOR storage (m[col*4 + row]), matching
// OpenGL's native layout: the decoder uploads these straight via
// glUniformMatrix4fv(loc, 1, GL_FALSE, m) (transpose == GL_FALSE), so the 16 floats
// must already be column-major. Multiplication composes transforms the GL way:
// mat_mul(A, B) yields a matrix M with M*v == A*(B*v), so to apply model then view
// then projection to a vertex you build P * V * M and feed that as uMVP.
// ---------------------------------------------------------------------------
using Mat4 = std::array<float, 16>;

inline Mat4 mat_identity() {
    return Mat4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

// C = A * B (column-major). C[c*4+r] = sum_k A[k*4+r] * B[c*4+k].
inline Mat4 mat_mul(const Mat4& a, const Mat4& b) {
    Mat4 c{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[static_cast<size_t>(k) * 4 + row] *
                       b[static_cast<size_t>(col) * 4 + k];
            }
            c[static_cast<size_t>(col) * 4 + row] = sum;
        }
    }
    return c;
}

// Translation by (x,y,z), column-major (translation lives in the last column).
inline Mat4 mat_translate(float x, float y, float z) {
    Mat4 m = mat_identity();
    m[12] = x;
    m[13] = y;
    m[14] = z;
    return m;
}

// Rotation about the +Y axis by `radians`, column-major.
//   [ c  0  s  0 ]
//   [ 0  1  0  0 ]
//   [-s  0  c  0 ]
//   [ 0  0  0  1 ]
inline Mat4 mat_rotate_y(float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 m = mat_identity();
    m[0] = c;   m[2] = -s;   // column 0: (c, 0, -s, 0)
    m[8] = s;   m[10] = c;   // column 2: (s, 0,  c, 0)
    return m;
}

// Rotation about the +X axis by `radians`, column-major.
//   [ 1  0   0  0 ]
//   [ 0  c  -s  0 ]
//   [ 0  s   c  0 ]
//   [ 0  0   0  1 ]
inline Mat4 mat_rotate_x(float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 m = mat_identity();
    m[5] = c;   m[6] = s;    // column 1: (0, c, s, 0)
    m[9] = -s;  m[10] = c;   // column 2: (0, -s, c, 0)
    return m;
}

// Perspective projection, column-major (right-handed, clip z in [-1,1]). Mirrors
// gluPerspective / the canonical GL projection matrix.
inline Mat4 mat_perspective(float fovy_radians, float aspect, float znear, float zfar) {
    const float f = 1.0f / std::tan(fovy_radians * 0.5f);
    const float nf = 1.0f / (znear - zfar);
    Mat4 m{};
    m[0] = f / aspect;                  // col 0
    m[5] = f;                           // col 1
    m[10] = (zfar + znear) * nf;        // col 2
    m[11] = -1.0f;
    m[14] = (2.0f * zfar * znear) * nf; // col 3
    return m;
}

// ---------------------------------------------------------------------------
// build_spinning_cube_stream — one frame of a textured cube rotating in place.
//
// Geometry: a unit cube centered at the origin, 36 vertices (6 faces * 2 tris * 3
// verts), each vertex = interleaved position(x,y,z) + uv(u,v) = 5 floats, stride
// 20 bytes. A small procedural texture (the same solid-green 2x2 style as
// build_triangle_stream, so the on-screen result is unambiguous) is sampled by the
// fragment shader. The vertex shader applies a per-frame uMVP = P * V * M where M
// rotates about Y (and a fixed tilt about X so multiple faces are visible) by an
// angle proportional to frame/total_frames, V translates the cube back along -Z,
// and P is a fixed perspective. Depth testing is enabled (GL_DEPTH_TEST +
// GL_LESS via OP_DEPTH_FUNC) so the cube's far faces are correctly occluded.
//
// Virtual ids are CONSTANT across frames (vVS=1, vFS=2, vPROG=1, vVBO=1, vTEX=1) —
// the executor decodes each frame with a FRESH HostState, so re-using the same
// virtual ids every frame resolves to fresh real GL objects per frame (matching
// how run_live_integration_probe replays build_triangle_stream every frame). Uses
// ONLY ops decode_batch handles. Returns the encoded bytes.
// ---------------------------------------------------------------------------
inline std::vector<uint8_t> build_spinning_cube_stream(int frame, int total_frames,
                                                       int fbw, int fbh) {
    Encoder e;

    // Constant virtual IDs (v-prefixed to dodge the x86 <sys/reg.h> FS macro, per
    // build_triangle_stream's note).
    const uint32_t vVS = 1, vFS = 2, vPROG = 1, vVBO = 1, vTEX = 1;

    // 3D position + uv. uMVP is a full mat4 now (cube needs perspective + rotation).
    const char* vsrc =
        "attribute vec3 aPos; attribute vec2 aUv; varying vec2 vUv;"
        "uniform mat4 uMVP;"
        "void main(){ vUv = aUv; gl_Position = uMVP * vec4(aPos, 1.0); }";
    const char* fsrc =
        "precision mediump float; varying vec2 vUv; uniform sampler2D uTex;"
        "void main(){ gl_FragColor = texture2D(uTex, vUv); }";

    // --- shaders + program ---
    e.u8(OP_CREATE_SHADER); e.u32(vVS); e.u32(GL_VERTEX_SHADER);
    e.u8(OP_SHADER_SOURCE); e.u32(vVS); e.str(vsrc);
    e.u8(OP_COMPILE_SHADER); e.u32(vVS);
    e.u8(OP_CREATE_SHADER); e.u32(vFS); e.u32(GL_FRAGMENT_SHADER);
    e.u8(OP_SHADER_SOURCE); e.u32(vFS); e.str(fsrc);
    e.u8(OP_COMPILE_SHADER); e.u32(vFS);
    e.u8(OP_CREATE_PROGRAM); e.u32(vPROG);
    e.u8(OP_ATTACH_SHADER); e.u32(vPROG); e.u32(vVS);
    e.u8(OP_ATTACH_SHADER); e.u32(vPROG); e.u32(vFS);
    e.u8(OP_BIND_ATTRIB_LOCATION); e.u32(vPROG); e.u32(0); e.str("aPos");
    e.u8(OP_BIND_ATTRIB_LOCATION); e.u32(vPROG); e.u32(1); e.str("aUv");
    e.u8(OP_LINK_PROGRAM); e.u32(vPROG);

    // --- VBO: 36 interleaved (px,py,pz, u,v) vertices for the 6 cube faces. ---
    // Per face: two triangles (v0,v1,v2) + (v0,v2,v3), CCW, full-texture UVs.
    const float h = 0.6f;  // half-extent of the cube
    struct V { float x, y, z, u, v; };
    auto quad = [&](std::vector<V>& out, V a, V b, V c, V d) {
        out.push_back(a); out.push_back(b); out.push_back(c);
        out.push_back(a); out.push_back(c); out.push_back(d);
    };
    std::vector<V> vtx;
    vtx.reserve(36);
    // +Z (front)
    quad(vtx, {-h,-h, h, 0,0}, { h,-h, h, 1,0}, { h, h, h, 1,1}, {-h, h, h, 0,1});
    // -Z (back)
    quad(vtx, { h,-h,-h, 0,0}, {-h,-h,-h, 1,0}, {-h, h,-h, 1,1}, { h, h,-h, 0,1});
    // +X (right)
    quad(vtx, { h,-h, h, 0,0}, { h,-h,-h, 1,0}, { h, h,-h, 1,1}, { h, h, h, 0,1});
    // -X (left)
    quad(vtx, {-h,-h,-h, 0,0}, {-h,-h, h, 1,0}, {-h, h, h, 1,1}, {-h, h,-h, 0,1});
    // +Y (top)
    quad(vtx, {-h, h, h, 0,0}, { h, h, h, 1,0}, { h, h,-h, 1,1}, {-h, h,-h, 0,1});
    // -Y (bottom)
    quad(vtx, {-h,-h,-h, 0,0}, { h,-h,-h, 1,0}, { h,-h, h, 1,1}, {-h,-h, h, 0,1});

    std::vector<float> verts;
    verts.reserve(vtx.size() * 5);
    for (const V& p : vtx) {
        verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z);
        verts.push_back(p.u); verts.push_back(p.v);
    }
    const uint32_t vert_count = static_cast<uint32_t>(vtx.size());        // 36
    const uint32_t vbytes = static_cast<uint32_t>(verts.size() * sizeof(float));

    e.u8(OP_GEN_BUFFER); e.u32(vVBO);
    e.u8(OP_BIND_BUFFER); e.u32(GL_ARRAY_BUFFER); e.u32(vVBO);
    e.u8(OP_BUFFER_DATA); e.u32(GL_ARRAY_BUFFER);
    e.blob(verts.data(), vbytes); e.u32(GL_STATIC_DRAW);

    // --- 2x2 solid-green texture (unambiguous on screen), same as the triangle. ---
    const uint8_t tex[2 * 2 * 4] = {
        0, 220, 0, 255,  0, 220, 0, 255,
        0, 220, 0, 255,  0, 220, 0, 255,
    };
    e.u8(OP_GEN_TEXTURE); e.u32(vTEX);
    e.u8(OP_ACTIVE_TEXTURE); e.u32(GL_TEXTURE0);
    e.u8(OP_BIND_TEXTURE); e.u32(GL_TEXTURE_2D); e.u32(vTEX);
    e.u8(OP_TEX_PARAMETERI); e.u32(GL_TEXTURE_2D); e.u32(GL_TEXTURE_MIN_FILTER); e.i32(GL_NEAREST);
    e.u8(OP_TEX_PARAMETERI); e.u32(GL_TEXTURE_2D); e.u32(GL_TEXTURE_MAG_FILTER); e.i32(GL_NEAREST);
    e.u8(OP_TEX_IMAGE_2D); e.u32(GL_TEXTURE_2D); e.i32(0); e.u32(GL_RGBA);
    e.i32(2); e.i32(2); e.u32(GL_RGBA); e.u32(GL_UNSIGNED_BYTE);
    e.blob(tex, sizeof(tex));

    // --- per-frame MVP: P * V * M (column-major; uploaded transpose=GL_FALSE). ---
    const float aspect = fbh > 0 ? static_cast<float>(fbw) / static_cast<float>(fbh) : 1.0f;
    const Mat4 proj = mat_perspective(60.0f * 3.14159265358979323846f / 180.0f, aspect,
                                      0.1f, 10.0f);
    const Mat4 view = mat_translate(0.0f, 0.0f, -2.6f);  // push the cube back from the eye
    const float denom = total_frames > 0 ? static_cast<float>(total_frames) : 1.0f;
    const float angle = 2.0f * 3.14159265358979323846f * static_cast<float>(frame) / denom;
    // Spin about Y, plus a fixed ~26.5deg tilt about X so the top face is visible.
    const Mat4 model = mat_mul(mat_rotate_x(0.4636476f), mat_rotate_y(angle));
    const Mat4 mvp = mat_mul(proj, mat_mul(view, model));

    // --- the frame ---
    e.u8(OP_VIEWPORT); e.i32(0); e.i32(0); e.i32(fbw); e.i32(fbh);
    e.u8(OP_ENABLE); e.u32(GL_DEPTH_TEST);
    e.u8(OP_DEPTH_FUNC); e.u32(GL_LESS);
    e.u8(OP_CLEARCOLOR); e.f32(0.10f); e.f32(0.10f); e.f32(0.40f); e.f32(1.0f);
    e.u8(OP_CLEAR);  // decoder clears COLOR|DEPTH together
    e.u8(OP_USE_PROGRAM); e.u32(vPROG);
    e.u8(OP_UNIFORM_MATRIX4FV); e.u32(vPROG); e.str("uMVP");
    for (float f : mvp) e.f32(f);
    e.u8(OP_UNIFORM1I); e.u32(vPROG); e.str("uTex"); e.i32(0);
    e.u8(OP_ENABLE_VAA); e.u32(0);
    e.u8(OP_VERTEX_ATTRIB_POINTER); e.u32(0); e.i32(3); e.u32(GL_FLOAT);
    e.u8(0); e.i32(5 * sizeof(float)); e.u32(0);                  // aPos: 3 floats @ off 0
    e.u8(OP_ENABLE_VAA); e.u32(1);
    e.u8(OP_VERTEX_ATTRIB_POINTER); e.u32(1); e.i32(2); e.u32(GL_FLOAT);
    e.u8(0); e.i32(5 * sizeof(float)); e.u32(3 * sizeof(float));  // aUv: 2 floats @ off 12
    e.u8(OP_DRAW_ARRAYS); e.u32(GL_TRIANGLES); e.i32(0); e.i32(static_cast<int32_t>(vert_count));
    e.u8(OP_END);
    return e.bytes();
}

// ---------------------------------------------------------------------------
// run_screen_cube_demo — in-process, two-thread, on-screen spinning-cube demo.
//
// Allocates a heap ring, starts a GpuExecutorService bound to `win` (so each
// rendered AHB is presented to the ANativeWindow via the cross-context external-OES
// path), spawns a producer thread that pushes `frames` cube frames (each followed
// by flush_and_wait so it stays in lock-step with the GL thread's present), then
// stops and joins. Returns a report whose first line is the gating verdict.
//
// PASS requires: the window context + present program were created (window_ready),
// every requested frame was presented (frames_presented == frames), the GPU is
// hardware (software == false), and no EGL/GL error was recorded. Pixels on-screen
// can't be sampled from here; visual confirmation is done on device.
// ---------------------------------------------------------------------------
inline std::string run_screen_cube_demo(ANativeWindow* win, int frames) {
    std::ostringstream out;
    if (frames <= 0) frames = 120;
    const uint32_t kFrames = static_cast<uint32_t>(frames);
    constexpr uint32_t kRingBytes = 1u << 16;  // 64 KiB data ring (a cube frame ~1.7 KiB)

    // Render at the window's resolution when available, else a sane default.
    int W = win ? ANativeWindow_getWidth(win) : 0;
    int H = win ? ANativeWindow_getHeight(win) : 0;
    if (W <= 0) W = 1280;
    if (H <= 0) H = 720;

    if (win == nullptr) {
        out << "ALR GPU SCREEN CUBE: FAIL\nalr gpu screen error=null-window";
        return out.str();
    }

    // ---- Allocate + init the shared ring region (heap; both threads see it). ----
    std::vector<uint8_t> region(ring_region_size(kRingBytes), 0u);
    if (!ring_init(region.data(), kRingBytes)) {
        out << "ALR GPU SCREEN CUBE: FAIL\nalr gpu screen error=ring-init";
        return out.str();
    }

    // ---- Start the executor with the window (creates pbuffer + window contexts,
    //      AHB-FBO + depth, and the external-OES present program on its GL thread). ----
    GpuExecutorService svc(region.data(), region.size(), W, H,
                           GpuExecutorService::PresentFn{}, win);
    const bool started = svc.start();
    if (!started) {
        out << "ALR GPU SCREEN CUBE: FAIL\nalr gpu screen error=executor-start:" << svc.error();
        svc.stop();
        return out.str();
    }

    // ---- Producer thread: push cube frame i + flush_and_wait, for `frames`. ----
    std::atomic<uint32_t> frames_sent{0};
    std::atomic<bool> producer_ok{true};
    std::thread producer([&] {
        RingProducer prod(region.data());
        if (!prod.valid()) {
            producer_ok.store(false, std::memory_order_release);
            return;
        }
        for (uint32_t f = 0; f < kFrames; ++f) {
            const std::vector<uint8_t> stream =
                build_spinning_cube_stream(static_cast<int>(f), static_cast<int>(kFrames), W, H);
            size_t off = 0;
            const auto* base = stream.data();
            const size_t total = stream.size();
            while (off < total) {
                const uint32_t chunk =
                    static_cast<uint32_t>(std::min<size_t>(total - off, kRingBytes / 2));
                if (prod.append(base + off, chunk)) {
                    off += chunk;
                } else {
                    std::this_thread::yield();  // ring full — let the consumer drain
                }
            }
            // One sync point per frame: bump req_seq, block until the executor has
            // decoded + presented this frame (reply_seq catches up).
            prod.flush_and_wait(1u << 24);
            frames_sent.fetch_add(1, std::memory_order_acq_rel);
        }
        prod.close();
    });

    producer.join();
    svc.stop();  // drains any final frame, then joins the GL thread

    // ---- Build the report. ----
    const uint32_t presented = svc.frames_presented();
    const uint32_t sent = frames_sent.load(std::memory_order_acquire);
    const bool sw = svc.software();
    const bool no_err = svc.error().empty();
    const bool win_ok = svc.window_ready();
    const bool prod_ok = producer_ok.load(std::memory_order_acquire);

    const bool pass = win_ok && (presented == kFrames) && (sent == kFrames) && !sw &&
                      no_err && prod_ok;

    out << "ALR GPU SCREEN CUBE: " << (pass ? "PASS" : "FAIL");
    out << "\nalr gpu screen model=in-process producer thread + GL executor thread, "
           "decode->AHB-FBO then cross-context external-OES present to ANativeWindow";
    out << "\nalr gpu screen size=" << W << "x" << H;
    out << "\nalr gpu screen frames_requested=" << kFrames;
    out << "\nalr gpu screen frames_sent=" << sent;
    out << "\nalr gpu screen frames_presented=" << presented;
    out << "\nalr gpu screen window present ready=" << (win_ok ? "true" : "false");
    out << "\nalr gpu screen renderer=" << svc.renderer_string();
    out << "\nalr gpu screen software renderer=" << (sw ? "true" : "false");
    out << "\nalr gpu screen producer ok=" << (prod_ok ? "true" : "false");
    out << "\nalr gpu screen error=" << (no_err ? "(none)" : svc.error());
    return out.str();
}

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_SCREEN_HPP
