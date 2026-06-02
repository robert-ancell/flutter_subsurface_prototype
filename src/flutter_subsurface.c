#include "flutter_subsurface.h"

#include <gdk/gdkwayland.h>
#include <string.h>

struct _FlutterSubsurface {
    GObject parent_instance;

    struct wl_compositor    *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_surface       *surface;
    struct wl_subsurface    *subsurface;
};

G_DEFINE_TYPE(FlutterSubsurface, flutter_subsurface, G_TYPE_OBJECT)

/* ── Wayland registry ─────────────────────────────────────────────────────── */

static void registry_global(void *data, struct wl_registry *registry,
                             uint32_t name, const char *interface,
                             uint32_t version) {
    FlutterSubsurface *self = data;
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

/* ── GObject vfuncs ───────────────────────────────────────────────────────── */

static void flutter_subsurface_dispose(GObject *object) {
    FlutterSubsurface *self = FLUTTER_SUBSURFACE(object);

    if (self->subsurface) {
        wl_subsurface_destroy(self->subsurface);
        self->subsurface = NULL;
    }
    if (self->surface) {
        wl_surface_destroy(self->surface);
        self->surface = NULL;
    }
    if (self->subcompositor) {
        wl_subcompositor_destroy(self->subcompositor);
        self->subcompositor = NULL;
    }
    if (self->compositor) {
        wl_compositor_destroy(self->compositor);
        self->compositor = NULL;
    }

    G_OBJECT_CLASS(flutter_subsurface_parent_class)->dispose(object);
}

static void flutter_subsurface_class_init(FlutterSubsurfaceClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = flutter_subsurface_dispose;
}

static void flutter_subsurface_init(FlutterSubsurface *self G_GNUC_UNUSED) {
}

/* ── Public API ───────────────────────────────────────────────────────────── */

FlutterSubsurface *flutter_subsurface_new(GtkWidget *widget) {
    GdkDisplay *gdk_display = gtk_widget_get_display(widget);
    if (!GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        g_warning("FlutterSubsurface requires a Wayland display");
        return NULL;
    }

    FlutterSubsurface *self = g_object_new(FLUTTER_TYPE_SUBSURFACE, NULL);

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
        g_object_unref(self);
        return NULL;
    }

    GtkWidget *toplevel   = gtk_widget_get_toplevel(widget);
    GdkWindow *gdk_window = gtk_widget_get_window(toplevel);
    struct wl_surface *parent_surface =
        gdk_wayland_window_get_wl_surface(gdk_window);

    self->surface = wl_compositor_create_surface(self->compositor);
    self->subsurface = wl_subcompositor_get_subsurface(
        self->subcompositor, self->surface, parent_surface);

    wl_subsurface_set_sync(self->subsurface);

    gint x, y;
    gtk_widget_translate_coordinates(widget, toplevel, 0, 0, &x, &y);
    wl_subsurface_set_position(self->subsurface, x, y);

    return self;
}

struct wl_surface *flutter_subsurface_get_surface(FlutterSubsurface *self) {
    return self->surface;
}

void flutter_subsurface_set_position(FlutterSubsurface *self,
                                     gint x, gint y) {
    if (self->subsurface)
        wl_subsurface_set_position(self->subsurface, x, y);
}
