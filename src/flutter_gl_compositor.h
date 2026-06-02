#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <glib.h>
#include <wayland-client.h>

typedef struct _FlutterGLCompositor FlutterGLCompositor;

typedef struct {
    GLuint texture;
    size_t width;
    size_t height;
} FlutterBackingStore;

/**
 * flutter_gl_compositor_new_subsurface:
 * @wl_display: the Wayland display
 * @wl_surface: the Wayland surface for the subsurface
 * @width: initial width in logical pixels
 * @height: initial height in logical pixels
 * @scale: display scale factor
 *
 * Creates a compositor that renders to a Wayland subsurface via EGL.
 *
 * Returns: a new compositor, or %NULL on failure.
 */
FlutterGLCompositor *flutter_gl_compositor_new_subsurface(
    struct wl_display *wl_display,
    struct wl_surface *wl_surface,
    size_t width, size_t height, gint scale);

/**
 * flutter_gl_compositor_new_gdk:
 * @egl_display: the EGL display (from GDK)
 * @share_context: the GDK EGL context to share with
 *
 * Creates a compositor that shares textures with GDK for use with
 * gdk_cairo_draw_from_gl().
 *
 * Returns: a new compositor, or %NULL on failure.
 */
FlutterGLCompositor *flutter_gl_compositor_new_gdk(EGLDisplay egl_display,
                                                    EGLContext share_context);

void flutter_gl_compositor_free(FlutterGLCompositor *compositor);

/**
 * flutter_gl_compositor_make_current:
 *
 * Makes the renderer EGL context current on the calling thread.
 */
gboolean flutter_gl_compositor_make_current(FlutterGLCompositor *compositor);

/**
 * flutter_gl_compositor_clear_current:
 *
 * Releases the EGL context from the calling thread.
 */
void flutter_gl_compositor_clear_current(FlutterGLCompositor *compositor);

/**
 * flutter_gl_compositor_create_backing_store:
 *
 * Creates an OpenGL texture to use as a render target.
 */
FlutterBackingStore *flutter_gl_compositor_create_backing_store(
    FlutterGLCompositor *compositor, size_t width, size_t height);

/**
 * flutter_gl_compositor_collect_backing_store:
 *
 * Deletes a backing store previously created by create_backing_store.
 */
void flutter_gl_compositor_collect_backing_store(
    FlutterGLCompositor *compositor, FlutterBackingStore *store);

/**
 * flutter_gl_compositor_present:
 *
 * Blits a texture to the EGL surface and swaps buffers (subsurface mode).
 * Restores the renderer context afterwards.
 */
void flutter_gl_compositor_present(FlutterGLCompositor *compositor,
                                    GLuint texture_id, GLenum texture_format,
                                    size_t width, size_t height);

/**
 * flutter_gl_compositor_resize:
 *
 * Resizes the EGL window (subsurface mode only).
 */
void flutter_gl_compositor_resize(FlutterGLCompositor *compositor,
                                   size_t width, size_t height);

/**
 * flutter_gl_compositor_render_clear:
 *
 * Renders a solid dark frame and swaps (subsurface mode only).
 * Used as the initial frame before the renderer starts.
 */
void flutter_gl_compositor_render_clear(FlutterGLCompositor *compositor,
                                         size_t width, size_t height);
