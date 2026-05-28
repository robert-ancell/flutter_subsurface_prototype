#pragma once

#include <GLES2/gl2.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SUBSURFACE_WIDGET_TYPE (subsurface_widget_get_type())
G_DECLARE_FINAL_TYPE(SubsurfaceWidget, subsurface_widget,
                     SUBSURFACE, WIDGET, GtkWidget)

GtkWidget *subsurface_widget_new(void);

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
                               gint              width,
                               gint              height);

G_END_DECLS
