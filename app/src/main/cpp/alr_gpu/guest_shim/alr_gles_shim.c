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

/* Client handle packing shared by glGetUniformLocation AND glGetAttribLocation:
 *   bits[16..31] = vprog (>= 1), bits[0..15] = per-program name-table index.
 * vprog >= 1 makes a real packed handle always >= 0x10000, so it never collides with
 * a small literal attribute index from glBindAttribLocation (0..15) — that lets
 * glEnableVertexAttribArray / glVertexAttribPointer tell the two apart (see below).
 * A handle of -1 stays -1 (GL "not found"). The WIRE carries the NAME, not the handle. */
#define ALR_UNIFORM_PACK(vprog, idx)  ((GLint)(((uint32_t)(vprog) << 16) | ((uint32_t)(idx) & 0xFFFFu)))
#define ALR_UNIFORM_VPROG(h)          ((uint32_t)(((uint32_t)(h)) >> 16))
#define ALR_UNIFORM_IDX(h)            ((int)(((uint32_t)(h)) & 0xFFFFu))

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

struct ModeArgs { uint32_t mode; };
static void build_cull_face(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_CULL_FACE); alr_enc_u32(e, ((struct ModeArgs*)p)->mode);
}
static void build_front_face(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_FRONT_FACE); alr_enc_u32(e, ((struct ModeArgs*)p)->mode);
}
void glCullFace(GLenum mode)  { struct ModeArgs a = { (uint32_t)mode }; alr_shim_emit(build_cull_face, &a); }
void glFrontFace(GLenum mode) { struct ModeArgs a = { (uint32_t)mode }; alr_shim_emit(build_front_face, &a); }

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
        alr_shim_shader_set_srclen((uint32_t)shader, len);  /* for glGetShaderiv(GL_SHADER_SOURCE_LENGTH) */
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
    alr_shim_shader_set_srclen((uint32_t)shader, (uint32_t)off);  /* for glGetShaderiv(GL_SHADER_SOURCE_LENGTH) */
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

struct BufferSubDataArgs { uint32_t target, offset; const void *data; uint32_t len; };
static void build_buffer_subdata(AlrEncoder *e, void *p) {
    struct BufferSubDataArgs *a = (struct BufferSubDataArgs*)p;
    alr_enc_u8(e, ALR_OP_BUFFER_SUBDATA);
    alr_enc_u32(e, a->target);
    alr_enc_u32(e, a->offset);
    alr_enc_blob(e, a->data, a->len);
}
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
    uint32_t len = (size > 0) ? (uint32_t)size : 0;
    struct BufferSubDataArgs a = { (uint32_t)target, (uint32_t)offset, data, len };
    alr_shim_emit(build_buffer_subdata, &a);
}

