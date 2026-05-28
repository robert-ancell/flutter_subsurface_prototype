#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define FLUTTER_GL_AREA_VIEW_TYPE (flutter_gl_area_view_get_type())
G_DECLARE_FINAL_TYPE(FlutterGLAreaView, flutter_gl_area_view,
                     FLUTTER, GL_AREA_VIEW, GtkGLArea)

GtkWidget  *flutter_gl_area_view_new(void);

EGLDisplay  flutter_gl_area_view_get_egl_display(FlutterGLAreaView *self);
EGLContext  flutter_gl_area_view_get_egl_context(FlutterGLAreaView *self);

/**
 * FlutterGLAreaBackingStore:
 * @texture: OpenGL ES 2 texture name
 * @width: width of the texture in pixels
 * @height: height of the texture in pixels
 *
 * An off-screen render target owned by a #FlutterGLAreaView.
 * Obtain one with flutter_gl_area_view_create_backing_store() and release it
 * with flutter_gl_area_view_collect_backing_store() when it is no longer needed.
 */
typedef struct {
    GLuint texture;
    size_t width;
    size_t height;
} FlutterGLAreaBackingStore;

/**
 * flutter_gl_area_view_create_backing_store:
 * @self: the widget
 * @width: texture width in pixels
 * @height: texture height in pixels
 *
 * Allocates a new #FlutterGLAreaBackingStore containing a GL_RGBA texture of
 * the given size.  Must be called with an EGL context that shares objects
 * with the widget's context current on the calling thread (e.g. from the
 * renderer thread).
 *
 * Returns: a newly allocated #FlutterGLAreaBackingStore; free with
 *          flutter_gl_area_view_collect_backing_store().
 */
FlutterGLAreaBackingStore *flutter_gl_area_view_create_backing_store(
    FlutterGLAreaView *self, size_t width, size_t height);

/**
 * flutter_gl_area_view_collect_backing_store:
 * @self: the widget
 * @backing_store: the backing store to free
 *
 * Releases the GL texture and frees the #FlutterGLAreaBackingStore.  Must be
 * called with an EGL context that shares objects with the widget's context
 * current on the calling thread.
 */
void flutter_gl_area_view_collect_backing_store(FlutterGLAreaView         *self,
                                                FlutterGLAreaBackingStore *backing_store);

/**
 * flutter_gl_area_view_present:
 * @self: the widget
 * @texture_id: OpenGL ES 2 texture name, accessible from a context that
 *              shares objects with the widget's EGL context
 * @texture_format: internal format of the texture (e.g. %GL_RGBA)
 * @width: width of the texture in pixels
 * @height: height of the texture in pixels
 *
 * Blit @texture_id into the GL area.  Safe to call from any thread;
 * rendering is dispatched to the GLib main context.  When called faster
 * than the main context can drain frames, only the most recent call before
 * each dispatch is rendered.
 */
void flutter_gl_area_view_present(FlutterGLAreaView *self,
                                  GLuint             texture_id,
                                  GLenum             texture_format,
                                  size_t             width,
                                  size_t             height);

G_END_DECLS
