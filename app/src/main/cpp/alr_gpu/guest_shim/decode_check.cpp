// decode_check.cpp — STAGE 2 of the off-device wire-format check.
//
// Reads the byte stream wc_emit produced (the REAL guest shim's encoder output) and
// decodes it with the COMMITTED host decoder (alr_gpu/alr_gpu_decode.hpp). The gl*
// entry points the decoder calls are RECORDING STUBS (below), so after decode we can
// assert the decoded GL calls match what the cube + mesh sequences issued — proving
// the guest encoder ↔ host decoder agree byte-for-byte. Host-only; never shipped.
//
// Build with -Istubinc (so <GLES2/gl2.h>/<EGL/egl.h> resolve to the harness stubs)
// and -I<repo>/app/src/main/cpp (so the decoder's "alr_gpu/..." include resolves).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "alr_gpu/alr_gpu_decode.hpp"  // the committed decoder (source of truth)

namespace rec {
struct BindAttrib { std::string name; GLuint index; };
struct VapIndexed { GLuint index; GLint size; uint32_t offset; };
struct VapNamed { std::string name; GLint size; uint32_t offset; };
struct BufData { GLenum target; GLsizeiptr size; };
struct DrawArr { GLenum mode; GLsizei count; };
struct DrawElem { GLenum mode; GLsizei count; GLenum type; uint32_t offset; };
struct TexImg { GLsizei w, h; GLenum fmt, type; std::vector<uint8_t> pixels; };

static std::vector<BindAttrib> bind_attribs;
static std::vector<VapIndexed> vaps_idx;     // cube path (glBindAttribLocation index)
static std::vector<VapNamed> vaps_named;      // mesh path (glGetAttribLocation by name)
static std::vector<std::string> enabled_named;
static std::vector<BufData> bufdatas;
static std::vector<DrawArr> draws;
static std::vector<DrawElem> draw_elems;
static std::vector<TexImg> teximgs;
static std::vector<GLenum> cull_modes, front_modes;
struct FbTex { GLenum attachment, textarget; GLuint texture; GLint level; };
struct RbStore { GLenum ifmt; GLsizei w, h; };
struct FbRb { GLenum attachment, rbtarget; };
static std::vector<GLuint> fb_binds;     // real fb id per glBindFramebuffer (0 = default)
static std::vector<FbTex> fb_texs;
static std::vector<RbStore> rb_stores;
static std::vector<FbRb> fb_rbs;
struct BufSub { GLenum target; GLintptr offset; GLsizeiptr size; };
struct TexSub { GLint xoff, yoff, w, h; GLenum fmt, type; std::vector<uint8_t> pixels; };
static std::vector<BufSub> buf_subs;
static std::vector<GLenum> mipmaps;
static std::vector<TexSub> tex_subs;
struct DrawArrInst { GLenum mode; GLsizei count, inst; };
struct DrawElemInst { GLenum mode; GLsizei count; GLenum type; GLsizei inst; };
static std::vector<GLuint> va_binds;     // real VAO id per glBindVertexArray (0 = default)
static std::vector<DrawArrInst> draw_arr_inst;
static std::vector<DrawElemInst> draw_elem_inst;
struct VadPlain { GLuint index; GLuint divisor; };
struct VadNamed { std::string name; GLuint divisor; };
static std::vector<VadPlain> vad_plain;
static std::vector<VadNamed> vad_named;
// GLES3 core: UBO binding, sampler objects, MRT / read-buffer / invalidate
struct BufBaseRec { GLenum target; GLuint index; GLuint buffer; };
struct BufRangeRec { GLenum target; GLuint index; GLuint buffer; GLintptr offset; GLsizeiptr size; };
struct UboBindingRec { std::string name; GLuint binding; };
struct SamplerParamRec { GLuint sampler; GLenum pname; GLint param; };
struct BindSamplerRec { GLuint unit; GLuint sampler; };
struct InvalidateRec { GLenum target; std::vector<GLenum> attachments; };
static std::vector<BufBaseRec> buf_bases;            // glBindBufferBase
static std::vector<BufRangeRec> buf_ranges;          // glBindBufferRange
static std::vector<UboBindingRec> ubo_bindings;      // glUniformBlockBinding (by name)
static std::vector<BindSamplerRec> bind_samplers;    // glBindSampler
static std::vector<SamplerParamRec> sampler_params;  // glSamplerParameteri
static std::vector<std::vector<GLenum>> draw_buffers_lists;  // glDrawBuffers
static std::vector<GLenum> read_buffers;             // glReadBuffer
static std::vector<InvalidateRec> invalidates;       // glInvalidateFramebuffer
static std::map<GLuint, std::string> block_idx_name; // glGetUniformBlockIndex -> name
static GLuint next_block_idx = 0;
// per-fragment / raster state setters
struct Blend2 { GLenum a, b; };
struct Blend4 { GLenum a, b, c, d; };
struct ColorMaskRec { GLboolean r, g, b, a; };
struct StencilFuncRec { GLenum func; GLint ref; GLuint mask; };
struct StencilFuncSepRec { GLenum face, func; GLint ref; GLuint mask; };
struct Stencil3 { GLenum a, b, c; };
struct Stencil4 { GLenum a, b, c, d; };
struct F2 { GLfloat a, b; };
struct SampleCovRec { GLfloat value; GLboolean invert; };
static std::vector<Blend2> blend_funcs;            // glBlendFunc(s,d)
static std::vector<Blend4> blend_func_seps;        // glBlendFuncSeparate
static std::vector<GLenum> blend_eqs;              // glBlendEquation
static std::vector<Blend2> blend_eq_seps;          // glBlendEquationSeparate
static std::vector<std::vector<float>> blend_colors;  // glBlendColor
static std::vector<ColorMaskRec> color_masks;      // glColorMask
static std::vector<GLboolean> depth_masks;         // glDepthMask
static std::vector<F2> depth_ranges;               // glDepthRangef
static std::vector<GLfloat> clear_depths;          // glClearDepthf
static std::vector<GLint> clear_stencils;          // glClearStencil
static std::vector<StencilFuncRec> stencil_funcs;  // glStencilFunc
static std::vector<StencilFuncSepRec> stencil_func_seps; // glStencilFuncSeparate
static std::vector<Stencil3> stencil_ops;          // glStencilOp
static std::vector<Stencil4> stencil_op_seps;      // glStencilOpSeparate
static std::vector<GLuint> stencil_masks;          // glStencilMask
static std::vector<Blend2> stencil_mask_seps;      // glStencilMaskSeparate (face,mask)
static std::vector<F2> polygon_offsets;            // glPolygonOffset
static std::vector<GLfloat> line_widths;           // glLineWidth
static std::vector<SampleCovRec> sample_coverages; // glSampleCoverage
static std::map<GLint, std::string> uniform_loc_name;  // glGetUniformLocation -> name
static std::map<GLint, std::string> attrib_loc_name;   // glGetAttribLocation  -> name
static std::map<std::string, std::vector<float>> mat_by_name;   // glUniformMatrix4fv
static std::map<std::string, GLint> i1_by_name;                 // glUniform1i
static std::map<std::string, std::vector<float>> fv_by_name;    // glUniform{1..4}f[v]
static std::map<std::string, std::vector<int32_t>> iv_by_name;  // glUniform{1..4}iv / {2..4}i
static std::map<std::string, std::vector<float>> matN_by_name;  // glUniformMatrix{2,3}fv
static GLint next_loc = 0;
static GLuint next_obj = 1000;
static bool unpack_align1 = false;

static void put_fv(GLint loc, const float *v, size_t n) {
    auto it = uniform_loc_name.find(loc);
    if (it != uniform_loc_name.end()) fv_by_name[it->second] = std::vector<float>(v, v + n);
}
static void put_iv(GLint loc, const int32_t *v, size_t n) {
    auto it = uniform_loc_name.find(loc);
    if (it != uniform_loc_name.end()) iv_by_name[it->second] = std::vector<int32_t>(v, v + n);
}
static void put_matN(GLint loc, const float *v, size_t n) {
    auto it = uniform_loc_name.find(loc);
    if (it != uniform_loc_name.end()) matN_by_name[it->second] = std::vector<float>(v, v + n);
}
}  // namespace rec

