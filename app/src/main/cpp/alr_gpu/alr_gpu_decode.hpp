// ALR GPU command-stream decoder (Phase 4 / GPU-native Linux app track, M1).
//
// This is the HOST half of a gfxstream-style GLES marshalling layer: a byte
// stream of GL ops (encoded guest-side by a future libGLESv2/libEGL shim) is
// decoded here and replayed as REAL GLES2 calls on the device's Mali GPU. The
// guest's GL object names (shaders/programs/buffers/textures) are CLIENT-SIDE
// VIRTUAL IDs; this decoder owns the virtual->real translation tables so the
// guest never blocks on a round-trip for object creation (the key optimization
// from the design — see /tmp/gpu-native-app/DESIGN.md §2.3).
//
// M1 scope (this file): the op enum + decoder + virtual-ID maps + a SELF-CONTAINED
// host-only probe `alr_gpu_run_draw_probe()` that hand-builds a byte stream for a
// real shaded+textured triangle, decodes it on an EGL pbuffer + GLES2 context, and
// glReadPixels-verifies the result on Mali. No guest, no ring, no IPC — the
// de-risking keystone. Later milestones feed `alr_gpu_decode_batch()` the same op
// stream over a shared-memory ring from a real guest binary.
//
// Public-API only (EGL/GLES2), W^X-safe, no vendor-private GPU access. Header-only
// so it can be #included by runtime_report.cpp with a one-line add (the file the
// concurrent PC-gate workstream owns) — this keeps the GPU work in its own TU.

#ifndef ALR_GPU_ALR_GPU_DECODE_HPP
#define ALR_GPU_ALR_GPU_DECODE_HPP

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// GLES3 entry points the decoder replays. Declared here (rather than pulling
// <GLES3/gl3.h>) so this header stays GLES2-include-only and free of header-version
// clashes; the executor links libGLESv2 (which exports the GLES3 core symbols) and
// runs them on the GLES3 context GpuExecutorService requests (fallback GLES2). On a
// GLES2-only fallback these decode to no-effect calls on unsupported entry points —
// only reached if a guest actually emits GLES3 ops, which a GLES2 guest never does.
extern "C" {
void glGenVertexArrays(GLsizei n, GLuint* arrays);
void glBindVertexArray(GLuint array);
void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices,
                             GLsizei instancecount);
void glVertexAttribDivisor(GLuint index, GLuint divisor);
// GLES3 core: UBO binding, sampler objects, MRT / read-buffer / FBO invalidation.
void glBindBufferBase(GLenum target, GLuint index, GLuint buffer);
void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset,
                       GLsizeiptr size);
GLuint glGetUniformBlockIndex(GLuint program, const GLchar* uniformBlockName);
void glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding);
void glGenSamplers(GLsizei count, GLuint* samplers);
void glBindSampler(GLuint unit, GLuint sampler);
void glSamplerParameteri(GLuint sampler, GLenum pname, GLint param);
void glDrawBuffers(GLsizei n, const GLenum* bufs);
void glReadBuffer(GLenum src);
void glInvalidateFramebuffer(GLenum target, GLsizei numAttachments, const GLenum* attachments);
}

// GL_INVALID_INDEX (returned by glGetUniformBlockIndex for an absent block).
#ifndef GL_INVALID_INDEX
#define GL_INVALID_INDEX 0xFFFFFFFFu
#endif

