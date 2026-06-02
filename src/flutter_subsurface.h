#pragma once

#include <glib-object.h>
#include <gtk/gtk.h>
#include <wayland-client.h>

G_BEGIN_DECLS

#define FLUTTER_TYPE_SUBSURFACE (flutter_subsurface_get_type())
G_DECLARE_FINAL_TYPE(FlutterSubsurface, flutter_subsurface, FLUTTER, SUBSURFACE, GObject)

/**
 * flutter_subsurface_new:
 * @widget: the GTK widget that owns this subsurface (must be realized)
 *
 * Creates a Wayland subsurface attached to the parent surface of
 * @widget's toplevel window.
 *
 * Returns: (transfer full): a new subsurface, or %NULL on failure.
 */
FlutterSubsurface *flutter_subsurface_new(GtkWidget *widget);

/**
 * flutter_subsurface_get_surface:
 *
 * Returns the wl_surface for this subsurface (for EGL window creation, etc).
 */
struct wl_surface *flutter_subsurface_get_surface(FlutterSubsurface *subsurface);

/**
 * flutter_subsurface_set_position:
 *
 * Updates the subsurface position relative to the parent surface.
 */
void flutter_subsurface_set_position(FlutterSubsurface *subsurface,
                                     gint x, gint y);

G_END_DECLS
