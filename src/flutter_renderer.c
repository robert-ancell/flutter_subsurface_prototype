#include "flutter_renderer.h"

G_DEFINE_INTERFACE(FlutterRenderer, flutter_renderer, GTK_TYPE_WIDGET)

static void flutter_renderer_default_init(FlutterRendererInterface *iface G_GNUC_UNUSED) {
}

FlutterBackingStore *flutter_renderer_create_backing_store(FlutterRenderer *self,
                                                            size_t           width,
                                                            size_t           height) {
    return FLUTTER_RENDERER_GET_IFACE(self)->create_backing_store(self, width, height);
}

void flutter_renderer_collect_backing_store(FlutterRenderer     *self,
                                             FlutterBackingStore *backing_store) {
    FLUTTER_RENDERER_GET_IFACE(self)->collect_backing_store(self, backing_store);
}

void flutter_renderer_present(FlutterRenderer     *self,
                               FlutterBackingStore *backing_store) {
    FLUTTER_RENDERER_GET_IFACE(self)->present(self, backing_store);
}

gboolean flutter_renderer_make_current(FlutterRenderer *self) {
    return FLUTTER_RENDERER_GET_IFACE(self)->make_current(self);
}

void flutter_renderer_clear_current(FlutterRenderer *self) {
    FLUTTER_RENDERER_GET_IFACE(self)->clear_current(self);
}
