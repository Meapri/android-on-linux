/* alr_khr_gles2.h — a MINIMAL, self-contained GLES2 header for the ALR shim.
 *
 * Just the GLES2 types, tokens, and the entry-point prototypes the spinning
 * cube uses, with the STANDARD Khronos values (from the public OpenGL ES 2.0
 * registry). Vendored so the shim (libGLESv2.so.2) and the cube build with
 * zig cc / gcc on a host that has no Khronos headers, and so the guest rootfs
 * (which ships no Mali GL headers) can rebuild them.
 *
 * ABI-compatible with <GLES2/gl2.h>: identical typedefs and #define values, so a
 * program that instead includes the real gl2.h links against the same exported
 * symbols. This is NOT a GL implementation — it only declares the symbols the
 * shim exports. Pure C.
 */
#ifndef ALR_KHR_GLES2_H
#define ALR_KHR_GLES2_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>   /* ssize_t (used by GLsizeiptr) */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- GL base types (match <KHR/khrplatform.h> / <GLES2/gl2.h>) ---- */
typedef void             GLvoid;
typedef unsigned int     GLenum;
typedef unsigned char    GLboolean;
typedef unsigned int     GLbitfield;
typedef int8_t           GLbyte;
typedef short            GLshort;
typedef int              GLint;
typedef int              GLsizei;
typedef uint8_t          GLubyte;
typedef unsigned short   GLushort;
typedef unsigned int     GLuint;
typedef float            GLfloat;
typedef float            GLclampf;
typedef int32_t          GLfixed;
typedef char             GLchar;
typedef intptr_t         GLintptr;
typedef ssize_t          GLsizeiptr;

/* ---- tokens (standard registry values) ---- */
#define GL_FALSE                          0
#define GL_TRUE                           1
#define GL_NO_ERROR                       0

#define GL_POINTS                         0x0000
#define GL_LINES                          0x0001
#define GL_TRIANGLES                      0x0004
#define GL_TRIANGLE_STRIP                 0x0005
#define GL_TRIANGLE_FAN                   0x0006

#define GL_DEPTH_BUFFER_BIT               0x00000100
#define GL_STENCIL_BUFFER_BIT             0x00000400
#define GL_COLOR_BUFFER_BIT               0x00004000

#define GL_NEVER                          0x0200
#define GL_LESS                           0x0201
#define GL_EQUAL                          0x0202
#define GL_LEQUAL                         0x0203
#define GL_GREATER                        0x0204
#define GL_GEQUAL                         0x0206
#define GL_ALWAYS                         0x0207

#define GL_CULL_FACE                      0x0B44
#define GL_DEPTH_TEST                     0x0B71
#define GL_BLEND                          0x0BE2
#define GL_SCISSOR_TEST                   0x0C11

#define GL_TEXTURE_2D                     0x0DE1
#define GL_UNPACK_ALIGNMENT               0x0CF5

#define GL_BYTE                           0x1400
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_SHORT                          0x1402
#define GL_UNSIGNED_SHORT                 0x1403
#define GL_INT                            0x1404
#define GL_UNSIGNED_INT                   0x1405
#define GL_FLOAT                          0x1406

#define GL_RGB                            0x1907
#define GL_RGBA                           0x1908
#define GL_LUMINANCE                      0x1909
#define GL_LUMINANCE_ALPHA                0x190A

#define GL_FRONT                          0x0404
#define GL_BACK                           0x0405
#define GL_FRONT_AND_BACK                 0x0408
#define GL_CW                             0x0900
#define GL_CCW                            0x0901

#define GL_MAX_VERTEX_ATTRIBS             0x8869
#define GL_MAX_TEXTURE_SIZE               0x0D33
#define GL_MAX_TEXTURE_IMAGE_UNITS        0x8872

#define GL_NEAREST                        0x2600
#define GL_LINEAR                         0x2601
#define GL_NEAREST_MIPMAP_NEAREST         0x2700
#define GL_LINEAR_MIPMAP_NEAREST          0x2701
#define GL_NEAREST_MIPMAP_LINEAR          0x2702
#define GL_LINEAR_MIPMAP_LINEAR           0x2703
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803

#define GL_REPEAT                         0x2901
#define GL_CLAMP_TO_EDGE                  0x812F

#define GL_TEXTURE0                       0x84C0
#define GL_TEXTURE1                       0x84C1
#define GL_TEXTURE2                       0x84C2

#define GL_ARRAY_BUFFER                   0x8892
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_STREAM_DRAW                    0x88E0
#define GL_STATIC_DRAW                    0x88E4
#define GL_DYNAMIC_DRAW                   0x88E8

