#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <glib.h>

typedef struct _FlutterGLCompositor FlutterGLCompositor;

typedef struct {
    GLuint texture;
    size_t width;
    size_t height;
} FlutterBackingStore;

/**
 * flutter_gl_compositor_new:
 * @egl_display: the EGL display
 * @share_context: the EGL context to share with
 *
 * Creates a compositor that provides a renderer EGL context sharing
 * objects with @share_context, and manages backing store textures.
 *
 * Returns: a new compositor, or %NULL on failure.
 */
FlutterGLCompositor *flutter_gl_compositor_new(EGLDisplay egl_display,
                                               EGLContext share_context);

void flutter_gl_compositor_free(FlutterGLCompositor *compositor);

/**
 * flutter_gl_compositor_get_egl_display:
 *
 * Returns the EGL display used by this compositor.
 */
EGLDisplay flutter_gl_compositor_get_egl_display(FlutterGLCompositor *compositor);

/**
 * flutter_gl_compositor_make_current:
 *
 * Makes the renderer EGL context current on the calling thread.
 */
gboolean flutter_gl_compositor_make_current(FlutterGLCompositor *compositor);

/**
 * flutter_gl_compositor_clear_current:
 *
 * Releases the EGL context from the calling thread.
 */
void flutter_gl_compositor_clear_current(FlutterGLCompositor *compositor);

/**
 * flutter_gl_compositor_create_backing_store:
 *
 * Creates an OpenGL texture to use as a render target.
 */
FlutterBackingStore *flutter_gl_compositor_create_backing_store(
    FlutterGLCompositor *compositor, size_t width, size_t height);

/**
 * flutter_gl_compositor_collect_backing_store:
 *
 * Deletes a backing store previously created by create_backing_store.
 */
void flutter_gl_compositor_collect_backing_store(
    FlutterGLCompositor *compositor, FlutterBackingStore *store);