extern "C" {
void glViewport(GLint, GLint, GLsizei, GLsizei) {}
void glClearColor(GLclampf, GLclampf, GLclampf, GLclampf) {}
void glClear(GLbitfield) {}
void glEnable(GLenum) {}
void glDisable(GLenum) {}
void glScissor(GLint, GLint, GLsizei, GLsizei) {}
void glDepthFunc(GLenum) {}
void glCullFace(GLenum mode) { rec::cull_modes.push_back(mode); }
void glFrontFace(GLenum mode) { rec::front_modes.push_back(mode); }

GLuint glCreateShader(GLenum) { return rec::next_obj++; }
void glShaderSource(GLuint, GLsizei, const GLchar *const *, const GLint *) {}
void glCompileShader(GLuint) {}
GLuint glCreateProgram(void) { return rec::next_obj++; }
void glAttachShader(GLuint, GLuint) {}
void glBindAttribLocation(GLuint, GLuint index, const GLchar *name) {
    rec::bind_attribs.push_back({name ? std::string(name) : std::string(), index});
}
void glLinkProgram(GLuint) {}
void glUseProgram(GLuint) {}

void glGenBuffers(GLsizei n, GLuint *b) { for (GLsizei i = 0; i < n; ++i) b[i] = rec::next_obj++; }
void glBindBuffer(GLenum, GLuint) {}
void glBufferData(GLenum target, GLsizeiptr size, const void *, GLenum) {
    rec::bufdatas.push_back({target, size});
}

void glEnableVertexAttribArray(GLuint index) {
    auto it = rec::attrib_loc_name.find(static_cast<GLint>(index));
    if (it != rec::attrib_loc_name.end()) rec::enabled_named.push_back(it->second);
}
void glVertexAttribPointer(GLuint index, GLint size, GLenum, GLboolean, GLsizei, const void *p) {
    const uint32_t off = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
    auto it = rec::attrib_loc_name.find(static_cast<GLint>(index));
    if (it != rec::attrib_loc_name.end()) rec::vaps_named.push_back({it->second, size, off});
    else rec::vaps_idx.push_back({index, size, off});
}
void glDrawArrays(GLenum mode, GLint, GLsizei count) { rec::draws.push_back({mode, count}); }
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *p) {
    rec::draw_elems.push_back({mode, count, type,
                               static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p))});
}

