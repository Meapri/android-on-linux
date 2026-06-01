/* alr-gles-cube.c — an ordinary, unmodified glibc GLES2 program: a spinning,
 * depth-tested, textured cube. It is NOT ALR-aware; it just links -lEGL -lGLESv2
 * and happens to resolve those SONAMEs to the ALR marshalling shim
 * (libEGL.so.1 / libGLESv2.so.2). Every GL/EGL call it makes is encoded by the
 * shim into the SPSC command ring and replayed on the device's real Mali GPU
 * host-side; this binary never touches a real GPU itself.
 *
 * It is the guest test binary referenced by README.md ("alr-gles-cube") and
 * built as the final step of build-shim.sh. It deliberately restricts itself to
 * the exact GL/EGL entry points the shim implements (see the shim sources): in
 * particular it uses glBindAttribLocation (NOT glGetAttribLocation), the VBO-
 * offset form of glVertexAttribPointer (a byte offset cast to void*, never a
 * client array), glDrawArrays (NOT glDrawElements), and no FBO/renderbuffer or
 * float-uniform calls.
 *
 * Geometry: 36 vertices (12 triangles, 6 quad faces) drawn with one
 * glDrawArrays(GL_TRIANGLES, 0, 36). Each vertex is interleaved position(3f) +
 * texcoord(2f) => stride 20 bytes, attribute offsets 0 (aPos) and 12 (aTex).
 * One VBO holds the whole mesh. One 8x8 RGBA texture is uploaded with
 * glTexImage2D (256 bytes, matching the documented wire-verification figure).
 * The shaders use a uMVP mat4 uniform (glUniformMatrix4fv) and a uTex sampler
 * (glUniform1i(loc, 0)). The MVP is recomputed on the CPU every frame to spin
 * the cube; the loop runs a fixed number of frames (argv[1] or default 120),
 * eglSwapBuffers each frame, then tears everything down.
 *
 * Pure C / glibc. Includes ONLY the vendored ALR headers — never the system
 * GL/EGL headers — so it builds on a host with no Khronos/NDK headers.
 */
#include "alr_khr_gles2.h"
#include "alr_khr_egl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Shaders. GLSL ES 1.00. Attribute *locations* are assigned with
 * glBindAttribLocation before linking (the shim implements that and NOT
 * glGetAttribLocation), so the shader does not need explicit layout qualifiers.
 * ------------------------------------------------------------------------- */
static const char *kVertexSrc =
    "attribute vec3 aPos;\n"
    "attribute vec2 aTex;\n"
    "uniform mat4 uMVP;\n"
    "varying vec2 vTex;\n"
    "void main() {\n"
    "    vTex = aTex;\n"
    "    gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "}\n";

static const char *kFragmentSrc =
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "varying vec2 vTex;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(uTex, vTex);\n"
    "}\n";

/* Attribute indices fixed via glBindAttribLocation (must precede glLinkProgram). */
enum { ATTR_POS = 0, ATTR_TEX = 1 };

/* Interleaved cube geometry: 36 vertices, each = {x,y,z, u,v}. Two triangles per
 * face, six faces. stride = 5 floats = 20 bytes; aPos at offset 0, aTex at 12. */
#define FLOATS_PER_VERT 5
#define VERT_COUNT      36
#define VERT_STRIDE     (FLOATS_PER_VERT * (int)sizeof(float))   /* 20 */
#define POS_OFFSET      0
#define TEX_OFFSET      (3 * (int)sizeof(float))                 /* 12 */

static const float kCube[VERT_COUNT * FLOATS_PER_VERT] = {
    /* +Z face */
    -1,-1, 1, 0,0,   1,-1, 1, 1,0,   1, 1, 1, 1,1,
    -1,-1, 1, 0,0,   1, 1, 1, 1,1,  -1, 1, 1, 0,1,
    /* -Z face */
     1,-1,-1, 0,0,  -1,-1,-1, 1,0,  -1, 1,-1, 1,1,
     1,-1,-1, 0,0,  -1, 1,-1, 1,1,   1, 1,-1, 0,1,
    /* +X face */
     1,-1, 1, 0,0,   1,-1,-1, 1,0,   1, 1,-1, 1,1,
     1,-1, 1, 0,0,   1, 1,-1, 1,1,   1, 1, 1, 0,1,
    /* -X face */
    -1,-1,-1, 0,0,  -1,-1, 1, 1,0,  -1, 1, 1, 1,1,
    -1,-1,-1, 0,0,  -1, 1, 1, 1,1,  -1, 1,-1, 0,1,
    /* +Y face */
    -1, 1, 1, 0,0,   1, 1, 1, 1,0,   1, 1,-1, 1,1,
    -1, 1, 1, 0,0,   1, 1,-1, 1,1,  -1, 1,-1, 0,1,
    /* -Y face */
    -1,-1,-1, 0,0,   1,-1,-1, 1,0,   1,-1, 1, 1,1,
    -1,-1,-1, 0,0,   1,-1, 1, 1,1,  -1,-1, 1, 0,1,
};

