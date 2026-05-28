#pragma once

#include "subsurface_widget.h"

typedef struct _Renderer Renderer;

/**
 * renderer_new:
 * @widget: the subsurface widget to present frames into
 * @width: render width in pixels
 * @height: render height in pixels
 *
 * Creates a renderer that runs on its own thread.  The renderer owns an
 * EGL context that shares objects with @widget's context, renders a
 * rotating RGB triangle into an FBO-backed texture, and calls
 * subsurface_widget_present() every 10 ms.
 *
 * Returns: a new #Renderer, or %NULL on failure.
 */
Renderer *renderer_new(SubsurfaceWidget *widget, size_t width, size_t height);

/**
 * renderer_free:
 *
 * Stops the render thread, blocks until it exits, and frees all resources.
 * Must be called before the @widget passed to renderer_new() is unrealized.
 */
void renderer_free(Renderer *renderer);