/* ---- vertex attrib + draw ---- */
struct IndexArgs { uint32_t index; };
static void build_enable_vaa(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_ENABLE_VAA); alr_enc_u32(e, ((struct IndexArgs*)p)->index);
}
struct EnableVaaNamedArgs { uint32_t vprog; const char *name; };
static void build_enable_vaa_named(AlrEncoder *e, void *p) {
    struct EnableVaaNamedArgs *a = (struct EnableVaaNamedArgs*)p;
    alr_enc_u8(e, ALR_OP_ENABLE_VAA_NAMED);
    alr_enc_u32(e, a->vprog);
    alr_enc_str(e, a->name);
}
void glEnableVertexAttribArray(GLuint index) {
    /* A packed glGetAttribLocation handle (high 16 bits set) -> emit BY NAME so the
     * host resolves the real location; a small literal index (glBindAttribLocation
     * path, e.g. the cube) -> the existing index-based op, unchanged. */
    if ((index >> 16) != 0) {
        uint32_t vprog = ALR_UNIFORM_VPROG(index);
        const char *name = alr_shim_attrib_name(vprog, ALR_UNIFORM_IDX(index));
        if (name) {
            struct EnableVaaNamedArgs a = { vprog, name };
            alr_shim_emit(build_enable_vaa_named, &a);
            return;
        }
        /* unknown handle: fall through to the index path (defensive) */
    }
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
struct VapNamedArgs { uint32_t vprog; const char *name; int32_t size; uint32_t type;
                      uint8_t norm; int32_t stride; uint32_t offset; };
static void build_vap_named(AlrEncoder *e, void *p) {
    struct VapNamedArgs *a = (struct VapNamedArgs*)p;
    alr_enc_u8(e, ALR_OP_VERTEX_ATTRIB_POINTER_NAMED);
    alr_enc_u32(e, a->vprog);
    alr_enc_str(e, a->name);            /* blob(name) — host resolves the real location */
    alr_enc_i32(e, a->size);
    alr_enc_u32(e, a->type);
    alr_enc_u8(e, a->norm);
    alr_enc_i32(e, a->stride);
    alr_enc_u32(e, a->offset);          /* VBO byte offset */
}
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                           GLsizei stride, const void *pointer) {
    /* VBO-offset path only: `pointer` is a byte offset into the bound ARRAY_BUFFER,
     * carried as a u32 (the host reinterpret_casts it back to a void* offset). A packed
     * glGetAttribLocation handle goes through the NAMED op (host resolves by name); a
     * literal index (glBindAttribLocation path) uses the existing index op. */
    if ((index >> 16) != 0) {
        uint32_t vprog = ALR_UNIFORM_VPROG(index);
        const char *name = alr_shim_attrib_name(vprog, ALR_UNIFORM_IDX(index));
        if (name) {
            struct VapNamedArgs a = { vprog, name, (int32_t)size, (uint32_t)type,
                                      (uint8_t)(normalized ? 1 : 0), (int32_t)stride,
                                      (uint32_t)(uintptr_t)pointer };
            alr_shim_emit(build_vap_named, &a);
            return;
        }
    }
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

struct DrawElementsArgs { uint32_t mode; int32_t count; uint32_t type; uint32_t offset; };
static void build_draw_elements(AlrEncoder *e, void *p) {
    struct DrawElementsArgs *a = (struct DrawElementsArgs*)p;
    alr_enc_u8(e, ALR_OP_DRAW_ELEMENTS);
    alr_enc_u32(e, a->mode); alr_enc_i32(e, a->count);
    alr_enc_u32(e, a->type); alr_enc_u32(e, a->offset);
}
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    /* VBO-offset path: `indices` is a byte offset into the bound ELEMENT_ARRAY_BUFFER
     * (carried as a u32). Client-side index arrays are out of scope, same as the
     * ARRAY_BUFFER vertex path. */
    struct DrawElementsArgs a = { (uint32_t)mode, (int32_t)count, (uint32_t)type,
                                  (uint32_t)(uintptr_t)indices };
    alr_shim_emit(build_draw_elements, &a);
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
 * handle, so the host is oblivious to this packing. (ALR_UNIFORM_PACK/VPROG/IDX are
 * defined once near the top of this file; glGetAttribLocation reuses the same scheme.) */

GLint glGetUniformLocation(GLuint program, const GLchar *name) {
    if (!name) return -1;
    int idx = alr_shim_uniform_intern((uint32_t)program, name);
    if (idx < 0) return -1;             /* table full / bad program -> GL "not found" */
    return ALR_UNIFORM_PACK((uint32_t)program, idx);
}

/* glGetAttribLocation — SAME by-name handle scheme as uniforms (no round-trip): intern
 * the attribute name into the per-program attrib table and return the packed handle.
 * glEnableVertexAttribArray / glVertexAttribPointer recognize a packed handle (high
 * 16 bits != 0, i.e. value >= 0x10000 since vprog >= 1) and emit the NAMED wire ops,
 * which carry the NAME so the host resolves the real location via the real
 * glGetAttribLocation at decode time. A small literal index from glBindAttribLocation
 * (0..15) is NOT a packed handle and still takes the index-based ops. */
GLint glGetAttribLocation(GLuint program, const GLchar *name) {
    if (!name) return -1;
    int idx = alr_shim_attrib_intern((uint32_t)program, name);
    if (idx < 0) return -1;
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

/* ---- generalized uniform setters (glUniform{1..4}f[v] / {2..4}i + {1..4}iv /
 * Matrix{2,3}fv). SAME by-name handle scheme as glUniform1i/glUniformMatrix4fv: the
 * location is a packed (vprog<<16 | idx) handle from glGetUniformLocation; recover the
 * program + name and emit the NAME + values. location -1 is a silent no-op. ---- */
struct UniformVecFArgs { uint32_t vprog; const char *name; uint8_t cols; uint32_t count; const float *v; };
static void build_uniform_fv(AlrEncoder *e, void *p) {
    struct UniformVecFArgs *a = (struct UniformVecFArgs*)p;
    alr_enc_u8(e, ALR_OP_UNIFORM_FV);
    alr_enc_u32(e, a->vprog);
    alr_enc_str(e, a->name);
    alr_enc_u8(e, a->cols);
    alr_enc_u32(e, a->count);
    uint32_t n = (uint32_t)a->cols * a->count;
    for (uint32_t i = 0; i < n; ++i) alr_enc_f32(e, a->v[i]);
}
static void emit_uniform_fv(GLint location, uint8_t cols, GLsizei count, const GLfloat *v) {
    if (location < 0 || !v || count <= 0) return;
    uint32_t vprog = ALR_UNIFORM_VPROG(location);
    const char *name = alr_shim_uniform_name(vprog, ALR_UNIFORM_IDX(location));
    if (!name) return;
    struct UniformVecFArgs a = { vprog, name, cols, (uint32_t)count, v };
    alr_shim_emit(build_uniform_fv, &a);
}
void glUniform1f(GLint loc, GLfloat x) { GLfloat v[1] = {x}; emit_uniform_fv(loc, 1, 1, v); }
void glUniform2f(GLint loc, GLfloat x, GLfloat y) { GLfloat v[2] = {x,y}; emit_uniform_fv(loc, 2, 1, v); }
void glUniform3f(GLint loc, GLfloat x, GLfloat y, GLfloat z) { GLfloat v[3] = {x,y,z}; emit_uniform_fv(loc, 3, 1, v); }
void glUniform4f(GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { GLfloat v[4] = {x,y,z,w}; emit_uniform_fv(loc, 4, 1, v); }
void glUniform1fv(GLint loc, GLsizei count, const GLfloat *v) { emit_uniform_fv(loc, 1, count, v); }
void glUniform2fv(GLint loc, GLsizei count, const GLfloat *v) { emit_uniform_fv(loc, 2, count, v); }
void glUniform3fv(GLint loc, GLsizei count, const GLfloat *v) { emit_uniform_fv(loc, 3, count, v); }
void glUniform4fv(GLint loc, GLsizei count, const GLfloat *v) { emit_uniform_fv(loc, 4, count, v); }

struct UniformVecIArgs { uint32_t vprog; const char *name; uint8_t cols; uint32_t count; const int32_t *v; };
static void build_uniform_iv(AlrEncoder *e, void *p) {
    struct UniformVecIArgs *a = (struct UniformVecIArgs*)p;
    alr_enc_u8(e, ALR_OP_UNIFORM_IV);
    alr_enc_u32(e, a->vprog);
    alr_enc_str(e, a->name);
    alr_enc_u8(e, a->cols);
    alr_enc_u32(e, a->count);
    uint32_t n = (uint32_t)a->cols * a->count;
    for (uint32_t i = 0; i < n; ++i) alr_enc_i32(e, a->v[i]);
}
static void emit_uniform_iv(GLint location, uint8_t cols, GLsizei count, const GLint *v) {
    if (location < 0 || !v || count <= 0) return;
    uint32_t vprog = ALR_UNIFORM_VPROG(location);
    const char *name = alr_shim_uniform_name(vprog, ALR_UNIFORM_IDX(location));
    if (!name) return;
    struct UniformVecIArgs a = { vprog, name, cols, (uint32_t)count, (const int32_t*)v };
    alr_shim_emit(build_uniform_iv, &a);
}
/* glUniform1i keeps its dedicated op (sampler unit path); 2i/3i/4i + *iv use OP_UNIFORM_IV. */
void glUniform2i(GLint loc, GLint x, GLint y) { GLint v[2] = {x,y}; emit_uniform_iv(loc, 2, 1, v); }
void glUniform3i(GLint loc, GLint x, GLint y, GLint z) { GLint v[3] = {x,y,z}; emit_uniform_iv(loc, 3, 1, v); }
void glUniform4i(GLint loc, GLint x, GLint y, GLint z, GLint w) { GLint v[4] = {x,y,z,w}; emit_uniform_iv(loc, 4, 1, v); }
void glUniform1iv(GLint loc, GLsizei count, const GLint *v) { emit_uniform_iv(loc, 1, count, v); }
void glUniform2iv(GLint loc, GLsizei count, const GLint *v) { emit_uniform_iv(loc, 2, count, v); }
void glUniform3iv(GLint loc, GLsizei count, const GLint *v) { emit_uniform_iv(loc, 3, count, v); }
void glUniform4iv(GLint loc, GLsizei count, const GLint *v) { emit_uniform_iv(loc, 4, count, v); }

struct UniformMatArgs { uint32_t vprog; const char *name; uint8_t dim; uint32_t count; const float *m; };
static void build_uniform_matrix_fv(AlrEncoder *e, void *p) {
    struct UniformMatArgs *a = (struct UniformMatArgs*)p;
    alr_enc_u8(e, ALR_OP_UNIFORM_MATRIX_FV);
    alr_enc_u32(e, a->vprog);
    alr_enc_str(e, a->name);
    alr_enc_u8(e, a->dim);
    alr_enc_u32(e, a->count);
    uint32_t n = (uint32_t)a->dim * a->dim * a->count;
    for (uint32_t i = 0; i < n; ++i) alr_enc_f32(e, a->m[i]);
}
static void emit_uniform_matrix_fv(GLint location, uint8_t dim, GLsizei count,
                                   GLboolean transpose, const GLfloat *m) {
    if (location < 0 || !m || count <= 0) return;
    uint32_t vprog = ALR_UNIFORM_VPROG(location);
    const char *name = alr_shim_uniform_name(vprog, ALR_UNIFORM_IDX(location));
    if (!name) return;
    /* Host applies GL_FALSE. glmark2 passes GL_FALSE; honor GL_TRUE for the common
     * count==1 case by transposing locally (dim<=3 here — mat4 has its own op). */
    if (transpose && count == 1) {
        float t[9];
        for (int rr = 0; rr < dim; ++rr)
            for (int cc = 0; cc < dim; ++cc) t[cc*dim+rr] = m[rr*dim+cc];
        struct UniformMatArgs a = { vprog, name, dim, 1, t };
        alr_shim_emit(build_uniform_matrix_fv, &a);
        return;
    }
    struct UniformMatArgs a = { vprog, name, dim, (uint32_t)count, m };
    alr_shim_emit(build_uniform_matrix_fv, &a);
}
void glUniformMatrix2fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v) {
    emit_uniform_matrix_fv(loc, 2, count, transpose, v);
}
void glUniformMatrix3fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v) {
    emit_uniform_matrix_fv(loc, 3, count, transpose, v);
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

struct GenMipmapArgs { uint32_t target; };
static void build_generate_mipmap(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_GENERATE_MIPMAP); alr_enc_u32(e, ((struct GenMipmapArgs*)p)->target);
}
void glGenerateMipmap(GLenum target) {
    struct GenMipmapArgs a = { (uint32_t)target }; alr_shim_emit(build_generate_mipmap, &a);
}

struct TexSubImage2DArgs {
    uint32_t target; int32_t level, xoff, yoff, w, h; uint32_t fmt, type;
    const void *pixels; uint32_t bytes;
};
static void build_tex_subimage_2d(AlrEncoder *e, void *p) {
    struct TexSubImage2DArgs *a = (struct TexSubImage2DArgs*)p;
    alr_enc_u8(e, ALR_OP_TEX_SUBIMAGE_2D);
    alr_enc_u32(e, a->target);
    alr_enc_i32(e, a->level);
    alr_enc_i32(e, a->xoff);
    alr_enc_i32(e, a->yoff);
    alr_enc_i32(e, a->w);
    alr_enc_i32(e, a->h);
    alr_enc_u32(e, a->fmt);
    alr_enc_u32(e, a->type);
    alr_enc_blob(e, a->pixels, a->bytes);
}
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                     GLsizei height, GLenum format, GLenum type, const void *pixels) {
    uint32_t bytes = 0;
    if (pixels && width > 0 && height > 0) {
        bytes = (uint32_t)width * (uint32_t)height *
                gl_format_components(format) * gl_type_bytes(type);  /* tight (host UNPACK=1) */
    }
    struct TexSubImage2DArgs a = { (uint32_t)target, (int32_t)level, (int32_t)xoffset,
                                   (int32_t)yoffset, (int32_t)width, (int32_t)height,
                                   (uint32_t)format, (uint32_t)type, pixels, bytes };
    alr_shim_emit(build_tex_subimage_2d, &a);
}

