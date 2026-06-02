#include "flutter_view.h"

#include <gdk/gdkwayland.h>
#include <string.h>
#include <wayland-client.h>

struct _FlutterView {
    GtkDrawingArea parent_instance;

    gboolean use_subsurface;
    gint scale;
    FlutterViewResizeFunc resize_func;
    gpointer              resize_data;

    /* ── Wayland subsurface mode fields ──────────────────────────────────── */

    struct wl_compositor    *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_surface       *surface;
    struct wl_subsurface    *subsurface;

    /* ── gdk_cairo_draw_from_gl mode fields ──────────────────────────────── */

    GdkGLContext *gdk_gl_context;

    /* Thread-safe pending-present state (non-subsurface path) */
    GMutex   present_mutex;
    gboolean present_scheduled;
    gboolean has_frame;
    GLuint   present_texture;
    size_t   present_width;
    size_t   present_height;

    /* ── Shared ──────────────────────────────────────────────────────────── */

    FlutterGLCompositor *gl_compositor;

    /* Resize synchronization: size_allocate blocks until the renderer
       delivers a frame at the new size. */
    GMutex   resize_mutex;
    GCond    resize_cond;
    gboolean resize_done;
    size_t   resize_expected_width;
    size_t   resize_expected_height;
};

G_DEFINE_TYPE(FlutterView, flutter_view, GTK_TYPE_DRAWING_AREA)

/* ── Wayland registry ─────────────────────────────────────────────────────── */

static void registry_global(void *data, struct wl_registry *registry,
                             uint32_t name, const char *interface,
                             uint32_t version) {
    FlutterView *self = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        self->compositor = wl_registry_bind(
            registry, name, &wl_compositor_interface, MIN(version, 4));
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        self->subcompositor = wl_registry_bind(
            registry, name, &wl_subcompositor_interface, 1);
    }
}

static void registry_global_remove(void *data G_GNUC_UNUSED,
                                    struct wl_registry *registry G_GNUC_UNUSED,
                                    uint32_t name G_GNUC_UNUSED) {}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* ── GtkWidget vfuncs ─────────────────────────────────────────────────────── */

static void realize_subsurface(FlutterView *self, GtkWidget *widget) {
    GdkDisplay *gdk_display = gtk_widget_get_display(widget);
    if (!GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        g_warning("FlutterView requires a Wayland display");
        return;
    }

    struct wl_display *display =
        gdk_wayland_display_get_wl_display(gdk_display);

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, self);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);

    if (!self->compositor || !self->subcompositor) {
        g_warning("Required Wayland globals not available "
                  "(wl_compositor=%p, wl_subcompositor=%p)",
                  (void *)self->compositor, (void *)self->subcompositor);
        return;
    }

    GtkWidget *toplevel   = gtk_widget_get_toplevel(widget);
    GdkWindow *gdk_window = gtk_widget_get_window(toplevel);
    struct wl_surface *parent_surface =
        gdk_wayland_window_get_wl_surface(gdk_window);

    self->surface   = wl_compositor_create_surface(self->compositor);
    self->subsurface = wl_subcompositor_get_subsurface(
        self->subcompositor, self->surface, parent_surface);

    wl_subsurface_set_sync(self->subsurface);

    gint x, y;
    gtk_widget_translate_coordinates(widget, toplevel, 0, 0, &x, &y);
    wl_subsurface_set_position(self->subsurface, x, y);

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    self->scale = gtk_widget_get_scale_factor(widget);

    self->gl_compositor = flutter_gl_compositor_new_subsurface(
        display, self->surface, alloc.width, alloc.height, self->scale);
    if (!self->gl_compositor)
        return;

    flutter_gl_compositor_render_clear(self->gl_compositor,
                                        (size_t)alloc.width * self->scale,
                                        (size_t)alloc.height * self->scale);
}

static void realize_gl(FlutterView *self, GtkWidget *widget) {
    GdkWindow *gdk_window = gtk_widget_get_window(widget);

    GError *error = NULL;
    self->gdk_gl_context = gdk_window_create_gl_context(gdk_window, &error);
    if (!self->gdk_gl_context) {
        g_warning("FlutterView: failed to create GDK GL context: %s",
                  error->message);
        g_error_free(error);
        return;
    }

    gdk_gl_context_set_use_es(self->gdk_gl_context, TRUE);
    gdk_gl_context_set_required_version(self->gdk_gl_context, 2, 0);

    if (!gdk_gl_context_realize(self->gdk_gl_context, &error)) {
        g_warning("FlutterView: failed to realize GDK GL context: %s",
                  error->message);
        g_error_free(error);
        g_clear_object(&self->gdk_gl_context);
        return;
    }

    gdk_gl_context_make_current(self->gdk_gl_context);
    EGLDisplay egl_display = eglGetCurrentDisplay();
    EGLContext gdk_egl_context = eglGetCurrentContext();
    gdk_gl_context_clear_current();

    self->gl_compositor = flutter_gl_compositor_new_gdk(egl_display,
                                                        gdk_egl_context);
}

