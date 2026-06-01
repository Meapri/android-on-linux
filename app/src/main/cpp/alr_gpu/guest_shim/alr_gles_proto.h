/* alr_gles_proto.h — the GUEST<->HOST GLES op-stream wire contract (M3).
 *
 * THIS IS THE CONTRACT. It defines the SAME `enum Op` VALUES and the SAME
 * little-endian wire encoding as the committed HOST decoder
 *   app/src/main/cpp/alr_gpu/alr_gpu_decode.hpp  (alr::gpu, decode_batch()).
 *
 * >>> MUST STAY IN SYNC WITH alr_gpu_decode.hpp <<<
 * The host decoder is the SOURCE OF TRUTH. Every opcode number and every field
 * order/type below was copied from that file's `enum Op` and the exact reads in
 * its decode_batch() switch. If you change one side you MUST change the other,
 * or the host silently mis-decodes the stream (wrong pixels, not a crash).
 *
 * Pure C (C99), glibc-guest-buildable with zig cc / gcc. No GL headers needed
 * here — this is just the byte protocol + a tiny encoder. The shims include it.
 *
 * WIRE FORMAT (matches alr_gpu_decode.hpp's Reader/Encoder exactly):
 *   - little-endian (the only ABI the host Reader assumes; aarch64 is LE).
 *   - each op is:  [u8 opcode][fields...]   <-- NOTE: a BARE u8 opcode, then the
 *     op's fields read back-to-back. There is NO per-op length/flags framing
 *     (the DESIGN.md §3.2 "[u16 op][u16 flags][u32 len]" sketch was superseded by
 *     the committed decoder, which reads a single u8 then the typed fields).
 *   - u32 = 4 bytes LE, i32 = 4 bytes LE, f32 = 4 bytes LE (IEEE-754),
 *     u8  = 1 byte.
 *   - blob = u32 len followed by exactly `len` raw bytes. NO padding/alignment
 *     (the host Encoder::blob writes len then the bytes; Reader::blob reads len
 *     then `len` bytes — byte-for-byte, no 8B pad despite the DESIGN prose).
 *   - the host stops cleanly at end-of-buffer (a missing trailing op is fine);
 *     OP_END (0) also stops it. An UNKNOWN opcode is fail-stop on the host, so
 *     never emit an opcode not in this enum.
 */
#ifndef ALR_GLES_PROTO_H
#define ALR_GLES_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Opcodes. COPIED VERBATIM from alr_gpu_decode.hpp `enum Op : uint8_t`. ----
 * The numeric values are the wire contract; do not renumber.                   */