namespace alr::gpu {

// ---- The op-stream opcode set. A superset of the original 7-op marshalling
// probe (VIEWPORT/CLEARCOLOR/CLEAR/SCISSOR) plus the GLES2 ops the cube needs:
// shader/program/buffer/texture creation, uploads, attrib binding, draw. ----
enum Op : uint8_t {
    OP_END = 0,
    // --- original marshalling-probe ops (kept identical) ---
    OP_VIEWPORT = 1,        // i32 x,y,w,h
    OP_CLEARCOLOR = 2,      // f32 r,g,b,a
    OP_CLEAR = 3,           // (mask is implied GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT)
    OP_ENABLE_SCISSOR = 4,
    OP_SCISSOR = 5,         // i32 x,y,w,h
    OP_DISABLE_SCISSOR = 6,
    // --- shaders + program ---
    OP_CREATE_SHADER = 20,      // u32 vshader_id, u32 gl_type(VERTEX/FRAGMENT)
    OP_SHADER_SOURCE = 21,      // u32 vshader_id, u32 len, bytes[len]
    OP_COMPILE_SHADER = 22,     // u32 vshader_id
    OP_CREATE_PROGRAM = 23,     // u32 vprog_id
    OP_ATTACH_SHADER = 24,      // u32 vprog_id, u32 vshader_id
    OP_BIND_ATTRIB_LOCATION = 25, // u32 vprog_id, u32 index, u32 name_len, bytes
    OP_LINK_PROGRAM = 26,       // u32 vprog_id
    OP_USE_PROGRAM = 27,        // u32 vprog_id
    // --- buffers (VBO) ---
    OP_GEN_BUFFER = 30,         // u32 vbuf_id
    OP_BIND_BUFFER = 31,        // u32 target, u32 vbuf_id
    OP_BUFFER_DATA = 32,        // u32 target, u32 len, bytes[len], u32 usage
    OP_BUFFER_SUBDATA = 33,     // u32 target, u32 offset, blob(data)
    // --- vertex attrib + draw ---
    OP_ENABLE_VAA = 40,         // u32 index
    OP_VERTEX_ATTRIB_POINTER = 41, // u32 index, i32 size, u32 type, u8 norm, i32 stride, u32 offset
    OP_DRAW_ARRAYS = 42,        // u32 mode, i32 first, i32 count
    // --- uniforms (locations are real GL locations, looked up host-side by name) ---
    OP_UNIFORM_MATRIX4FV = 50,  // u32 vprog_id, u32 name_len, bytes(name), f32[16]
    OP_UNIFORM1I = 51,          // u32 vprog_id, u32 name_len, bytes(name), i32 value
    // generalized uniform setters (glUniform{1..4}f[v] / {2..4}i + {1..4}iv / Matrix{2,3}fv):
    OP_UNIFORM_FV = 52,         // u32 vprog, blob(name), u8 cols(1..4), u32 count, f32[cols*count]
    OP_UNIFORM_IV = 53,         // u32 vprog, blob(name), u8 cols(1..4), u32 count, i32[cols*count]
    OP_UNIFORM_MATRIX_FV = 54,  // u32 vprog, blob(name), u8 dim(2..3), u32 count, f32[dim*dim*count]
    // --- textures ---
    OP_GEN_TEXTURE = 60,        // u32 vtex_id
    OP_ACTIVE_TEXTURE = 61,     // u32 unit (GL_TEXTURE0+n)
    OP_BIND_TEXTURE = 62,       // u32 target, u32 vtex_id
    OP_TEX_PARAMETERI = 63,     // u32 target, u32 pname, i32 param
    OP_TEX_IMAGE_2D = 64,       // u32 target,i32 level,u32 ifmt,i32 w,i32 h,u32 fmt,u32 type,u32 len,bytes
    OP_GENERATE_MIPMAP = 65,    // u32 target
    OP_TEX_SUBIMAGE_2D = 66,    // u32 target,i32 level,i32 xoff,i32 yoff,i32 w,i32 h,u32 fmt,u32 type,blob
    // --- state ---
    OP_ENABLE = 70,             // u32 cap
    OP_DISABLE = 71,            // u32 cap
    OP_DEPTH_FUNC = 72,         // u32 func
    OP_CULL_FACE = 73,          // u32 mode (GL_FRONT/BACK/FRONT_AND_BACK)
    OP_FRONT_FACE = 74,         // u32 mode (GL_CW/GL_CCW)
    // --- attribute-by-NAME (glGetAttribLocation has no round-trip: the guest carries
    //     the attribute NAME, the host resolves the real location by name, exactly as
    //     uniforms do). The guest emits these (instead of the index-based OP_ENABLE_VAA
    //     / OP_VERTEX_ATTRIB_POINTER) when the location came from glGetAttribLocation. ---
    OP_ENABLE_VAA_NAMED = 80,           // u32 vprog, blob(name)
    OP_VERTEX_ATTRIB_POINTER_NAMED = 81,// u32 vprog, blob(name), i32 size, u32 type, u8 norm, i32 stride, u32 offset
    // --- indexed draw (meshes) ---
    OP_DRAW_ELEMENTS = 82,      // u32 mode, i32 count, u32 type, u32 offset (into bound ELEMENT_ARRAY_BUFFER)
    // --- framebuffer / renderbuffer objects (render-to-texture scenes). Virtual FBO/RBO
    //     ids like buffers/textures; vfb 0 = the executor's default (AHB) target. ---
    OP_GEN_FRAMEBUFFER = 90,         // u32 vfb_id
    OP_BIND_FRAMEBUFFER = 91,        // u32 target, u32 vfb_id (0 -> default_fbo)
    OP_FRAMEBUFFER_TEXTURE2D = 92,   // u32 target, u32 attachment, u32 textarget, u32 vtex_id, i32 level
    OP_GEN_RENDERBUFFER = 93,        // u32 vrb_id
    OP_BIND_RENDERBUFFER = 94,       // u32 target, u32 vrb_id
    OP_RENDERBUFFER_STORAGE = 95,    // u32 target, u32 internalformat, i32 w, i32 h
    OP_FRAMEBUFFER_RENDERBUFFER = 96,// u32 target, u32 attachment, u32 rbtarget, u32 vrb_id
    // --- GLES3: vertex array objects + instanced draws (vva 0 = default VAO). ---
    OP_GEN_VERTEX_ARRAY = 100,       // u32 vva_id
    OP_BIND_VERTEX_ARRAY = 101,      // u32 vva_id (0 -> default VAO)
    OP_DRAW_ARRAYS_INSTANCED = 102,  // u32 mode, i32 first, i32 count, i32 instancecount
    OP_DRAW_ELEMENTS_INSTANCED = 103,// u32 mode, i32 count, u32 type, u32 offset, i32 instancecount
    OP_VERTEX_ATTRIB_DIVISOR = 104,      // u32 index, u32 divisor
    OP_VERTEX_ATTRIB_DIVISOR_NAMED = 105,// u32 vprog, blob(name), u32 divisor
    // --- per-fragment / raster STATE setters (blend / write-mask / depth-range /
    //     stencil / polygon-offset / line-width / sample-coverage). Plain scalar/enum
    //     state replayed 1:1; the state glmark2's blend/effect/shading/refract/shadow
    //     scenes (and GTK4-GL/SDL2 apps) set away from the GL defaults. No virtual ids. ---
    OP_BLEND_FUNC = 110,             // u32 sfactor, u32 dfactor
    OP_BLEND_FUNC_SEPARATE = 111,    // u32 srcRGB, u32 dstRGB, u32 srcAlpha, u32 dstAlpha
    OP_BLEND_EQUATION = 112,         // u32 mode
    OP_BLEND_EQUATION_SEPARATE = 113,// u32 modeRGB, u32 modeAlpha
    OP_BLEND_COLOR = 114,            // f32 r,g,b,a
    OP_COLOR_MASK = 115,             // u8 r, u8 g, u8 b, u8 a
    OP_DEPTH_MASK = 116,             // u8 flag
    OP_DEPTH_RANGEF = 117,           // f32 near, f32 far
    OP_CLEAR_DEPTHF = 118,           // f32 depth
    OP_CLEAR_STENCIL = 119,          // i32 s
    OP_STENCIL_FUNC = 120,           // u32 func, i32 ref, u32 mask
    OP_STENCIL_FUNC_SEPARATE = 121,  // u32 face, u32 func, i32 ref, u32 mask
    OP_STENCIL_OP = 122,             // u32 sfail, u32 dpfail, u32 dppass
    OP_STENCIL_OP_SEPARATE = 123,    // u32 face, u32 sfail, u32 dpfail, u32 dppass
    OP_STENCIL_MASK = 124,           // u32 mask
    OP_STENCIL_MASK_SEPARATE = 125,  // u32 face, u32 mask
    OP_POLYGON_OFFSET = 126,         // f32 factor, f32 units
    OP_LINE_WIDTH = 127,             // f32 width
    OP_SAMPLE_COVERAGE = 128,        // f32 value, u8 invert
    // --- GLES3 core: UBO binding, sampler objects, MRT / read-buffer / FBO invalidation.
    //     Replayed on the GLES3 context GpuExecutorService requests (Mali-G615 = ES3.2).
    //     UBOs reuse the virtual buffer ids; samplers get their own virtual ids (like VAOs);
    //     uniform-block ops carry the block NAME (host resolves the real index by name). ---
    OP_BIND_BUFFER_BASE = 130,       // u32 target, u32 index, u32 vbuf_id
    OP_BIND_BUFFER_RANGE = 131,      // u32 target, u32 index, u32 vbuf_id, u32 offset, u32 size
    OP_UNIFORM_BLOCK_BINDING = 132,  // u32 vprog, blob(block_name), u32 binding
    OP_GEN_SAMPLER = 133,            // u32 vsampler_id
    OP_BIND_SAMPLER = 134,           // u32 unit, u32 vsampler_id (0 -> none)
    OP_SAMPLER_PARAMETERI = 135,     // u32 vsampler_id, u32 pname, i32 param
    OP_DRAW_BUFFERS = 136,           // u32 n, u32 bufs[n]
    OP_READ_BUFFER = 137,            // u32 mode
    OP_INVALIDATE_FRAMEBUFFER = 138, // u32 target, u32 n, u32 attachments[n]
};

// Host-side decode state: the virtual->real GL name translation tables. The guest
// allocates virtual ids monotonically and never learns the real Mali names.
struct HostState {
    std::map<uint32_t, GLuint> shaders;   // vshader_id -> real shader
    std::map<uint32_t, GLuint> programs;  // vprog_id   -> real program
    std::map<uint32_t, GLuint> buffers;   // vbuf_id    -> real buffer
    std::map<uint32_t, GLuint> textures;  // vtex_id    -> real texture
    std::map<uint32_t, GLuint> framebuffers;   // vfb_id -> real FBO
    std::map<uint32_t, GLuint> renderbuffers;  // vrb_id -> real RBO
    std::map<uint32_t, GLuint> vertex_arrays;  // vva_id -> real VAO (GLES3)
    std::map<uint32_t, GLuint> samplers;       // vsampler_id -> real sampler object (GLES3)
    GLuint cur_program = 0;               // real program currently in use (for uniforms)
    // The guest's framebuffer 0 is its "default" target. In this marshalling executor
    // the default target is the AHB-backed FBO, NOT GL's window framebuffer 0 — so the
    // caller (GpuExecutorService) sets default_fbo = the AHB FBO before decode, and a
    // guest glBindFramebuffer(.,0) binds that. 0 here (the header default) means GL's
    // real default framebuffer, which is correct for the host-only probes / harness.
    GLuint default_fbo = 0;
    bool ok = true;                       // decode integrity (bad opcode / short read)
    int decoded = 0;                      // ops successfully dispatched