static void flutter_view_realize(GtkWidget *widget) {
    FlutterView *self = FLUTTER_VIEW(widget);

    GTK_WIDGET_CLASS(flutter_view_parent_class)->realize(widget);

    if (self->use_subsurface)
        realize_subsurface(self, widget);
    else
        realize_gl(self, widget);
}

static void flutter_view_unrealize(GtkWidget *widget) {
    FlutterView *self = FLUTTER_VIEW(widget);

    if (!self->use_subsurface) {
        g_mutex_lock(&self->present_mutex);
        self->has_frame = FALSE;
        self->present_texture = 0;
        g_mutex_unlock(&self->present_mutex);
        g_clear_object(&self->gdk_gl_context);
    }

    if (self->gl_compositor) {
        flutter_gl_compositor_free(self->gl_compositor);
        self->gl_compositor = NULL;
    }

    if (self->use_subsurface) {
        if (self->subsurface) {
            wl_subsurface_destroy(self->subsurface);
            self->subsurface = NULL;
        }
        if (self->surface) {
            wl_surface_destroy(self->surface);
            self->surface = NULL;
        }
    }

    GTK_WIDGET_CLASS(flutter_view_parent_class)->unrealize(widget);
}

static void flutter_view_size_allocate(GtkWidget     *widget,
                                       GtkAllocation *allocation) {
    GTK_WIDGET_CLASS(flutter_view_parent_class)
        ->size_allocate(widget, allocation);

    FlutterView *self = FLUTTER_VIEW(widget);

    gint scale = gtk_widget_get_scale_factor(widget);
    size_t pw = (size_t)allocation->width * scale;
    size_t ph = (size_t)allocation->height * scale;

    if (self->use_subsurface) {
        if (!self->subsurface)
            return;

        GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
        gint x, y;
        gtk_widget_translate_coordinates(widget, toplevel, 0, 0, &x, &y);
        wl_subsurface_set_position(self->subsurface, x, y);

        flutter_gl_compositor_resize(self->gl_compositor, pw, ph);
    }

    /* Block until the renderer delivers a frame at the new size. */
    g_mutex_lock(&self->resize_mutex);
    self->resize_done = FALSE;
    self->resize_expected_width = pw;
    self->resize_expected_height = ph;
    g_mutex_unlock(&self->resize_mutex);

    if (self->resize_func)
        self->resize_func((size_t)allocation->width,
                          (size_t)allocation->height,
                          scale, self->resize_data);

    gint64 deadline = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
    g_mutex_lock(&self->resize_mutex);
    while (!self->resize_done) {
        if (!g_cond_wait_until(&self->resize_cond, &self->resize_mutex, deadline))
            break;
    }
    g_mutex_unlock(&self->resize_mutex);
}

static gboolean flutter_view_draw(GtkWidget *widget, cairo_t *cr) {
    FlutterView *self = FLUTTER_VIEW(widget);

    if (self->use_subsurface) {
        return FALSE;
    }

    /* gdk_cairo_draw_from_gl mode */
    g_mutex_lock(&self->present_mutex);
    gboolean has_frame = self->has_frame;
    GLuint   texture   = self->present_texture;
    size_t   width     = self->present_width;
    size_t   height    = self->present_height;
    g_mutex_unlock(&self->present_mutex);

    if (!has_frame || !self->gdk_gl_context) {
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        return TRUE;
    }

    GdkWindow *gdk_window = gtk_widget_get_window(widget);
    gint scale = gdk_window_get_scale_factor(gdk_window);

    gdk_cairo_draw_from_gl(cr, gdk_window,
                           (int)texture, GL_TEXTURE, scale,
                           0, 0, (int)width, (int)height);
    return TRUE;
}

static void flutter_view_get_preferred_width(GtkWidget *widget G_GNUC_UNUSED,
                                             gint *minimum, gint *natural) {
    *minimum = 1;
    *natural = 400;
}

static void flutter_view_get_preferred_height(GtkWidget *widget G_GNUC_UNUSED,
                                              gint *minimum, gint *natural) {
    *minimum = 1;
    *natural = 300;
}

static void flutter_view_finalize(GObject *object) {
    FlutterView *self = FLUTTER_VIEW(object);

    g_mutex_clear(&self->resize_mutex);
    g_cond_clear(&self->resize_cond);
    g_mutex_clear(&self->present_mutex);

    if (self->subcompositor) {
        wl_subcompositor_destroy(self->subcompositor);
        self->subcompositor = NULL;
    }
    if (self->compositor) {
        wl_compositor_destroy(self->compositor);
        self->compositor = NULL;
    }

    G_OBJECT_CLASS(flutter_view_parent_class)->finalize(object);
}

