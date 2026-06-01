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
struct TexImg { GLsizei w, h; GLenum fmt, type; };

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
void glTexImage2D(GLenum, GLint, GLint, GLsizei w, GLsizei h, GLint, GLenum fmt, GLenum type,
                  const void *) { rec::teximgs.push_back({w, h, fmt, type}); }
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
    check(st.decoded == 56, "decoded op count == 56 (34 cube + 4 uniform-var + 10 mesh + 8 fbo)");
    check(st.shaders.size() == 2, "2 shaders mapped");
    check(st.programs.size() == 1, "1 program mapped");
    check(st.buffers.size() == 2, "2 buffers mapped (vbo + ebo)");
    check(st.textures.size() == 1, "1 texture mapped");

    bool ba_ok = rec::bind_attribs.size() == 2;
    for (auto &b : rec::bind_attribs) {
        if (b.name == "aPos") ba_ok = ba_ok && (b.index == 0);
        else if (b.name == "aUV") ba_ok = ba_ok && (b.index == 1);
        else ba_ok = false;
    }
    check(ba_ok, "glBindAttribLocation: aPos->0, aUV->1 (index path intact)");

    check(has_buf(kGL_ARRAY_BUFFER, 36 * 5 * 4), "glBufferData ARRAY_BUFFER 720 bytes (vbo)");
    check(has_buf(kGL_ELEMENT_ARRAY_BUFFER, 6 * 2), "glBufferData ELEMENT_ARRAY_BUFFER 12 bytes (ebo)");

    bool tex_ok = rec::teximgs.size() == 1 && rec::teximgs[0].w == 8 && rec::teximgs[0].h == 8 &&
                  rec::teximgs[0].fmt == kGL_RGBA && rec::teximgs[0].type == kGL_UNSIGNED_BYTE;
    check(tex_ok, "glTexImage2D 8x8 RGBA/UNSIGNED_BYTE (256-byte upload)");
    check(rec::unpack_align1, "host forced UNPACK_ALIGNMENT=1 before upload");

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

    std::printf("\nALR GPU WIRE-FORMAT CHECK: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
