#include "flutter_view.h"

G_DEFINE_INTERFACE(FlutterView, flutter_view, GTK_TYPE_WIDGET)

static void flutter_view_default_init(FlutterViewInterface *iface G_GNUC_UNUSED) {}

FlutterBackingStore *flutter_view_create_backing_store(FlutterView *self,
                                                       size_t       width,
                                                       size_t       height) {
    g_return_val_if_fail(FLUTTER_IS_VIEW(self), NULL);
    FlutterViewInterface *iface = FLUTTER_VIEW_GET_IFACE(self);
    g_return_val_if_fail(iface->create_backing_store != NULL, NULL);
    return iface->create_backing_store(self, width, height);
}

void flutter_view_collect_backing_store(FlutterView         *self,
                                        FlutterBackingStore *backing_store) {
    g_return_if_fail(FLUTTER_IS_VIEW(self));
    FlutterViewInterface *iface = FLUTTER_VIEW_GET_IFACE(self);
    g_return_if_fail(iface->collect_backing_store != NULL);
    iface->collect_backing_store(self, backing_store);
}

void flutter_view_present(FlutterView *self,
                          GLuint       texture_id,
                          GLenum       texture_format,
                          size_t       width,
                          size_t       height) {
    g_return_if_fail(FLUTTER_IS_VIEW(self));
    FlutterViewInterface *iface = FLUTTER_VIEW_GET_IFACE(self);
    g_return_if_fail(iface->present != NULL);
    iface->present(self, texture_id, texture_format, width, height);
}

gboolean flutter_view_make_current(FlutterView *self) {
    g_return_val_if_fail(FLUTTER_IS_VIEW(self), FALSE);
    FlutterViewInterface *iface = FLUTTER_VIEW_GET_IFACE(self);
    g_return_val_if_fail(iface->make_current != NULL, FALSE);
    return iface->make_current(self);
}

void flutter_view_clear_current(FlutterView *self) {
    g_return_if_fail(FLUTTER_IS_VIEW(self));
    FlutterViewInterface *iface = FLUTTER_VIEW_GET_IFACE(self);
    g_return_if_fail(iface->clear_current != NULL);
    iface->clear_current(self);
}
