#include "flutter_gl_compositor.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

struct _FlutterGLCompositor {
    EGLDisplay egl_display;
    EGLContext renderer_egl_context;
    EGLSurface renderer_egl_surface;
};

/* ── Public API ───────────────────────────────────────────────────────────── */

FlutterGLCompositor *flutter_gl_compositor_new(EGLDisplay egl_display,
                                               EGLContext share_context) {
    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE,
    };
    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    static const EGLint pbuffer_attribs[] = {
        EGL_WIDTH,  1,
        EGL_HEIGHT, 1,
        EGL_NONE,
    };

    FlutterGLCompositor *self = g_new0(FlutterGLCompositor, 1);
    self->egl_display          = egl_display;
    self->renderer_egl_context = EGL_NO_CONTEXT;
    self->renderer_egl_surface = EGL_NO_SURFACE;

    EGLConfig config;
    EGLint    num_configs;
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(self->egl_display, config_attribs,
                         &config, 1, &num_configs) || num_configs == 0) {
        g_warning("FlutterGLCompositor: failed to choose EGL config");
        flutter_gl_compositor_free(self);
        return NULL;
    }

    self->renderer_egl_context = eglCreateContext(
        self->egl_display, config, share_context, context_attribs);
    if (self->renderer_egl_context == EGL_NO_CONTEXT) {
        g_warning("FlutterGLCompositor: failed to create renderer EGL context");
        flutter_gl_compositor_free(self);
        return NULL;
    }

    self->renderer_egl_surface = eglCreatePbufferSurface(
        self->egl_display, config, pbuffer_attribs);
    if (self->renderer_egl_surface == EGL_NO_SURFACE) {
        g_warning("FlutterGLCompositor: failed to create renderer pbuffer");
        flutter_gl_compositor_free(self);
        return NULL;
    }

    return self;
}

void flutter_gl_compositor_free(FlutterGLCompositor *self) {
    if (!self)
        return;

    if (self->renderer_egl_surface != EGL_NO_SURFACE)
        eglDestroySurface(self->egl_display, self->renderer_egl_surface);
    if (self->renderer_egl_context != EGL_NO_CONTEXT)
        eglDestroyContext(self->egl_display, self->renderer_egl_context);

    g_free(self);
}

EGLDisplay flutter_gl_compositor_get_egl_display(FlutterGLCompositor *self) {
    return self->egl_display;
}

gboolean flutter_gl_compositor_make_current(FlutterGLCompositor *self) {
    return eglMakeCurrent(self->egl_display, self->renderer_egl_surface,
                          self->renderer_egl_surface,
                          self->renderer_egl_context) == EGL_TRUE;
}

void flutter_gl_compositor_clear_current(FlutterGLCompositor *self) {
    eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
}

FlutterBackingStore *flutter_gl_compositor_create_backing_store(
    FlutterGLCompositor *self G_GNUC_UNUSED,
    size_t width, size_t height) {

    FlutterBackingStore *store = g_new0(FlutterBackingStore, 1);
    store->width  = width;
    store->height = height;

    glGenTextures(1, &store->texture);
    glBindTexture(GL_TEXTURE_2D, store->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 (GLsizei)width, (GLsizei)height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    return store;
}

void flutter_gl_compositor_collect_backing_store(
    FlutterGLCompositor *self G_GNUC_UNUSED,
    FlutterBackingStore *store) {
    if (!store)
        return;
    glDeleteTextures(1, &store->texture);
    g_free(store);
}
