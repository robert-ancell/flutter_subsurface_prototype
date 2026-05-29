#pragma once

#include "flutter_view.h"

G_BEGIN_DECLS

/**
 * FlutterSubsurfaceViewResizeFunc:
 * @width: new width in logical pixels
 * @height: new height in logical pixels
 * @scale: display scale factor
 * @user_data: data passed to flutter_subsurface_view_new()
 *
 * Called when the subsurface view is resized and needs a new frame.
 */
typedef void (*FlutterSubsurfaceViewResizeFunc)(size_t   width,
                                                size_t   height,
                                                gint     scale,
                                                gpointer user_data);

#define FLUTTER_SUBSURFACE_VIEW_TYPE (flutter_subsurface_view_get_type())
G_DECLARE_FINAL_TYPE(FlutterSubsurfaceView, flutter_subsurface_view,
                     FLUTTER, SUBSURFACE_VIEW, GtkWidget)

GtkWidget  *flutter_subsurface_view_new(FlutterSubsurfaceViewResizeFunc resize_func,
                                        gpointer                        resize_data);

EGLDisplay  flutter_subsurface_view_get_egl_display(FlutterSubsurfaceView *self);
EGLContext  flutter_subsurface_view_get_egl_context(FlutterSubsurfaceView *self);

G_END_DECLS
