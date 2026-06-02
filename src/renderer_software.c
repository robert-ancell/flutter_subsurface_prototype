#include "renderer_software.h"

#include <gtk/gtk.h>
#include <math.h>
#include <string.h>

struct _RendererSoftware {
    FlutterRenderer *renderer;

    size_t width;
    size_t height;

    FlutterBackingStore *backing_store;

    /* Thread control */
    GThread  *thread;
    GMutex    mutex;
    GCond     cond;
    gboolean  running;

    /* Pending resize, protected by mutex */
    gboolean resize_pending;
    size_t   pending_width;
    size_t   pending_height;
};

/* ── Software rasterization helpers ───────────────────────────────────────── */

static inline guint32 make_argb(guint8 r, guint8 g, guint8 b, guint8 a) {
    return ((guint32)a << 24) | ((guint32)r << 16) |
           ((guint32)g << 8)  | (guint32)b;
}

/* Compute the sign of (p2-p1) x (p-p1) for point-in-triangle testing. */
static inline float edge_func(float x1, float y1,
                               float x2, float y2,
                               float px, float py) {
    return (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
}

static void render_frame(RendererSoftware *r, float angle) {
    int w = (int)r->width;
    int h = (int)r->height;
    int stride = w * 4;
    guint8 *buf = r->backing_store->software.buffer;

    /* Clear to dark background */
    guint32 bg = make_argb(20, 20, 31, 255);
    guint32 *pixels = (guint32 *)buf;
    for (int i = 0; i < w * h; i++)
        pixels[i] = bg;

    /* Triangle vertices in NDC [-1,1], same as GL renderer */
    static const float verts[][2] = {
        { 0.0f,  0.6f},
        {-0.6f, -0.4f},
        { 0.6f, -0.4f},
    };
    /* Vertex colours (RGB) */
    static const float colors[][3] = {
        {1.0f, 0.2f, 0.2f},
        {0.2f, 1.0f, 0.2f},
        {0.2f, 0.2f, 1.0f},
    };

    float cosA = cosf(angle);
    float sinA = sinf(angle);

    /* Transform vertices: rotate then map from NDC to pixel coords */
    float px[3], py[3];
    for (int i = 0; i < 3; i++) {
        float rx = verts[i][0] * cosA - verts[i][1] * sinA;
        float ry = verts[i][0] * sinA + verts[i][1] * cosA;
        px[i] = (rx * 0.5f + 0.5f) * (float)w;
        py[i] = (0.5f - ry * 0.5f) * (float)h;
    }

    /* Bounding box */
    int min_x = (int)fminf(fminf(px[0], px[1]), px[2]);
    int max_x = (int)ceilf(fmaxf(fmaxf(px[0], px[1]), px[2]));
    int min_y = (int)fminf(fminf(py[0], py[1]), py[2]);
    int max_y = (int)ceilf(fmaxf(fmaxf(py[0], py[1]), py[2]));

    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x > w) max_x = w;
    if (max_y > h) max_y = h;

    float area = edge_func(px[0], py[0], px[1], py[1], px[2], py[2]);
    if (fabsf(area) < 1e-6f)
        return;

    float inv_area = 1.0f / area;

    for (int y = min_y; y < max_y; y++) {
        for (int x = min_x; x < max_x; x++) {
            float fx = (float)x + 0.5f;
            float fy = (float)y + 0.5f;

            float w0 = edge_func(px[1], py[1], px[2], py[2], fx, fy) * inv_area;
            float w1 = edge_func(px[2], py[2], px[0], py[0], fx, fy) * inv_area;
            float w2 = edge_func(px[0], py[0], px[1], py[1], fx, fy) * inv_area;

            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                continue;

            /* Interpolate colour */
            float cr = w0 * colors[0][0] + w1 * colors[1][0] + w2 * colors[2][0];
            float cg = w0 * colors[0][1] + w1 * colors[1][1] + w2 * colors[2][1];
            float cb = w0 * colors[0][2] + w1 * colors[1][2] + w2 * colors[2][2];

            guint8 ri = (guint8)(cr * 255.0f);
            guint8 gi = (guint8)(cg * 255.0f);
            guint8 bi = (guint8)(cb * 255.0f);

            guint32 *pixel = (guint32 *)(buf + y * stride + x * 4);
            *pixel = make_argb(ri, gi, bi, 255);
        }
    }
}