/* ---- deletes (no host opcodes; the host frees on teardown / leaks until then).
 * The cube deletes once at exit. We reset the local uniform table for a deleted
 * program so its handles can't be reused stale. ---- */
void glDeleteShader(GLuint shader)  { (void)shader;  /* no opcode; advisory */ }
void glDeleteProgram(GLuint program){ alr_shim_program_reset((uint32_t)program); }
void glDeleteBuffers(GLsizei n, const GLuint *buffers)   { (void)n; (void)buffers; }
void glDeleteTextures(GLsizei n, const GLuint *textures) { (void)n; (void)textures; }

/* ---- framebuffer / renderbuffer objects (render-to-texture). Virtual ids via new
 * counters (gen returns immediately, no round-trip); the host maps virtual->real. A
 * framebuffer id of 0 stays 0 on the wire — the host maps it to its default (AHB)
 * target. glCheckFramebufferStatus is optimistic (no round-trip): returns COMPLETE. ---- */
static void build_gen_framebuffer(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_GEN_FRAMEBUFFER); alr_enc_u32(e, ((struct VidArgs*)p)->vid);
}
void glGenFramebuffers(GLsizei n, GLuint *framebuffers) {
    if (n <= 0 || !framebuffers) return;
    AlrShimState *s = alr_shim();
    for (GLsizei i = 0; i < n; ++i) {
        uint32_t vid = alloc_id(&s->next_framebuffer);
        struct VidArgs a = { vid };
        alr_shim_emit(build_gen_framebuffer, &a);
        framebuffers[i] = (GLuint)vid;
    }
}
struct BindFbArgs { uint32_t target, vid; };
static void build_bind_framebuffer(AlrEncoder *e, void *p) {
    struct BindFbArgs *a = (struct BindFbArgs*)p;
    alr_enc_u8(e, ALR_OP_BIND_FRAMEBUFFER); alr_enc_u32(e, a->target); alr_enc_u32(e, a->vid);
}
void glBindFramebuffer(GLenum target, GLuint framebuffer) {
    struct BindFbArgs a = { (uint32_t)target, (uint32_t)framebuffer };  /* 0 stays 0 (host default) */
    alr_shim_emit(build_bind_framebuffer, &a);
}
struct FbTex2DArgs { uint32_t target, attachment, textarget, vtex; int32_t level; };
static void build_framebuffer_texture2d(AlrEncoder *e, void *p) {
    struct FbTex2DArgs *a = (struct FbTex2DArgs*)p;
    alr_enc_u8(e, ALR_OP_FRAMEBUFFER_TEXTURE2D);
    alr_enc_u32(e, a->target); alr_enc_u32(e, a->attachment);
    alr_enc_u32(e, a->textarget); alr_enc_u32(e, a->vtex); alr_enc_i32(e, a->level);
}
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                            GLuint texture, GLint level) {
    struct FbTex2DArgs a = { (uint32_t)target, (uint32_t)attachment, (uint32_t)textarget,
                             (uint32_t)texture, (int32_t)level };
    alr_shim_emit(build_framebuffer_texture2d, &a);
}
static void build_gen_renderbuffer(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_GEN_RENDERBUFFER); alr_enc_u32(e, ((struct VidArgs*)p)->vid);
}
void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers) {
    if (n <= 0 || !renderbuffers) return;
    AlrShimState *s = alr_shim();
    for (GLsizei i = 0; i < n; ++i) {
        uint32_t vid = alloc_id(&s->next_renderbuffer);
        struct VidArgs a = { vid };
        alr_shim_emit(build_gen_renderbuffer, &a);
        renderbuffers[i] = (GLuint)vid;
    }
}
struct BindRbArgs { uint32_t target, vid; };
static void build_bind_renderbuffer(AlrEncoder *e, void *p) {
    struct BindRbArgs *a = (struct BindRbArgs*)p;
    alr_enc_u8(e, ALR_OP_BIND_RENDERBUFFER); alr_enc_u32(e, a->target); alr_enc_u32(e, a->vid);
}
void glBindRenderbuffer(GLenum target, GLuint renderbuffer) {
    struct BindRbArgs a = { (uint32_t)target, (uint32_t)renderbuffer };
    alr_shim_emit(build_bind_renderbuffer, &a);
}
struct RbStorageArgs { uint32_t target, ifmt; int32_t w, h; };
static void build_renderbuffer_storage(AlrEncoder *e, void *p) {
    struct RbStorageArgs *a = (struct RbStorageArgs*)p;
    alr_enc_u8(e, ALR_OP_RENDERBUFFER_STORAGE);
    alr_enc_u32(e, a->target); alr_enc_u32(e, a->ifmt); alr_enc_i32(e, a->w); alr_enc_i32(e, a->h);
}
void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    struct RbStorageArgs a = { (uint32_t)target, (uint32_t)internalformat, (int32_t)width, (int32_t)height };
    alr_shim_emit(build_renderbuffer_storage, &a);
}
struct FbRbArgs { uint32_t target, attachment, rbtarget, vrb; };
static void build_framebuffer_renderbuffer(AlrEncoder *e, void *p) {
    struct FbRbArgs *a = (struct FbRbArgs*)p;
    alr_enc_u8(e, ALR_OP_FRAMEBUFFER_RENDERBUFFER);
    alr_enc_u32(e, a->target); alr_enc_u32(e, a->attachment);
    alr_enc_u32(e, a->rbtarget); alr_enc_u32(e, a->vrb);
}
void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                               GLuint renderbuffer) {
    struct FbRbArgs a = { (uint32_t)target, (uint32_t)attachment, (uint32_t)renderbuffertarget,
                          (uint32_t)renderbuffer };
    alr_shim_emit(build_framebuffer_renderbuffer, &a);
}
/* Optimistic (no round-trip): assume the host FBO is complete. A real incomplete FBO
 * surfaces host-side (logged) as a failed draw, matching the glGetError/compile-status
 * optimism elsewhere. GL_FRAMEBUFFER_COMPLETE = 0x8CD5. */
