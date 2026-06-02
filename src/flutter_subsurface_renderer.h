#pragma once

#include <gtk/gtk.h>

#include "flutter_renderer.h"

G_BEGIN_DECLS

#define FLUTTER_TYPE_SUBSURFACE_RENDERER (flutter_subsurface_renderer_get_type())
G_DECLARE_FINAL_TYPE(FlutterSubsurfaceRenderer, flutter_subsurface_renderer,
                     FLUTTER, SUBSURFACE_RENDERER, GtkDrawingArea)

/**
 * flutter_subsurface_renderer_new:
 * @resize_func: (nullable): callback invoked when the view is resized
 * @resize_data: user data passed to @resize_func
 *
 * Creates a new #FlutterSubsurfaceRenderer widget that renders via a
 * Wayland subsurface.
 *
 * Returns: (transfer floating): the new widget
 */
GtkWidget *flutter_subsurface_renderer_new(FlutterRendererResizeFunc resize_func,
                                            gpointer                  resize_data);

G_END_DECLS
