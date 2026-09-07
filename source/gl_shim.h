/* GL entry points that need adjusting before reaching Mesa. */

#ifndef BSNX_GL_SHIM_H
#define BSNX_GL_SHIM_H

#include <GLES3/gl3.h>

/* Probe driver capabilities once a context is current. */
void gl_shim_init(void);

void gl_shim_CompressedTexImage2D(GLenum target, GLint level,
                                  GLenum internalformat, GLsizei width,
                                  GLsizei height, GLint border,
                                  GLsizei imageSize, const void *data);
void *gl_shim_eglGetProcAddress(const char *name);

#endif
