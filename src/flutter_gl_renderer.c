#include "flutter_gl_renderer.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gdk/gdkwayland.h>

#include "flutter_gl_compositor.h"
#include "flutter_view_resize.h"

struct _FlutterGLRenderer {
    GtkDrawingArea parent_instance;

    FlutterRendererResizeFunc resize_func;
    gpointer                  resize_data;

    GdkGLContext *gdk_gl_context;

    /* Thread-safe pending-present state */
    GMutex   present_mutex;
    gboolean present_scheduled;
    gboolean has_frame;
    GLuint   present_texture;
    size_t   present_width;
    size_t   present_height;

    FlutterGLCompositor *gl_compositor;
    FlutterViewResize   *resize;
};

static void flutter_gl_renderer_iface_init(FlutterRendererInterface *iface);

G_DEFINE_TYPE_WITH_CODE(FlutterGLRenderer, flutter_gl_renderer,
                        GTK_TYPE_DRAWING_AREA,
                        G_IMPLEMENT_INTERFACE(FLUTTER_TYPE_RENDERER,
                                             flutter_gl_renderer_iface_init))

/* ── GtkWidget vfuncs ─────────────────────────────────────────────────────── */

static void flutter_gl_renderer_realize(GtkWidget *widget) {
    FlutterGLRenderer *self = FLUTTER_GL_RENDERER(widget);

    GTK_WIDGET_CLASS(flutter_gl_renderer_parent_class)->realize(widget);

    GdkWindow *gdk_window = gtk_widget_get_window(widget);

    GError *error = NULL;
    self->gdk_gl_context = gdk_window_create_gl_context(gdk_window, &error);
    if (!self->gdk_gl_context) {
        g_warning("FlutterGLRenderer: failed to create GDK GL context: %s",
                  error->message);
        g_error_free(error);
        return;
    }

    gdk_gl_context_set_use_es(self->gdk_gl_context, TRUE);
    gdk_gl_context_set_required_version(self->gdk_gl_context, 2, 0);

    if (!gdk_gl_context_realize(self->gdk_gl_context, &error)) {
        g_warning("FlutterGLRenderer: failed to realize GDK GL context: %s",
                  error->message);
        g_error_free(error);
        g_clear_object(&self->gdk_gl_context);
        return;
    }

    gdk_gl_context_make_current(self->gdk_gl_context);
    EGLDisplay egl_display = eglGetCurrentDisplay();
    EGLContext gdk_egl_context = eglGetCurrentContext();
    gdk_gl_context_clear_current();

    self->gl_compositor = flutter_gl_compositor_new(egl_display,
                                                    gdk_egl_context);
}

static void flutter_gl_renderer_unrealize(GtkWidget *widget) {
    FlutterGLRenderer *self = FLUTTER_GL_RENDERER(widget);

    g_mutex_lock(&self->present_mutex);
    self->has_frame = FALSE;
    self->present_texture = 0;
    g_mutex_unlock(&self->present_mutex);

    if (self->gl_compositor) {
        flutter_gl_compositor_free(self->gl_compositor);
        self->gl_compositor = NULL;
    }

    g_clear_object(&self->gdk_gl_context);

    GTK_WIDGET_CLASS(flutter_gl_renderer_parent_class)->unrealize(widget);
}

static void flutter_gl_renderer_size_allocate(GtkWidget     *widget,
                                               GtkAllocation *allocation) {
    GTK_WIDGET_CLASS(flutter_gl_renderer_parent_class)
        ->size_allocate(widget, allocation);

    FlutterGLRenderer *self = FLUTTER_GL_RENDERER(widget);

    gint scale = gtk_widget_get_scale_factor(widget);
    size_t pw = (size_t)allocation->width * scale;
    size_t ph = (size_t)allocation->height * scale;

    if (self->resize_func)
        self->resize_func((size_t)allocation->width,
                          (size_t)allocation->height,
                          scale, self->resize_data);

    flutter_view_resize_wait(self->resize, pw, ph);
}

