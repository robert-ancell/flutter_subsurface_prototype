#include <gtk/gtk.h>

#include "renderer_gl.h"
#include "renderer_software.h"
#include "flutter_renderer.h"
#include "flutter_gl_renderer.h"
#include "flutter_subsurface_renderer.h"
#include "flutter_software_renderer.h"

/* ── Command-line options ─────────────────────────────────────────────────── */

static gboolean opt_subsurface = FALSE;
static gboolean opt_software   = FALSE;

static const GOptionEntry option_entries[] = {
    { "subsurface", 0, 0, G_OPTION_ARG_NONE, &opt_subsurface,
      "Use Wayland subsurface renderer", NULL },
    { "software", 0, 0, G_OPTION_ARG_NONE, &opt_software,
      "Use software renderer", NULL },
    { NULL }
};

/* ── App callbacks ────────────────────────────────────────────────────────── */

typedef struct {
    RendererGL       *renderer_gl;
    RendererSoftware *renderer_sw;
} AppData;

static gboolean on_delete_event(GtkWidget *window   G_GNUC_UNUSED,
                                 GdkEvent  *event    G_GNUC_UNUSED,
                                 gpointer   data) {
    AppData *app_data = data;
    renderer_gl_free(app_data->renderer_gl);
    app_data->renderer_gl = NULL;
    renderer_software_free(app_data->renderer_sw);
    app_data->renderer_sw = NULL;
    return FALSE;
}

static void on_resize_gl(size_t width, size_t height,
                          gint scale, gpointer user_data) {
    AppData *app_data = user_data;
    if (app_data->renderer_gl)
        renderer_gl_resize(app_data->renderer_gl, width, height, scale);
}

static void on_resize_sw(size_t width, size_t height,
                          gint scale, gpointer user_data) {
    AppData *app_data = user_data;
    if (app_data->renderer_sw)
        renderer_software_resize(app_data->renderer_sw, width, height, scale);
}

static void activate(GtkApplication *app, gpointer user_data G_GNUC_UNUSED) {
    if (opt_subsurface && opt_software) {
        g_printerr("Error: --subsurface and --software are mutually exclusive\n");
        g_application_quit(G_APPLICATION(app));
        return;
    }

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Subsurface Prototype");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    AppData *app_data = g_new0(AppData, 1);

    GtkWidget *widget;
    if (opt_software) {
        widget = flutter_software_renderer_new(on_resize_sw, app_data);
    } else if (opt_subsurface) {
        widget = flutter_subsurface_renderer_new(on_resize_gl, app_data);
    } else {
        widget = flutter_gl_renderer_new(on_resize_gl, app_data);
    }
    gtk_container_add(GTK_CONTAINER(window), widget);

    gtk_widget_show_all(window);

    if (opt_software)
        app_data->renderer_sw = renderer_software_new(FLUTTER_RENDERER(widget));
    else
        app_data->renderer_gl = renderer_gl_new(FLUTTER_RENDERER(widget));

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
