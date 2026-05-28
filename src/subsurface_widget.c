#include "subsurface_widget.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gdk/gdkwayland.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>

struct _SubsurfaceWidget {
    GtkWidget parent_instance;

    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_surface *surface;
    struct wl_subsurface *subsurface;

    struct wl_egl_window *egl_window;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
};

G_DEFINE_TYPE(SubsurfaceWidget, subsurface_widget, GTK_TYPE_WIDGET)

static void registry_global(void *data, struct wl_registry *registry,
                             uint32_t name, const char *interface,
                             uint32_t version) {
    SubsurfaceWidget *self = data;
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
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static gboolean setup_egl(SubsurfaceWidget *self, struct wl_display *display,
                           gint width, gint height) {
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
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE,
    };
    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(self->egl_display, config_attribs, &config, 1,
                         &num_configs) ||
        num_configs == 0) {
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

    self->egl_window = wl_egl_window_create(self->surface, width, height);
    if (!self->egl_window) {
        g_warning("Failed to create wl_egl_window");
        return FALSE;
    }

    self->egl_surface = eglCreateWindowSurface(
        self->egl_display, config, (EGLNativeWindowType)self->egl_window, NULL);
    if (self->egl_surface == EGL_NO_SURFACE) {
        g_warning("Failed to create EGL window surface");
        return FALSE;
    }

    return TRUE;
}

static void render(SubsurfaceWidget *self, gint width, gint height) {
    eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                   self->egl_context);
    glViewport(0, 0, width, height);
    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(self->egl_display, self->egl_surface);
}

static void subsurface_widget_realize(GtkWidget *widget) {
    SubsurfaceWidget *self = SUBSURFACE_WIDGET(widget);

    GTK_WIDGET_CLASS(subsurface_widget_parent_class)->realize(widget);

    GdkDisplay *gdk_display = gtk_widget_get_display(widget);
    if (!GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        g_warning("SubsurfaceWidget requires a Wayland display");
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

    GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
    GdkWindow *gdk_window = gtk_widget_get_window(toplevel);
    struct wl_surface *parent_surface =
        gdk_wayland_window_get_wl_surface(gdk_window);

    self->surface = wl_compositor_create_surface(self->compositor);
    self->subsurface = wl_subcompositor_get_subsurface(
        self->subcompositor, self->surface, parent_surface);

    // Commit in sync with parent so position updates are applied together
    // with the parent surface's frame.
    wl_subsurface_set_sync(self->subsurface);

    gint x, y;
    gtk_widget_translate_coordinates(widget, toplevel, 0, 0, &x, &y);
    wl_subsurface_set_position(self->subsurface, x, y);

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    if (!setup_egl(self, display, alloc.width, alloc.height))
        return;

    render(self, alloc.width, alloc.height);
}

static void subsurface_widget_unrealize(GtkWidget *widget) {
    SubsurfaceWidget *self = SUBSURFACE_WIDGET(widget);

    if (self->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
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

    if (self->subsurface) {
        wl_subsurface_destroy(self->subsurface);
        self->subsurface = NULL;
    }
    if (self->surface) {
        wl_surface_destroy(self->surface);
        self->surface = NULL;
    }

    GTK_WIDGET_CLASS(subsurface_widget_parent_class)->unrealize(widget);
}

static void subsurface_widget_size_allocate(GtkWidget *widget,
                                             GtkAllocation *allocation) {
    GTK_WIDGET_CLASS(subsurface_widget_parent_class)
        ->size_allocate(widget, allocation);

    SubsurfaceWidget *self = SUBSURFACE_WIDGET(widget);
    if (!self->subsurface)
        return;

    GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
    gint x, y;
    gtk_widget_translate_coordinates(widget, toplevel, 0, 0, &x, &y);
    wl_subsurface_set_position(self->subsurface, x, y);

    if (self->egl_window) {
        wl_egl_window_resize(self->egl_window,
                             allocation->width, allocation->height, 0, 0);
        render(self, allocation->width, allocation->height);
    }
}

static void subsurface_widget_get_preferred_width(GtkWidget *widget G_GNUC_UNUSED,
                                                   gint *minimum,
                                                   gint *natural) {
    *minimum = 1;
    *natural = 400;
}

static void subsurface_widget_get_preferred_height(GtkWidget *widget G_GNUC_UNUSED,
                                                    gint *minimum,
                                                    gint *natural) {
    *minimum = 1;
    *natural = 300;
}

static void subsurface_widget_finalize(GObject *object) {
    SubsurfaceWidget *self = SUBSURFACE_WIDGET(object);

    if (self->subcompositor) {
        wl_subcompositor_destroy(self->subcompositor);
        self->subcompositor = NULL;
    }
    if (self->compositor) {
        wl_compositor_destroy(self->compositor);
        self->compositor = NULL;
    }

    G_OBJECT_CLASS(subsurface_widget_parent_class)->finalize(object);
}

static void subsurface_widget_class_init(SubsurfaceWidgetClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->finalize = subsurface_widget_finalize;

    widget_class->realize = subsurface_widget_realize;
    widget_class->unrealize = subsurface_widget_unrealize;
    widget_class->size_allocate = subsurface_widget_size_allocate;
    widget_class->get_preferred_width = subsurface_widget_get_preferred_width;
    widget_class->get_preferred_height = subsurface_widget_get_preferred_height;
}

static void subsurface_widget_init(SubsurfaceWidget *self) {
    gtk_widget_set_has_window(GTK_WIDGET(self), FALSE);
    self->egl_display = EGL_NO_DISPLAY;
    self->egl_context = EGL_NO_CONTEXT;
    self->egl_surface = EGL_NO_SURFACE;
}

GtkWidget *subsurface_widget_new(void) {
    return g_object_new(SUBSURFACE_WIDGET_TYPE, NULL);
}