enum AlrOp {
    ALR_OP_END = 0,
    /* original marshalling-probe ops (kept identical to the host) */
    ALR_OP_VIEWPORT = 1,         /* i32 x,y,w,h */
    ALR_OP_CLEARCOLOR = 2,       /* f32 r,g,b,a */
    ALR_OP_CLEAR = 3,            /* (no args; host clears COLOR|DEPTH) */
    ALR_OP_ENABLE_SCISSOR = 4,   /* (no args) */
    ALR_OP_SCISSOR = 5,          /* i32 x,y,w,h */
    ALR_OP_DISABLE_SCISSOR = 6,  /* (no args) */
    /* shaders + program */
    ALR_OP_CREATE_SHADER = 20,       /* u32 vshader_id, u32 gl_type */
    ALR_OP_SHADER_SOURCE = 21,       /* u32 vshader_id, blob(source) */
    ALR_OP_COMPILE_SHADER = 22,      /* u32 vshader_id */
    ALR_OP_CREATE_PROGRAM = 23,      /* u32 vprog_id */
    ALR_OP_ATTACH_SHADER = 24,       /* u32 vprog_id, u32 vshader_id */
    ALR_OP_BIND_ATTRIB_LOCATION = 25,/* u32 vprog_id, u32 index, blob(name) */
    ALR_OP_LINK_PROGRAM = 26,        /* u32 vprog_id */
    ALR_OP_USE_PROGRAM = 27,         /* u32 vprog_id */
    /* buffers (VBO) */
    ALR_OP_GEN_BUFFER = 30,          /* u32 vbuf_id */
    ALR_OP_BIND_BUFFER = 31,         /* u32 target, u32 vbuf_id */
    ALR_OP_BUFFER_DATA = 32,         /* u32 target, blob(data), u32 usage */
    ALR_OP_BUFFER_SUBDATA = 33,      /* u32 target, u32 offset, blob(data) */
    /* vertex attrib + draw */
    ALR_OP_ENABLE_VAA = 40,          /* u32 index */
    ALR_OP_VERTEX_ATTRIB_POINTER = 41,/* u32 index, i32 size, u32 type, u8 norm, i32 stride, u32 offset */
    ALR_OP_DRAW_ARRAYS = 42,         /* u32 mode, i32 first, i32 count */
    /* uniforms (carry the uniform NAME as a blob; host looks up the real loc) */
    ALR_OP_UNIFORM_MATRIX4FV = 50,   /* u32 vprog_id, blob(name), f32[16] */
    ALR_OP_UNIFORM1I = 51,           /* u32 vprog_id, blob(name), i32 value */
    ALR_OP_UNIFORM_FV = 52,          /* u32 vprog, blob(name), u8 cols(1..4), u32 count, f32[cols*count] */
    ALR_OP_UNIFORM_IV = 53,          /* u32 vprog, blob(name), u8 cols(1..4), u32 count, i32[cols*count] */
    ALR_OP_UNIFORM_MATRIX_FV = 54,   /* u32 vprog, blob(name), u8 dim(2..3), u32 count, f32[dim*dim*count] */
    /* textures */
    ALR_OP_GEN_TEXTURE = 60,         /* u32 vtex_id */
    ALR_OP_ACTIVE_TEXTURE = 61,      /* u32 unit (GL_TEXTURE0+n) */
    ALR_OP_BIND_TEXTURE = 62,        /* u32 target, u32 vtex_id */
    ALR_OP_TEX_PARAMETERI = 63,      /* u32 target, u32 pname, i32 param */
    ALR_OP_TEX_IMAGE_2D = 64,        /* u32 target, i32 level, u32 ifmt, i32 w, i32 h, u32 fmt, u32 type, blob(pixels) */
    ALR_OP_GENERATE_MIPMAP = 65,     /* u32 target */
    ALR_OP_TEX_SUBIMAGE_2D = 66,     /* u32 target, i32 level, i32 xoff, i32 yoff, i32 w, i32 h, u32 fmt, u32 type, blob */
    /* state */
    ALR_OP_ENABLE = 70,              /* u32 cap */
    ALR_OP_DISABLE = 71,             /* u32 cap */
    ALR_OP_DEPTH_FUNC = 72,          /* u32 func */
    ALR_OP_CULL_FACE = 73,           /* u32 mode */
    ALR_OP_FRONT_FACE = 74,          /* u32 mode */
    /* attribute-by-NAME (glGetAttribLocation has no round-trip — carry the NAME, the
     * host resolves the real location by name, exactly like uniforms). Emitted instead
     * of the index-based ENABLE_VAA / VERTEX_ATTRIB_POINTER when the location came from
     * glGetAttribLocation (a packed handle); the glBindAttribLocation/index path still
     * emits the plain index ops above. */
    ALR_OP_ENABLE_VAA_NAMED = 80,            /* u32 vprog, blob(name) */
    ALR_OP_VERTEX_ATTRIB_POINTER_NAMED = 81, /* u32 vprog, blob(name), i32 size, u32 type, u8 norm, i32 stride, u32 offset */
    /* indexed draw (meshes) */
    ALR_OP_DRAW_ELEMENTS = 82,       /* u32 mode, i32 count, u32 type, u32 offset (into bound ELEMENT_ARRAY_BUFFER) */
    /* framebuffer / renderbuffer objects (render-to-texture). Virtual ids; vfb 0 = default. */
    ALR_OP_GEN_FRAMEBUFFER = 90,         /* u32 vfb_id */
    ALR_OP_BIND_FRAMEBUFFER = 91,        /* u32 target, u32 vfb_id (0 -> default) */
    ALR_OP_FRAMEBUFFER_TEXTURE2D = 92,   /* u32 target, u32 attachment, u32 textarget, u32 vtex_id, i32 level */
    ALR_OP_GEN_RENDERBUFFER = 93,        /* u32 vrb_id */
    ALR_OP_BIND_RENDERBUFFER = 94,       /* u32 target, u32 vrb_id */
    ALR_OP_RENDERBUFFER_STORAGE = 95,    /* u32 target, u32 internalformat, i32 w, i32 h */
    ALR_OP_FRAMEBUFFER_RENDERBUFFER = 96,/* u32 target, u32 attachment, u32 rbtarget, u32 vrb_id */
    /* GLES3: vertex array objects + instanced draws (vva 0 = default VAO) */
    ALR_OP_GEN_VERTEX_ARRAY = 100,       /* u32 vva_id */
    ALR_OP_BIND_VERTEX_ARRAY = 101,      /* u32 vva_id (0 -> default) */
    ALR_OP_DRAW_ARRAYS_INSTANCED = 102,  /* u32 mode, i32 first, i32 count, i32 instancecount */
    ALR_OP_DRAW_ELEMENTS_INSTANCED = 103,/* u32 mode, i32 count, u32 type, u32 offset, i32 instancecount */
    ALR_OP_VERTEX_ATTRIB_DIVISOR = 104,      /* u32 index, u32 divisor */
    ALR_OP_VERTEX_ATTRIB_DIVISOR_NAMED = 105 /* u32 vprog, blob(name), u32 divisor */
};

