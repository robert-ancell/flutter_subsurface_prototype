#pragma once

#include <gtk/gtk.h>

#include "flutter_renderer.h"

G_BEGIN_DECLS

#define FLUTTER_TYPE_GL_RENDERER (flutter_gl_renderer_get_type())
G_DECLARE_FINAL_TYPE(FlutterGLRenderer, flutter_gl_renderer,
                     FLUTTER, GL_RENDERER, GtkDrawingArea)

/**
 * flutter_gl_renderer_new:
 * @resize_func: (nullable): callback invoked when the view is resized
 * @resize_data: user data passed to @resize_func
 *
 * Creates a new #FlutterGLRenderer widget that renders using
 * gdk_cairo_draw_from_gl.
 *
 * Returns: (transfer floating): the new widget
 */
GtkWidget *flutter_gl_renderer_new(FlutterRendererResizeFunc resize_func,
                                    gpointer                  resize_data);

G_END_DECLS
