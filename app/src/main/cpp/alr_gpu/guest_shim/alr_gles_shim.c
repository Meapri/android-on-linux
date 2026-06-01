/* alr_gles_shim.c — the guest-side GLES2 entry points for the ALR GPU shim.
 *
 * Exported ABI of libGLESv2.so.2. Each GL call the spinning cube makes is encoded
 * into ONE wire op (alr_gles_proto.h) and appended to the SPSC ring; the host
 * GPU thread decodes it (alr_gpu_decode.hpp) and replays it as a REAL GLES2 call
 * on the device's Mali context. The wire format here is byte-for-byte identical
 * to the committed host decoder — every field's type and order below was matched
 * to alr_gpu_decode.hpp's Reader sequence.
 *
 * THE CRUX — CLIENT-SIDE VIRTUAL IDs (DESIGN §2.3), so the guest NEVER blocks on a
 * round-trip to create a GL object:
 *   - glGenBuffers/glGenTextures/glCreateShader/glCreateProgram allocate a virtual
 *     id from a monotonic per-type counter (starts at 1; 0 is reserved/"none"),
 *     return it IMMEDIATELY, and emit a create-op carrying that virtual id. The
 *     host's HostState maps virtual->real; the guest never learns the real name.
 *   - glGetUniformLocation does NOT round-trip either: it returns a small client
 *     handle = an index into a per-program NAME table. glUniform1i / glUniformMatrix4fv
 *     look the NAME back up from that handle and emit the op carrying the NAME as a
 *     blob; the host resolves the real location BY NAME at decode time.
 *
 * UNIFORM HANDLE EDGE CASES (the issue flagged mid-M3):
 *   - A handle of -1 (GL "no such uniform") makes glUniform* a NO-OP with NO emit,
 *     matching GL semantics (real GL ignores location -1). We must not encode a
 *     bogus name for it.
 *   - A valid handle VALUE of 0 is fine: the table index 0 is the first interned
 *     uniform, and the WIRE carries the NAME (not the handle), so handle 0 encodes
 *     the correct name. The only reserved/invalid sentinel is -1.
 *
 * OPTIMISTIC QUERIES: glGetError->GL_NO_ERROR (unless the shim set a sticky error,
 * e.g. an oversized upload); glGetShaderiv(COMPILE_STATUS)/glGetProgramiv(LINK_
 * STATUS)->GL_TRUE. We can't cheaply round-trip these, and the cube's shaders are
 * known-good; a real failure surfaces as wrong/blank pixels host-side, logged
 * there. Info-log queries return empty.
 *
 * glVertexAttribPointer: VBO-offset path ONLY (the `pointer` arg is treated as a
 * uintptr_t byte offset into the bound ARRAY_BUFFER, exactly as the host decoder
 * reinterprets it). Client vertex arrays are out of scope (the cube binds a VBO).
 *
 * Pure C / glibc. Public gl* entry points have default visibility; the build
 * script hides the rest. Shares the one process-global runtime + ring + virtual-ID
 * space with libEGL.so.1 (alr_shim_runtime.c is linked into both).
 */
#include "alr_khr_gles2.h"
#include "alr_shim_internal.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>   /* malloc/free (glShaderSource count>1 coalesce) */
#include <string.h>

/* The virtual-ID counters live in AlrShimState (shared with EGL). Allocation must
 * be atomic w.r.t. concurrent callers; the cube is single-threaded, but a tiny
 * mutex keeps multi-threaded guests correct. Counters start at 1 (0 reserved). */
static pthread_mutex_t g_id_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t alloc_id(uint32_t *counter) {
    pthread_mutex_lock(&g_id_lock);
    uint32_t v = (*counter)++;
    pthread_mutex_unlock(&g_id_lock);
    return v;
}

/* ============================ encode helpers ============================ *
 * Each GL call packs its args into a small POD `ctx` struct and an emit-builder
 * memcpys/encodes them in the exact field order the host Reader expects. The
 * builder runs under alr_shim_emit's scratch+ring lock. */

