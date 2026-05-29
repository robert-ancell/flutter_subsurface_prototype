#pragma once

#include "flutter_view.h"

G_BEGIN_DECLS

#define FLUTTER_GL_AREA_VIEW_TYPE (flutter_gl_area_view_get_type())
G_DECLARE_FINAL_TYPE(FlutterGLAreaView, flutter_gl_area_view,
                     FLUTTER, GL_AREA_VIEW, GtkDrawingArea)

GtkWidget *flutter_gl_area_view_new(void);

G_END_DECLS
