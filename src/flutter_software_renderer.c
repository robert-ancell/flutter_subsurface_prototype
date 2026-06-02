#include "flutter_software_renderer.h"

#include <string.h>

#include "flutter_view_resize.h"

struct _FlutterSoftwareRenderer {
    GtkDrawingArea parent_instance;

    FlutterRendererResizeFunc resize_func;
    gpointer                  resize_data;

    /* Thread-safe pending-present state */
    GMutex            present_mutex;
    gboolean          present_scheduled;
    gboolean          has_frame;
    cairo_surface_t  *present_surface;

    FlutterViewResize *resize;
};

static void flutter_software_renderer_iface_init(FlutterRendererInterface *iface);

G_DEFINE_TYPE_WITH_CODE(FlutterSoftwareRenderer, flutter_software_renderer,
                        GTK_TYPE_DRAWING_AREA,
                        G_IMPLEMENT_INTERFACE(FLUTTER_TYPE_RENDERER,
                                             flutter_software_renderer_iface_init))

/* ── GtkWidget vfuncs ─────────────────────────────────────────────────────── */

static void flutter_software_renderer_realize(GtkWidget *widget) {
    GTK_WIDGET_CLASS(flutter_software_renderer_parent_class)->realize(widget);
}

static void flutter_software_renderer_unrealize(GtkWidget *widget) {
    FlutterSoftwareRenderer *self = FLUTTER_SOFTWARE_RENDERER(widget);

    g_mutex_lock(&self->present_mutex);
    self->has_frame = FALSE;
    if (self->present_surface) {
        cairo_surface_destroy(self->present_surface);
        self->present_surface = NULL;
    }
    g_mutex_unlock(&self->present_mutex);

    GTK_WIDGET_CLASS(flutter_software_renderer_parent_class)->unrealize(widget);
}

static void flutter_software_renderer_size_allocate(GtkWidget     *widget,
                                                     GtkAllocation *allocation) {
    GTK_WIDGET_CLASS(flutter_software_renderer_parent_class)
        ->size_allocate(widget, allocation);

    FlutterSoftwareRenderer *self = FLUTTER_SOFTWARE_RENDERER(widget);

    gint scale = gtk_widget_get_scale_factor(widget);
    size_t pw = (size_t)allocation->width * scale;
    size_t ph = (size_t)allocation->height * scale;

    if (self->resize_func)
        self->resize_func((size_t)allocation->width,
                          (size_t)allocation->height,
                          scale, self->resize_data);

    flutter_view_resize_wait(self->resize, pw, ph);
}

static gboolean flutter_software_renderer_draw(GtkWidget *widget, cairo_t *cr) {
    FlutterSoftwareRenderer *self = FLUTTER_SOFTWARE_RENDERER(widget);

    g_mutex_lock(&self->present_mutex);
    gboolean has_frame = self->has_frame;
    cairo_surface_t *surface = self->present_surface;
    if (surface)
        cairo_surface_reference(surface);
    g_mutex_unlock(&self->present_mutex);

    if (!has_frame || !surface) {
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        if (surface)
            cairo_surface_destroy(surface);
        return TRUE;
    }

    gint scale = gtk_widget_get_scale_factor(widget);
    cairo_scale(cr, 1.0 / scale, 1.0 / scale);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);

    cairo_surface_destroy(surface);
    return TRUE;
}

static void flutter_software_renderer_get_preferred_width(
    GtkWidget *widget G_GNUC_UNUSED, gint *minimum, gint *natural) {
    *minimum = 1;
    *natural = 400;
}

static void flutter_software_renderer_get_preferred_height(
    GtkWidget *widget G_GNUC_UNUSED, gint *minimum, gint *natural) {
    *minimum = 1;
    *natural = 300;
}

static void flutter_software_renderer_finalize(GObject *object) {
    FlutterSoftwareRenderer *self = FLUTTER_SOFTWARE_RENDERER(object);

    flutter_view_resize_free(self->resize);
    if (self->present_surface)
        cairo_surface_destroy(self->present_surface);
    g_mutex_clear(&self->present_mutex);

    G_OBJECT_CLASS(flutter_software_renderer_parent_class)->finalize(object);
}

