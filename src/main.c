#include <gtk/gtk.h>

#include "subsurface_widget.h"

static void activate(GtkApplication *app, gpointer user_data G_GNUC_UNUSED) {
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Subsurface Prototype");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    GtkWidget *widget = subsurface_widget_new();
    gtk_container_add(GTK_CONTAINER(window), widget);

    gtk_widget_show_all(window);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new(
        "com.example.flutter-subsurface-prototype", 0);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