static void flutter_view_class_init(FlutterViewClass *klass) {
    GObjectClass    *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass  *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->finalize = flutter_view_finalize;

    widget_class->realize              = flutter_view_realize;
    widget_class->unrealize            = flutter_view_unrealize;
    widget_class->size_allocate        = flutter_view_size_allocate;
    widget_class->draw                 = flutter_view_draw;
    widget_class->get_preferred_width  = flutter_view_get_preferred_width;
    widget_class->get_preferred_height = flutter_view_get_preferred_height;
}

static void flutter_view_init(FlutterView *self) {
    g_mutex_init(&self->resize_mutex);
    g_cond_init(&self->resize_cond);
    g_mutex_init(&self->present_mutex);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

GtkWidget *flutter_view_new(gboolean              use_subsurface,
                            FlutterViewResizeFunc  resize_func,
                            gpointer              resize_data) {
    FlutterView *self = g_object_new(FLUTTER_TYPE_VIEW, NULL);
    self->use_subsurface = use_subsurface;
    self->resize_func = resize_func;
    self->resize_data = resize_data;
    if (use_subsurface)
        gtk_widget_set_has_window(GTK_WIDGET(self), FALSE);
    return GTK_WIDGET(self);
}

/* Callback dispatched to the main thread to trigger a redraw/parent commit. */
static gboolean queue_draw_idle(gpointer data) {
    FlutterView *self = FLUTTER_VIEW(data);

    if (!self->use_subsurface) {
        g_mutex_lock(&self->present_mutex);
        self->present_scheduled = FALSE;
        g_mutex_unlock(&self->present_mutex);
    }

    gtk_widget_queue_draw(GTK_WIDGET(self));
    g_object_unref(self);
    return G_SOURCE_REMOVE;
}

static void present_subsurface(FlutterView *self,
                               GLuint texture_id, GLenum texture_format,
                               size_t width, size_t height) {
    flutter_gl_compositor_present(self->gl_compositor,
                                   texture_id, texture_format, width, height);

    /* Signal any blocked resize if this frame matches the expected size. */
    g_mutex_lock(&self->resize_mutex);
    if (!self->resize_done &&
        width == self->resize_expected_width &&
        height == self->resize_expected_height) {
        self->resize_done = TRUE;
        g_cond_signal(&self->resize_cond);
    }
    g_mutex_unlock(&self->resize_mutex);

    /* Drive a parent-surface commit (sync mode) from the main thread. */
    g_object_ref(self);
    g_main_context_invoke(NULL, queue_draw_idle, self);
}

static void present_gl(FlutterView *self,
                       GLuint texture_id, size_t width, size_t height) {
    g_mutex_lock(&self->present_mutex);

    self->present_texture = texture_id;
    self->present_width   = width;
    self->present_height  = height;
    self->has_frame       = TRUE;

    gboolean was_scheduled = self->present_scheduled;
    if (!was_scheduled) {
        self->present_scheduled = TRUE;
        g_object_ref(self);
    }

    g_mutex_unlock(&self->present_mutex);

    /* Signal any blocked resize if this frame matches the expected size. */
    g_mutex_lock(&self->resize_mutex);
    if (!self->resize_done &&
        width == self->resize_expected_width &&
        height == self->resize_expected_height) {
        self->resize_done = TRUE;
        g_cond_signal(&self->resize_cond);
    }
    g_mutex_unlock(&self->resize_mutex);

    if (!was_scheduled)
        g_main_context_invoke(NULL, queue_draw_idle, self);
}

/* ── Public API (called by the renderer) ──────────────────────────────────── */

FlutterBackingStore *flutter_view_create_backing_store(FlutterView *self,
                                                       size_t       width,
                                                       size_t       height) {
    return flutter_gl_compositor_create_backing_store(self->gl_compositor,
                                                      width, height);
}

void flutter_view_collect_backing_store(FlutterView         *self,
                                        FlutterBackingStore *backing_store) {
    flutter_gl_compositor_collect_backing_store(self->gl_compositor,
                                                backing_store);
}

void flutter_view_present(FlutterView *self,
                          GLuint       texture_id,
                          GLenum       texture_format,
                          size_t       width,
                          size_t       height) {
    if (self->use_subsurface)
        present_subsurface(self, texture_id, texture_format, width, height);
    else
        present_gl(self, texture_id, width, height);
}

gboolean flutter_view_make_current(FlutterView *self) {
    return flutter_gl_compositor_make_current(self->gl_compositor);
}

void flutter_view_clear_current(FlutterView *self) {
    flutter_gl_compositor_clear_current(self->gl_compositor);
}