    GLuint real_prog(uint32_t v) const {
        auto it = programs.find(v);
        return it == programs.end() ? 0 : it->second;
    }
    GLuint real_tex(uint32_t v) const {
        auto it = textures.find(v);
        return it == textures.end() ? 0 : it->second;
    }
    GLuint real_fb(uint32_t v) const {            // vfb 0 -> the executor's default target
        if (v == 0) return default_fbo;
        auto it = framebuffers.find(v);
        return it == framebuffers.end() ? 0 : it->second;
    }
    GLuint real_rb(uint32_t v) const {
        auto it = renderbuffers.find(v);
        return it == renderbuffers.end() ? 0 : it->second;
    }
    GLuint real_va(uint32_t v) const {            // vva 0 -> the default VAO (real 0)
        if (v == 0) return 0;
        auto it = vertex_arrays.find(v);
        return it == vertex_arrays.end() ? 0 : it->second;
    }
    GLuint real_sampler(uint32_t v) const {       // vsampler 0 -> "no sampler" (real 0)
        if (v == 0) return 0;
        auto it = samplers.find(v);
        return it == samplers.end() ? 0 : it->second;
    }
    GLuint real_buf(uint32_t v) const {           // UBO binding resolves the virtual buffer id
        auto it = buffers.find(v);
        return it == buffers.end() ? 0 : it->second;
    }
};

// A bounds-checked little-endian cursor over the op stream.
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool u8(uint8_t& v) { return take(&v, 1); }
    bool u32(uint32_t& v) { return take(&v, 4); }
    bool i32(int32_t& v) { return take(&v, 4); }
    bool f32(float& v) { return take(&v, 4); }
    // Read a length-prefixed blob: u32 len, then `len` bytes (pointer into stream).
    bool blob(const uint8_t*& data, uint32_t& len) {
        if (!u32(len)) return false;
        if (pos_ + len > n_) return false;
        data = p_ + pos_;
        pos_ += len;
        return true;
    }
    bool floats(float* dst, size_t count) {
        for (size_t i = 0; i < count; ++i)
            if (!f32(dst[i])) return false;
        return true;
    }
    bool done() const { return pos_ >= n_; }
    size_t pos() const { return pos_; }

private:
    template <typename T>
    bool take(T* out, size_t bytes) {
        if (pos_ + bytes > n_) return false;
        std::memcpy(out, p_ + pos_, bytes);
        pos_ += bytes;
        return true;
    }
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// THE DECODER. Replays one batch of ops as real GLES2 on the current context.
// Must be called with a current EGL context (the caller owns EGL setup so this
// stays transport-agnostic: M1 calls it on a pbuffer, M4 on an AHB-backed FBO).
// Returns false if the stream was malformed; GL errors are left for the caller to
// inspect via glGetError() (the probe checks it).
// ---------------------------------------------------------------------------
inline bool decode_batch(const uint8_t* data, size_t len, HostState& st) {
    Reader r(data, len);
    bool running = true;
    while (running && st.ok) {
        uint8_t op = 0;
        if (!r.u8(op)) { break; }  // clean end of buffer
        switch (op) {
            case OP_END: running = false; break;
            case OP_VIEWPORT: {
                int32_t x, y, w, h;
                if (!r.i32(x) || !r.i32(y) || !r.i32(w) || !r.i32(h)) { st.ok = false; break; }
                glViewport(x, y, w, h); ++st.decoded; break;
            }
            case OP_CLEARCOLOR: {
                float c[4];
                if (!r.floats(c, 4)) { st.ok = false; break; }
                glClearColor(c[0], c[1], c[2], c[3]); ++st.decoded; break;
            }
            case OP_CLEAR:
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); ++st.decoded; break;
            case OP_ENABLE_SCISSOR: glEnable(GL_SCISSOR_TEST); ++st.decoded; break;
            case OP_DISABLE_SCISSOR: glDisable(GL_SCISSOR_TEST); ++st.decoded; break;
            case OP_SCISSOR: {
                int32_t x, y, w, h;
                if (!r.i32(x) || !r.i32(y) || !r.i32(w) || !r.i32(h)) { st.ok = false; break; }
                glScissor(x, y, w, h); ++st.decoded; break;
            }
            case OP_CREATE_SHADER: {
                uint32_t vid, type;
                if (!r.u32(vid) || !r.u32(type)) { st.ok = false; break; }
                st.shaders[vid] = glCreateShader(type); ++st.decoded; break;
            }
            case OP_SHADER_SOURCE: {
                uint32_t vid, slen; const uint8_t* src;
                if (!r.u32(vid)) { st.ok = false; break; }
                if (!r.blob(src, slen)) { st.ok = false; break; }
                GLuint sh = st.shaders.count(vid) ? st.shaders[vid] : 0;
                const char* s = reinterpret_cast<const char*>(src);
                const GLint l = static_cast<GLint>(slen);
                glShaderSource(sh, 1, &s, &l); ++st.decoded; break;
            }
            case OP_COMPILE_SHADER: {
                uint32_t vid;
                if (!r.u32(vid)) { st.ok = false; break; }
                glCompileShader(st.shaders.count(vid) ? st.shaders[vid] : 0); ++st.decoded; break;
            }
            case OP_CREATE_PROGRAM: {
                uint32_t vid;
                if (!r.u32(vid)) { st.ok = false; break; }
                st.programs[vid] = glCreateProgram(); ++st.decoded; break;
            }
            case OP_ATTACH_SHADER: {
                uint32_t vp, vs;
                if (!r.u32(vp) || !r.u32(vs)) { st.ok = false; break; }
                glAttachShader(st.real_prog(vp), st.shaders.count(vs) ? st.shaders[vs] : 0);
                ++st.decoded; break;
            }
            case OP_BIND_ATTRIB_LOCATION: {
                uint32_t vp, index, nlen; const uint8_t* name;
                if (!r.u32(vp) || !r.u32(index)) { st.ok = false; break; }
                if (!r.blob(name, nlen)) { st.ok = false; break; }
                std::string nm(reinterpret_cast<const char*>(name), nlen);
                glBindAttribLocation(st.real_prog(vp), index, nm.c_str()); ++st.decoded; break;
            }
            case OP_LINK_PROGRAM: {
                uint32_t vp;
                if (!r.u32(vp)) { st.ok = false; break; }
                glLinkProgram(st.real_prog(vp)); ++st.decoded; break;
            }
            case OP_USE_PROGRAM: {
                uint32_t vp;
                if (!r.u32(vp)) { st.ok = false; break; }
                st.cur_program = st.real_prog(vp);
                glUseProgram(st.cur_program); ++st.decoded; break;
            }
            case OP_GEN_BUFFER: {
                uint32_t vid;
                if (!r.u32(vid)) { st.ok = false; break; }
                GLuint b = 0; glGenBuffers(1, &b); st.buffers[vid] = b; ++st.decoded; break;
            }
            case OP_BIND_BUFFER: {
                uint32_t target, vid;
                if (!r.u32(target) || !r.u32(vid)) { st.ok = false; break; }
                glBindBuffer(target, st.buffers.count(vid) ? st.buffers[vid] : 0);
                ++st.decoded; break;
            }
            case OP_BUFFER_DATA: {
                uint32_t target, dlen, usage; const uint8_t* d;
                if (!r.u32(target)) { st.ok = false; break; }
                if (!r.blob(d, dlen)) { st.ok = false; break; }
                if (!r.u32(usage)) { st.ok = false; break; }
                glBufferData(target, static_cast<GLsizeiptr>(dlen), d, usage); ++st.decoded; break;
            }
            case OP_BUFFER_SUBDATA: {
                uint32_t target, offset, dlen; const uint8_t* d;
                if (!r.u32(target) || !r.u32(offset)) { st.ok = false; break; }
                if (!r.blob(d, dlen)) { st.ok = false; break; }
                glBufferSubData(target, static_cast<GLintptr>(offset),
                                static_cast<GLsizeiptr>(dlen), d);
                ++st.decoded; break;
            }
            case OP_ENABLE_VAA: {
                uint32_t index;
                if (!r.u32(index)) { st.ok = false; break; }
                glEnableVertexAttribArray(index); ++st.decoded; break;
            }
            case OP_VERTEX_ATTRIB_POINTER: {
                uint32_t index, type, offset; int32_t size, stride; uint8_t norm;
                if (!r.u32(index) || !r.i32(size) || !r.u32(type) || !r.u8(norm) ||
                    !r.i32(stride) || !r.u32(offset)) { st.ok = false; break; }
                // VBO offset path only (a client-array pointer is out of scope for M1).
                glVertexAttribPointer(index, size, type, norm ? GL_TRUE : GL_FALSE, stride,
                                      reinterpret_cast<const void*>(
                                          static_cast<uintptr_t>(offset)));
                ++st.decoded; break;
            }
            case OP_DRAW_ARRAYS: {
                uint32_t mode; int32_t first, count;
                if (!r.u32(mode) || !r.i32(first) || !r.i32(count)) { st.ok = false; break; }
                glDrawArrays(mode, first, count); ++st.decoded; break;
            }
            case OP_UNIFORM_MATRIX4FV: {
                uint32_t vp, nlen; const uint8_t* name; float m[16];
                if (!r.u32(vp)) { st.ok = false; break; }
                if (!r.blob(name, nlen)) { st.ok = false; break; }
                if (!r.floats(m, 16)) { st.ok = false; break; }
                std::string nm(reinterpret_cast<const char*>(name), nlen);
                GLint loc = glGetUniformLocation(st.real_prog(vp), nm.c_str());
                if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, m);
                ++st.decoded; break;
            }
            case OP_UNIFORM1I: {
                uint32_t vp, nlen; const uint8_t* name; int32_t val;
                if (!r.u32(vp)) { st.ok = false; break; }
                if (!r.blob(name, nlen)) { st.ok = false; break; }
                if (!r.i32(val)) { st.ok = false; break; }
                std::string nm(reinterpret_cast<const char*>(name), nlen);
                GLint loc = glGetUniformLocation(st.real_prog(vp), nm.c_str());
                if (loc >= 0) glUniform1i(loc, val);
                ++st.decoded; break;
            }
            case OP_UNIFORM_FV: {
                uint32_t vp, nlen, count; const uint8_t* name; uint8_t cols;
                if (!r.u32(vp)) { st.ok = false; break; }
                if (!r.blob(name, nlen)) { st.ok = false; break; }
                if (!r.u8(cols) || !r.u32(count)) { st.ok = false; break; }
                if (cols < 1 || cols > 4) { st.ok = false; break; }
                std::vector<float> v(static_cast<size_t>(cols) * count);
                if (!r.floats(v.data(), v.size())) { st.ok = false; break; }
                std::string nm(reinterpret_cast<const char*>(name), nlen);
                GLint loc = glGetUniformLocation(st.real_prog(vp), nm.c_str());
                if (loc >= 0) {
                    switch (cols) {
                        case 1: glUniform1fv(loc, count, v.data()); break;
                        case 2: glUniform2fv(loc, count, v.data()); break;
                        case 3: glUniform3fv(loc, count, v.data()); break;
                        case 4: glUniform4fv(loc, count, v.data()); break;
                    }
                }
                ++st.decoded; break;
            }
            case OP_UNIFORM_IV: {
                uint32_t vp, nlen, count; const uint8_t* name; uint8_t cols;
                if (!r.u32(vp)) { st.ok = false; break; }
                if (!r.blob(name, nlen)) { st.ok = false; break; }
                if (!r.u8(cols) || !r.u32(count)) { st.ok = false; break; }
                if (cols < 1 || cols > 4) { st.ok = false; break; }
                std::vector<int32_t> v(static_cast<size_t>(cols) * count);
                bool rok = true;
                for (auto& e : v) { if (!r.i32(e)) { rok = false; break; } }
                if (!rok) { st.ok = false; break; }
                std::string nm(reinterpret_cast<const char*>(name), nlen);
                GLint loc = glGetUniformLocation(st.real_prog(vp), nm.c_str());
                if (loc >= 0) {
                    switch (cols) {
                        case 1: glUniform1iv(loc, count, v.data()); break;
                        case 2: glUniform2iv(loc, count, v.data()); break;
                        case 3: glUniform3iv(loc, count, v.data()); break;
                        case 4: glUniform4iv(loc, count, v.data()); break;
                    }
                }
                ++st.decoded; break;
            }
            case OP_UNIFORM_MATRIX_FV: {
                uint32_t vp, nlen, count; const uint8_t* name; uint8_t dim;
                if (!r.u32(vp)) { st.ok = false; break; }
                if (!r.blob(name, nlen)) { st.ok = false; break; }
                if (!r.u8(dim) || !r.u32(count)) { st.ok = false; break; }
                if (dim < 2 || dim > 3) { st.ok = false; break; }  // mat4 uses OP_UNIFORM_MATRIX4FV
                std::vector<float> v(static_cast<size_t>(dim) * dim * count);
                if (!r.floats(v.data(), v.size())) { st.ok = false; break; }
                std::string nm(reinterpret_cast<const char*>(name), nlen);
                GLint loc = glGetUniformLocation(st.real_prog(vp), nm.c_str());
                if (loc >= 0) {
                    if (dim == 2) glUniformMatrix2fv(loc, count, GL_FALSE, v.data());
                    else          glUniformMatrix3fv(loc, count, GL_FALSE, v.data());
                }
                ++st.decoded; break;
            }
            case OP_GEN_TEXTURE: {
                uint32_t vid;
                if (!r.u32(vid)) { st.ok = false; break; }
                GLuint t = 0; glGenTextures(1, &t); st.textures[vid] = t; ++st.decoded; break;
            }
            case OP_ACTIVE_TEXTURE: {
                uint32_t unit;
                if (!r.u32(unit)) { st.ok = false; break; }
                glActiveTexture(unit); ++st.decoded; break;
            }
            case OP_BIND_TEXTURE: {
                uint32_t target, vid;
                if (!r.u32(target) || !r.u32(vid)) { st.ok = false; break; }
                glBindTexture(target, st.textures.count(vid) ? st.textures[vid] : 0);
                ++st.decoded; break;
            }
            case OP_TEX_PARAMETERI: {
                uint32_t target, pname; int32_t param;
                if (!r.u32(target) || !r.u32(pname) || !r.i32(param)) { st.ok = false; break; }
                glTexParameteri(target, pname, param); ++st.decoded; break;
            }
            case OP_TEX_IMAGE_2D: {
                uint32_t target, ifmt, fmt, type, dlen; int32_t level, w, h; const uint8_t* d;
                if (!r.u32(target) || !r.i32(level) || !r.u32(ifmt) || !r.i32(w) || !r.i32(h) ||
                    !r.u32(fmt) || !r.u32(type)) { st.ok = false; break; }
                if (!r.blob(d, dlen)) { st.ok = false; break; }
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(target, level, static_cast<GLint>(ifmt), w, h, 0, fmt, type,
                             dlen ? d : nullptr);
                ++st.decoded; break;
            }
            case OP_GENERATE_MIPMAP: {
                uint32_t target; if (!r.u32(target)) { st.ok = false; break; }
                glGenerateMipmap(target); ++st.decoded; break;
            }
            case OP_TEX_SUBIMAGE_2D: {
                uint32_t target, fmt, type, dlen; int32_t level, xoff, yoff, w, h; const uint8_t* d;
                if (!r.u32(target) || !r.i32(level) || !r.i32(xoff) || !r.i32(yoff) ||
                    !r.i32(w) || !r.i32(h) || !r.u32(fmt) || !r.u32(type)) { st.ok = false; break; }
                if (!r.blob(d, dlen)) { st.ok = false; break; }
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexSubImage2D(target, level, xoff, yoff, w, h, fmt, type, dlen ? d : nullptr);
                ++st.decoded; break;
            }
            case OP_ENABLE: {
                uint32_t cap; if (!r.u32(cap)) { st.ok = false; break; }
                glEnable(cap); ++st.decoded; break;
            }
            case OP_DISABLE: {
                uint32_t cap; if (!r.u32(cap)) { st.ok = false; break; }
                glDisable(cap); ++st.decoded; break;
            }
            case OP_DEPTH_FUNC: {
                uint32_t func; if (!r.u32(func)) { st.ok = false; break; }
                glDepthFunc(func); ++st.decoded; break;
            }
            case OP_CULL_FACE: {
                uint32_t mode; if (!r.u32(mode)) { st.ok = false; break; }
                glCullFace(mode); ++st.decoded; break;
            }
            case OP_FRONT_FACE: {
                uint32_t mode; if (!r.u32(mode)) { st.ok = false; break; }
                glFrontFace(mode); ++st.decoded; break;
            }
            case OP_ENABLE_VAA_NAMED: {
                uint32_t vp, nlen; const uint8_t* name;
                if (!r.u32(vp)) { st.ok = false; break; }
                if (!r.blob(name, nlen)) { st.ok = false; break; }
                std::string nm(reinterpret_cast<const char*>(name), nlen);
                // Resolve the real attribute location BY NAME on the (linked) program,
                // mirroring the uniform path. A negative location is a silent no-op
                // (GL ignores it), matching the guest's own -1 handling.
                GLint loc = glGetAttribLocation(st.real_prog(vp), nm.c_str());
                if (loc >= 0) glEnableVertexAttribArray(static_cast<GLuint>(loc));
                ++st.decoded; break;
            }
            case OP_VERTEX_ATTRIB_POINTER_NAMED: {
                uint32_t vp, nlen, type, offset; int32_t size, stride; uint8_t norm;
                const uint8_t* name;
                if (!r.u32(vp)) { st.ok = false; break; }
                if (!r.blob(name, nlen)) { st.ok = false; break; }
                if (!r.i32(size) || !r.u32(type) || !r.u8(norm) || !r.i32(stride) ||
                    !r.u32(offset)) { st.ok = false; break; }
                std::string nm(reinterpret_cast<const char*>(name), nlen);
                GLint loc = glGetAttribLocation(st.real_prog(vp), nm.c_str());
                if (loc >= 0) {
                    glVertexAttribPointer(static_cast<GLuint>(loc), size, type,
                                          norm ? GL_TRUE : GL_FALSE, stride,
                                          reinterpret_cast<const void*>(
                                              static_cast<uintptr_t>(offset)));
                }
                ++st.decoded; break;
            }
            case OP_DRAW_ELEMENTS: {
                uint32_t mode, type, offset; int32_t count;
                if (!r.u32(mode) || !r.i32(count) || !r.u32(type) || !r.u32(offset)) {
                    st.ok = false; break;
                }
                // Indices live in the bound ELEMENT_ARRAY_BUFFER (a prior OP_BIND_BUFFER
                // + OP_BUFFER_DATA on that target); offset is a byte offset into it.
                glDrawElements(mode, count, type,
                               reinterpret_cast<const void*>(static_cast<uintptr_t>(offset)));
                ++st.decoded; break;
            }
            case OP_GEN_FRAMEBUFFER: {
                uint32_t vid; if (!r.u32(vid)) { st.ok = false; break; }
                GLuint f = 0; glGenFramebuffers(1, &f); st.framebuffers[vid] = f; ++st.decoded; break;
            }
            case OP_BIND_FRAMEBUFFER: {
                uint32_t target, vid;
                if (!r.u32(target) || !r.u32(vid)) { st.ok = false; break; }
                glBindFramebuffer(target, st.real_fb(vid));  // vid 0 -> default (AHB) target
                ++st.decoded; break;
            }
            case OP_FRAMEBUFFER_TEXTURE2D: {
                uint32_t target, attachment, textarget, vtex; int32_t level;
                if (!r.u32(target) || !r.u32(attachment) || !r.u32(textarget) ||
                    !r.u32(vtex) || !r.i32(level)) { st.ok = false; break; }
                glFramebufferTexture2D(target, attachment, textarget, st.real_tex(vtex), level);
                ++st.decoded; break;
            }
            case OP_GEN_RENDERBUFFER: {
                uint32_t vid; if (!r.u32(vid)) { st.ok = false; break; }
                GLuint rb = 0; glGenRenderbuffers(1, &rb); st.renderbuffers[vid] = rb; ++st.decoded; break;
            }
            case OP_BIND_RENDERBUFFER: {
                uint32_t target, vid;
                if (!r.u32(target) || !r.u32(vid)) { st.ok = false; break; }
                glBindRenderbuffer(target, st.real_rb(vid)); ++st.decoded; break;
            }
            case OP_RENDERBUFFER_STORAGE: {
                uint32_t target, ifmt; int32_t w, h;
                if (!r.u32(target) || !r.u32(ifmt) || !r.i32(w) || !r.i32(h)) { st.ok = false; break; }
                glRenderbufferStorage(target, ifmt, w, h); ++st.decoded; break;
            }
            case OP_FRAMEBUFFER_RENDERBUFFER: {
                uint32_t target, attachment, rbtarget, vrb;
                if (!r.u32(target) || !r.u32(attachment) || !r.u32(rbtarget) ||
                    !r.u32(vrb)) { st.ok = false; break; }
                glFramebufferRenderbuffer(target, attachment, rbtarget, st.real_rb(vrb));
                ++st.decoded; break;
            }
            case OP_GEN_VERTEX_ARRAY: {
                uint32_t vid; if (!r.u32(vid)) { st.ok = false; break; }
                GLuint va = 0; glGenVertexArrays(1, &va); st.vertex_arrays[vid] = va; ++st.decoded; break;
            }
            case OP_BIND_VERTEX_ARRAY: {
                uint32_t vid; if (!r.u32(vid)) { st.ok = false; break; }
                glBindVertexArray(st.real_va(vid)); ++st.decoded; break;
            }
            case OP_DRAW_ARRAYS_INSTANCED: {
                uint32_t mode; int32_t first, count, inst;
                if (!r.u32(mode) || !r.i32(first) || !r.i32(count) || !r.i32(inst)) {
                    st.ok = false; break;
                }
                glDrawArraysInstanced(mode, first, count, inst); ++st.decoded; break;
            }
            case OP_DRAW_ELEMENTS_INSTANCED: {
                uint32_t mode, type, offset; int32_t count, inst;
                if (!r.u32(mode) || !r.i32(count) || !r.u32(type) || !r.u32(offset) ||
                    !r.i32(inst)) { st.ok = false; break; }
                glDrawElementsInstanced(mode, count, type,
                                        reinterpret_cast<const void*>(static_cast<uintptr_t>(offset)),
                                        inst);
                ++st.decoded; break;
            }
            case OP_VERTEX_ATTRIB_DIVISOR: {
                uint32_t index, divisor;
                if (!r.u32(index) || !r.u32(divisor)) { st.ok = false; break; }
                glVertexAttribDivisor(index, divisor); ++st.decoded; break;
            }
            case OP_VERTEX_ATTRIB_DIVISOR_NAMED: {
                uint32_t vp, nlen, divisor; const uint8_t* name;
                if (!r.u32(vp)) { st.ok = false; break; }
                if (!r.blob(name, nlen)) { st.ok = false; break; }
                if (!r.u32(divisor)) { st.ok = false; break; }
                std::string nm(reinterpret_cast<const char*>(name), nlen);
                GLint loc = glGetAttribLocation(st.real_prog(vp), nm.c_str());
                if (loc >= 0) glVertexAttribDivisor(static_cast<GLuint>(loc), divisor);
                ++st.decoded; break;
            }
            // ---- per-fragment / raster state setters (replayed 1:1; no virtual ids) ----
            case OP_BLEND_FUNC: {
                uint32_t s, d;
                if (!r.u32(s) || !r.u32(d)) { st.ok = false; break; }
                glBlendFunc(s, d); ++st.decoded; break;
            }
            case OP_BLEND_FUNC_SEPARATE: {
                uint32_t sr, dr, sa, da;
                if (!r.u32(sr) || !r.u32(dr) || !r.u32(sa) || !r.u32(da)) { st.ok = false; break; }
                glBlendFuncSeparate(sr, dr, sa, da); ++st.decoded; break;
            }
            case OP_BLEND_EQUATION: {
                uint32_t mode; if (!r.u32(mode)) { st.ok = false; break; }
                glBlendEquation(mode); ++st.decoded; break;
            }
            case OP_BLEND_EQUATION_SEPARATE: {
                uint32_t mr, ma;
                if (!r.u32(mr) || !r.u32(ma)) { st.ok = false; break; }
                glBlendEquationSeparate(mr, ma); ++st.decoded; break;
            }
            case OP_BLEND_COLOR: {
                float c[4];
                if (!r.floats(c, 4)) { st.ok = false; break; }
                glBlendColor(c[0], c[1], c[2], c[3]); ++st.decoded; break;
            }
            case OP_COLOR_MASK: {
                uint8_t cr, cg, cb, ca;
                if (!r.u8(cr) || !r.u8(cg) || !r.u8(cb) || !r.u8(ca)) { st.ok = false; break; }
                glColorMask(cr ? GL_TRUE : GL_FALSE, cg ? GL_TRUE : GL_FALSE,
                            cb ? GL_TRUE : GL_FALSE, ca ? GL_TRUE : GL_FALSE);
                ++st.decoded; break;
            }
            case OP_DEPTH_MASK: {
                uint8_t f; if (!r.u8(f)) { st.ok = false; break; }
                glDepthMask(f ? GL_TRUE : GL_FALSE); ++st.decoded; break;
            }
            case OP_DEPTH_RANGEF: {
                float n, fr;
                if (!r.f32(n) || !r.f32(fr)) { st.ok = false; break; }
                glDepthRangef(n, fr); ++st.decoded; break;
            }
            case OP_CLEAR_DEPTHF: {
                float d; if (!r.f32(d)) { st.ok = false; break; }
                glClearDepthf(d); ++st.decoded; break;
            }
            case OP_CLEAR_STENCIL: {
                int32_t s; if (!r.i32(s)) { st.ok = false; break; }
                glClearStencil(s); ++st.decoded; break;
            }
            case OP_STENCIL_FUNC: {
                uint32_t func, mask; int32_t ref;
                if (!r.u32(func) || !r.i32(ref) || !r.u32(mask)) { st.ok = false; break; }
                glStencilFunc(func, ref, mask); ++st.decoded; break;
            }
            case OP_STENCIL_FUNC_SEPARATE: {
                uint32_t face, func, mask; int32_t ref;
                if (!r.u32(face) || !r.u32(func) || !r.i32(ref) || !r.u32(mask)) { st.ok = false; break; }
                glStencilFuncSeparate(face, func, ref, mask); ++st.decoded; break;
            }
            case OP_STENCIL_OP: {
                uint32_t sf, df, dp;
                if (!r.u32(sf) || !r.u32(df) || !r.u32(dp)) { st.ok = false; break; }
                glStencilOp(sf, df, dp); ++st.decoded; break;
            }
            case OP_STENCIL_OP_SEPARATE: {
                uint32_t face, sf, df, dp;
                if (!r.u32(face) || !r.u32(sf) || !r.u32(df) || !r.u32(dp)) { st.ok = false; break; }
                glStencilOpSeparate(face, sf, df, dp); ++st.decoded; break;
            }
            case OP_STENCIL_MASK: {
                uint32_t m; if (!r.u32(m)) { st.ok = false; break; }
                glStencilMask(m); ++st.decoded; break;
            }
            case OP_STENCIL_MASK_SEPARATE: {
                uint32_t face, m;
                if (!r.u32(face) || !r.u32(m)) { st.ok = false; break; }
                glStencilMaskSeparate(face, m); ++st.decoded; break;
            }
            case OP_POLYGON_OFFSET: {
                float factor, units;
                if (!r.f32(factor) || !r.f32(units)) { st.ok = false; break; }
                glPolygonOffset(factor, units); ++st.decoded; break;
            }
            case OP_LINE_WIDTH: {
                float w; if (!r.f32(w)) { st.ok = false; break; }
                glLineWidth(w); ++st.decoded; break;
            }
            case OP_SAMPLE_COVERAGE: {
                float v; uint8_t inv;
                if (!r.f32(v) || !r.u8(inv)) { st.ok = false; break; }
                glSampleCoverage(v, inv ? GL_TRUE : GL_FALSE); ++st.decoded; break;
            }
            // ---- GLES3 core: UBO binding, sampler objects, MRT / read-buffer / invalidate ----
            case OP_BIND_BUFFER_BASE: {
                uint32_t target, index, vid;
                if (!r.u32(target) || !r.u32(index) || !r.u32(vid)) { st.ok = false; break; }
                glBindBufferBase(target, index, st.real_buf(vid)); ++st.decoded; break;
            }
            case OP_BIND_BUFFER_RANGE: {
                uint32_t target, index, vid, offset, size;
                if (!r.u32(target) || !r.u32(index) || !r.u32(vid) ||
                    !r.u32(offset) || !r.u32(size)) { st.ok = false; break; }
                glBindBufferRange(target, index, st.real_buf(vid),
                                  static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size));
                ++st.decoded; break;
            }
            case OP_UNIFORM_BLOCK_BINDING: {
                uint32_t vp, nlen, binding; const uint8_t* name;
                if (!r.u32(vp)) { st.ok = false; break; }
                if (!r.blob(name, nlen)) { st.ok = false; break; }
                if (!r.u32(binding)) { st.ok = false; break; }
                std::string nm(reinterpret_cast<const char*>(name), nlen);
                // Resolve the real block index BY NAME (no guest round-trip), mirroring the
                // uniform/attribute path. GL_INVALID_INDEX (absent block) is a silent no-op.
                GLuint idx = glGetUniformBlockIndex(st.real_prog(vp), nm.c_str());
                if (idx != GL_INVALID_INDEX)
                    glUniformBlockBinding(st.real_prog(vp), idx, binding);
                ++st.decoded; break;
            }
            case OP_GEN_SAMPLER: {
                uint32_t vid; if (!r.u32(vid)) { st.ok = false; break; }
                GLuint s = 0; glGenSamplers(1, &s); st.samplers[vid] = s; ++st.decoded; break;
            }
            case OP_BIND_SAMPLER: {
                uint32_t unit, vid;
                if (!r.u32(unit) || !r.u32(vid)) { st.ok = false; break; }
                glBindSampler(unit, st.real_sampler(vid)); ++st.decoded; break;
            }
            case OP_SAMPLER_PARAMETERI: {
                uint32_t vid, pname; int32_t param;
                if (!r.u32(vid) || !r.u32(pname) || !r.i32(param)) { st.ok = false; break; }
                glSamplerParameteri(st.real_sampler(vid), pname, param); ++st.decoded; break;
            }
            case OP_DRAW_BUFFERS: {
                uint32_t n; if (!r.u32(n)) { st.ok = false; break; }
                std::vector<GLenum> bufs(n);
                bool rok = true;
                for (uint32_t i = 0; i < n; ++i) { uint32_t b; if (!r.u32(b)) { rok = false; break; } bufs[i] = b; }
                if (!rok) { st.ok = false; break; }
                glDrawBuffers(static_cast<GLsizei>(n), bufs.empty() ? nullptr : bufs.data());
                ++st.decoded; break;
            }
            case OP_READ_BUFFER: {
                uint32_t mode; if (!r.u32(mode)) { st.ok = false; break; }
                glReadBuffer(mode); ++st.decoded; break;
            }
            case OP_INVALIDATE_FRAMEBUFFER: {
                uint32_t target, n;
                if (!r.u32(target) || !r.u32(n)) { st.ok = false; break; }
                std::vector<GLenum> att(n);
                bool rok = true;
                for (uint32_t i = 0; i < n; ++i) { uint32_t a; if (!r.u32(a)) { rok = false; break; } att[i] = a; }
                if (!rok) { st.ok = false; break; }
                glInvalidateFramebuffer(target, static_cast<GLsizei>(n),
                                        att.empty() ? nullptr : att.data());
                ++st.decoded; break;
            }
            default:
                st.ok = false; break;  // unknown opcode -> fail-stop (fail-safe)
        }
    }
    return st.ok;
}

// ---- A tiny encoder mirroring the decoder, used by M1 to hand-build a stream
// (and later reused by the guest shim's emitter). Little-endian. ----
class Encoder {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u32(uint32_t v) { raw(&v, 4); }
    void i32(int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void blob(const void* p, uint32_t n) { u32(n); if (n) raw(p, n); }
    void str(const char* s) { blob(s, static_cast<uint32_t>(std::strlen(s))); }
    const std::vector<uint8_t>& bytes() const { return buf_; }

private:
    void raw(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    std::vector<uint8_t> buf_;
};

}  // namespace alr::gpu

#endif  // ALR_GPU_ALR_GPU_DECODE_HPP