/* ---- state ops ---- */
struct ViewportArgs { int32_t x, y, w, h; };
static void build_viewport(AlrEncoder *e, void *p) {
    struct ViewportArgs *a = (struct ViewportArgs*)p;
    alr_enc_u8(e, ALR_OP_VIEWPORT);
    alr_enc_i32(e, a->x); alr_enc_i32(e, a->y); alr_enc_i32(e, a->w); alr_enc_i32(e, a->h);
}
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    struct ViewportArgs a = { x, y, width, height };
    alr_shim_emit(build_viewport, &a);
}

struct ClearColorArgs { float r, g, b, a; };
static void build_clearcolor(AlrEncoder *e, void *p) {
    struct ClearColorArgs *a = (struct ClearColorArgs*)p;
    alr_enc_u8(e, ALR_OP_CLEARCOLOR);
    alr_enc_f32(e, a->r); alr_enc_f32(e, a->g); alr_enc_f32(e, a->b); alr_enc_f32(e, a->a);
}
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
    struct ClearColorArgs args = { r, g, b, a };
    alr_shim_emit(build_clearcolor, &args);
}

static void build_clear(AlrEncoder *e, void *p) { (void)p; alr_enc_u8(e, ALR_OP_CLEAR); }
void glClear(GLbitfield mask) {
    /* The host clears COLOR|DEPTH unconditionally; mask is advisory and dropped to
     * keep the op zero-arg as the decoder expects. */
    (void)mask;
    alr_shim_emit(build_clear, NULL);
}

struct CapArgs { uint32_t cap; };
static void build_enable(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_ENABLE); alr_enc_u32(e, ((struct CapArgs*)p)->cap);
}
static void build_disable(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_DISABLE); alr_enc_u32(e, ((struct CapArgs*)p)->cap);
}
void glEnable(GLenum cap)  { struct CapArgs a = { cap }; alr_shim_emit(build_enable, &a); }
void glDisable(GLenum cap) { struct CapArgs a = { cap }; alr_shim_emit(build_disable, &a); }

struct DepthFuncArgs { uint32_t func; };
static void build_depthfunc(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_DEPTH_FUNC); alr_enc_u32(e, ((struct DepthFuncArgs*)p)->func);
}
void glDepthFunc(GLenum func) { struct DepthFuncArgs a = { func }; alr_shim_emit(build_depthfunc, &a); }

struct ScissorArgs { int32_t x, y, w, h; };
static void build_scissor(AlrEncoder *e, void *p) {
    struct ScissorArgs *a = (struct ScissorArgs*)p;
    alr_enc_u8(e, ALR_OP_SCISSOR);
    alr_enc_i32(e, a->x); alr_enc_i32(e, a->y); alr_enc_i32(e, a->w); alr_enc_i32(e, a->h);
}
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    struct ScissorArgs a = { x, y, width, height };
    alr_shim_emit(build_scissor, &a);
}

/* ---- shaders + program (creation = client-side virtual id) ---- */
struct CreateShaderArgs { uint32_t vid, type; };
static void build_create_shader(AlrEncoder *e, void *p) {
    struct CreateShaderArgs *a = (struct CreateShaderArgs*)p;
    alr_enc_u8(e, ALR_OP_CREATE_SHADER); alr_enc_u32(e, a->vid); alr_enc_u32(e, a->type);
}
GLuint glCreateShader(GLenum type) {
    AlrShimState *s = alr_shim();
    uint32_t vid = alloc_id(&s->next_shader);
    struct CreateShaderArgs a = { vid, (uint32_t)type };
    alr_shim_emit(build_create_shader, &a);
    return (GLuint)vid;                 /* return the virtual id immediately — NO round-trip */
}