GLenum glCheckFramebufferStatus(GLenum target) { (void)target; return 0x8CD5; }
void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers)   { (void)n; (void)framebuffers; }
void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers) { (void)n; (void)renderbuffers; }

/* ---- GLES3: vertex array objects + instanced draws. VAOs use virtual ids (gen
 * returns immediately; host maps virtual->real); id 0 stays 0 (default VAO). The host
 * runs these on the GLES3 context GpuExecutorService requests. ---- */
static void build_gen_vertex_array(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_GEN_VERTEX_ARRAY); alr_enc_u32(e, ((struct VidArgs*)p)->vid);
}
void glGenVertexArrays(GLsizei n, GLuint *arrays) {
    if (n <= 0 || !arrays) return;
    AlrShimState *s = alr_shim();
    for (GLsizei i = 0; i < n; ++i) {
        uint32_t vid = alloc_id(&s->next_vertex_array);
        struct VidArgs a = { vid };
        alr_shim_emit(build_gen_vertex_array, &a);
        arrays[i] = (GLuint)vid;
    }
}
struct BindVaArgs { uint32_t vid; };
static void build_bind_vertex_array(AlrEncoder *e, void *p) {
    alr_enc_u8(e, ALR_OP_BIND_VERTEX_ARRAY); alr_enc_u32(e, ((struct BindVaArgs*)p)->vid);
}
void glBindVertexArray(GLuint array) {
    struct BindVaArgs a = { (uint32_t)array };  /* 0 stays 0 (host default VAO) */
    alr_shim_emit(build_bind_vertex_array, &a);
}
void glDeleteVertexArrays(GLsizei n, const GLuint *arrays) { (void)n; (void)arrays; }