static gboolean flutter_gl_renderer_draw(GtkWidget *widget, cairo_t *cr) {
    FlutterGLRenderer *self = FLUTTER_GL_RENDERER(widget);

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

static void flutter_gl_renderer_get_preferred_width(
    GtkWidget *widget G_GNUC_UNUSED, gint *minimum, gint *natural) {
    *minimum = 1;
    *natural = 400;
}

static void flutter_gl_renderer_get_preferred_height(
    GtkWidget *widget G_GNUC_UNUSED, gint *minimum, gint *natural) {
    *minimum = 1;
    *natural = 300;
}

static void flutter_gl_renderer_finalize(GObject *object) {
    FlutterGLRenderer *self = FLUTTER_GL_RENDERER(object);

    flutter_view_resize_free(self->resize);
    g_mutex_clear(&self->present_mutex);

    G_OBJECT_CLASS(flutter_gl_renderer_parent_class)->finalize(object);
}

static void flutter_gl_renderer_class_init(FlutterGLRendererClass *klass) {
    GObjectClass   *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->finalize = flutter_gl_renderer_finalize;

    widget_class->realize              = flutter_gl_renderer_realize;
    widget_class->unrealize            = flutter_gl_renderer_unrealize;
    widget_class->size_allocate        = flutter_gl_renderer_size_allocate;
    widget_class->draw                 = flutter_gl_renderer_draw;
    widget_class->get_preferred_width  = flutter_gl_renderer_get_preferred_width;
    widget_class->get_preferred_height = flutter_gl_renderer_get_preferred_height;
}

static void flutter_gl_renderer_init(FlutterGLRenderer *self) {
    self->resize = flutter_view_resize_new();
    g_mutex_init(&self->present_mutex);
}

/* ── Present (called from render thread) ──────────────────────────────────── */

static gboolean queue_draw_idle(gpointer data) {
    FlutterGLRenderer *self = FLUTTER_GL_RENDERER(data);

    g_mutex_lock(&self->present_mutex);
    self->present_scheduled = FALSE;
    g_mutex_unlock(&self->present_mutex);

    gtk_widget_queue_draw(GTK_WIDGET(self));
    g_object_unref(self);
    return G_SOURCE_REMOVE;
}

static void flutter_gl_renderer_present_impl(FlutterRenderer *renderer,
                                              GLuint           texture_id,
                                              GLenum           texture_format G_GNUC_UNUSED,
                                              size_t           width,
                                              size_t           height) {
    FlutterGLRenderer *self = FLUTTER_GL_RENDERER(renderer);

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

    flutter_view_resize_notify(self->resize, width, height);

    if (!was_scheduled)
        g_main_context_invoke(NULL, queue_draw_idle, self);
}

static FlutterBackingStore *flutter_gl_renderer_create_backing_store(
    FlutterRenderer *renderer, size_t width, size_t height) {
    FlutterGLRenderer *self = FLUTTER_GL_RENDERER(renderer);
    return flutter_gl_compositor_create_backing_store(self->gl_compositor,
                                                      width, height);
}

static void flutter_gl_renderer_collect_backing_store(
    FlutterRenderer *renderer, FlutterBackingStore *backing_store) {
    FlutterGLRenderer *self = FLUTTER_GL_RENDERER(renderer);
    flutter_gl_compositor_collect_backing_store(self->gl_compositor,
                                                backing_store);
}

static gboolean flutter_gl_renderer_make_current_impl(FlutterRenderer *renderer) {
    FlutterGLRenderer *self = FLUTTER_GL_RENDERER(renderer);
    return flutter_gl_compositor_make_current(self->gl_compositor);
}

static void flutter_gl_renderer_clear_current_impl(FlutterRenderer *renderer) {
    FlutterGLRenderer *self = FLUTTER_GL_RENDERER(renderer);
    flutter_gl_compositor_clear_current(self->gl_compositor);
}

static void flutter_gl_renderer_iface_init(FlutterRendererInterface *iface) {
    iface->create_backing_store  = flutter_gl_renderer_create_backing_store;
    iface->collect_backing_store = flutter_gl_renderer_collect_backing_store;
    iface->present               = flutter_gl_renderer_present_impl;
    iface->make_current          = flutter_gl_renderer_make_current_impl;
    iface->clear_current         = flutter_gl_renderer_clear_current_impl;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

GtkWidget *flutter_gl_renderer_new(FlutterRendererResizeFunc resize_func,
                                    gpointer                  resize_data) {
    FlutterGLRenderer *self = g_object_new(FLUTTER_TYPE_GL_RENDERER, NULL);
    self->resize_func = resize_func;
    self->resize_data = resize_data;
    return GTK_WIDGET(self);
}
