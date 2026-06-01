/* wc_emit.c — STAGE 1 of the off-device wire-format check.
 *
 * Links the REAL guest GLES2 shim (alr_gles_shim.c) against a linear-buffer STUB
 * runtime (defined below) instead of the ring runtime (alr_shim_runtime.c). It then
 * drives the shim through a representative cube GL sequence; every wire op the shim
 * emits is captured into a flat buffer and written to argv[1]. STAGE 2
 * (decode_check.cpp) decodes that buffer with the COMMITTED host decoder and asserts
 * the bytes round-trip. This proves the GUEST encoder matches the host decoder
 * off-device (the seam the device probes — which use the host-side Encoder — never
 * exercise). Host-only; never shipped to the device.
 *
 * The stub runtime is faithful where it matters for the wire: alr_shim_emit() runs
 * the shim's build() into the SAME scratch the real runtime uses and appends the
 * exact bytes; the uniform-name table mirrors alr_shim_runtime.c's intern/name/reset
 * semantics (so the shim's by-name uniform handle packing is tested for real).
 */
#include "alr_khr_gles2.h"
#include "alr_shim_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ----- capture buffer (the "linear-buffer stub runtime") ----- */
static AlrShimState g_state;            /* virtual-id counters + scratch + uniform table */
static uint8_t      g_capture[512 * 1024];
static size_t       g_capture_len = 0;

AlrShimState *alr_shim(void) {
    static int inited = 0;
    if (!inited) {
        memset(&g_state, 0, sizeof(g_state));
        g_state.next_shader = g_state.next_program = g_state.next_buffer = g_state.next_texture = 1;
        g_state.next_framebuffer = g_state.next_renderbuffer = 1;
        g_state.gl_error = GL_NO_ERROR;
        g_state.doorbell_fd = -1;
        g_state.ring_ok = 1;            /* pretend attached so emits are captured, not dropped */
        inited = 1;
    }
    return &g_state;
}

void alr_shim_emit(void (*build)(AlrEncoder *e, void *ctx), void *ctx) {
    AlrShimState *s = alr_shim();
    AlrEncoder e;
    alr_enc_init(&e, s->scratch, sizeof(s->scratch));
    build(&e, ctx);
    if (e.overflow) { s->gl_error = 0x0505; return; }
    if (g_capture_len + e.len <= sizeof(g_capture)) {
        memcpy(g_capture + g_capture_len, e.buf, e.len);
        g_capture_len += e.len;
    } else {
        fprintf(stderr, "wc_emit: capture buffer overflow\n");
    }
}

void alr_shim_flush_and_wait(void) { /* sync point — no transport in the harness */ }
void alr_shim_flush(void) { /* no-op */ }

/* uniform-name table — mirrors alr_shim_runtime.c exactly (intern dedups by name and
 * returns an index; name returns the stored string; reset clears a program). */
static AlrProgramUniforms *find_prog(AlrShimState *s, uint32_t vprog, int create) {
    int free_slot = -1;
    for (int i = 0; i < ALR_SHIM_MAX_PROGRAMS; ++i) {
        if (s->progs[i].vprog == vprog && vprog != 0) return &s->progs[i];
        if (s->progs[i].vprog == 0 && free_slot < 0) free_slot = i;
    }
    if (create && free_slot >= 0) {
        s->progs[free_slot].vprog = vprog;
        s->progs[free_slot].count = 0;
        return &s->progs[free_slot];
    }
    return NULL;
}
int alr_shim_uniform_intern(uint32_t vprog, const char *name) {
    AlrShimState *s = alr_shim();
    if (!name) return -1;
    AlrProgramUniforms *p = find_prog(s, vprog, 1);
    if (!p) return -1;
    for (int i = 0; i < p->count; ++i)
        if (strncmp(p->names[i].name, name, ALR_SHIM_MAX_UNIFORM_NAME) == 0) return i;
    if (p->count >= ALR_SHIM_MAX_UNIFORMS_PER_PROG) return -1;
    int idx = p->count++;
    strncpy(p->names[idx].name, name, ALR_SHIM_MAX_UNIFORM_NAME - 1);
    p->names[idx].name[ALR_SHIM_MAX_UNIFORM_NAME - 1] = '\0';
    return idx;
}
const char *alr_shim_uniform_name(uint32_t vprog, int handle) {
    AlrShimState *s = alr_shim();
    if (handle < 0) return NULL;
    AlrProgramUniforms *p = find_prog(s, vprog, 0);
    if (p && handle < p->count) return p->names[handle].name;
    return NULL;
}
/* attribute table — same scheme, separate array (mirrors alr_shim_runtime.c). */
static AlrProgramUniforms *find_attr(AlrShimState *s, uint32_t vprog, int create) {
    int free_slot = -1;
    for (int i = 0; i < ALR_SHIM_MAX_PROGRAMS; ++i) {
        if (s->attribs[i].vprog == vprog && vprog != 0) return &s->attribs[i];
        if (s->attribs[i].vprog == 0 && free_slot < 0) free_slot = i;
    }
    if (create && free_slot >= 0) { s->attribs[free_slot].vprog = vprog;
                                    s->attribs[free_slot].count = 0; return &s->attribs[free_slot]; }
    return NULL;
}
int alr_shim_attrib_intern(uint32_t vprog, const char *name) {
    AlrShimState *s = alr_shim();
    if (!name) return -1;
    AlrProgramUniforms *p = find_attr(s, vprog, 1);
    if (!p) return -1;
    for (int i = 0; i < p->count; ++i)
        if (strncmp(p->names[i].name, name, ALR_SHIM_MAX_UNIFORM_NAME) == 0) return i;
    if (p->count >= ALR_SHIM_MAX_UNIFORMS_PER_PROG) return -1;
    int idx = p->count++;
    strncpy(p->names[idx].name, name, ALR_SHIM_MAX_UNIFORM_NAME - 1);
    p->names[idx].name[ALR_SHIM_MAX_UNIFORM_NAME - 1] = '\0';
    return idx;
}
const char *alr_shim_attrib_name(uint32_t vprog, int handle) {
    AlrShimState *s = alr_shim();
    if (handle < 0) return NULL;
    AlrProgramUniforms *p = find_attr(s, vprog, 0);
    if (p && handle < p->count) return p->names[handle].name;
    return NULL;
}
void alr_shim_program_reset(uint32_t vprog) {
    AlrShimState *s = alr_shim();
    AlrProgramUniforms *p = find_prog(s, vprog, 0);
    if (p) p->count = 0;
    AlrProgramUniforms *a = find_attr(s, vprog, 0);
    if (a) a->count = 0;
}

