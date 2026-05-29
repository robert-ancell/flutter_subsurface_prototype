#include <gtk/gtk.h>

#include "renderer.h"
#include "flutter_gl_view.h"
#include "flutter_subsurface_view.h"

/* ── Command-line options ─────────────────────────────────────────────────── */

static gboolean opt_glarea    = FALSE;
static gboolean opt_subsurface = FALSE;

static const GOptionEntry option_entries[] = {
    { "glarea",     0, 0, G_OPTION_ARG_NONE, &opt_glarea,
      "Use GtkGLArea renderer", NULL },
    { "subsurface", 0, 0, G_OPTION_ARG_NONE, &opt_subsurface,
      "Use Wayland subsurface renderer (default)", NULL },
    { NULL }
};

static gint on_handle_local_options(GApplication *app     G_GNUC_UNUSED,
                                     GVariantDict *options G_GNUC_UNUSED,
                                     gpointer      data    G_GNUC_UNUSED) {
    if (opt_glarea && opt_subsurface) {
        g_printerr("Error: --glarea and --subsurface are mutually exclusive\n");
        return 1;
    }
    return -1; /* continue normal startup */
}

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

static void on_size_allocate(GtkWidget     *widget     G_GNUC_UNUSED,
                              GtkAllocation *allocation,
                              gpointer       data) {
    AppData *app_data = data;
    if (app_data->renderer) {
        gint scale = gtk_widget_get_scale_factor(widget);
        renderer_resize(app_data->renderer,
                        (size_t)allocation->width,
                        (size_t)allocation->height,
                        scale);
    }
}

static void activate(GtkApplication *app, gpointer user_data G_GNUC_UNUSED) {
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Subsurface Prototype");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    GtkWidget *widget = opt_glarea
        ? flutter_gl_view_new()
        : flutter_subsurface_view_new();
    gtk_container_add(GTK_CONTAINER(window), widget);

    gtk_widget_show_all(window);

    /* Widget is realized after show_all; create the renderer now that the
       widget's EGL context exists. */
    AppData *app_data  = g_new0(AppData, 1);
    app_data->renderer = renderer_new(FLUTTER_VIEW(widget));

    g_signal_connect(widget, "size-allocate",
                     G_CALLBACK(on_size_allocate), app_data);
    g_signal_connect(window, "delete-event",
                     G_CALLBACK(on_delete_event), app_data);
    g_signal_connect_swapped(window, "destroy",
                             G_CALLBACK(g_free), app_data);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new(
        "com.example.flutter-subsurface-prototype", 0);

    g_application_add_main_option_entries(G_APPLICATION(app), option_entries);
    g_signal_connect(app, "handle-local-options",
                     G_CALLBACK(on_handle_local_options), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
