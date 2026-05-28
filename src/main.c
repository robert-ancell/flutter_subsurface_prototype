#include <gtk/gtk.h>

#include "renderer.h"
#include "flutter_subsurface_view.h"

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
    if (app_data->renderer)
        renderer_resize(app_data->renderer,
                        (size_t)allocation->width,
                        (size_t)allocation->height);
}

static void activate(GtkApplication *app, gpointer user_data G_GNUC_UNUSED) {
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Subsurface Prototype");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    GtkWidget *widget = flutter_subsurface_view_new();
    gtk_container_add(GTK_CONTAINER(window), widget);

    gtk_widget_show_all(window);

    /* Widget is realized after show_all; create the renderer now that the
       widget's EGL context exists. */
    AppData *app_data  = g_new0(AppData, 1);
    app_data->renderer = renderer_new(FLUTTER_SUBSURFACE_VIEW(widget));

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
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