/* ---------------------------------------------------------------------------
 * AlrEncoder — a tiny growable little-endian byte builder. Mirrors the host's
 * alr::gpu::Encoder (u8/u32/i32/f32/blob), so a stream built here decodes there.
 *
 * It writes into a caller-owned buffer of fixed capacity (the shims encode
 * directly into a scratch buffer, then RingProducer::append the result). On
 * overflow it sets `overflow=1` and stops writing (the shims size scratch >=
 * the largest single op + payload; uploads use a generous scratch).
 * ------------------------------------------------------------------------- */
typedef struct AlrEncoder {
    uint8_t *buf;   /* caller-owned */
    size_t   cap;   /* capacity in bytes */
    size_t   len;   /* bytes written so far */
    int      overflow; /* set if a write didn't fit */
} AlrEncoder;

static inline void alr_enc_init(AlrEncoder *e, uint8_t *buf, size_t cap) {
    e->buf = buf; e->cap = cap; e->len = 0; e->overflow = 0;
}

static inline void alr_enc_raw(AlrEncoder *e, const void *p, size_t n) {
    if (e->overflow) return;
    if (e->len + n > e->cap) { e->overflow = 1; return; }
    if (n) memcpy(e->buf + e->len, p, n);
    e->len += n;
}
static inline void alr_enc_u8(AlrEncoder *e, uint8_t v)  { alr_enc_raw(e, &v, 1); }
static inline void alr_enc_u32(AlrEncoder *e, uint32_t v){ alr_enc_raw(e, &v, 4); }
static inline void alr_enc_i32(AlrEncoder *e, int32_t v) { alr_enc_raw(e, &v, 4); }
static inline void alr_enc_f32(AlrEncoder *e, float v)   { alr_enc_raw(e, &v, 4); }
static inline void alr_enc_blob(AlrEncoder *e, const void *p, uint32_t n) {
    alr_enc_u32(e, n);
    if (n) alr_enc_raw(e, p, n);
}
static inline void alr_enc_str(AlrEncoder *e, const char *s) {
    alr_enc_blob(e, s, (uint32_t)(s ? strlen(s) : 0));
}

/* Build is little-endian by construction (memcpy of native LE scalars). If this
 * shim is ever cross-built for a big-endian guest, the encoder must byte-swap.
 * aarch64-linux-gnu (the target) is little-endian, so the contract holds. */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* ALR_GLES_PROTO_H */
