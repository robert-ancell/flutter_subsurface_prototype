#pragma once

#include "flutter_renderer.h"

typedef struct _Renderer Renderer;

/**
 * renderer_new:
 * @renderer: the renderer widget to present frames into
 *
 * Creates a renderer that runs on its own thread.  The renderer calls
 * flutter_renderer_make_current() to acquire a GL context, renders a rotating
 * RGB triangle into an FBO-backed texture, and calls flutter_renderer_present()
 * every ~16 ms.  The initial render size is taken from the widget's current
 * allocation.
 *
 * Returns: a new #Renderer, or %NULL on failure.
 */
Renderer *renderer_new(FlutterRenderer *renderer);

/**
 * renderer_resize:
 * @renderer: the renderer
 * @width: new render width in logical pixels
 * @height: new render height in logical pixels
 * @scale: scale factor (e.g. 2 for HiDPI)
 *
 * Schedules a resize of the render target.  The actual framebuffer size
 * is @width * @scale by @height * @scale.  Safe to call from any thread,
 * including the GTK main thread from a size-allocate handler.
 */
void renderer_resize(Renderer *renderer, size_t width, size_t height, gint scale);

/**
 * renderer_free:
 *
 * Stops the render thread, blocks until it exits, and frees all resources.
 * Must be called before the widget passed to renderer_new() is unrealized.
 */
void renderer_free(Renderer *renderer);
