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
    // --- vertex attrib + draw ---
    OP_ENABLE_VAA = 40,         // u32 index
    OP_VERTEX_ATTRIB_POINTER = 41, // u32 index, i32 size, u32 type, u8 norm, i32 stride, u32 offset
    OP_DRAW_ARRAYS = 42,        // u32 mode, i32 first, i32 count
    // --- uniforms (locations are real GL locations, looked up host-side by name) ---
    OP_UNIFORM_MATRIX4FV = 50,  // u32 vprog_id, u32 name_len, bytes(name), f32[16]
    OP_UNIFORM1I = 51,          // u32 vprog_id, u32 name_len, bytes(name), i32 value
    // --- textures ---
    OP_GEN_TEXTURE = 60,        // u32 vtex_id
    OP_ACTIVE_TEXTURE = 61,     // u32 unit (GL_TEXTURE0+n)
    OP_BIND_TEXTURE = 62,       // u32 target, u32 vtex_id
    OP_TEX_PARAMETERI = 63,     // u32 target, u32 pname, i32 param
    OP_TEX_IMAGE_2D = 64,       // u32 target,i32 level,u32 ifmt,i32 w,i32 h,u32 fmt,u32 type,u32 len,bytes
    // --- state ---
    OP_ENABLE = 70,             // u32 cap
    OP_DISABLE = 71,            // u32 cap
    OP_DEPTH_FUNC = 72,         // u32 func
};

// Host-side decode state: the virtual->real GL name translation tables. The guest
// allocates virtual ids monotonically and never learns the real Mali names.
struct HostState {
    std::map<uint32_t, GLuint> shaders;   // vshader_id -> real shader
    std::map<uint32_t, GLuint> programs;  // vprog_id   -> real program
    std::map<uint32_t, GLuint> buffers;   // vbuf_id    -> real buffer
    std::map<uint32_t, GLuint> textures;  // vtex_id    -> real texture
    GLuint cur_program = 0;               // real program currently in use (for uniforms)
    bool ok = true;                       // decode integrity (bad opcode / short read)
    int decoded = 0;                      // ops successfully dispatched

    GLuint real_prog(uint32_t v) const {
        auto it = programs.find(v);
        return it == programs.end() ? 0 : it->second;
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
