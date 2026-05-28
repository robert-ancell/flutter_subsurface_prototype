#pragma once

#include "flutter_view.h"

G_BEGIN_DECLS

#define FLUTTER_SUBSURFACE_VIEW_TYPE (flutter_subsurface_view_get_type())
G_DECLARE_FINAL_TYPE(FlutterSubsurfaceView, flutter_subsurface_view,
                     FLUTTER, SUBSURFACE_VIEW, GtkWidget)

GtkWidget  *flutter_subsurface_view_new(void);

EGLDisplay  flutter_subsurface_view_get_egl_display(FlutterSubsurfaceView *self);
EGLContext  flutter_subsurface_view_get_egl_context(FlutterSubsurfaceView *self);

FlutterBackingStore *flutter_subsurface_view_create_backing_store(
    FlutterSubsurfaceView *self, size_t width, size_t height);

void flutter_subsurface_view_collect_backing_store(FlutterSubsurfaceView *self,
                                                   FlutterBackingStore   *backing_store);

void flutter_subsurface_view_present(FlutterSubsurfaceView *self,
                                     GLuint                 texture_id,
                                     GLenum                 texture_format,
                                     size_t                 width,
                                     size_t                 height);

G_END_DECLS
