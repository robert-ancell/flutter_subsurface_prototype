#include "flutter_view_resize.h"

struct _FlutterViewResize {
    GMutex   mutex;
    GCond    cond;
    gboolean done;
    size_t   expected_width;
    size_t   expected_height;
};

FlutterViewResize *flutter_view_resize_new(void) {
    FlutterViewResize *self = g_new0(FlutterViewResize, 1);
    g_mutex_init(&self->mutex);
    g_cond_init(&self->cond);
    return self;
}

void flutter_view_resize_free(FlutterViewResize *self) {
    if (!self)
        return;
    g_mutex_clear(&self->mutex);
    g_cond_clear(&self->cond);
    g_free(self);
}

void flutter_view_resize_wait(FlutterViewResize *self,
                              size_t width, size_t height) {
    g_mutex_lock(&self->mutex);
    self->done = FALSE;
    self->expected_width = width;
    self->expected_height = height;
    g_mutex_unlock(&self->mutex);

    gint64 deadline = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
    g_mutex_lock(&self->mutex);
    while (!self->done) {
        if (!g_cond_wait_until(&self->cond, &self->mutex, deadline))
            break;
    }
    g_mutex_unlock(&self->mutex);
}

void flutter_view_resize_notify(FlutterViewResize *self,
                                size_t width, size_t height) {
    g_mutex_lock(&self->mutex);
    if (!self->done &&
        width == self->expected_width &&
        height == self->expected_height) {
        self->done = TRUE;
        g_cond_signal(&self->cond);
    }
    g_mutex_unlock(&self->mutex);
}