struct DrawArraysInstArgs { uint32_t mode; int32_t first, count, inst; };
static void build_draw_arrays_instanced(AlrEncoder *e, void *p) {
    struct DrawArraysInstArgs *a = (struct DrawArraysInstArgs*)p;
    alr_enc_u8(e, ALR_OP_DRAW_ARRAYS_INSTANCED);
    alr_enc_u32(e, a->mode); alr_enc_i32(e, a->first);
    alr_enc_i32(e, a->count); alr_enc_i32(e, a->inst);
}
void glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
    struct DrawArraysInstArgs a = { (uint32_t)mode, (int32_t)first, (int32_t)count,
                                    (int32_t)instancecount };
    alr_shim_emit(build_draw_arrays_instanced, &a);
}
struct DrawElemsInstArgs { uint32_t mode; int32_t count; uint32_t type, offset; int32_t inst; };
static void build_draw_elements_instanced(AlrEncoder *e, void *p) {
    struct DrawElemsInstArgs *a = (struct DrawElemsInstArgs*)p;
    alr_enc_u8(e, ALR_OP_DRAW_ELEMENTS_INSTANCED);
    alr_enc_u32(e, a->mode); alr_enc_i32(e, a->count);
    alr_enc_u32(e, a->type); alr_enc_u32(e, a->offset); alr_enc_i32(e, a->inst);
}
void glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices,
                             GLsizei instancecount) {
    struct DrawElemsInstArgs a = { (uint32_t)mode, (int32_t)count, (uint32_t)type,
                                   (uint32_t)(uintptr_t)indices, (int32_t)instancecount };
    alr_shim_emit(build_draw_elements_instanced, &a);
}

/* per-instance attribute divisor — same packed-handle by-name scheme as the VAA/VAP
 * path: a glGetAttribLocation handle (high 16 bits set) carries the NAME; a literal
 * index uses the plain op. */
