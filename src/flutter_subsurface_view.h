#pragma once

#include "flutter_view.h"

G_BEGIN_DECLS

typedef struct _Renderer Renderer;

#define FLUTTER_SUBSURFACE_VIEW_TYPE (flutter_subsurface_view_get_type())
G_DECLARE_FINAL_TYPE(FlutterSubsurfaceView, flutter_subsurface_view,
                     FLUTTER, SUBSURFACE_VIEW, GtkWidget)

GtkWidget  *flutter_subsurface_view_new(void);
void        flutter_subsurface_view_set_renderer(FlutterSubsurfaceView *self,
                                                 Renderer              *renderer);

EGLDisplay  flutter_subsurface_view_get_egl_display(FlutterSubsurfaceView *self);
EGLContext  flutter_subsurface_view_get_egl_context(FlutterSubsurfaceView *self);

G_END_DECLS
