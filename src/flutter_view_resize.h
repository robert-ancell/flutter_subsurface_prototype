#pragma once

#include <glib.h>

typedef struct _FlutterViewResize FlutterViewResize;

/**
 * flutter_view_resize_new:
 *
 * Creates a resize synchronization object.
 */
FlutterViewResize *flutter_view_resize_new(void);

void flutter_view_resize_free(FlutterViewResize *resize);

/**
 * flutter_view_resize_wait:
 * @resize: the resize object
 * @width: expected frame width in pixels
 * @height: expected frame height in pixels
 *
 * Blocks until flutter_view_resize_notify() is called with matching
 * dimensions, or until a 100ms timeout expires.
 */
void flutter_view_resize_wait(FlutterViewResize *resize,
                              size_t width, size_t height);

/**
 * flutter_view_resize_notify:
 * @resize: the resize object
 * @width: rendered frame width in pixels
 * @height: rendered frame height in pixels
 *
 * Signals the resize object that a frame of the given size has been
 * delivered. If the size matches what flutter_view_resize_wait() is
 * waiting for, the blocked thread is woken.
 */
void flutter_view_resize_notify(FlutterViewResize *resize,
                                size_t width, size_t height);