/* ----- the cube GL sequence (drives the REAL shim entry points) ----- */
static const char *kVS =
    "attribute vec3 aPos; attribute vec2 aUV; uniform mat4 uMVP; varying vec2 vUV;"
    "void main(){ vUV = aUV; gl_Position = uMVP * vec4(aPos, 1.0); }";
static const char *kFS =
    "precision mediump float; varying vec2 vUV; uniform sampler2D uTex;"
    "void main(){ gl_FragColor = texture2D(uTex, vUV); }";

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: wc_emit <out.bin>\n"); return 2; }

    glViewport(0, 0, 64, 64);
    glClearColor(0.1f, 0.1f, 0.4f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &kVS, NULL);
    glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &kFS, NULL);
    glCompileShader(fs);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    glLinkProgram(prog);
    glUseProgram(prog);

    /* interleaved pos(3f)+uv(2f), stride 20; 36 verts (a cube's 12 triangles). */
    float verts[36 * 5];
    memset(verts, 0, sizeof(verts));
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(verts), verts, GL_STATIC_DRAW);

    /* 8x8 RGBA texture = 256 bytes (matches the documented wire result). */
    unsigned char pixels[8 * 8 * 4];
    for (size_t i = 0; i < sizeof(pixels); ++i) pixels[i] = (unsigned char)i;
    GLuint tex;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    /* identity mat4 -> uMVP (column-major), sampler unit 0 -> uTex. */
    float mvp[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    GLint loc_mvp = glGetUniformLocation(prog, "uMVP");   /* client-side; emits nothing */
    GLint loc_tex = glGetUniformLocation(prog, "uTex");
    glUniformMatrix4fv(loc_mvp, 1, GL_FALSE, mvp);
    glUniform1i(loc_tex, 0);

    /* uniform scalar/vector/matrix variants (broaden glmark2 scene coverage). */
    GLint loc_color = glGetUniformLocation(prog, "uColor");
    GLint loc_time  = glGetUniformLocation(prog, "uTime");
    GLint loc_mode  = glGetUniformLocation(prog, "uMode2");
    GLint loc_nmat  = glGetUniformLocation(prog, "uNormalMatrix");
    float color3[3] = {0.2f, 0.4f, 0.6f};
    glUniform3fv(loc_color, 1, color3);             /* OP_UNIFORM_FV cols=3 */
    glUniform1f(loc_time, 1.5f);                    /* OP_UNIFORM_FV cols=1 */
    glUniform2i(loc_mode, 3, 7);                    /* OP_UNIFORM_IV cols=2 */
    float nm3[9] = {1,0,0, 0,1,0, 0,0,1};
    glUniformMatrix3fv(loc_nmat, 1, GL_FALSE, nm3); /* OP_UNIFORM_MATRIX_FV dim=3 */

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (const void *)12);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 36);

    /* --- glmark2 first-light op exercise: cull state + attribute-BY-NAME
     *     (glGetAttribLocation -> packed handle -> NAMED ops) + indexed draw. --- */
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    unsigned short idx[6] = {0, 1, 2, 2, 3, 0};
    GLuint ebo;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof(idx), idx, GL_STATIC_DRAW);
    GLint a_pos = glGetAttribLocation(prog, "position");  /* packed handle -> NAMED path */
    GLint a_nrm = glGetAttribLocation(prog, "normal");
    glEnableVertexAttribArray(a_pos);
    glVertexAttribPointer(a_pos, 3, GL_FLOAT, GL_FALSE, 24, (const void *)0);
    glEnableVertexAttribArray(a_nrm);
    glVertexAttribPointer(a_nrm, 3, GL_FLOAT, GL_FALSE, 24, (const void *)12);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0);

    /* --- FBO/renderbuffer (render-to-texture): attach the existing texture as color +
     *     a depth renderbuffer, then bind framebuffer 0 to return to the default target. --- */
    GLuint fbo, rbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 64, 64);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbo);
    (void)glCheckFramebufferStatus(GL_FRAMEBUFFER);  /* optimistic, no wire op */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);            /* back to default (host: AHB FBO) */

    FILE *f = fopen(argv[1], "wb");
    if (!f) { fprintf(stderr, "wc_emit: cannot open %s\n", argv[1]); return 2; }
    fwrite(g_capture, 1, g_capture_len, f);
    fclose(f);
    fprintf(stderr, "wc_emit: wrote %zu bytes to %s\n", g_capture_len, argv[1]);
    return 0;
}