/* ── Backing store management ─────────────────────────────────────────────── */

static void recreate_backing_store(RendererSoftware *r) {
    flutter_renderer_collect_backing_store(r->renderer, r->backing_store);
    r->backing_store = flutter_renderer_create_backing_store(
        r->renderer, r->width, r->height);
}

/* ── Render thread ────────────────────────────────────────────────────────── */

static gpointer renderer_software_thread_func(gpointer data) {
    RendererSoftware *r = data;

    r->backing_store = flutter_renderer_create_backing_store(
        r->renderer, r->width, r->height);
    if (!r->backing_store) {
        g_warning("RendererSoftware: failed to create backing store");
        return NULL;
    }

    gint64 start = g_get_monotonic_time();

    for (;;) {
        gint64 deadline = g_get_monotonic_time() + 16 * G_TIME_SPAN_MILLISECOND;

        g_mutex_lock(&r->mutex);
        g_cond_wait_until(&r->cond, &r->mutex, deadline);
        gboolean running        = r->running;
        gboolean resize_pending = r->resize_pending;
        size_t   new_width      = r->pending_width;
        size_t   new_height     = r->pending_height;
        if (resize_pending)
            r->resize_pending = FALSE;
        g_mutex_unlock(&r->mutex);

        if (!running)
            break;

        if (resize_pending && new_width > 0 && new_height > 0 &&
            (new_width != r->width || new_height != r->height)) {
            r->width  = new_width;
            r->height = new_height;
            recreate_backing_store(r);
        }

        float elapsed = (float)(g_get_monotonic_time() - start) / (float)G_USEC_PER_SEC;
        float angle   = elapsed * ((float)G_PI * 2.0f / 4.0f);

        render_frame(r, angle);
        flutter_renderer_present(r->renderer, r->backing_store);
    }

    flutter_renderer_collect_backing_store(r->renderer, r->backing_store);
    r->backing_store = NULL;
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

RendererSoftware *renderer_software_new(FlutterRenderer *renderer) {
    GtkAllocation alloc;
    gtk_widget_get_allocation(GTK_WIDGET(renderer), &alloc);

    gint scale = gtk_widget_get_scale_factor(GTK_WIDGET(renderer));

    RendererSoftware *r = g_new0(RendererSoftware, 1);
    r->renderer = renderer;
    r->width    = (size_t)alloc.width * (size_t)scale;
    r->height   = (size_t)alloc.height * (size_t)scale;
    r->running  = TRUE;
    g_mutex_init(&r->mutex);
    g_cond_init(&r->cond);

    r->thread = g_thread_new("renderer-sw", renderer_software_thread_func, r);

    return r;
}

void renderer_software_free(RendererSoftware *r) {
    if (!r)
        return;

    g_mutex_lock(&r->mutex);
    r->running = FALSE;
    g_cond_signal(&r->cond);
    g_mutex_unlock(&r->mutex);

    g_thread_join(r->thread);

    g_mutex_clear(&r->mutex);
    g_cond_clear(&r->cond);
    g_free(r);
}

void renderer_software_resize(RendererSoftware *r,
                               size_t width, size_t height, gint scale) {
    g_mutex_lock(&r->mutex);
    r->pending_width  = width * (size_t)scale;
    r->pending_height = height * (size_t)scale;
    r->resize_pending = TRUE;
    g_cond_signal(&r->cond);
    g_mutex_unlock(&r->mutex);
}
