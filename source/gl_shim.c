/* GL entry points that need adjusting before reaching Mesa. */

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <string.h>

#include "error.h"
#include "gl_shim.h"
#include "so_util.h"

#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif

static int g_have_etc1;

void gl_shim_init(void) {
  g_have_etc1 = 0;
  GLint count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint i = 0; i < count; i++) {
    const char *name = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
    if (name && !strcmp(name, "GL_OES_compressed_ETC1_RGB8_texture")) {
      g_have_etc1 = 1;
      break;
    }
  }
  trace("ETC1 texture extension %s", g_have_etc1 ? "present" : "absent (using ETC2)");
}

void gl_shim_CompressedTexImage2D(GLenum target, GLint level,
                                  GLenum internalformat, GLsizei width,
                                  GLsizei height, GLint border,
                                  GLsizei imageSize, const void *data) {
  /* Roughly half of BombSquad's Android textures are ETC1. Every valid ETC1
   * block is also a valid ETC2 RGB8 block with identical decoding, so where
   * the driver only advertises the GLES 3.0 core format the same bytes can
   * be handed over under the ETC2 name. */
  if (internalformat == GL_ETC1_RGB8_OES && !g_have_etc1)
    internalformat = GL_COMPRESSED_RGB8_ETC2;

  glCompressedTexImage2D(target, level, internalformat, width, height, border,
                         imageSize, data);
}

void *gl_shim_eglGetProcAddress(const char *name) {
  if (!name) return NULL;
  /* Anything the loader already re-points has to keep its shimmed address. */
  if (!strcmp(name, "glCompressedTexImage2D"))
    return (void *)gl_shim_CompressedTexImage2D;
  return (void *)eglGetProcAddress(name);
}
