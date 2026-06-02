#pragma once

#include <gtk/gtk.h>

#include "flutter_renderer.h"

G_BEGIN_DECLS

#define FLUTTER_TYPE_SOFTWARE_RENDERER (flutter_software_renderer_get_type())
G_DECLARE_FINAL_TYPE(FlutterSoftwareRenderer, flutter_software_renderer,
                     FLUTTER, SOFTWARE_RENDERER, GtkDrawingArea)

/**
 * flutter_software_renderer_new:
 * @resize_func: (nullable): callback invoked when the view is resized
 * @resize_data: user data passed to @resize_func
 *
 * Creates a new #FlutterSoftwareRenderer widget that renders software
 * backing stores using Cairo.
 *
 * Returns: (transfer floating): the new widget
 */
GtkWidget *flutter_software_renderer_new(FlutterRendererResizeFunc resize_func,
                                          gpointer                  resize_data);

G_END_DECLS
