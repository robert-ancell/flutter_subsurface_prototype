#pragma once

#include "flutter_view.h"

G_BEGIN_DECLS

#define FLUTTER_GL_VIEW_TYPE (flutter_gl_view_get_type())
G_DECLARE_FINAL_TYPE(FlutterGLView, flutter_gl_view,
                     FLUTTER, GL_VIEW, GtkDrawingArea)

GtkWidget *flutter_gl_view_new(void);

G_END_DECLS