static void flutter_software_renderer_class_init(
    FlutterSoftwareRendererClass *klass) {
    GObjectClass   *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->finalize = flutter_software_renderer_finalize;

    widget_class->realize              = flutter_software_renderer_realize;
    widget_class->unrealize            = flutter_software_renderer_unrealize;
    widget_class->size_allocate        = flutter_software_renderer_size_allocate;
    widget_class->draw                 = flutter_software_renderer_draw;
    widget_class->get_preferred_width  = flutter_software_renderer_get_preferred_width;
    widget_class->get_preferred_height = flutter_software_renderer_get_preferred_height;
}

static void flutter_software_renderer_init(FlutterSoftwareRenderer *self) {
    self->resize = flutter_view_resize_new();
    g_mutex_init(&self->present_mutex);
}

/* ── Present (called from render thread) ──────────────────────────────────── */

static gboolean queue_draw_idle(gpointer data) {
    FlutterSoftwareRenderer *self = FLUTTER_SOFTWARE_RENDERER(data);

    g_mutex_lock(&self->present_mutex);
    self->present_scheduled = FALSE;
    g_mutex_unlock(&self->present_mutex);

    gtk_widget_queue_draw(GTK_WIDGET(self));
    g_object_unref(self);
    return G_SOURCE_REMOVE;
}

static void flutter_software_renderer_present_impl(FlutterRenderer     *renderer,
                                                    FlutterBackingStore *backing_store) {
    g_assert(backing_store->type == FLUTTER_BACKING_STORE_TYPE_SOFTWARE);

    FlutterSoftwareRenderer *self = FLUTTER_SOFTWARE_RENDERER(renderer);

    size_t width  = backing_store->width;
    size_t height = backing_store->height;

    /* Create a cairo surface from the pixel buffer (ARGB32 premultiplied). */
    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        backing_store->software.buffer,
        CAIRO_FORMAT_ARGB32,
        (int)width, (int)height,
        cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, (int)width));

    g_mutex_lock(&self->present_mutex);

    if (self->present_surface)
        cairo_surface_destroy(self->present_surface);
    self->present_surface = surface;
    self->has_frame       = TRUE;

    gboolean was_scheduled = self->present_scheduled;
    if (!was_scheduled) {
        self->present_scheduled = TRUE;
        g_object_ref(self);
    }

    g_mutex_unlock(&self->present_mutex);

    flutter_view_resize_notify(self->resize, width, height);

    if (!was_scheduled)
        g_main_context_invoke(NULL, queue_draw_idle, self);
}

static FlutterBackingStore *flutter_software_renderer_create_backing_store(
    FlutterRenderer *renderer G_GNUC_UNUSED, size_t width, size_t height) {
    FlutterBackingStore *store = g_new0(FlutterBackingStore, 1);
    store->type   = FLUTTER_BACKING_STORE_TYPE_SOFTWARE;
    store->width  = width;
    store->height = height;

    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, (int)width);
    store->software.buffer = g_malloc0((size_t)stride * height);

    return store;
}

static void flutter_software_renderer_collect_backing_store(
    FlutterRenderer *renderer G_GNUC_UNUSED, FlutterBackingStore *backing_store) {
    if (!backing_store)
        return;
    g_free(backing_store->software.buffer);
    g_free(backing_store);
}

static gboolean flutter_software_renderer_make_current(
    FlutterRenderer *renderer G_GNUC_UNUSED) {
    return TRUE;
}

static void flutter_software_renderer_clear_current(
    FlutterRenderer *renderer G_GNUC_UNUSED) {
}

static void flutter_software_renderer_iface_init(FlutterRendererInterface *iface) {
    iface->create_backing_store  = flutter_software_renderer_create_backing_store;
    iface->collect_backing_store = flutter_software_renderer_collect_backing_store;
    iface->present               = flutter_software_renderer_present_impl;
    iface->make_current          = flutter_software_renderer_make_current;
    iface->clear_current         = flutter_software_renderer_clear_current;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

GtkWidget *flutter_software_renderer_new(FlutterRendererResizeFunc resize_func,
                                          gpointer                  resize_data) {
    FlutterSoftwareRenderer *self =
        g_object_new(FLUTTER_TYPE_SOFTWARE_RENDERER, NULL);
    self->resize_func = resize_func;
    self->resize_data = resize_data;
    return GTK_WIDGET(self);
}
