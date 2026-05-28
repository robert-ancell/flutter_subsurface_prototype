#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define SUBSURFACE_WIDGET_TYPE (subsurface_widget_get_type())
G_DECLARE_FINAL_TYPE(SubsurfaceWidget, subsurface_widget,
                     SUBSURFACE, WIDGET, GtkWidget)

GtkWidget *subsurface_widget_new(void);

G_END_DECLS
