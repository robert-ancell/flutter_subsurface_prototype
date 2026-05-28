#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define FLUTTER_SUBSURFACE_VIEW_TYPE (flutter_subsurface_view_get_type())
G_DECLARE_FINAL_TYPE(FlutterSubsurfaceView, flutter_subsurface_view,
                     FLUTTER, SUBSURFACE_VIEW, GtkWidget)

GtkWidget  *flutter_subsurface_view_new(void);

EGLDisplay  flutter_subsurface_view_get_egl_display(FlutterSubsurfaceView *self);
EGLContext  flutter_subsurface_view_get_egl_context(FlutterSubsurfaceView *self);

/**
 * SubsurfaceBackingStore:
 * @texture: OpenGL ES 2 texture name
 * @width: width of the texture in pixels
 * @height: height of the texture in pixels
 *
 * An off-screen render target owned by a #FlutterSubsurfaceView.
 * Obtain one with flutter_subsurface_view_create_backing_store() and release it
 * with flutter_subsurface_view_collect_backing_store() when it is no longer needed.
 */
typedef struct {
    GLuint texture;
    size_t width;
    size_t height;
} SubsurfaceBackingStore;

/**
 * flutter_subsurface_view_create_backing_store:
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
 *          flutter_subsurface_view_collect_backing_store().
 */
SubsurfaceBackingStore *flutter_subsurface_view_create_backing_store(
    FlutterSubsurfaceView *self, size_t width, size_t height);

/**
 * flutter_subsurface_view_collect_backing_store:
 * @self: the widget
 * @backing_store: the backing store to free
 *
 * Releases the GL texture and frees the #SubsurfaceBackingStore.  Must be
 * called with an EGL context that shares objects with the widget's context
 * current on the calling thread.
 */
void flutter_subsurface_view_collect_backing_store(FlutterSubsurfaceView       *self,
                                             SubsurfaceBackingStore *backing_store);

/**
 * flutter_subsurface_view_present:
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
void flutter_subsurface_view_present(FlutterSubsurfaceView *self,
                               GLuint            texture_id,
                               GLenum            texture_format,
                               size_t            width,
                               size_t            height);

G_END_DECLS
