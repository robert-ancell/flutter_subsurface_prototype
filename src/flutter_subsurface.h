#pragma once

#include <glib.h>
#include <gtk/gtk.h>
#include <wayland-client.h>

typedef struct _FlutterSubsurface FlutterSubsurface;

/**
 * flutter_subsurface_new:
 * @widget: the GTK widget that owns this subsurface (must be realized)
 *
 * Creates a Wayland subsurface attached to the parent surface of
 * @widget's toplevel window.
 *
 * Returns: a new subsurface, or %NULL on failure.
 */
FlutterSubsurface *flutter_subsurface_new(GtkWidget *widget);

void flutter_subsurface_free(FlutterSubsurface *subsurface);

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