struct ShaderSourceArgs { uint32_t vid; const char *src; uint32_t len; };
static void build_shader_source(AlrEncoder *e, void *p) {
    struct ShaderSourceArgs *a = (struct ShaderSourceArgs*)p;
    alr_enc_u8(e, ALR_OP_SHADER_SOURCE); alr_enc_u32(e, a->vid);
    alr_enc_blob(e, a->src, a->len);
}
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) {
    /* Concatenate the `count` source chunks into the shim scratch is unnecessary:
     * the cube passes count==1. To be robust we coalesce into a small local buffer
     * only when count>1; otherwise we point straight at the single chunk. */
    if (count <= 0 || !string) return;
    if (count == 1) {
        const char *src = string[0] ? string[0] : "";
        uint32_t len = (length && length[0] >= 0) ? (uint32_t)length[0] : (uint32_t)strlen(src);
        struct ShaderSourceArgs a = { (uint32_t)shader, src, len };
        alr_shim_emit(build_shader_source, &a);
        return;
    }
    /* count>1: join into a heap buffer (rare; keeps the wire a single blob). */
    size_t total = 0;
    for (GLsizei i = 0; i < count; ++i) {
        const char *s = string[i] ? string[i] : "";
        total += (length && length[i] >= 0) ? (size_t)length[i] : strlen(s);
    }
    char *joined = (char*)malloc(total + 1);
    if (!joined) { alr_shim()->gl_error = 0x0505; return; }
    size_t off = 0;
    for (GLsizei i = 0; i < count; ++i) {
        const char *s = string[i] ? string[i] : "";
        size_t n = (length && length[i] >= 0) ? (size_t)length[i] : strlen(s);
        memcpy(joined + off, s, n); off += n;
    }
    joined[off] = '\0';
    struct ShaderSourceArgs a = { (uint32_t)shader, joined, (uint32_t)off };
    alr_shim_emit(build_shader_source, &a);
    free(joined);
}

struct VidArgs { uint32_t vid; };
static void build_compile_shader(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_COMPILE_SHADER); alr_enc_u32(e, ((struct VidArgs*)p)->vid);
}
void glCompileShader(GLuint shader) {
    struct VidArgs a = { (uint32_t)shader }; alr_shim_emit(build_compile_shader, &a);
}

static void build_create_program(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_CREATE_PROGRAM); alr_enc_u32(e, ((struct VidArgs*)p)->vid);
}
GLuint glCreateProgram(void) {
    AlrShimState *s = alr_shim();
    uint32_t vid = alloc_id(&s->next_program);
    struct VidArgs a = { vid };
    alr_shim_emit(build_create_program, &a);
    return (GLuint)vid;                 /* virtual program id, returned immediately */
}

struct AttachArgs { uint32_t vprog, vshader; };
static void build_attach(AlrEncoder *e, void *p) {
    struct AttachArgs *a = (struct AttachArgs*)p;
    alr_enc_u8(e, ALR_OP_ATTACH_SHADER); alr_enc_u32(e, a->vprog); alr_enc_u32(e, a->vshader);
}
void glAttachShader(GLuint program, GLuint shader) {
    struct AttachArgs a = { (uint32_t)program, (uint32_t)shader };
    alr_shim_emit(build_attach, &a);
}

struct BindAttribArgs { uint32_t vprog, index; const char *name; };
static void build_bind_attrib(AlrEncoder *e, void *p) {
    struct BindAttribArgs *a = (struct BindAttribArgs*)p;
    alr_enc_u8(e, ALR_OP_BIND_ATTRIB_LOCATION);
    alr_enc_u32(e, a->vprog); alr_enc_u32(e, a->index);
    alr_enc_str(e, a->name);            /* blob(name): u32 len + bytes, no NUL */
}
void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) {
    if (!name) return;
    struct BindAttribArgs a = { (uint32_t)program, (uint32_t)index, name };
    alr_shim_emit(build_bind_attrib, &a);
}

static void build_link(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_LINK_PROGRAM); alr_enc_u32(e, ((struct VidArgs*)p)->vid);
}
void glLinkProgram(GLuint program) {
    /* A relink invalidates the program's uniform locations; the host re-resolves by
     * name on the next glUniform* anyway, but reset the local name table so stale
     * handles from a previous link don't leak. (The cube links once.) */
    alr_shim_program_reset((uint32_t)program);
    struct VidArgs a = { (uint32_t)program }; alr_shim_emit(build_link, &a);
}

