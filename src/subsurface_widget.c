#include "subsurface_widget.h"

#include <gdk/gdkwayland.h>
#include <string.h>
#include <wayland-client.h>

struct _SubsurfaceWidget {
    GtkWidget parent_instance;

    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_surface *surface;
    struct wl_subsurface *subsurface;
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
    wl_surface_commit(self->surface);
}

static void subsurface_widget_unrealize(GtkWidget *widget) {
    SubsurfaceWidget *self = SUBSURFACE_WIDGET(widget);

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
    wl_surface_commit(self->surface);
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

// Draw a placeholder so the widget area is visible before subsurface
// content is connected.
static gboolean subsurface_widget_draw(GtkWidget *widget, cairo_t *cr) {
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
    cairo_rectangle(cr, 0, 0, alloc.width, alloc.height);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, 0, 0);
    cairo_line_to(cr, alloc.width, alloc.height);
    cairo_move_to(cr, alloc.width, 0);
    cairo_line_to(cr, 0, alloc.height);
    cairo_stroke(cr);

    return FALSE;
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
    widget_class->draw = subsurface_widget_draw;
}

static void subsurface_widget_init(SubsurfaceWidget *self) {
    gtk_widget_set_has_window(GTK_WIDGET(self), FALSE);
}

GtkWidget *subsurface_widget_new(void) {
    return g_object_new(SUBSURFACE_WIDGET_TYPE, NULL);
}