struct VadArgs { uint32_t index, divisor; };
static void build_vad(AlrEncoder *e, void *p) {
    struct VadArgs *a = (struct VadArgs*)p;
    alr_enc_u8(e, ALR_OP_VERTEX_ATTRIB_DIVISOR); alr_enc_u32(e, a->index); alr_enc_u32(e, a->divisor);
}
struct VadNamedArgs { uint32_t vprog; const char *name; uint32_t divisor; };
static void build_vad_named(AlrEncoder *e, void *p) {
    struct VadNamedArgs *a = (struct VadNamedArgs*)p;
    alr_enc_u8(e, ALR_OP_VERTEX_ATTRIB_DIVISOR_NAMED);
    alr_enc_u32(e, a->vprog); alr_enc_str(e, a->name); alr_enc_u32(e, a->divisor);
}
void glVertexAttribDivisor(GLuint index, GLuint divisor) {
    if ((index >> 16) != 0) {
        uint32_t vprog = ALR_UNIFORM_VPROG(index);
        const char *name = alr_shim_attrib_name(vprog, ALR_UNIFORM_IDX(index));
        if (name) {
            struct VadNamedArgs a = { vprog, name, (uint32_t)divisor };
            alr_shim_emit(build_vad_named, &a);
            return;
        }
    }
    struct VadArgs a = { (uint32_t)index, (uint32_t)divisor };
    alr_shim_emit(build_vad, &a);
}

/* ===========================================================================
 * GLES2 dispatch-surface completion. glmark2 builds a FULL gl* dispatch table via
 * dlsym; a name the shim doesn't export = a NULL slot that crashes if CALLED. These
 * define every remaining gl* glmark2 references with semantics that are CORRECT for
 * the opaque `build` first-light scene (host GL defaults match). State setters that a
 * blend/stencil/2d scene would need are accepted-but-dropped here; promoting them to
 * real wire ops is a per-scene follow-up. Queries are optimistic (no round-trip).
 * Exact GLES2 ABI signatures (glmark2 calls through the real prototype).
 * =========================================================================== */

/* --- state setters: accepted, host GL defaults are correct for the build scene.
 *     (blend/stencil/color+depth mask off the default path -> wire op follow-up.) --- */
void glBlendFunc(GLenum a, GLenum b) { (void)a; (void)b; }
void glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { (void)a;(void)b;(void)c;(void)d; }
void glBlendEquation(GLenum a) { (void)a; }
void glBlendEquationSeparate(GLenum a, GLenum b) { (void)a; (void)b; }
void glBlendColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { (void)r;(void)g;(void)b;(void)a; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { (void)r;(void)g;(void)b;(void)a; }
void glDepthMask(GLboolean f) { (void)f; }
void glDepthRangef(GLclampf n, GLclampf f) { (void)n; (void)f; }
void glClearDepthf(GLclampf d) { (void)d; }   /* host clears DEPTH to its default (1.0) */
void glClearStencil(GLint s) { (void)s; }
void glStencilFunc(GLenum a, GLint b, GLuint c) { (void)a;(void)b;(void)c; }
void glStencilFuncSeparate(GLenum a, GLenum b, GLint c, GLuint d) { (void)a;(void)b;(void)c;(void)d; }
void glStencilOp(GLenum a, GLenum b, GLenum c) { (void)a;(void)b;(void)c; }
void glStencilOpSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { (void)a;(void)b;(void)c;(void)d; }
void glStencilMask(GLuint m) { (void)m; }
void glStencilMaskSeparate(GLenum a, GLuint m) { (void)a; (void)m; }
void glHint(GLenum a, GLenum b) { (void)a; (void)b; }
void glLineWidth(GLfloat w) { (void)w; }
void glPolygonOffset(GLfloat a, GLfloat b) { (void)a; (void)b; }
void glSampleCoverage(GLclampf v, GLboolean i) { (void)v; (void)i; }
void glDetachShader(GLuint p, GLuint s) { (void)p; (void)s; }
void glReleaseShaderCompiler(void) {}
void glValidateProgram(GLuint p) { (void)p; }
void glShaderBinary(GLsizei n, const GLuint *sh, GLenum fmt, const void *bin, GLsizei len) {
    (void)n; (void)sh; (void)fmt; (void)bin; (void)len;  /* no online compiler binary path */
}

/* --- constant generic vertex attributes: the build scene's attribs are all array-backed
 *     (VBO), so a constant attrib is never read; accept + drop. --- */
void glVertexAttrib1f(GLuint i, GLfloat x) { (void)i; (void)x; }
void glVertexAttrib2f(GLuint i, GLfloat x, GLfloat y) { (void)i;(void)x;(void)y; }
void glVertexAttrib3f(GLuint i, GLfloat x, GLfloat y, GLfloat z) { (void)i;(void)x;(void)y;(void)z; }
void glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { (void)i;(void)x;(void)y;(void)z;(void)w; }
void glVertexAttrib1fv(GLuint i, const GLfloat *v) { (void)i; (void)v; }
void glVertexAttrib2fv(GLuint i, const GLfloat *v) { (void)i; (void)v; }
void glVertexAttrib3fv(GLuint i, const GLfloat *v) { (void)i; (void)v; }
void glVertexAttrib4fv(GLuint i, const GLfloat *v) { (void)i; (void)v; }

/* --- glTexParameterf/fv/iv: route to the existing integer tex-param wire op. --- */
void glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    glTexParameteri(target, pname, (GLint)param);
}
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) {
    if (params) glTexParameteri(target, pname, (GLint)params[0]);
}
void glTexParameteriv(GLenum target, GLenum pname, const GLint *params) {
    if (params) glTexParameteri(target, pname, params[0]);
}

