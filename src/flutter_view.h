#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * FlutterBackingStore:
 * @texture: OpenGL ES 2 texture name
 * @width: width of the texture in pixels
 * @height: height of the texture in pixels
 *
 * An off-screen render target managed by a #FlutterView.
 */
typedef struct {
    GLuint texture;
    size_t width;
    size_t height;
} FlutterBackingStore;

/**
 * FlutterViewResizeFunc:
 * @width: new width in logical pixels
 * @height: new height in logical pixels
 * @scale: display scale factor
 * @user_data: data passed to flutter_view_new()
 *
 * Called when the view is resized and needs a new frame.
 */
typedef void (*FlutterViewResizeFunc)(size_t   width,
                                      size_t   height,
                                      gint     scale,
                                      gpointer user_data);

#define FLUTTER_TYPE_VIEW (flutter_view_get_type())
G_DECLARE_FINAL_TYPE(FlutterView, flutter_view, FLUTTER, VIEW, GtkDrawingArea)

GtkWidget *flutter_view_new(gboolean              use_subsurface,
                            FlutterViewResizeFunc  resize_func,
                            gpointer              resize_data);

FlutterBackingStore *flutter_view_create_backing_store(FlutterView *self,
                                                       size_t       width,
                                                       size_t       height);

void flutter_view_collect_backing_store(FlutterView         *self,
                                        FlutterBackingStore *backing_store);

void flutter_view_present(FlutterView *self,
                          GLuint       texture_id,
                          GLenum       texture_format,
                          size_t       width,
                          size_t       height);

gboolean flutter_view_make_current(FlutterView *self);
void     flutter_view_clear_current(FlutterView *self);

G_END_DECLS