static void build_use(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_USE_PROGRAM); alr_enc_u32(e, ((struct VidArgs*)p)->vid);
}
void glUseProgram(GLuint program) {
    struct VidArgs a = { (uint32_t)program }; alr_shim_emit(build_use, &a);
}

/* ---- buffers (VBO) ---- */
static void build_gen_buffer(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_GEN_BUFFER); alr_enc_u32(e, ((struct VidArgs*)p)->vid);
}
void glGenBuffers(GLsizei n, GLuint *buffers) {
    if (n <= 0 || !buffers) return;
    AlrShimState *s = alr_shim();
    for (GLsizei i = 0; i < n; ++i) {
        uint32_t vid = alloc_id(&s->next_buffer);
        struct VidArgs a = { vid };
        alr_shim_emit(build_gen_buffer, &a);  /* one create-op per id */
        buffers[i] = (GLuint)vid;             /* virtual id, no round-trip */
    }
}

struct BindBufferArgs { uint32_t target, vid; };
static void build_bind_buffer(AlrEncoder *e, void *p) {
    struct BindBufferArgs *a = (struct BindBufferArgs*)p;
    alr_enc_u8(e, ALR_OP_BIND_BUFFER); alr_enc_u32(e, a->target); alr_enc_u32(e, a->vid);
}
void glBindBuffer(GLenum target, GLuint buffer) {
    struct BindBufferArgs a = { (uint32_t)target, (uint32_t)buffer };
    alr_shim_emit(build_bind_buffer, &a);
}

struct BufferDataArgs { uint32_t target; const void *data; uint32_t len; uint32_t usage; };
static void build_buffer_data(AlrEncoder *e, void *p) {
    struct BufferDataArgs *a = (struct BufferDataArgs*)p;
    alr_enc_u8(e, ALR_OP_BUFFER_DATA);
    alr_enc_u32(e, a->target);
    alr_enc_blob(e, a->data, a->len);   /* blob(data) */
    alr_enc_u32(e, a->usage);           /* usage AFTER the blob (matches decoder) */
}
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    uint32_t len = (size > 0) ? (uint32_t)size : 0;
    struct BufferDataArgs a = { (uint32_t)target, data, len, (uint32_t)usage };
    alr_shim_emit(build_buffer_data, &a);
}

/* ---- vertex attrib + draw ---- */
struct IndexArgs { uint32_t index; };
static void build_enable_vaa(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_ENABLE_VAA); alr_enc_u32(e, ((struct IndexArgs*)p)->index);
}
void glEnableVertexAttribArray(GLuint index) {
    struct IndexArgs a = { (uint32_t)index }; alr_shim_emit(build_enable_vaa, &a);
}
void glDisableVertexAttribArray(GLuint index) {
    /* No DISABLE_VAA opcode in the wire protocol; the host enables exactly what the
     * draw needs and the cube never disables mid-frame. No-op (advisory). */
    (void)index;
}

struct VapArgs { uint32_t index; int32_t size; uint32_t type; uint8_t norm;
                 int32_t stride; uint32_t offset; };
static void build_vap(AlrEncoder *e, void *p) {
    struct VapArgs *a = (struct VapArgs*)p;
    alr_enc_u8(e, ALR_OP_VERTEX_ATTRIB_POINTER);
    alr_enc_u32(e, a->index);
    alr_enc_i32(e, a->size);            /* i32 size */
    alr_enc_u32(e, a->type);
    alr_enc_u8(e, a->norm);             /* u8 norm */
    alr_enc_i32(e, a->stride);          /* i32 stride */
    alr_enc_u32(e, a->offset);          /* u32 offset (VBO byte offset) */
}
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                           GLsizei stride, const void *pointer) {
    /* VBO-offset path only: `pointer` is a byte offset into the bound ARRAY_BUFFER,
     * carried as a u32 (the host reinterpret_casts it back to a void* offset). */
    struct VapArgs a = { (uint32_t)index, (int32_t)size, (uint32_t)type,
                         (uint8_t)(normalized ? 1 : 0), (int32_t)stride,
                         (uint32_t)(uintptr_t)pointer };
    alr_shim_emit(build_vap, &a);
}

