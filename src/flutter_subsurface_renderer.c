#include "flutter_subsurface_renderer.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gdk/gdkwayland.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "flutter_subsurface.h"
#include "flutter_view_resize.h"

struct _FlutterSubsurfaceRenderer {
    GtkDrawingArea parent_instance;

    FlutterRendererResizeFunc resize_func;
    gpointer                  resize_data;

    FlutterSubsurface *subsurface;

    struct wl_egl_window *egl_window;
    EGLDisplay            egl_display;
    EGLContext            egl_context;
    EGLSurface            egl_surface;

    FlutterGLCompositor *gl_compositor;
    FlutterViewResize   *resize;
};

static void flutter_subsurface_renderer_iface_init(FlutterRendererInterface *iface);

G_DEFINE_TYPE_WITH_CODE(FlutterSubsurfaceRenderer, flutter_subsurface_renderer,
                        GTK_TYPE_DRAWING_AREA,
                        G_IMPLEMENT_INTERFACE(FLUTTER_TYPE_RENDERER,
                                             flutter_subsurface_renderer_iface_init))

/* ── EGL setup ────────────────────────────────────────────────────────────── */

static gboolean setup_egl(FlutterSubsurfaceRenderer *self,
                           struct wl_display *display,
                           size_t width, size_t height, gint scale) {
    self->egl_display = eglGetDisplay((EGLNativeDisplayType)display);
    if (self->egl_display == EGL_NO_DISPLAY) {
        g_warning("Failed to get EGL display");
        return FALSE;
    }
    if (!eglInitialize(self->egl_display, NULL, NULL)) {
        g_warning("Failed to initialize EGL");
        return FALSE;
    }

    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE,
    };
    EGLConfig config;
    EGLint    num_configs;
    if (!eglChooseConfig(self->egl_display, config_attribs, &config, 1,
                         &num_configs) || num_configs == 0) {
        g_warning("Failed to choose EGL config");
        return FALSE;
    }

    eglBindAPI(EGL_OPENGL_ES_API);

    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    self->egl_context = eglCreateContext(self->egl_display, config,
                                         EGL_NO_CONTEXT, context_attribs);
    if (self->egl_context == EGL_NO_CONTEXT) {
        g_warning("Failed to create EGL context");
        return FALSE;
    }

    struct wl_surface *wl_surface =
        flutter_subsurface_get_surface(self->subsurface);

    self->egl_window = wl_egl_window_create(wl_surface,
                                             width * scale,
                                             height * scale);
    if (!self->egl_window) {
        g_warning("Failed to create wl_egl_window");
        return FALSE;
    }

    self->egl_surface = eglCreateWindowSurface(
        self->egl_display, config,
        (EGLNativeWindowType)self->egl_window, NULL);
    if (self->egl_surface == EGL_NO_SURFACE) {
        g_warning("Failed to create EGL window surface");
        return FALSE;
    }

    wl_surface_set_buffer_scale(wl_surface, scale);

    eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                   self->egl_context);
    eglSwapInterval(self->egl_display, 0);

    self->gl_compositor = flutter_gl_compositor_new(self->egl_display,
                                                    self->egl_context);
    if (!self->gl_compositor)
        return FALSE;

    eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    return TRUE;
}

static void render_clear(FlutterSubsurfaceRenderer *self,
                          size_t width, size_t height) {
    eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                   self->egl_context);
    glViewport(0, 0, width, height);
    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(self->egl_display, self->egl_surface);
    eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
}

/* ── GtkWidget vfuncs ─────────────────────────────────────────────────────── */

static void flutter_subsurface_renderer_realize(GtkWidget *widget) {
    FlutterSubsurfaceRenderer *self = FLUTTER_SUBSURFACE_RENDERER(widget);

    GTK_WIDGET_CLASS(flutter_subsurface_renderer_parent_class)->realize(widget);

    self->subsurface = flutter_subsurface_new(widget);
    if (!self->subsurface)
        return;

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    gint scale = gtk_widget_get_scale_factor(widget);

    GdkDisplay *gdk_display = gtk_widget_get_display(widget);
    struct wl_display *display =
        gdk_wayland_display_get_wl_display(gdk_display);

    if (!setup_egl(self, display, alloc.width, alloc.height, scale))
        return;

    render_clear(self, (size_t)alloc.width * scale,
                       (size_t)alloc.height * scale);
}