/* framebuffer / renderbuffer objects */
#define GL_FRAMEBUFFER                    0x8D40
#define GL_RENDERBUFFER                   0x8D41
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_STENCIL_ATTACHMENT             0x8D20
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_DEPTH_COMPONENT16              0x81A5
#define GL_RGBA4                          0x8056
#define GL_RGB565                         0x8D62
#define GL_RGB5_A1                        0x8057
#define GL_STENCIL_INDEX8                 0x8D48

#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84

#define GL_VENDOR                         0x1F00
#define GL_RENDERER                       0x1F01
#define GL_VERSION                        0x1F02
#define GL_EXTENSIONS                     0x1F03
#define GL_SHADING_LANGUAGE_VERSION       0x8B8C

/* ---- entry points the cube/shim use (the exported ABI of libGLESv2.so.2) ---- */
void   glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void   glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void   glClear(GLbitfield mask);
void   glEnable(GLenum cap);
void   glDisable(GLenum cap);
void   glDepthFunc(GLenum func);
void   glScissor(GLint x, GLint y, GLsizei width, GLsizei height);

GLuint glCreateShader(GLenum type);
void   glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
void   glCompileShader(GLuint shader);
void   glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
void   glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
void   glDeleteShader(GLuint shader);

GLuint glCreateProgram(void);
void   glAttachShader(GLuint program, GLuint shader);
void   glBindAttribLocation(GLuint program, GLuint index, const GLchar *name);
void   glLinkProgram(GLuint program);
void   glUseProgram(GLuint program);
void   glGetProgramiv(GLuint program, GLenum pname, GLint *params);
void   glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
void   glDeleteProgram(GLuint program);

void   glGenBuffers(GLsizei n, GLuint *buffers);
void   glBindBuffer(GLenum target, GLuint buffer);
void   glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void   glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
void   glDeleteBuffers(GLsizei n, const GLuint *buffers);

void   glGenTextures(GLsizei n, GLuint *textures);
void   glBindTexture(GLenum target, GLuint texture);
void   glActiveTexture(GLenum texture);
void   glTexParameteri(GLenum target, GLenum pname, GLint param);
void   glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                    GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
void   glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                       GLsizei height, GLenum format, GLenum type, const void *pixels);
void   glGenerateMipmap(GLenum target);
void   glDeleteTextures(GLsizei n, const GLuint *textures);
void   glPixelStorei(GLenum pname, GLint param);

void   glEnableVertexAttribArray(GLuint index);
void   glDisableVertexAttribArray(GLuint index);
void   glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                             GLsizei stride, const void *pointer);
void   glDrawArrays(GLenum mode, GLint first, GLsizei count);
void   glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);
GLint  glGetAttribLocation(GLuint program, const GLchar *name);

void   glCullFace(GLenum mode);
void   glFrontFace(GLenum mode);
void   glGetIntegerv(GLenum pname, GLint *params);

GLint  glGetUniformLocation(GLuint program, const GLchar *name);
void   glUniform1i(GLint location, GLint v0);
void   glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void   glUniform1f(GLint location, GLfloat x);
void   glUniform2f(GLint location, GLfloat x, GLfloat y);
void   glUniform3f(GLint location, GLfloat x, GLfloat y, GLfloat z);
void   glUniform4f(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void   glUniform1fv(GLint location, GLsizei count, const GLfloat *value);
void   glUniform2fv(GLint location, GLsizei count, const GLfloat *value);
void   glUniform3fv(GLint location, GLsizei count, const GLfloat *value);
void   glUniform4fv(GLint location, GLsizei count, const GLfloat *value);
void   glUniform2i(GLint location, GLint x, GLint y);
void   glUniform3i(GLint location, GLint x, GLint y, GLint z);
void   glUniform4i(GLint location, GLint x, GLint y, GLint z, GLint w);
void   glUniform1iv(GLint location, GLsizei count, const GLint *value);
void   glUniform2iv(GLint location, GLsizei count, const GLint *value);
void   glUniform3iv(GLint location, GLsizei count, const GLint *value);
void   glUniform4iv(GLint location, GLsizei count, const GLint *value);
void   glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void   glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);

void   glGenFramebuffers(GLsizei n, GLuint *framebuffers);
void   glBindFramebuffer(GLenum target, GLuint framebuffer);
void   glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
void   glGenRenderbuffers(GLsizei n, GLuint *renderbuffers);
void   glBindRenderbuffer(GLenum target, GLuint renderbuffer);
void   glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
void   glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
GLenum glCheckFramebufferStatus(GLenum target);
void   glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers);
void   glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers);

GLenum glGetError(void);
const GLubyte *glGetString(GLenum name);
void   glFlush(void);
void   glFinish(void);

#ifdef __cplusplus
}
#endif

#endif /* ALR_KHR_GLES2_H */