struct DrawArraysArgs { uint32_t mode; int32_t first, count; };
static void build_draw_arrays(AlrEncoder *e, void *p) {
    struct DrawArraysArgs *a = (struct DrawArraysArgs*)p;
    alr_enc_u8(e, ALR_OP_DRAW_ARRAYS);
    alr_enc_u32(e, a->mode); alr_enc_i32(e, a->first); alr_enc_i32(e, a->count);
}
void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    struct DrawArraysArgs a = { (uint32_t)mode, (int32_t)first, (int32_t)count };
    alr_shim_emit(build_draw_arrays, &a);
}

/* ---- uniforms (carry the NAME; host resolves the real location) ----
 *
 * glGetUniformLocation returns a CLIENT handle (an index into the per-program
 * name table). The setters look the name back up from that handle and emit it.
 * The problem: GL's setters take only `location`, not a program, yet we need the
 * vprog at emit time (to find the name table AND to fill the op's vprog field).
 *
 * SCHEME (what we do): glGetUniformLocation packs the handle as
 *     bits[0..15] = local uniform index, bits[16..31] = vprog.
 * The cube's vprog ids are tiny (1, 2, ...) and indices are tiny, so this fits in
 * 32 bits with room to spare, and the setters recover BOTH the program and the
 * name with no global scan and no cross-program ambiguity. vprog is never 0 (ids
 * start at 1), so a real packed handle is never 0; a handle of -1 stays -1 (GL
 * "not found") and makes the setter a no-op. The WIRE carries the NAME, not the
 * handle, so the host is oblivious to this packing. */

#define ALR_UNIFORM_PACK(vprog, idx)  ((GLint)(((uint32_t)(vprog) << 16) | ((uint32_t)(idx) & 0xFFFFu)))
#define ALR_UNIFORM_VPROG(h)          ((uint32_t)(((uint32_t)(h)) >> 16))
#define ALR_UNIFORM_IDX(h)            ((int)(((uint32_t)(h)) & 0xFFFFu))

GLint glGetUniformLocation(GLuint program, const GLchar *name) {
    if (!name) return -1;
    int idx = alr_shim_uniform_intern((uint32_t)program, name);
    if (idx < 0) return -1;             /* table full / bad program -> GL "not found" */
    return ALR_UNIFORM_PACK((uint32_t)program, idx);
}

struct Uniform1iArgs { uint32_t vprog; const char *name; int32_t value; };
static void build_uniform1i(AlrEncoder *e, void *p) {
    struct Uniform1iArgs *a = (struct Uniform1iArgs*)p;
    alr_enc_u8(e, ALR_OP_UNIFORM1I);
    alr_enc_u32(e, a->vprog);
    alr_enc_str(e, a->name);            /* blob(name) */
    alr_enc_i32(e, a->value);
}
void glUniform1i(GLint location, GLint v0) {
    if (location < 0) return;           /* GL: location -1 is a silent no-op */
    uint32_t vprog = ALR_UNIFORM_VPROG(location);
    int idx = ALR_UNIFORM_IDX(location);
    const char *name = alr_shim_uniform_name(vprog, idx);
    if (!name) return;                  /* unknown handle -> no-op (defensive) */
    struct Uniform1iArgs a = { vprog, name, (int32_t)v0 };
    alr_shim_emit(build_uniform1i, &a);
}

struct UniformMat4Args { uint32_t vprog; const char *name; const float *m; };
static void build_uniform_mat4(AlrEncoder *e, void *p) {
    struct UniformMat4Args *a = (struct UniformMat4Args*)p;
    alr_enc_u8(e, ALR_OP_UNIFORM_MATRIX4FV);
    alr_enc_u32(e, a->vprog);
    alr_enc_str(e, a->name);            /* blob(name) */
    for (int i = 0; i < 16; ++i) alr_enc_f32(e, a->m[i]);   /* f32[16], column-major */
}
void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    if (location < 0 || count <= 0 || !value) return;       /* -1 handle -> no-op */
    /* The wire op carries a single mat4 (count==1, transpose==GL_FALSE on host). The
     * cube always passes count==1, transpose==GL_FALSE. If transpose is requested we
     * honor GL by transposing locally before sending (host applies GL_FALSE). */
    uint32_t vprog = ALR_UNIFORM_VPROG(location);
    int idx = ALR_UNIFORM_IDX(location);
    const char *name = alr_shim_uniform_name(vprog, idx);
    if (!name) return;
    float m[16];
    if (transpose) {
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) m[c*4+r] = value[r*4+c];
    } else {
        memcpy(m, value, sizeof(m));
    }
    struct UniformMat4Args a = { vprog, name, m };
    alr_shim_emit(build_uniform_mat4, &a);
}