static void flutter_subsurface_renderer_unrealize(GtkWidget *widget) {
    FlutterSubsurfaceRenderer *self = FLUTTER_SUBSURFACE_RENDERER(widget);

    if (self->gl_compositor) {
        if (self->egl_display != EGL_NO_DISPLAY)
            eglMakeCurrent(self->egl_display, self->egl_surface,
                           self->egl_surface, self->egl_context);
        flutter_gl_compositor_free(self->gl_compositor);
        self->gl_compositor = NULL;
        if (self->egl_display != EGL_NO_DISPLAY)
            eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                           EGL_NO_CONTEXT);
    }

    if (self->egl_display != EGL_NO_DISPLAY) {
        if (self->egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(self->egl_display, self->egl_surface);
        if (self->egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(self->egl_display, self->egl_context);
        eglTerminate(self->egl_display);
        self->egl_surface = EGL_NO_SURFACE;
        self->egl_context = EGL_NO_CONTEXT;
        self->egl_display = EGL_NO_DISPLAY;
    }

    if (self->egl_window) {
        wl_egl_window_destroy(self->egl_window);
        self->egl_window = NULL;
    }

    flutter_subsurface_free(self->subsurface);
    self->subsurface = NULL;

    GTK_WIDGET_CLASS(flutter_subsurface_renderer_parent_class)->unrealize(widget);
}

static void flutter_subsurface_renderer_size_allocate(GtkWidget     *widget,
                                                       GtkAllocation *allocation) {
    GTK_WIDGET_CLASS(flutter_subsurface_renderer_parent_class)
        ->size_allocate(widget, allocation);

    FlutterSubsurfaceRenderer *self = FLUTTER_SUBSURFACE_RENDERER(widget);

    if (!self->subsurface)
        return;

    gint scale = gtk_widget_get_scale_factor(widget);
    size_t pw = (size_t)allocation->width * scale;
    size_t ph = (size_t)allocation->height * scale;

    GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
    gint x, y;
    gtk_widget_translate_coordinates(widget, toplevel, 0, 0, &x, &y);
    flutter_subsurface_set_position(self->subsurface, x, y);

    if (self->egl_window)
        wl_egl_window_resize(self->egl_window, pw, ph, 0, 0);

    if (self->resize_func)
        self->resize_func((size_t)allocation->width,
                          (size_t)allocation->height,
                          scale, self->resize_data);

    flutter_view_resize_wait(self->resize, pw, ph);
}

static gboolean flutter_subsurface_renderer_draw(GtkWidget *widget G_GNUC_UNUSED,
                                                  cairo_t   *cr G_GNUC_UNUSED) {
    return FALSE;
}

static void flutter_subsurface_renderer_get_preferred_width(
    GtkWidget *widget G_GNUC_UNUSED, gint *minimum, gint *natural) {
    *minimum = 1;
    *natural = 400;
}

static void flutter_subsurface_renderer_get_preferred_height(
    GtkWidget *widget G_GNUC_UNUSED, gint *minimum, gint *natural) {
    *minimum = 1;
    *natural = 300;
}

static void flutter_subsurface_renderer_finalize(GObject *object) {
    FlutterSubsurfaceRenderer *self = FLUTTER_SUBSURFACE_RENDERER(object);

    flutter_view_resize_free(self->resize);

    G_OBJECT_CLASS(flutter_subsurface_renderer_parent_class)->finalize(object);
}

static void flutter_subsurface_renderer_class_init(
    FlutterSubsurfaceRendererClass *klass) {
    GObjectClass   *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->finalize = flutter_subsurface_renderer_finalize;

    widget_class->realize              = flutter_subsurface_renderer_realize;
    widget_class->unrealize            = flutter_subsurface_renderer_unrealize;
    widget_class->size_allocate        = flutter_subsurface_renderer_size_allocate;
    widget_class->draw                 = flutter_subsurface_renderer_draw;
    widget_class->get_preferred_width  = flutter_subsurface_renderer_get_preferred_width;
    widget_class->get_preferred_height = flutter_subsurface_renderer_get_preferred_height;
}

static void flutter_subsurface_renderer_init(FlutterSubsurfaceRenderer *self) {
    self->egl_display = EGL_NO_DISPLAY;
    self->egl_context = EGL_NO_CONTEXT;
    self->egl_surface = EGL_NO_SURFACE;
    self->resize = flutter_view_resize_new();
    gtk_widget_set_has_window(GTK_WIDGET(self), FALSE);
}

/* ── Present (called from render thread) ──────────────────────────────────── */

static gboolean queue_draw_idle(gpointer data) {
    gtk_widget_queue_draw(GTK_WIDGET(data));
    g_object_unref(data);
    return G_SOURCE_REMOVE;
}

static void flutter_subsurface_renderer_present(FlutterRenderer *renderer,
                                                 GLuint           texture_id,
                                                 GLenum           texture_format G_GNUC_UNUSED,
                                                 size_t           width,
                                                 size_t           height) {
    FlutterSubsurfaceRenderer *self = FLUTTER_SUBSURFACE_RENDERER(renderer);

    if (self->egl_display == EGL_NO_DISPLAY ||
        self->egl_surface == EGL_NO_SURFACE)
        return;

    EGLint cur_w, cur_h;
    eglQuerySurface(self->egl_display, self->egl_surface, EGL_WIDTH,  &cur_w);
    eglQuerySurface(self->egl_display, self->egl_surface, EGL_HEIGHT, &cur_h);
    if ((size_t)cur_w != width || (size_t)cur_h != height)
        wl_egl_window_resize(self->egl_window, width, height, 0, 0);

    eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                   self->egl_context);
    flutter_gl_compositor_blit(self->gl_compositor, texture_id, width, height);
    eglSwapBuffers(self->egl_display, self->egl_surface);

    flutter_gl_compositor_make_current(self->gl_compositor);

    flutter_view_resize_notify(self->resize, width, height);

    g_object_ref(self);
    g_main_context_invoke(NULL, queue_draw_idle, self);
}