GLint glGetUniformLocation(GLuint, const GLchar *name) {
    GLint loc = rec::next_loc++;
    rec::uniform_loc_name[loc] = name ? std::string(name) : std::string();
    return loc;
}
GLint glGetAttribLocation(GLuint, const GLchar *name) {
    GLint loc = rec::next_loc++;
    rec::attrib_loc_name[loc] = name ? std::string(name) : std::string();
    return loc;
}
void glUniformMatrix4fv(GLint loc, GLsizei, GLboolean, const GLfloat *v) {
    auto it = rec::uniform_loc_name.find(loc);
    if (it != rec::uniform_loc_name.end()) rec::mat_by_name[it->second] = std::vector<float>(v, v + 16);
}
void glUniform1i(GLint loc, GLint v0) {
    auto it = rec::uniform_loc_name.find(loc);
    if (it != rec::uniform_loc_name.end()) rec::i1_by_name[it->second] = v0;
}
void glUniform1fv(GLint loc, GLsizei c, const GLfloat *v) { rec::put_fv(loc, v, (size_t)1 * c); }
void glUniform2fv(GLint loc, GLsizei c, const GLfloat *v) { rec::put_fv(loc, v, (size_t)2 * c); }
void glUniform3fv(GLint loc, GLsizei c, const GLfloat *v) { rec::put_fv(loc, v, (size_t)3 * c); }
void glUniform4fv(GLint loc, GLsizei c, const GLfloat *v) { rec::put_fv(loc, v, (size_t)4 * c); }
void glUniform1iv(GLint loc, GLsizei c, const GLint *v) { rec::put_iv(loc, v, (size_t)1 * c); }
void glUniform2iv(GLint loc, GLsizei c, const GLint *v) { rec::put_iv(loc, v, (size_t)2 * c); }
void glUniform3iv(GLint loc, GLsizei c, const GLint *v) { rec::put_iv(loc, v, (size_t)3 * c); }
void glUniform4iv(GLint loc, GLsizei c, const GLint *v) { rec::put_iv(loc, v, (size_t)4 * c); }
void glUniformMatrix2fv(GLint loc, GLsizei c, GLboolean, const GLfloat *v) { rec::put_matN(loc, v, (size_t)4 * c); }
void glUniformMatrix3fv(GLint loc, GLsizei c, GLboolean, const GLfloat *v) { rec::put_matN(loc, v, (size_t)9 * c); }

