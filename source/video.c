/* EGL context on the console's default window.
 *
 * The Switch changes resolution when it is docked, and the guest only learns
 * about that through nativeOnSurfaceChanged, so the mode is polled here and
 * surfaced to the frame loop.
 */

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdbool.h>
#include <switch.h>

#include "config.h"
#include "error.h"
#include "video.h"

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLContext g_context = EGL_NO_CONTEXT;
static EGLSurface g_surface = EGL_NO_SURFACE;
static EGLConfig g_config;

static int g_width = SURFACE_W_HANDHELD;
static int g_height = SURFACE_H_HANDHELD;
static AppletOperationMode g_mode;
static bool g_resize_pending;

static void size_for_mode(AppletOperationMode mode, int *width, int *height) {
  if (mode == AppletOperationMode_Console) {
    *width = SURFACE_W_DOCKED;
    *height = SURFACE_H_DOCKED;
  } else {
    *width = SURFACE_W_HANDHELD;
    *height = SURFACE_H_HANDHELD;
  }
}

static void apply_window_size(void) {
  NWindow *window = nwindowGetDefault();
  nwindowSetDimensions(window, (u32)g_width, (u32)g_height);
  nwindowSetTransform(window, 0u);
}

void video_init(void) {
  g_mode = appletGetOperationMode();
  size_for_mode(g_mode, &g_width, &g_height);
  apply_window_size();

  g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_display == EGL_NO_DISPLAY) fatal_error("eglGetDisplay failed.");
  if (!eglInitialize(g_display, NULL, NULL)) fatal_error("eglInitialize failed.");
  if (!eglBindAPI(EGL_OPENGL_ES_API)) fatal_error("eglBindAPI failed.");

  /* BombSquad's Android renderer targets GLES 3.0 and asks for a depth
   * buffer and 8-bit alpha; MSAA is left to the engine's own framebuffers. */
  static const EGLint config_attribs[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_DEPTH_SIZE, 24,
      EGL_STENCIL_SIZE, 8,
      EGL_NONE};

  EGLint num_configs = 0;
  if (!eglChooseConfig(g_display, config_attribs, &g_config, 1, &num_configs) ||
      num_configs < 1) {
    fatal_error("No suitable EGL config (GLES 3.0 with depth+stencil).");
  }

  g_surface = eglCreateWindowSurface(
      g_display, g_config, (EGLNativeWindowType)nwindowGetDefault(), NULL);
  if (g_surface == EGL_NO_SURFACE) fatal_error("eglCreateWindowSurface failed.");

  static const EGLint context_attribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
                                           EGL_CONTEXT_MINOR_VERSION, 0,
                                           EGL_NONE};
  g_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, context_attribs);
  if (g_context == EGL_NO_CONTEXT) fatal_error("eglCreateContext failed.");

  if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context))
    fatal_error("eglMakeCurrent failed.");

  eglSwapInterval(g_display, 1);

  trace("GL_VENDOR %s", (const char *)glGetString(GL_VENDOR));
  trace("GL_RENDERER %s", (const char *)glGetString(GL_RENDERER));
  trace("GL_VERSION %s", (const char *)glGetString(GL_VERSION));
  trace("GL_SHADING_LANGUAGE_VERSION %s",
        (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
}

/* The window's buffers are allocated when the surface is created, so a
 * resolution change means building a new surface. The context and every GL
 * object in it are kept, so the engine only has to hear about the new size. */
static void rebuild_surface(void) {
  eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(g_display, g_surface);
  g_surface = EGL_NO_SURFACE;

  apply_window_size();

  g_surface = eglCreateWindowSurface(
      g_display, g_config, (EGLNativeWindowType)nwindowGetDefault(), NULL);
  if (g_surface == EGL_NO_SURFACE)
    fatal_error("Could not recreate the display surface at %dx%d.", g_width,
                g_height);
  if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context))
    fatal_error("Could not bind the display surface at %dx%d.", g_width, g_height);
  eglSwapInterval(g_display, 1);
}

void video_swap(void) {
  eglSwapBuffers(g_display, g_surface);

  const AppletOperationMode mode = appletGetOperationMode();
  if (mode == g_mode) return;

  g_mode = mode;
  size_for_mode(mode, &g_width, &g_height);
  rebuild_surface();
  g_resize_pending = true;
  trace("display mode changed to %dx%d", g_width, g_height);
}

void video_shutdown(void) {
  if (g_display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (g_context != EGL_NO_CONTEXT) eglDestroyContext(g_display, g_context);
  if (g_surface != EGL_NO_SURFACE) eglDestroySurface(g_display, g_surface);
  eglTerminate(g_display);
  g_display = EGL_NO_DISPLAY;
}

int video_width(void) { return g_width; }
int video_height(void) { return g_height; }

bool video_take_resize(void) {
  if (!g_resize_pending) return false;
  g_resize_pending = false;
  return true;
}