/* --- compressed / copy textures: the build scene uses none; accept + drop (a real
 *     wire op is a follow-up for BCn/copy-using scenes). --- */
void glCompressedTexImage2D(GLenum t, GLint l, GLenum f, GLsizei w, GLsizei h, GLint b, GLsizei s, const void *d) {
    (void)t;(void)l;(void)f;(void)w;(void)h;(void)b;(void)s;(void)d;
}
void glCompressedTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum f, GLsizei s, const void *d) {
    (void)t;(void)l;(void)xo;(void)yo;(void)w;(void)h;(void)f;(void)s;(void)d;
}
void glCopyTexImage2D(GLenum t, GLint l, GLenum f, GLint x, GLint y, GLsizei w, GLsizei h, GLint b) {
    (void)t;(void)l;(void)f;(void)x;(void)y;(void)w;(void)h;(void)b;
}
void glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {
    (void)t;(void)l;(void)xo;(void)yo;(void)x;(void)y;(void)w;(void)h;
}

/* --- optimistic query stubs (no round-trip). Return sane defaults so glmark2's
 *     canvas/scene init doesn't trip; real values would need a host handshake. --- */
void glGetBooleanv(GLenum pname, GLboolean *params) { (void)pname; if (params) params[0] = GL_FALSE; }
void glGetFloatv(GLenum pname, GLfloat *params) { (void)pname; if (params) params[0] = 0.0f; }
void glGetShaderPrecisionFormat(GLenum st, GLenum pt, GLint *range, GLint *precision) {
    (void)st; (void)pt;                      /* advertise IEEE single high-float precision */
    if (range) { range[0] = 127; range[1] = 127; }
    if (precision) *precision = 23;
}
void glGetActiveAttrib(GLuint p, GLuint i, GLsizei buf, GLsizei *len, GLint *size, GLenum *type, GLchar *name) {
    (void)p; (void)i; if (len) *len = 0; if (size) *size = 0; if (type) *type = 0;
    if (name && buf > 0) name[0] = '\0';
}
void glGetActiveUniform(GLuint p, GLuint i, GLsizei buf, GLsizei *len, GLint *size, GLenum *type, GLchar *name) {
    (void)p; (void)i; if (len) *len = 0; if (size) *size = 0; if (type) *type = 0;
    if (name && buf > 0) name[0] = '\0';
}
void glGetAttachedShaders(GLuint p, GLsizei maxc, GLsizei *count, GLuint *shaders) {
    (void)p; (void)maxc; (void)shaders; if (count) *count = 0;
}
void glGetShaderSource(GLuint s, GLsizei buf, GLsizei *len, GLchar *src) {
    (void)s; if (len) *len = 0; if (src && buf > 0) src[0] = '\0';
}
void glGetBufferParameteriv(GLenum t, GLenum p, GLint *params) { (void)t;(void)p; if (params) params[0] = 0; }
void glGetRenderbufferParameteriv(GLenum t, GLenum p, GLint *params) { (void)t;(void)p; if (params) params[0] = 0; }
void glGetFramebufferAttachmentParameteriv(GLenum t, GLenum a, GLenum p, GLint *params) {
    (void)t;(void)a;(void)p; if (params) params[0] = 0;
}
void glGetTexParameterfv(GLenum t, GLenum p, GLfloat *params) { (void)t;(void)p; if (params) params[0] = 0.0f; }
void glGetTexParameteriv(GLenum t, GLenum p, GLint *params) { (void)t;(void)p; if (params) params[0] = 0; }
void glGetUniformfv(GLuint p, GLint loc, GLfloat *params) { (void)p;(void)loc; if (params) params[0] = 0.0f; }
void glGetUniformiv(GLuint p, GLint loc, GLint *params) { (void)p;(void)loc; if (params) params[0] = 0; }
void glGetVertexAttribfv(GLuint i, GLenum p, GLfloat *params) { (void)i;(void)p; if (params) params[0] = 0.0f; }
void glGetVertexAttribiv(GLuint i, GLenum p, GLint *params) { (void)i;(void)p; if (params) params[0] = 0; }
void glGetVertexAttribPointerv(GLuint i, GLenum p, void **pointer) { (void)i;(void)p; if (pointer) *pointer = (void*)0; }
GLboolean glIsBuffer(GLuint x) { (void)x; return GL_FALSE; }
GLboolean glIsEnabled(GLenum x) { (void)x; return GL_FALSE; }
GLboolean glIsFramebuffer(GLuint x) { (void)x; return GL_FALSE; }
GLboolean glIsProgram(GLuint x) { (void)x; return GL_FALSE; }
GLboolean glIsRenderbuffer(GLuint x) { (void)x; return GL_FALSE; }
GLboolean glIsShader(GLuint x) { (void)x; return GL_FALSE; }
GLboolean glIsTexture(GLuint x) { (void)x; return GL_FALSE; }
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *data) {
    (void)x; (void)y; (void)fmt; (void)type;        /* no host readback path; zero-fill */
    if (data && w > 0 && h > 0) {
        uint32_t comp = gl_format_components(fmt), ts = gl_type_bytes(type);
        memset(data, 0, (size_t)w * (size_t)h * comp * ts);
    }
}
void *glMapBufferOES(GLenum t, GLenum a) { (void)t; (void)a; return (void*)0; }  /* -> subdata fallback */
GLboolean glUnmapBufferOES(GLenum t) { (void)t; return GL_FALSE; }
void glGetBufferPointervOES(GLenum t, GLenum p, void **params) { (void)t;(void)p; if (params) *params = (void*)0; }