void glGenTextures(GLsizei n, GLuint *t) { for (GLsizei i = 0; i < n; ++i) t[i] = rec::next_obj++; }
void glActiveTexture(GLenum) {}
void glBindTexture(GLenum, GLuint) {}
void glTexParameteri(GLenum, GLenum, GLint) {}
// Capture the bytes the decoder hands us so the wire-check can assert the guest repacked
// a (possibly UNPACK-padded) source into TIGHT rows. The decoder forces UNPACK_ALIGNMENT=1,
// so the blob it passes is exactly w*h*comp*ts tight bytes.
static size_t tight_bytes(GLsizei w, GLsizei h, GLenum fmt, GLenum type) {
    // GL token values inline (the minimal stub header doesn't define these names).
    size_t comp = (fmt == 0x1908) ? 4 : (fmt == 0x1907) ? 3        // RGBA / RGB
                : (fmt == 0x190A) ? 2 : (fmt == 0x1909) ? 1 : 4;   // LUMINANCE_ALPHA / LUMINANCE
    size_t ts = (type == 0x1401 || type == 0x1400) ? 1             // UNSIGNED_BYTE / BYTE
              : (type == 0x1403 || type == 0x1402) ? 2 : 4;        // UNSIGNED_SHORT / SHORT
    return (size_t)w * (size_t)h * comp * ts;
}
void glTexImage2D(GLenum, GLint, GLint, GLsizei w, GLsizei h, GLint, GLenum fmt, GLenum type,
                  const void *px) {
    rec::TexImg t{w, h, fmt, type, {}};
    if (px) { size_t n = tight_bytes(w, h, fmt, type);
              t.pixels.assign((const uint8_t*)px, (const uint8_t*)px + n); }
    rec::teximgs.push_back(std::move(t));
}
void glPixelStorei(GLenum pname, GLint param) {
    if (pname == GL_UNPACK_ALIGNMENT && param == 1) rec::unpack_align1 = true;
}
void glGenFramebuffers(GLsizei n, GLuint *f) { for (GLsizei i = 0; i < n; ++i) f[i] = rec::next_obj++; }
void glBindFramebuffer(GLenum, GLuint fb) { rec::fb_binds.push_back(fb); }
void glFramebufferTexture2D(GLenum, GLenum att, GLenum tt, GLuint tex, GLint lvl) {
    rec::fb_texs.push_back({att, tt, tex, lvl});
}
void glGenRenderbuffers(GLsizei n, GLuint *r) { for (GLsizei i = 0; i < n; ++i) r[i] = rec::next_obj++; }
void glBindRenderbuffer(GLenum, GLuint) {}
void glRenderbufferStorage(GLenum, GLenum ifmt, GLsizei w, GLsizei h) {
    rec::rb_stores.push_back({ifmt, w, h});
}
void glFramebufferRenderbuffer(GLenum, GLenum att, GLenum rbt, GLuint) {
    rec::fb_rbs.push_back({att, rbt});
}
void glBufferSubData(GLenum t, GLintptr off, GLsizeiptr sz, const void *) {
    rec::buf_subs.push_back({t, off, sz});
}
void glGenerateMipmap(GLenum t) { rec::mipmaps.push_back(t); }
void glTexSubImage2D(GLenum, GLint, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum f, GLenum ty,
                     const void *px) {
    rec::TexSub t{xo, yo, w, h, f, ty, {}};
    if (px) { size_t n = tight_bytes(w, h, f, ty);
              t.pixels.assign((const uint8_t*)px, (const uint8_t*)px + n); }
    rec::tex_subs.push_back(std::move(t));
}
void glGenVertexArrays(GLsizei n, GLuint *a) { for (GLsizei i = 0; i < n; ++i) a[i] = rec::next_obj++; }
void glBindVertexArray(GLuint va) { rec::va_binds.push_back(va); }
void glDrawArraysInstanced(GLenum m, GLint, GLsizei c, GLsizei inst) {
    rec::draw_arr_inst.push_back({m, c, inst});
}
void glDrawElementsInstanced(GLenum m, GLsizei c, GLenum t, const void *, GLsizei inst) {
    rec::draw_elem_inst.push_back({m, c, t, inst});
}
void glVertexAttribDivisor(GLuint index, GLuint divisor) {
    auto it = rec::attrib_loc_name.find(static_cast<GLint>(index));
    if (it != rec::attrib_loc_name.end()) rec::vad_named.push_back({it->second, divisor});
    else rec::vad_plain.push_back({index, divisor});
}
// per-fragment / raster state setters
void glBlendFunc(GLenum s, GLenum d) { rec::blend_funcs.push_back({s, d}); }
void glBlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) {
    rec::blend_func_seps.push_back({sr, dr, sa, da});
}
void glBlendEquation(GLenum m) { rec::blend_eqs.push_back(m); }
void glBlendEquationSeparate(GLenum mr, GLenum ma) { rec::blend_eq_seps.push_back({mr, ma}); }
void glBlendColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
    rec::blend_colors.push_back({r, g, b, a});
}
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    rec::color_masks.push_back({r, g, b, a});
}
void glDepthMask(GLboolean f) { rec::depth_masks.push_back(f); }
void glDepthRangef(GLclampf n, GLclampf f) { rec::depth_ranges.push_back({n, f}); }
void glClearDepthf(GLclampf d) { rec::clear_depths.push_back(d); }
void glClearStencil(GLint s) { rec::clear_stencils.push_back(s); }
void glStencilFunc(GLenum func, GLint ref, GLuint mask) {
    rec::stencil_funcs.push_back({func, ref, mask});
}
void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) {
    rec::stencil_func_seps.push_back({face, func, ref, mask});
}
void glStencilOp(GLenum sf, GLenum df, GLenum dp) { rec::stencil_ops.push_back({sf, df, dp}); }
void glStencilOpSeparate(GLenum face, GLenum sf, GLenum df, GLenum dp) {
    rec::stencil_op_seps.push_back({face, sf, df, dp});
}
void glStencilMask(GLuint m) { rec::stencil_masks.push_back(m); }
void glStencilMaskSeparate(GLenum face, GLuint m) { rec::stencil_mask_seps.push_back({face, m}); }
void glPolygonOffset(GLfloat factor, GLfloat units) { rec::polygon_offsets.push_back({factor, units}); }
void glLineWidth(GLfloat w) { rec::line_widths.push_back(w); }
void glSampleCoverage(GLclampf v, GLboolean inv) { rec::sample_coverages.push_back({v, inv}); }
// GLES3 core: UBO binding, sampler objects, MRT / read-buffer / invalidate
void glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
    rec::buf_bases.push_back({target, index, buffer});
}
void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    rec::buf_ranges.push_back({target, index, buffer, offset, size});
}
GLuint glGetUniformBlockIndex(GLuint, const GLchar *name) {
    GLuint idx = rec::next_block_idx++;
    rec::block_idx_name[idx] = name ? std::string(name) : std::string();
    return idx;
}
void glUniformBlockBinding(GLuint, GLuint blockIndex, GLuint binding) {
    auto it = rec::block_idx_name.find(blockIndex);
    if (it != rec::block_idx_name.end()) rec::ubo_bindings.push_back({it->second, binding});
}
void glGenSamplers(GLsizei n, GLuint *s) { for (GLsizei i = 0; i < n; ++i) s[i] = rec::next_obj++; }
void glBindSampler(GLuint unit, GLuint sampler) { rec::bind_samplers.push_back({unit, sampler}); }
void glSamplerParameteri(GLuint sampler, GLenum pname, GLint param) {
    rec::sampler_params.push_back({sampler, pname, param});
}
void glDrawBuffers(GLsizei n, const GLenum *bufs) {
    rec::draw_buffers_lists.push_back(std::vector<GLenum>(bufs, bufs + (n > 0 ? n : 0)));
}
void glReadBuffer(GLenum src) { rec::read_buffers.push_back(src); }
void glInvalidateFramebuffer(GLenum target, GLsizei n, const GLenum *att) {
    rec::invalidates.push_back({target, std::vector<GLenum>(att, att + (n > 0 ? n : 0))});
}
}  // extern "C"

