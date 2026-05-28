#pragma once

#include "flutter_view.h"

G_BEGIN_DECLS

#define FLUTTER_GL_AREA_VIEW_TYPE (flutter_gl_area_view_get_type())
G_DECLARE_FINAL_TYPE(FlutterGLAreaView, flutter_gl_area_view,
                     FLUTTER, GL_AREA_VIEW, GtkGLArea)

GtkWidget  *flutter_gl_area_view_new(void);

EGLDisplay  flutter_gl_area_view_get_egl_display(FlutterGLAreaView *self);
EGLContext  flutter_gl_area_view_get_egl_context(FlutterGLAreaView *self);

G_END_DECLS
