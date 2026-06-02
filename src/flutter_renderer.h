#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gtk/gtk.h>

#include "embedder.h"

G_BEGIN_DECLS

/**
 * FlutterRendererResizeFunc:
 * @width: new width in logical pixels
 * @height: new height in logical pixels
 * @scale: display scale factor
 * @user_data: data passed to the renderer constructor
 *
 * Called when the renderer widget is resized and needs a new frame.
 */
typedef void (*FlutterRendererResizeFunc)(size_t   width,
                                          size_t   height,
                                          gint     scale,
                                          gpointer user_data);

#define FLUTTER_TYPE_RENDERER (flutter_renderer_get_type())
G_DECLARE_INTERFACE(FlutterRenderer, flutter_renderer, FLUTTER, RENDERER, GtkWidget)

struct _FlutterRendererInterface {
    GTypeInterface parent_iface;

    FlutterBackingStore *(*create_backing_store)(FlutterRenderer *self,
                                                 size_t           width,
                                                 size_t           height);
    void (*collect_backing_store)(FlutterRenderer     *self,
                                  FlutterBackingStore *backing_store);
    void (*present)(FlutterRenderer *self,
                    GLuint           texture_id,
                    GLenum           texture_format,
                    size_t           width,
                    size_t           height);
    gboolean (*make_current)(FlutterRenderer *self);
    void     (*clear_current)(FlutterRenderer *self);
};

/**
 * flutter_renderer_create_backing_store:
 * @self: the renderer
 * @width: texture width in pixels
 * @height: texture height in pixels
 *
 * Allocates an OpenGL texture for the render thread to draw into.
 * Must be called from a thread where the renderer GL context is current.
 *
 * Returns: (transfer full): a newly allocated backing store
 */
FlutterBackingStore *flutter_renderer_create_backing_store(FlutterRenderer *self,
                                                            size_t           width,
                                                            size_t           height);

/**
 * flutter_renderer_collect_backing_store:
 * @self: the renderer
 * @backing_store: (transfer full): the backing store to release
 *
 * Frees a backing store previously created with
 * flutter_renderer_create_backing_store(). Must be called from a thread
 * where the renderer GL context is current.
 */
void flutter_renderer_collect_backing_store(FlutterRenderer     *self,
                                             FlutterBackingStore *backing_store);

/**
 * flutter_renderer_present:
 * @self: the renderer
 * @texture_id: OpenGL texture containing the rendered frame
 * @texture_format: GL format of the texture (e.g. %GL_RGBA)
 * @width: frame width in pixels
 * @height: frame height in pixels
 *
 * Presents a rendered frame to the display.
 * Must be called from the render thread.
 */
void flutter_renderer_present(FlutterRenderer *self,
                               GLuint           texture_id,
                               GLenum           texture_format,
                               size_t           width,
                               size_t           height);

/**
 * flutter_renderer_make_current:
 * @self: the renderer
 *
 * Makes the renderer GL context current on the calling thread.
 *
 * Returns: %TRUE on success
 */
gboolean flutter_renderer_make_current(FlutterRenderer *self);

/**
 * flutter_renderer_clear_current:
 * @self: the renderer
 *
 * Releases the renderer GL context from the calling thread.
 */
void flutter_renderer_clear_current(FlutterRenderer *self);

G_END_DECLS