static FlutterBackingStore *flutter_subsurface_renderer_create_backing_store(
    FlutterRenderer *renderer, size_t width, size_t height) {
    FlutterSubsurfaceRenderer *self = FLUTTER_SUBSURFACE_RENDERER(renderer);
    return flutter_gl_compositor_create_backing_store(self->gl_compositor,
                                                      width, height);
}

static void flutter_subsurface_renderer_collect_backing_store(
    FlutterRenderer *renderer, FlutterBackingStore *backing_store) {
    FlutterSubsurfaceRenderer *self = FLUTTER_SUBSURFACE_RENDERER(renderer);
    flutter_gl_compositor_collect_backing_store(self->gl_compositor,
                                                backing_store);
}

static gboolean flutter_subsurface_renderer_make_current(FlutterRenderer *renderer) {
    FlutterSubsurfaceRenderer *self = FLUTTER_SUBSURFACE_RENDERER(renderer);
    return flutter_gl_compositor_make_current(self->gl_compositor);
}

static void flutter_subsurface_renderer_clear_current(FlutterRenderer *renderer) {
    FlutterSubsurfaceRenderer *self = FLUTTER_SUBSURFACE_RENDERER(renderer);
    flutter_gl_compositor_clear_current(self->gl_compositor);
}

static void flutter_subsurface_renderer_iface_init(FlutterRendererInterface *iface) {
    iface->create_backing_store  = flutter_subsurface_renderer_create_backing_store;
    iface->collect_backing_store = flutter_subsurface_renderer_collect_backing_store;
    iface->present               = flutter_subsurface_renderer_present;
    iface->make_current          = flutter_subsurface_renderer_make_current;
    iface->clear_current         = flutter_subsurface_renderer_clear_current;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

GtkWidget *flutter_subsurface_renderer_new(FlutterRendererResizeFunc resize_func,
                                            gpointer                  resize_data) {
    FlutterSubsurfaceRenderer *self =
        g_object_new(FLUTTER_TYPE_SUBSURFACE_RENDERER, NULL);
    self->resize_func = resize_func;
    self->resize_data = resize_data;
    return GTK_WIDGET(self);
}