/* ---- 4x4 column-major matrix helpers (CPU side, plain <math.h>) ---- */
static void mat4_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* Column-major C = A * B. */
static void mat4_mul(float *out, const float *a, const float *b) {
    float r[16];
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            r[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

/* Right-handed perspective, column-major (matches glUniformMatrix4fv transpose=0). */
static void mat4_perspective(float *m, float fovy_rad, float aspect,
                             float znear, float zfar) {
    float f = 1.0f / tanf(fovy_rad * 0.5f);
    memset(m, 0, 16 * sizeof(float));
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1.0f;
    m[14] = (2.0f * zfar * znear) / (znear - zfar);
}

static void mat4_translate(float *m, float x, float y, float z) {
    mat4_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

static void mat4_rotate_y(float *m, float a) {
    mat4_identity(m);
    float c = cosf(a), s = sinf(a);
    m[0] = c;  m[2] = -s;
    m[8] = s;  m[10] = c;
}

static void mat4_rotate_x(float *m, float a) {
    mat4_identity(m);
    float c = cosf(a), s = sinf(a);
    m[5] = c;  m[6] = s;
    m[9] = -s; m[10] = c;
}

/* ---- build an 8x8 RGBA checkerboard (256 bytes, tight-packed) ---- */
#define TEX_DIM 8
static void make_texture(unsigned char *px) {
    for (int y = 0; y < TEX_DIM; ++y) {
        for (int x = 0; x < TEX_DIM; ++x) {
            unsigned char *p = px + (y * TEX_DIM + x) * 4;
            int dark = ((x ^ y) & 1);
            if (dark) { p[0] = 30;  p[1] = 30;  p[2] = 40;  p[3] = 255; }
            else      { p[0] = 220; p[1] = 160; p[2] = 60;  p[3] = 255; }
        }
    }
}

/* ---- compile one shader from a source string ---- */
static GLuint make_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    const GLchar *arr[1] = { (const GLchar *)src };
    glShaderSource(sh, 1, arr, NULL);     /* length=NULL -> shim uses strlen */
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);   /* optimistic in the shim */
    if (ok != GL_TRUE) {
        fprintf(stderr, "alr-gles-cube: shader compile reported failure (type=0x%x)\n",
                (unsigned)type);
    }
    return sh;
}

int main(int argc, char **argv) {
    int frames = 120;
    if (argc > 1) {
        int n = atoi(argv[1]);
        if (n > 0) frames = n;
    }

    /* ---------------- EGL bring-up (shim sentinels; the host owns the real
     * surface). A NULL native window is accepted by the shim's
     * eglCreateWindowSurface (the window handle is ignored guest-side). ---- */
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "alr-gles-cube: eglGetDisplay failed\n");
        return 1;
    }
    EGLint egl_major = 0, egl_minor = 0;
    if (!eglInitialize(dpy, &egl_major, &egl_minor)) {
        fprintf(stderr, "alr-gles-cube: eglInitialize failed (0x%x)\n", eglGetError());
        return 1;
    }

    const EGLint cfg_attrs[] = {
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLConfig config = NULL;
    EGLint num_config = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &config, 1, &num_config) || num_config < 1) {
        fprintf(stderr, "alr-gles-cube: eglChooseConfig failed (0x%x)\n", eglGetError());
        return 1;
    }

    EGLSurface surface = eglCreateWindowSurface(dpy, config,
                                                (EGLNativeWindowType)0, NULL);
    if (surface == EGL_NO_SURFACE) {
        fprintf(stderr, "alr-gles-cube: eglCreateWindowSurface failed (0x%x)\n",
                eglGetError());
        return 1;
    }

    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "alr-gles-cube: eglCreateContext failed (0x%x)\n", eglGetError());
        return 1;
    }

    if (!eglMakeCurrent(dpy, surface, surface, ctx)) {
        fprintf(stderr, "alr-gles-cube: eglMakeCurrent failed (0x%x)\n", eglGetError());
        return 1;
    }

    /* Informational: drives glGetString through the shim. */
    const GLubyte *renderer = glGetString(GL_RENDERER);
    const GLubyte *version  = glGetString(GL_VERSION);
    printf("alr-gles-cube: EGL %d.%d, GL_RENDERER=\"%s\", GL_VERSION=\"%s\", frames=%d\n",
           (int)egl_major, (int)egl_minor,
           renderer ? (const char *)renderer : "?",
           version  ? (const char *)version  : "?",
           frames);

    /* ---------------- GLES2 setup ---------------- */
    GLuint vs = make_shader(GL_VERTEX_SHADER,   kVertexSrc);
    GLuint fs = make_shader(GL_FRAGMENT_SHADER, kFragmentSrc);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    /* Fix attribute locations BEFORE linking (no glGetAttribLocation). */
    glBindAttribLocation(prog, ATTR_POS, "aPos");
    glBindAttribLocation(prog, ATTR_TEX, "aTex");
    glLinkProgram(prog);
    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);    /* optimistic in the shim */
    if (linked != GL_TRUE) {
        fprintf(stderr, "alr-gles-cube: program link reported failure\n");
    }
    glUseProgram(prog);

    /* One VBO with the interleaved cube mesh. */
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(kCube), kCube, GL_STATIC_DRAW);

    /* One 8x8 RGBA texture, bound on unit 0. */
    unsigned char texels[TEX_DIM * TEX_DIM * 4];
    make_texture(texels);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_DIM, TEX_DIM, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, texels);

    /* Uniform locations resolved by name (the shim hands back a client handle). */
    GLint locMVP = glGetUniformLocation(prog, "uMVP");
    GLint locTex = glGetUniformLocation(prog, "uTex");
    glUniform1i(locTex, 0);     /* sampler uTex -> texture unit 0 */

    /* Depth test + viewport. */
    const int fb_w = 1280, fb_h = 720;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glViewport(0, 0, fb_w, fb_h);

    /* Static projection. */
    float proj[16];
    mat4_perspective(proj, 60.0f * 3.14159265f / 180.0f,
                     (float)fb_w / (float)fb_h, 0.1f, 100.0f);

    /* ---------------- render loop ---------------- */
    for (int frame = 0; frame < frames; ++frame) {
        float t = (float)frame * 0.03f;

        float rx[16], ry[16], rot[16], view[16], mv[16], mvp[16];
        mat4_rotate_x(rx, t * 0.7f);
        mat4_rotate_y(ry, t);
        mat4_mul(rot, ry, rx);                 /* model rotation */
        mat4_translate(view, 0.0f, 0.0f, -6.0f);
        mat4_mul(mv, view, rot);               /* view * model */
        mat4_mul(mvp, proj, mv);               /* projection * view * model */

        glClearColor(0.06f, 0.06f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(prog);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);

        /* VBO-offset attribute setup: pointer args are BYTE OFFSETS into the
         * bound ARRAY_BUFFER (cast to void*), never client array pointers. */
        glEnableVertexAttribArray(ATTR_POS);
        glVertexAttribPointer(ATTR_POS, 3, GL_FLOAT, GL_FALSE, VERT_STRIDE,
                              (const void *)(intptr_t)POS_OFFSET);
        glEnableVertexAttribArray(ATTR_TEX);
        glVertexAttribPointer(ATTR_TEX, 2, GL_FLOAT, GL_FALSE, VERT_STRIDE,
                              (const void *)(intptr_t)TEX_OFFSET);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);

        glUniformMatrix4fv(locMVP, 1, GL_FALSE, mvp);

        glDrawArrays(GL_TRIANGLES, 0, VERT_COUNT);

        eglSwapBuffers(dpy, surface);          /* per-frame host sync point */
    }

    glFinish();

    /* ---------------- teardown ---------------- */
    glDeleteTextures(1, &tex);
    glDeleteBuffers(1, &vbo);
    glDeleteProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surface);
    eglTerminate(dpy);

    printf("alr-gles-cube: done (%d frames)\n", frames);
    return 0;
}
