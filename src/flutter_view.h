#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gtk/gtk.h>

#include "flutter_gl_compositor.h"

G_BEGIN_DECLS

/**
 * FlutterViewResizeFunc:
 * @width: new width in logical pixels
 * @height: new height in logical pixels
 * @scale: display scale factor
 * @user_data: data passed to flutter_view_new()
 *
 * Called when the view is resized and needs a new frame.
 */
typedef void (*FlutterViewResizeFunc)(size_t   width,
                                      size_t   height,
                                      gint     scale,
                                      gpointer user_data);

#define FLUTTER_TYPE_VIEW (flutter_view_get_type())
G_DECLARE_FINAL_TYPE(FlutterView, flutter_view, FLUTTER, VIEW, GtkDrawingArea)

/**
 * flutter_view_new:
 * @use_subsurface: if %TRUE, render via a Wayland subsurface;
 *   if %FALSE, render using gdk_cairo_draw_from_gl
 * @resize_func: (nullable): callback invoked when the view is resized
 * @resize_data: user data passed to @resize_func
 *
 * Creates a new #FlutterView widget.
 *
 * Returns: (transfer floating): the new widget
 */
GtkWidget *flutter_view_new(gboolean              use_subsurface,
                            FlutterViewResizeFunc  resize_func,
                            gpointer              resize_data);

/**
 * flutter_view_create_backing_store:
 * @self: the view
 * @width: texture width in pixels
 * @height: texture height in pixels
 *
 * Allocates an OpenGL texture for the renderer to draw into.
 * Must be called from a thread where the renderer GL context is current.
 *
 * Returns: (transfer full): a newly allocated backing store
 */
FlutterBackingStore *flutter_view_create_backing_store(FlutterView *self,
                                                       size_t       width,
                                                       size_t       height);

/**
 * flutter_view_collect_backing_store:
 * @self: the view
 * @backing_store: (transfer full): the backing store to release
 *
 * Frees a backing store previously created with
 * flutter_view_create_backing_store(). Must be called from a thread
 * where the renderer GL context is current.
 */
void flutter_view_collect_backing_store(FlutterView         *self,
                                        FlutterBackingStore *backing_store);

/**
 * flutter_view_present:
 * @self: the view
 * @texture_id: OpenGL texture containing the rendered frame
 * @texture_format: GL format of the texture (e.g. %GL_RGBA)
 * @width: frame width in pixels
 * @height: frame height in pixels
 *
 * Presents a rendered frame to the display. In subsurface mode the
 * texture is blitted to the subsurface EGL surface; in GDK mode it
 * is scheduled for compositing via gdk_cairo_draw_from_gl().
 *
 * Must be called from the renderer thread.
 */
void flutter_view_present(FlutterView *self,
                          GLuint       texture_id,
                          GLenum       texture_format,
                          size_t       width,
                          size_t       height);

/**
 * flutter_view_make_current:
 * @self: the view
 *
 * Makes the renderer GL context current on the calling thread.
 *
 * Returns: %TRUE on success
 */
gboolean flutter_view_make_current(FlutterView *self);

/**
 * flutter_view_clear_current:
 * @self: the view
 *
 * Releases the renderer GL context from the calling thread.
 */
void     flutter_view_clear_current(FlutterView *self);

G_END_DECLS
