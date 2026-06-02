#include <gtk/gtk.h>

#include "renderer_gl.h"
#include "flutter_renderer.h"
#include "flutter_gl_renderer.h"
#include "flutter_subsurface_renderer.h"

/* ── Command-line options ─────────────────────────────────────────────────── */

static gboolean opt_subsurface = FALSE;

static const GOptionEntry option_entries[] = {
    { "subsurface", 0, 0, G_OPTION_ARG_NONE, &opt_subsurface,
      "Use Wayland subsurface renderer instead of gdk_cairo_draw_from_gl", NULL },
    { NULL }
};

/* ── App callbacks ────────────────────────────────────────────────────────── */

typedef struct {
    Renderer *renderer;
} AppData;

/* Stop the renderer before the window tears down its EGL context. */
static gboolean on_delete_event(GtkWidget *window   G_GNUC_UNUSED,
                                 GdkEvent  *event    G_GNUC_UNUSED,
                                 gpointer   data) {
    AppData *app_data = data;
    renderer_free(app_data->renderer);
    app_data->renderer = NULL;
    return FALSE;
}

static void on_resize(size_t width, size_t height,
                      gint scale, gpointer user_data) {
    AppData *app_data = user_data;
    if (app_data->renderer)
        renderer_resize(app_data->renderer, width, height, scale);
}

static void activate(GtkApplication *app, gpointer user_data G_GNUC_UNUSED) {
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Subsurface Prototype");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    AppData *app_data = g_new0(AppData, 1);

    GtkWidget *widget;
    if (opt_subsurface)
        widget = flutter_subsurface_renderer_new(on_resize, app_data);
    else
        widget = flutter_gl_renderer_new(on_resize, app_data);
    gtk_container_add(GTK_CONTAINER(window), widget);

    gtk_widget_show_all(window);

    app_data->renderer = renderer_new(FLUTTER_RENDERER(widget));

    g_signal_connect(window, "delete-event",
                     G_CALLBACK(on_delete_event), app_data);
    g_signal_connect_swapped(window, "destroy",
                             G_CALLBACK(g_free), app_data);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new(
        "com.example.flutter-subsurface-prototype", 0);

    g_application_add_main_option_entries(G_APPLICATION(app), option_entries);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
