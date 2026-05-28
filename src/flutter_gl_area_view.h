#pragma once

#include "flutter_view.h"

G_BEGIN_DECLS

#define FLUTTER_GL_AREA_VIEW_TYPE (flutter_gl_area_view_get_type())
G_DECLARE_FINAL_TYPE(FlutterGLAreaView, flutter_gl_area_view,
                     FLUTTER, GL_AREA_VIEW, GtkGLArea)

GtkWidget  *flutter_gl_area_view_new(void);

EGLDisplay  flutter_gl_area_view_get_egl_display(FlutterGLAreaView *self);
EGLContext  flutter_gl_area_view_get_egl_context(FlutterGLAreaView *self);

FlutterBackingStore *flutter_gl_area_view_create_backing_store(
    FlutterGLAreaView *self, size_t width, size_t height);

void flutter_gl_area_view_collect_backing_store(FlutterGLAreaView   *self,
                                                FlutterBackingStore *backing_store);

void flutter_gl_area_view_present(FlutterGLAreaView *self,
                                  GLuint             texture_id,
                                  GLenum             texture_format,
                                  size_t             width,
                                  size_t             height);

G_END_DECLS