/* ---- textures ---- */
static void build_gen_texture(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_GEN_TEXTURE); alr_enc_u32(e, ((struct VidArgs*)p)->vid);
}
void glGenTextures(GLsizei n, GLuint *textures) {
    if (n <= 0 || !textures) return;
    AlrShimState *s = alr_shim();
    for (GLsizei i = 0; i < n; ++i) {
        uint32_t vid = alloc_id(&s->next_texture);
        struct VidArgs a = { vid };
        alr_shim_emit(build_gen_texture, &a);
        textures[i] = (GLuint)vid;             /* virtual id, no round-trip */
    }
}

struct ActiveTexArgs { uint32_t unit; };
static void build_active_texture(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_ACTIVE_TEXTURE); alr_enc_u32(e, ((struct ActiveTexArgs*)p)->unit);
}
void glActiveTexture(GLenum texture) {
    struct ActiveTexArgs a = { (uint32_t)texture }; alr_shim_emit(build_active_texture, &a);
}

struct BindTexArgs { uint32_t target, vid; };
static void build_bind_texture(AlrEncoder *e, void *p) {
    struct BindTexArgs *a = (struct BindTexArgs*)p;
    alr_enc_u8(e, ALR_OP_BIND_TEXTURE); alr_enc_u32(e, a->target); alr_enc_u32(e, a->vid);
}
void glBindTexture(GLenum target, GLuint texture) {
    struct BindTexArgs a = { (uint32_t)target, (uint32_t)texture };
    alr_shim_emit(build_bind_texture, &a);
}

struct TexParamArgs { uint32_t target, pname; int32_t param; };
static void build_tex_parameteri(AlrEncoder *e, void *p) {
    struct TexParamArgs *a = (struct TexParamArgs*)p;
    alr_enc_u8(e, ALR_OP_TEX_PARAMETERI);
    alr_enc_u32(e, a->target); alr_enc_u32(e, a->pname); alr_enc_i32(e, a->param);
}
void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    struct TexParamArgs a = { (uint32_t)target, (uint32_t)pname, (int32_t)param };
    alr_shim_emit(build_tex_parameteri, &a);
}

struct TexImage2DArgs {
    uint32_t target; int32_t level; uint32_t ifmt; int32_t w, h;
    uint32_t fmt, type; const void *pixels; uint32_t bytes;
};
static void build_tex_image_2d(AlrEncoder *e, void *p) {
    struct TexImage2DArgs *a = (struct TexImage2DArgs*)p;
    alr_enc_u8(e, ALR_OP_TEX_IMAGE_2D);
    alr_enc_u32(e, a->target);
    alr_enc_i32(e, a->level);
    alr_enc_u32(e, a->ifmt);
    alr_enc_i32(e, a->w);
    alr_enc_i32(e, a->h);
    alr_enc_u32(e, a->fmt);
    alr_enc_u32(e, a->type);
    alr_enc_blob(e, a->pixels, a->bytes);   /* blob(pixels) LAST */
}
static uint32_t gl_type_bytes(GLenum type) {
    switch (type) {
        case GL_UNSIGNED_BYTE: case GL_BYTE: return 1;
        case GL_UNSIGNED_SHORT: case GL_SHORT: return 2;
        case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4;
        default: return 1;
    }
}
static uint32_t gl_format_components(GLenum fmt) {
    switch (fmt) {
        case GL_RGBA:            return 4;
        case GL_RGB:             return 3;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_LUMINANCE:       return 1;
        default:                 return 4;
    }
}
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                  GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) {
    (void)border;   /* GLES2 requires border==0; the host passes 0 unconditionally */
    uint32_t bytes = 0;
    if (pixels && width > 0 && height > 0) {
        /* Tightly-packed (host sets UNPACK_ALIGNMENT=1): w*h*comp*typesize bytes. */
        bytes = (uint32_t)width * (uint32_t)height *
                gl_format_components(format) * gl_type_bytes(type);
    }
    struct TexImage2DArgs a = { (uint32_t)target, (int32_t)level, (uint32_t)internalformat,
                                (int32_t)width, (int32_t)height, (uint32_t)format,
                                (uint32_t)type, pixels, bytes };
    alr_shim_emit(build_tex_image_2d, &a);
}

