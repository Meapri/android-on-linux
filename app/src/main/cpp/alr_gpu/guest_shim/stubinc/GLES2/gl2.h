/* stubinc/GLES2/gl2.h — host-only GLES2 decls for the wire-format check harness.
 *
 * This is NOT a GL implementation and is NEVER shipped to the device. It exists so
 * the COMMITTED host decoder (app/src/main/cpp/alr_gpu/alr_gpu_decode.hpp, which
 * does #include <GLES2/gl2.h>) compiles on a host that has no Khronos headers
 * (e.g. macOS). The harness's decode_check.cpp supplies recording implementations
 * of these prototypes, so decoding the shim's byte stream records the GL calls the
 * decoder would make and the harness asserts they match.
 *
 * Types/tokens mirror alr_khr_gles2.h (the guest side) so the two halves agree.
 * Only the subset alr_gpu_decode.hpp's decode_batch() actually references is here.
 */
#ifndef ALR_WIRECHECK_GLES2_GL2_H
#define ALR_WIRECHECK_GLES2_GL2_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void             GLvoid;
typedef unsigned int     GLenum;
typedef unsigned char    GLboolean;
typedef unsigned int     GLbitfield;
typedef int              GLint;
typedef int              GLsizei;
typedef unsigned char    GLubyte;
typedef unsigned int     GLuint;
typedef float            GLfloat;
typedef float            GLclampf;
typedef char             GLchar;
typedef intptr_t         GLintptr;
typedef ssize_t          GLsizeiptr;

/* tokens decode_batch() references */
#define GL_FALSE              0
#define GL_TRUE               1
#define GL_DEPTH_BUFFER_BIT   0x00000100
#define GL_COLOR_BUFFER_BIT   0x00004000
#define GL_SCISSOR_TEST       0x0C11
#define GL_UNPACK_ALIGNMENT   0x0CF5

/* every gl* entry point decode_batch() calls */
void   glViewport(GLint, GLint, GLsizei, GLsizei);
void   glClearColor(GLclampf, GLclampf, GLclampf, GLclampf);
void   glClear(GLbitfield);
void   glEnable(GLenum);
void   glDisable(GLenum);
void   glScissor(GLint, GLint, GLsizei, GLsizei);
GLuint glCreateShader(GLenum);
void   glShaderSource(GLuint, GLsizei, const GLchar *const *, const GLint *);
void   glCompileShader(GLuint);
GLuint glCreateProgram(void);
void   glAttachShader(GLuint, GLuint);
void   glBindAttribLocation(GLuint, GLuint, const GLchar *);
void   glLinkProgram(GLuint);
void   glUseProgram(GLuint);
void   glGenBuffers(GLsizei, GLuint *);
void   glBindBuffer(GLenum, GLuint);
void   glBufferData(GLenum, GLsizeiptr, const void *, GLenum);
void   glEnableVertexAttribArray(GLuint);
void   glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
void   glDrawArrays(GLenum, GLint, GLsizei);
void   glDrawElements(GLenum, GLsizei, GLenum, const void *);
GLint  glGetAttribLocation(GLuint, const GLchar *);
void   glCullFace(GLenum);
void   glFrontFace(GLenum);
GLint  glGetUniformLocation(GLuint, const GLchar *);
void   glUniformMatrix4fv(GLint, GLsizei, GLboolean, const GLfloat *);
void   glUniform1i(GLint, GLint);
void   glUniform1fv(GLint, GLsizei, const GLfloat *);
void   glUniform2fv(GLint, GLsizei, const GLfloat *);
void   glUniform3fv(GLint, GLsizei, const GLfloat *);
void   glUniform4fv(GLint, GLsizei, const GLfloat *);
void   glUniform1iv(GLint, GLsizei, const GLint *);
void   glUniform2iv(GLint, GLsizei, const GLint *);
void   glUniform3iv(GLint, GLsizei, const GLint *);
void   glUniform4iv(GLint, GLsizei, const GLint *);
void   glUniformMatrix2fv(GLint, GLsizei, GLboolean, const GLfloat *);
void   glUniformMatrix3fv(GLint, GLsizei, GLboolean, const GLfloat *);
void   glGenTextures(GLsizei, GLuint *);
void   glActiveTexture(GLenum);
void   glBindTexture(GLenum, GLuint);
void   glTexParameteri(GLenum, GLenum, GLint);
void   glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
void   glPixelStorei(GLenum, GLint);
void   glDepthFunc(GLenum);

#ifdef __cplusplus
}
#endif

#endif /* ALR_WIRECHECK_GLES2_GL2_H */
