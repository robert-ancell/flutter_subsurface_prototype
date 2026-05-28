#pragma once

#include "flutter_subsurface_view.h"

typedef struct _Renderer Renderer;

/**
 * renderer_new:
 * @widget: the subsurface widget to present frames into
 *
 * Creates a renderer that runs on its own thread.  The renderer owns an
 * EGL context that shares objects with @widget's context, renders a
 * rotating RGB triangle into an FBO-backed texture, and calls
 * flutter_subsurface_view_present() every 10 ms.  The initial render size is
 * taken from @widget's current allocation.
 *
 * Returns: a new #Renderer, or %NULL on failure.
 */
Renderer *renderer_new(FlutterSubsurfaceView *widget);

/**
 * renderer_resize:
 * @renderer: the renderer
 * @width: new render width in pixels
 * @height: new render height in pixels
 *
 * Schedules a resize of the render target.  Safe to call from any thread,
 * including the GTK main thread from a size-allocate handler.
 */
void renderer_resize(Renderer *renderer, size_t width, size_t height);

/**
 * renderer_free:
 *
 * Stops the render thread, blocks until it exits, and frees all resources.
 * Must be called before the @widget passed to renderer_new() is unrealized.
 */
void renderer_free(Renderer *renderer);