void glPixelStorei(GLenum pname, GLint param) {
    /* No PIXEL_STORE opcode; the host forces UNPACK_ALIGNMENT=1 before every upload
     * (see decoder OP_TEX_IMAGE_2D), which is what the cube's tight RGBA needs.
     * Accept + drop. */
    (void)pname; (void)param;
}

/* ---- deletes (no host opcodes; the host frees on teardown / leaks until then).
 * The cube deletes once at exit. We reset the local uniform table for a deleted
 * program so its handles can't be reused stale. ---- */
void glDeleteShader(GLuint shader)  { (void)shader;  /* no opcode; advisory */ }
void glDeleteProgram(GLuint program){ alr_shim_program_reset((uint32_t)program); }
void glDeleteBuffers(GLsizei n, const GLuint *buffers)   { (void)n; (void)buffers; }
void glDeleteTextures(GLsizei n, const GLuint *textures) { (void)n; (void)textures; }

/* ---- optimistic queries (no round-trip) ---- */
GLenum glGetError(void) {
    AlrShimState *s = alr_shim();
    GLenum e = (GLenum)s->gl_error;
    s->gl_error = GL_NO_ERROR;
    return e;
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    (void)shader;
    if (!params) return;
    if (pname == GL_COMPILE_STATUS)      *params = GL_TRUE;   /* optimistic */
    else if (pname == GL_INFO_LOG_LENGTH) *params = 0;
    else                                  *params = 0;
}
void glGetProgramiv(GLuint program, GLenum pname, GLint *params) {
    (void)program;
    if (!params) return;
    if (pname == GL_LINK_STATUS)         *params = GL_TRUE;   /* optimistic */
    else if (pname == GL_INFO_LOG_LENGTH) *params = 0;
    else                                  *params = 0;
}
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
    (void)shader;
    if (length) *length = 0;
    if (infoLog && bufSize > 0) infoLog[0] = '\0';
}
void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
    (void)program;
    if (length) *length = 0;
    if (infoLog && bufSize > 0) infoLog[0] = '\0';
}

const GLubyte *glGetString(GLenum name) {
    switch (name) {
        case GL_VENDOR:                   return (const GLubyte*)"Android-on-Linux (ALR)";
        /* GL_RENDERER is vendor-NEUTRAL on purpose: the real draws run on whatever
         * vendor GPU driver the host reaches via NDK libEGL/libGLESv2 (Mali, Adreno,
         * Xclipse, ...). Never hardcode a vendor here — it would be a lie on non-Mali
         * devices and could trip apps that branch on the renderer string. TODO: a ring
         * handshake can pass the host's actual GL_RENDERER through to the guest. */
        case GL_RENDERER:                 return (const GLubyte*)"ALR command-stream (host GPU passthrough)";
        case GL_VERSION:                  return (const GLubyte*)"OpenGL ES 2.0 ALR";
        case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte*)"OpenGL ES GLSL ES 1.00";
        case GL_EXTENSIONS:               return (const GLubyte*)"";
        default:                          return (const GLubyte*)"";
    }
}

/* ---- flush / finish ---- */
void glFlush(void)  { alr_shim_flush(); }          /* nudge the host; don't block */
void glFinish(void) { alr_shim_flush_and_wait(); } /* block until the host drains */
