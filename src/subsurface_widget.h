#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SUBSURFACE_WIDGET_TYPE (subsurface_widget_get_type())
G_DECLARE_FINAL_TYPE(SubsurfaceWidget, subsurface_widget,
                     SUBSURFACE, WIDGET, GtkWidget)

GtkWidget  *subsurface_widget_new(void);

EGLDisplay  subsurface_widget_get_egl_display(SubsurfaceWidget *self);
EGLContext  subsurface_widget_get_egl_context(SubsurfaceWidget *self);

/**
 * SubsurfaceBackingStore:
 * @texture: OpenGL ES 2 texture name
 * @width: width of the texture in pixels
 * @height: height of the texture in pixels
 *
 * An off-screen render target owned by a #SubsurfaceWidget.
 * Obtain one with subsurface_widget_create_backing_store() and release it
 * with subsurface_widget_collect_backing_store() when it is no longer needed.
 */
typedef struct {
    GLuint texture;
    size_t width;
    size_t height;
} SubsurfaceBackingStore;

/**
 * subsurface_widget_create_backing_store:
 * @self: the widget
 * @width: texture width in pixels
 * @height: texture height in pixels
 *
 * Allocates a new #SubsurfaceBackingStore containing a GL_RGBA texture of
 * the given size.  Must be called with an EGL context that shares objects
 * with the widget's context current on the calling thread (e.g. from the
 * renderer thread).
 *
 * Returns: a newly allocated #SubsurfaceBackingStore; free with
 *          subsurface_widget_collect_backing_store().
 */
SubsurfaceBackingStore *subsurface_widget_create_backing_store(
    SubsurfaceWidget *self, size_t width, size_t height);

/**
 * subsurface_widget_collect_backing_store:
 * @self: the widget
 * @backing_store: the backing store to free
 *
 * Releases the GL texture and frees the #SubsurfaceBackingStore.  Must be
 * called with an EGL context that shares objects with the widget's context
 * current on the calling thread.
 */
void subsurface_widget_collect_backing_store(SubsurfaceWidget       *self,
                                             SubsurfaceBackingStore *backing_store);

/**
 * subsurface_widget_present:
 * @self: the widget
 * @texture_id: OpenGL ES 2 texture name, accessible from a context that
 *              shares objects with the widget's EGL context
 * @texture_format: internal format of the texture (e.g. %GL_RGBA)
 * @width: width of the texture in pixels
 * @height: height of the texture in pixels
 *
 * Blit @texture_id into the Wayland subsurface.  Safe to call from any
 * thread; rendering is dispatched to the GLib main context.  When called
 * faster than the main context can drain frames, only the most recent
 * call before each dispatch is rendered.
 */
void subsurface_widget_present(SubsurfaceWidget *self,
                               GLuint            texture_id,
                               GLenum            texture_format,
                               size_t            width,
                               size_t            height);

G_END_DECLS