/* ---- optimistic queries (no round-trip) ---- */
GLenum glGetError(void) {
    AlrShimState *s = alr_shim();
    GLenum e = (GLenum)s->gl_error;
    s->gl_error = GL_NO_ERROR;
    return e;
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    if (!params) return;
    if (pname == GL_COMPILE_STATUS)      *params = GL_TRUE;   /* optimistic */
    else if (pname == GL_INFO_LOG_LENGTH) *params = 0;
    else if (pname == GL_SHADER_SOURCE_LENGTH) {
        /* GL spec: length of the source INCLUDING the NUL terminator, or 0 if none.
         * Answer locally from the cached length (glmark2 validates this and rejects
         * the shader on a mismatch — the prior hardcoded 0 failed its build scene). */
        uint32_t L = 0;
        *params = alr_shim_shader_srclen((uint32_t)shader, &L) ? (GLint)(L + 1) : 0;
    }
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

/* glGetIntegerv — NO wire op (a host round-trip per query would defeat the design).
 * Return plausible GLES2 minimums so apps that gate on caps at init (glmark2 queries
 * a handful when creating its canvas) don't crash on a missing/zero value. These are
 * advisory; a real draw still runs on whatever the host GPU actually supports. */
void glGetIntegerv(GLenum pname, GLint *params) {
    if (!params) return;
    switch (pname) {
        case 0x8869: params[0] = 16;    break;  /* GL_MAX_VERTEX_ATTRIBS */
        case 0x0D33: params[0] = 4096;  break;  /* GL_MAX_TEXTURE_SIZE */
        case 0x8872: params[0] = 16;    break;  /* GL_MAX_TEXTURE_IMAGE_UNITS */
        case 0x8B4D: params[0] = 32;    break;  /* GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */
        case 0x8B4C: params[0] = 16;    break;  /* GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS */
        case 0x8DFB: params[0] = 256;   break;  /* GL_MAX_VERTEX_UNIFORM_VECTORS */
        case 0x8DFD: params[0] = 15;    break;  /* GL_MAX_VARYING_VECTORS */
        case 0x8DFC: params[0] = 224;   break;  /* GL_MAX_FRAGMENT_UNIFORM_VECTORS */
        case 0x84E8: params[0] = 16;    break;  /* GL_MAX_RENDERBUFFER_SIZE-ish / units */
        case 0x0D3A: params[0] = 16384; params[1] = 16384; break;  /* GL_MAX_VIEWPORT_DIMS (2) */
        default:     params[0] = 0;     break;
    }
}

/* glGetString passthrough: the host executor reads its REAL glGetString(GL_RENDERER/
 * VENDOR/VERSION) on its Mali context and publishes them into the ring header's identity
 * block (ring_set_identity). Here we read those back so a GLES guest sees the actual host
 * GPU (e.g. "Mali-G615") instead of a placeholder. The pointer aliases the shared mapping
 * and is stable for the ring's lifetime, so we can return it directly (no copy/cache).
 *
 * NEVER hardcode a vendor: alr_ring_identity returns NULL until the host has filled the
 * block (identity_ready), or when running ring-less (host_GPU absent / smoke run), in
 * which case we GRACEFULLY FALL BACK to the original vendor-neutral synthetic strings —
 * so a non-Mali host is reported truthfully and a host-less shim still answers sanely. */
const GLubyte *glGetString(GLenum name) {
    AlrShimState *s = alr_shim();
    const char *host;
    switch (name) {
        case GL_VENDOR:
            host = alr_ring_identity(&s->ring, ALR_IDENT_VENDOR);
            return host ? (const GLubyte*)host : (const GLubyte*)"Android-on-Linux (ALR)";
        /* GL_RENDERER is vendor-NEUTRAL in the fallback ON PURPOSE: the real draws run on
         * whatever vendor GPU driver the host reaches via NDK libEGL/libGLESv2 (Mali,
         * Adreno, Xclipse, ...). The passthrough above forwards the host's ACTUAL renderer
         * when known; the synthetic string is only the host-unknown fallback. */
        case GL_RENDERER:
            host = alr_ring_identity(&s->ring, ALR_IDENT_RENDERER);
            return host ? (const GLubyte*)host
                        : (const GLubyte*)"ALR command-stream (host GPU passthrough)";
        case GL_VERSION:
            host = alr_ring_identity(&s->ring, ALR_IDENT_VERSION);
            return host ? (const GLubyte*)host : (const GLubyte*)"OpenGL ES 2.0 ALR";
        case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte*)"OpenGL ES GLSL ES 1.00";
        case GL_EXTENSIONS:               return (const GLubyte*)"";
        default:                          return (const GLubyte*)"";
    }
}

/* ---- flush / finish ---- */
void glFlush(void)  { alr_shim_flush(); }          /* nudge the host; don't block */
void glFinish(void) { alr_shim_flush_and_wait(); } /* block until the host drains */
