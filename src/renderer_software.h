#pragma once

#include "flutter_renderer.h"

typedef struct _RendererSoftware RendererSoftware;

/**
 * renderer_software_new:
 * @renderer: the renderer widget to present frames into
 *
 * Creates a software renderer that runs on its own thread. Renders a
 * rotating RGB triangle into an ARGB32 pixel buffer and calls
 * flutter_renderer_present() every ~16 ms.
 *
 * Returns: a new #RendererSoftware, or %NULL on failure.
 */
RendererSoftware *renderer_software_new(FlutterRenderer *renderer);

/**
 * renderer_software_resize:
 * @renderer: the renderer
 * @width: new render width in logical pixels
 * @height: new render height in logical pixels
 * @scale: scale factor (e.g. 2 for HiDPI)
 *
 * Schedules a resize of the render target.
 */
void renderer_software_resize(RendererSoftware *renderer,
                               size_t width, size_t height, gint scale);

/**
 * renderer_software_free:
 *
 * Stops the render thread, blocks until it exits, and frees all resources.
 */
void renderer_software_free(RendererSoftware *renderer);