// GL tokens the assertions compare against (not all in the minimal stub header).
static constexpr unsigned kGL_ARRAY_BUFFER         = 0x8892;
static constexpr unsigned kGL_ELEMENT_ARRAY_BUFFER = 0x8893;
static constexpr unsigned kGL_RGBA                 = 0x1908;
static constexpr unsigned kGL_UNSIGNED_BYTE        = 0x1401;
static constexpr unsigned kGL_UNSIGNED_SHORT       = 0x1403;
static constexpr unsigned kGL_TRIANGLES            = 0x0004;
static constexpr unsigned kGL_BACK                 = 0x0405;
static constexpr unsigned kGL_CCW                  = 0x0901;
// blend / stencil tokens (standard registry values) for the state-setter assertions
static constexpr unsigned kGL_SRC_ALPHA            = 0x0302;
static constexpr unsigned kGL_ONE_MINUS_SRC_ALPHA  = 0x0303;
static constexpr unsigned kGL_ONE                  = 1;
static constexpr unsigned kGL_ZERO                 = 0;
static constexpr unsigned kGL_FUNC_ADD             = 0x8006;
static constexpr unsigned kGL_FUNC_SUBTRACT        = 0x800A;
static constexpr unsigned kGL_FRONT                = 0x0404;
static constexpr unsigned kGL_ALWAYS               = 0x0207;
static constexpr unsigned kGL_KEEP                 = 0x1E00;
static constexpr unsigned kGL_REPLACE              = 0x1E01;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_fail = 1;
}
static bool has_buf(GLenum target, GLsizeiptr size) {
    for (auto &b : rec::bufdatas) if (b.target == target && b.size == size) return true;
    return false;
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: wc_decode <stream.bin>\n"); return 2; }
    FILE *f = std::fopen(argv[1], "rb");
    if (!f) { std::fprintf(stderr, "wc_decode: cannot open %s\n", argv[1]); return 2; }
    std::vector<uint8_t> bytes;
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    if (n > 0) { bytes.resize(static_cast<size_t>(n)); size_t r = std::fread(bytes.data(), 1, bytes.size(), f); (void)r; }
    std::fclose(f);
    std::printf("wc_decode: %zu bytes\n", bytes.size());

    alr::gpu::HostState st;
    const bool ok = alr::gpu::decode_batch(bytes.data(), bytes.size(), st);

    // --- the stream decoded into exactly the cube + mesh GL calls. ---
    check(ok && st.ok, "decode_batch returned true (well-formed, no bad/unknown opcode)");
    check(st.decoded == 102, "decoded op count == 102 (+3 RGB UNPACK-repack: gen/bind/teximg)");
    check(st.shaders.size() == 2, "2 shaders mapped");
    check(st.programs.size() == 1, "1 program mapped");
    check(st.buffers.size() == 3, "3 buffers mapped (vbo + ebo + ubo)");
    check(st.textures.size() == 2, "2 textures mapped (RGBA cube + RGB padded)");
    check(st.samplers.size() == 1, "1 sampler object mapped (GLES3)");

    bool ba_ok = rec::bind_attribs.size() == 2;
    for (auto &b : rec::bind_attribs) {
        if (b.name == "aPos") ba_ok = ba_ok && (b.index == 0);
        else if (b.name == "aUV") ba_ok = ba_ok && (b.index == 1);
        else ba_ok = false;
    }
    check(ba_ok, "glBindAttribLocation: aPos->0, aUV->1 (index path intact)");

    check(has_buf(kGL_ARRAY_BUFFER, 36 * 5 * 4), "glBufferData ARRAY_BUFFER 720 bytes (vbo)");
    check(has_buf(kGL_ELEMENT_ARRAY_BUFFER, 6 * 2), "glBufferData ELEMENT_ARRAY_BUFFER 12 bytes (ebo)");

    bool tex_ok = rec::teximgs.size() == 2 && rec::teximgs[0].w == 8 && rec::teximgs[0].h == 8 &&
                  rec::teximgs[0].fmt == kGL_RGBA && rec::teximgs[0].type == kGL_UNSIGNED_BYTE &&
                  rec::teximgs[0].pixels.size() == 256;
    check(tex_ok, "glTexImage2D[0] 8x8 RGBA/UNSIGNED_BYTE (256-byte tight upload)");
    check(rec::unpack_align1, "host forced UNPACK_ALIGNMENT=1 before upload");

    // RGB 3x2 uploaded with source UNPACK_ALIGNMENT=4 (rows padded 9->12) must arrive
    // TIGHT: 9-byte rows back-to-back = {10..18, 20..28}, with the 0xEE pad bytes DROPPED.
    static constexpr unsigned kGL_RGB = 0x1907;
    bool rgb_ok = rec::teximgs.size() == 2 && rec::teximgs[1].w == 3 && rec::teximgs[1].h == 2 &&
                  rec::teximgs[1].fmt == kGL_RGB && rec::teximgs[1].type == kGL_UNSIGNED_BYTE &&
                  rec::teximgs[1].pixels.size() == 18;     // 3*2*3, NOT 24 (no padding)
    if (rgb_ok) {
        const auto &px = rec::teximgs[1].pixels;
        for (int c = 0; c < 9 && rgb_ok; ++c) rgb_ok = rgb_ok && (px[c] == (uint8_t)(10 + c));
        for (int c = 0; c < 9 && rgb_ok; ++c) rgb_ok = rgb_ok && (px[9 + c] == (uint8_t)(20 + c));
    }
    check(rgb_ok, "glTexImage2D[1] 3x2 RGB repacked to TIGHT 18 bytes (UNPACK align-4 pad dropped)");

    auto mvp = rec::mat_by_name.find("uMVP");
    bool mvp_ok = mvp != rec::mat_by_name.end() && mvp->second.size() == 16;
    if (mvp_ok) {
        const float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        for (int i = 0; i < 16; ++i) mvp_ok = mvp_ok && (mvp->second[i] == id[i]);
    }
    check(mvp_ok, "uMVP resolved BY NAME, 16 floats == identity");
    auto utex = rec::i1_by_name.find("uTex");
    check(utex != rec::i1_by_name.end() && utex->second == 0, "uTex resolved BY NAME -> sampler unit 0");

    // uniform variants (glUniform3fv / glUniform1f / glUniform2i / glUniformMatrix3fv)
    auto col = rec::fv_by_name.find("uColor");
    check(col != rec::fv_by_name.end() && col->second.size() == 3 &&
              col->second[0] == 0.2f && col->second[1] == 0.4f && col->second[2] == 0.6f,
          "glUniform3fv uColor = {0.2,0.4,0.6} BY NAME");
    auto tm = rec::fv_by_name.find("uTime");
    check(tm != rec::fv_by_name.end() && tm->second.size() == 1 && tm->second[0] == 1.5f,
          "glUniform1f uTime = 1.5 (OP_UNIFORM_FV cols=1)");
    auto md = rec::iv_by_name.find("uMode2");
    check(md != rec::iv_by_name.end() && md->second.size() == 2 &&
              md->second[0] == 3 && md->second[1] == 7,
          "glUniform2i uMode2 = {3,7} (OP_UNIFORM_IV cols=2)");
    auto nm = rec::matN_by_name.find("uNormalMatrix");
    bool nm_ok = nm != rec::matN_by_name.end() && nm->second.size() == 9;
    if (nm_ok) {
        const float id3[9] = {1,0,0, 0,1,0, 0,0,1};
        for (int i = 0; i < 9; ++i) nm_ok = nm_ok && (nm->second[i] == id3[i]);
    }
    check(nm_ok, "glUniformMatrix3fv uNormalMatrix = identity3 (OP_UNIFORM_MATRIX_FV dim=3)");

    // cube: index-based VBO attribs
    bool vap_ok = rec::vaps_idx.size() == 2;
    for (auto &v : rec::vaps_idx) {
        if (v.index == 0) vap_ok = vap_ok && (v.size == 3 && v.offset == 0);
        else if (v.index == 1) vap_ok = vap_ok && (v.size == 2 && v.offset == 12);
        else vap_ok = false;
    }
    check(vap_ok, "cube: 2 index-based VBO attribs aPos(sz3,off0) aUV(sz2,off12)");
    check(rec::draws.size() == 1 && rec::draws[0].mode == kGL_TRIANGLES && rec::draws[0].count == 36,
          "cube: one glDrawArrays(GL_TRIANGLES, 36)");

    // mesh: glGetAttribLocation BY NAME -> NAMED ops -> host resolves real location
    bool vn_ok = rec::vaps_named.size() == 2;
    for (auto &v : rec::vaps_named) {
        if (v.name == "position") vn_ok = vn_ok && (v.size == 3 && v.offset == 0);
        else if (v.name == "normal") vn_ok = vn_ok && (v.size == 3 && v.offset == 12);
        else vn_ok = false;
    }
    check(vn_ok, "mesh: attrib-BY-NAME VAP position(sz3,off0) normal(sz3,off12)");
    bool en_ok = rec::enabled_named.size() == 2;
    bool has_pos = false, has_nrm = false;
    for (auto &nm : rec::enabled_named) { has_pos |= (nm == "position"); has_nrm |= (nm == "normal"); }
    check(en_ok && has_pos && has_nrm, "mesh: glEnableVertexAttribArray BY NAME (position, normal)");
    check(rec::draw_elems.size() == 1 && rec::draw_elems[0].mode == kGL_TRIANGLES &&
              rec::draw_elems[0].count == 6 && rec::draw_elems[0].type == kGL_UNSIGNED_SHORT &&
              rec::draw_elems[0].offset == 0,
          "mesh: one glDrawElements(GL_TRIANGLES, 6, UNSIGNED_SHORT, off0)");

    check(rec::cull_modes.size() == 1 && rec::cull_modes[0] == kGL_BACK, "glCullFace(GL_BACK)");
    check(rec::front_modes.size() == 1 && rec::front_modes[0] == kGL_CCW, "glFrontFace(GL_CCW)");

    // FBO/renderbuffer family
    check(st.framebuffers.size() == 1, "1 framebuffer mapped");
    check(st.renderbuffers.size() == 1, "1 renderbuffer mapped");
    bool fbind_ok = rec::fb_binds.size() == 2;
    if (fbind_ok) {
        bool any_nonzero = (rec::fb_binds[0] != 0) || (rec::fb_binds[1] != 0);
        bool any_zero = (rec::fb_binds[0] == 0) || (rec::fb_binds[1] == 0);
        fbind_ok = any_nonzero && any_zero;  // bound the FBO, then back to default (0)
    }
    check(fbind_ok, "glBindFramebuffer: real FBO then default(0) (vfb 0 -> host default)");
    bool ft_ok = rec::fb_texs.size() == 1 && rec::fb_texs[0].attachment == 0x8CE0 /*COLOR_ATTACHMENT0*/ &&
                 rec::fb_texs[0].textarget == 0x0DE1 /*TEXTURE_2D*/ && rec::fb_texs[0].level == 0 &&
                 rec::fb_texs[0].texture != 0;  // virtual tex resolved to a real id
    check(ft_ok, "glFramebufferTexture2D COLOR_ATTACHMENT0 <- mapped texture, level 0");
    bool rs_ok = rec::rb_stores.size() == 1 && rec::rb_stores[0].ifmt == 0x81A5 /*DEPTH_COMPONENT16*/ &&
                 rec::rb_stores[0].w == 64 && rec::rb_stores[0].h == 64;
    check(rs_ok, "glRenderbufferStorage DEPTH_COMPONENT16 64x64");
    check(rec::fb_rbs.size() == 1 && rec::fb_rbs[0].attachment == 0x8D00 /*DEPTH_ATTACHMENT*/ &&
              rec::fb_rbs[0].rbtarget == 0x8D41 /*RENDERBUFFER*/,
          "glFramebufferRenderbuffer DEPTH_ATTACHMENT <- renderbuffer");

    // completeness ops
    check(rec::buf_subs.size() == 1 && rec::buf_subs[0].target == 0x8892 /*ARRAY_BUFFER*/ &&
              rec::buf_subs[0].offset == 0 && rec::buf_subs[0].size == 16,
          "glBufferSubData ARRAY_BUFFER offset 0, 16 bytes");
    check(rec::mipmaps.size() == 1 && rec::mipmaps[0] == 0x0DE1 /*TEXTURE_2D*/,
          "glGenerateMipmap(GL_TEXTURE_2D)");
    bool ts_ok = rec::tex_subs.size() == 1 && rec::tex_subs[0].xoff == 1 && rec::tex_subs[0].yoff == 1 &&
                 rec::tex_subs[0].w == 4 && rec::tex_subs[0].h == 4 &&
                 rec::tex_subs[0].fmt == kGL_RGBA && rec::tex_subs[0].type == kGL_UNSIGNED_BYTE;
    check(ts_ok, "glTexSubImage2D 4x4 at (1,1) RGBA/UNSIGNED_BYTE (64-byte sub-upload)");

    // GLES3: VAO + instanced draws
    check(st.vertex_arrays.size() == 1, "1 vertex array (VAO) mapped");
    bool vab_ok = rec::va_binds.size() == 2;
    if (vab_ok) {
        bool nz = (rec::va_binds[0] != 0) || (rec::va_binds[1] != 0);
        bool z = (rec::va_binds[0] == 0) || (rec::va_binds[1] == 0);
        vab_ok = nz && z;  // bind the VAO, then back to default (0)
    }
    check(vab_ok, "glBindVertexArray: real VAO then default(0)");
    check(rec::draw_arr_inst.size() == 1 && rec::draw_arr_inst[0].mode == kGL_TRIANGLES &&
              rec::draw_arr_inst[0].count == 36 && rec::draw_arr_inst[0].inst == 4,
          "glDrawArraysInstanced(GL_TRIANGLES, 36, 4 instances)");
    check(rec::draw_elem_inst.size() == 1 && rec::draw_elem_inst[0].mode == kGL_TRIANGLES &&
              rec::draw_elem_inst[0].count == 6 && rec::draw_elem_inst[0].type == kGL_UNSIGNED_SHORT &&
              rec::draw_elem_inst[0].inst == 4,
          "glDrawElementsInstanced(GL_TRIANGLES, 6, UNSIGNED_SHORT, 4 instances)");
    check(rec::vad_plain.size() == 1 && rec::vad_plain[0].index == 0 && rec::vad_plain[0].divisor == 1,
          "glVertexAttribDivisor plain index 0 -> divisor 1");
    check(rec::vad_named.size() == 1 && rec::vad_named[0].name == "position" && rec::vad_named[0].divisor == 2,
          "glVertexAttribDivisor BY NAME (position) -> divisor 2");

    // ---- GLES3 core: UBO binding, sampler objects, MRT / read-buffer / invalidate ----
    static constexpr unsigned kGL_UNIFORM_BUFFER  = 0x8A11;
    static constexpr unsigned kGL_COLOR_ATTACH0   = 0x8CE0;
    static constexpr unsigned kGL_COLOR_ATTACH1   = 0x8CE1;
    static constexpr unsigned kGL_DEPTH_ATTACH    = 0x8D00;
    static constexpr unsigned kGL_FRAMEBUFFER     = 0x8D40;
    static constexpr unsigned kGL_TEX_MIN_FILTER  = 0x2801;
    static constexpr unsigned kGL_LINEAR          = 0x2601;
    // glBindBufferBase(UNIFORM_BUFFER, 0, ubo): virtual ubo id resolved to a real buffer.
    check(rec::buf_bases.size() == 1 && rec::buf_bases[0].target == kGL_UNIFORM_BUFFER &&
              rec::buf_bases[0].index == 0 && rec::buf_bases[0].buffer != 0,
          "glBindBufferBase(UNIFORM_BUFFER, 0) <- mapped UBO buffer");
    check(rec::buf_ranges.size() == 1 && rec::buf_ranges[0].target == kGL_UNIFORM_BUFFER &&
              rec::buf_ranges[0].index == 1 && rec::buf_ranges[0].buffer != 0 &&
              rec::buf_ranges[0].offset == 0 && rec::buf_ranges[0].size == 16,
          "glBindBufferRange(UNIFORM_BUFFER, 1, off0, size16) <- mapped UBO buffer");
    // glUniformBlockBinding resolved BY NAME ("Matrices") -> binding 0.
    check(rec::ubo_bindings.size() == 1 && rec::ubo_bindings[0].name == "Matrices" &&
              rec::ubo_bindings[0].binding == 0,
          "glUniformBlockBinding(\"Matrices\") -> binding 0 (resolved BY NAME, no round-trip)");
    // sampler object: bound to unit 0 (resolved to a real id), MIN_FILTER=LINEAR.
    check(rec::bind_samplers.size() == 1 && rec::bind_samplers[0].unit == 0 &&
              rec::bind_samplers[0].sampler != 0,
          "glBindSampler(unit 0) <- mapped sampler object");
    check(rec::sampler_params.size() == 1 && rec::sampler_params[0].sampler != 0 &&
              rec::sampler_params[0].pname == kGL_TEX_MIN_FILTER &&
              (unsigned)rec::sampler_params[0].param == kGL_LINEAR,
          "glSamplerParameteri(MIN_FILTER, LINEAR) <- mapped sampler object");
    // MRT draw-buffers [COLOR_ATTACHMENT0, COLOR_ATTACHMENT1].
    bool db_ok = rec::draw_buffers_lists.size() == 1 && rec::draw_buffers_lists[0].size() == 2 &&
                 rec::draw_buffers_lists[0][0] == kGL_COLOR_ATTACH0 &&
                 rec::draw_buffers_lists[0][1] == kGL_COLOR_ATTACH1;
    check(db_ok, "glDrawBuffers([COLOR_ATTACHMENT0, COLOR_ATTACHMENT1])");
    check(rec::read_buffers.size() == 1 && rec::read_buffers[0] == kGL_COLOR_ATTACH0,
          "glReadBuffer(COLOR_ATTACHMENT0)");
    bool inv_ok = rec::invalidates.size() == 1 && rec::invalidates[0].target == kGL_FRAMEBUFFER &&
                  rec::invalidates[0].attachments.size() == 1 &&
                  rec::invalidates[0].attachments[0] == kGL_DEPTH_ATTACH;
    check(inv_ok, "glInvalidateFramebuffer(FRAMEBUFFER, [DEPTH_ATTACHMENT])");

    // ---- per-fragment / raster state setters (the new wire ops) ----
    check(rec::blend_funcs.size() == 1 && rec::blend_funcs[0].a == kGL_SRC_ALPHA &&
              rec::blend_funcs[0].b == kGL_ONE_MINUS_SRC_ALPHA,
          "glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)");
    check(rec::blend_func_seps.size() == 1 && rec::blend_func_seps[0].a == kGL_ONE &&
              rec::blend_func_seps[0].b == kGL_ZERO && rec::blend_func_seps[0].c == kGL_SRC_ALPHA &&
              rec::blend_func_seps[0].d == kGL_ONE,
          "glBlendFuncSeparate(ONE, ZERO, SRC_ALPHA, ONE)");
    check(rec::blend_eqs.size() == 1 && rec::blend_eqs[0] == kGL_FUNC_ADD,
          "glBlendEquation(FUNC_ADD)");
    check(rec::blend_eq_seps.size() == 1 && rec::blend_eq_seps[0].a == kGL_FUNC_ADD &&
              rec::blend_eq_seps[0].b == kGL_FUNC_SUBTRACT,
          "glBlendEquationSeparate(FUNC_ADD, FUNC_SUBTRACT)");
    bool bc_ok = rec::blend_colors.size() == 1 && rec::blend_colors[0].size() == 4 &&
                 rec::blend_colors[0][0] == 0.25f && rec::blend_colors[0][1] == 0.5f &&
                 rec::blend_colors[0][2] == 0.75f && rec::blend_colors[0][3] == 1.0f;
    check(bc_ok, "glBlendColor(0.25, 0.5, 0.75, 1.0)");
    check(rec::color_masks.size() == 1 && rec::color_masks[0].r == GL_TRUE &&
              rec::color_masks[0].g == GL_FALSE && rec::color_masks[0].b == GL_TRUE &&
              rec::color_masks[0].a == GL_FALSE,
          "glColorMask(TRUE, FALSE, TRUE, FALSE)");
    check(rec::depth_masks.size() == 1 && rec::depth_masks[0] == GL_FALSE,
          "glDepthMask(GL_FALSE)");
    check(rec::depth_ranges.size() == 1 && rec::depth_ranges[0].a == 0.0f &&
              rec::depth_ranges[0].b == 0.5f,
          "glDepthRangef(0.0, 0.5)");
    check(rec::clear_depths.size() == 1 && rec::clear_depths[0] == 0.0f,
          "glClearDepthf(0.0)");
    check(rec::clear_stencils.size() == 1 && rec::clear_stencils[0] == 1,
          "glClearStencil(1)");
    check(rec::stencil_funcs.size() == 1 && rec::stencil_funcs[0].func == kGL_ALWAYS &&
              rec::stencil_funcs[0].ref == 1 && rec::stencil_funcs[0].mask == 0xFFu,
          "glStencilFunc(ALWAYS, 1, 0xFF)");
    check(rec::stencil_func_seps.size() == 1 && rec::stencil_func_seps[0].face == kGL_FRONT &&
              rec::stencil_func_seps[0].func == kGL_ALWAYS && rec::stencil_func_seps[0].ref == 2 &&
              rec::stencil_func_seps[0].mask == 0x0Fu,
          "glStencilFuncSeparate(FRONT, ALWAYS, 2, 0x0F)");
    check(rec::stencil_ops.size() == 1 && rec::stencil_ops[0].a == kGL_KEEP &&
              rec::stencil_ops[0].b == kGL_KEEP && rec::stencil_ops[0].c == kGL_REPLACE,
          "glStencilOp(KEEP, KEEP, REPLACE)");
    check(rec::stencil_op_seps.size() == 1 && rec::stencil_op_seps[0].a == kGL_FRONT &&
              rec::stencil_op_seps[0].b == kGL_KEEP && rec::stencil_op_seps[0].c == kGL_REPLACE &&
              rec::stencil_op_seps[0].d == kGL_KEEP,
          "glStencilOpSeparate(FRONT, KEEP, REPLACE, KEEP)");
    check(rec::stencil_masks.size() == 1 && rec::stencil_masks[0] == 0xFFu,
          "glStencilMask(0xFF)");
    check(rec::stencil_mask_seps.size() == 1 && rec::stencil_mask_seps[0].a == kGL_FRONT &&
              rec::stencil_mask_seps[0].b == 0x0Fu,
          "glStencilMaskSeparate(FRONT, 0x0F)");
    check(rec::polygon_offsets.size() == 1 && rec::polygon_offsets[0].a == 1.0f &&
              rec::polygon_offsets[0].b == 2.0f,
          "glPolygonOffset(1.0, 2.0)");
    check(rec::line_widths.size() == 1 && rec::line_widths[0] == 2.0f,
          "glLineWidth(2.0)");
    check(rec::sample_coverages.size() == 1 && rec::sample_coverages[0].value == 0.5f &&
              rec::sample_coverages[0].invert == GL_TRUE,
          "glSampleCoverage(0.5, GL_TRUE)");

    std::printf("\nALR GPU WIRE-FORMAT CHECK: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
