#include "flutter_gl_area_view.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

struct _FlutterGLAreaView {
    GtkDrawingArea parent_instance;

    GdkGLContext *gdk_gl_context;
    EGLDisplay    egl_display;

    /* Renderer context sharing with GDK's paint context */
    EGLContext renderer_egl_context;
    EGLSurface renderer_egl_surface;

    /* Thread-safe pending-present state */
    GMutex   present_mutex;
    gboolean present_scheduled;
    gboolean has_frame;
    GLuint   present_texture;
    size_t   present_width;
    size_t   present_height;
};

static void flutter_gl_area_view_iface_init(FlutterViewInterface *iface);

G_DEFINE_TYPE_WITH_CODE(FlutterGLAreaView, flutter_gl_area_view, GTK_TYPE_DRAWING_AREA,
    G_IMPLEMENT_INTERFACE(FLUTTER_TYPE_VIEW, flutter_gl_area_view_iface_init))

/* ── GtkWidget vfuncs ─────────────────────────────────────────────────────── */

static void flutter_gl_area_view_realize(GtkWidget *widget) {
    GTK_WIDGET_CLASS(flutter_gl_area_view_parent_class)->realize(widget);

    FlutterGLAreaView *self = FLUTTER_GL_AREA_VIEW(widget);
    GdkWindow *gdk_window = gtk_widget_get_window(widget);

    /* Create a GDK GL context for this window.  GDK will also create an
       internal "paint context" that shares with it — textures created in
       any context in the share group are visible to gdk_cairo_draw_from_gl. */
    GError *error = NULL;
    self->gdk_gl_context = gdk_window_create_gl_context(gdk_window, &error);
    if (!self->gdk_gl_context) {
        g_warning("FlutterGLAreaView: failed to create GDK GL context: %s",
                  error->message);
        g_error_free(error);
        return;
    }

    gdk_gl_context_set_use_es(self->gdk_gl_context, TRUE);
    gdk_gl_context_set_required_version(self->gdk_gl_context, 2, 0);

    if (!gdk_gl_context_realize(self->gdk_gl_context, &error)) {
        g_warning("FlutterGLAreaView: failed to realize GDK GL context: %s",
                  error->message);
        g_error_free(error);
        g_clear_object(&self->gdk_gl_context);
        return;
    }

    /* Make the GDK context current so we can query the underlying EGL
       display and context handle for creating the renderer context. */
    gdk_gl_context_make_current(self->gdk_gl_context);
    self->egl_display = eglGetCurrentDisplay();
    EGLContext gdk_egl_context = eglGetCurrentContext();

    /* Create a renderer context that shares objects with GDK's context.
       Textures created by the renderer are directly visible to
       gdk_cairo_draw_from_gl without any EGL image transfer. */
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

    EGLConfig config;
    EGLint    num_configs;
    eglBindAPI(EGL_OPENGL_ES_API);
    if (eglChooseConfig(self->egl_display, config_attribs,
                        &config, 1, &num_configs) && num_configs > 0) {
        self->renderer_egl_context = eglCreateContext(
            self->egl_display, config, gdk_egl_context, context_attribs);
        if (self->renderer_egl_context != EGL_NO_CONTEXT) {
            self->renderer_egl_surface = eglCreatePbufferSurface(
                self->egl_display, config, pbuffer_attribs);
            if (self->renderer_egl_surface == EGL_NO_SURFACE) {
                g_warning("FlutterGLAreaView: failed to create renderer pbuffer");
                eglDestroyContext(self->egl_display, self->renderer_egl_context);
                self->renderer_egl_context = EGL_NO_CONTEXT;
            }
        } else {
            g_warning("FlutterGLAreaView: failed to create renderer EGL context");
        }
    } else {
        g_warning("FlutterGLAreaView: failed to choose EGL config for renderer");
    }

    gdk_gl_context_clear_current();
}

static void flutter_gl_area_view_unrealize(GtkWidget *widget) {
    FlutterGLAreaView *self = FLUTTER_GL_AREA_VIEW(widget);

    g_mutex_lock(&self->present_mutex);
    self->has_frame = FALSE;
    self->present_texture = 0;
    g_mutex_unlock(&self->present_mutex);

    if (self->renderer_egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(self->egl_display, self->renderer_egl_surface);
        self->renderer_egl_surface = EGL_NO_SURFACE;
    }
    if (self->renderer_egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(self->egl_display, self->renderer_egl_context);
        self->renderer_egl_context = EGL_NO_CONTEXT;
    }

    g_clear_object(&self->gdk_gl_context);
    self->egl_display = EGL_NO_DISPLAY;

    GTK_WIDGET_CLASS(flutter_gl_area_view_parent_class)->unrealize(widget);
}

static gboolean flutter_gl_area_view_draw(GtkWidget *widget, cairo_t *cr) {
    FlutterGLAreaView *self = FLUTTER_GL_AREA_VIEW(widget);

    g_mutex_lock(&self->present_mutex);
    gboolean has_frame = self->has_frame;
    GLuint   texture   = self->present_texture;
    size_t   width     = self->present_width;
    size_t   height    = self->present_height;
    g_mutex_unlock(&self->present_mutex);

    if (!has_frame || !self->gdk_gl_context) {
        /* No frame yet — draw black. */
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        return TRUE;
    }

    GdkWindow *gdk_window = gtk_widget_get_window(widget);
    gint scale = gdk_window_get_scale_factor(gdk_window);

    gdk_cairo_draw_from_gl(cr, gdk_window,
                           (int)texture, GL_TEXTURE, scale,
                           0, 0, (int)width, (int)height);
    return TRUE;
}

static void flutter_gl_area_view_finalize(GObject *object) {
    FlutterGLAreaView *self = FLUTTER_GL_AREA_VIEW(object);
    g_mutex_clear(&self->present_mutex);
    G_OBJECT_CLASS(flutter_gl_area_view_parent_class)->finalize(object);
}

static void flutter_gl_area_view_class_init(FlutterGLAreaViewClass *klass) {
    GObjectClass   *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->finalize  = flutter_gl_area_view_finalize;
    widget_class->realize   = flutter_gl_area_view_realize;
    widget_class->unrealize = flutter_gl_area_view_unrealize;
    widget_class->draw      = flutter_gl_area_view_draw;
}

static void flutter_gl_area_view_init(FlutterGLAreaView *self) {
    self->egl_display          = EGL_NO_DISPLAY;
    self->renderer_egl_context = EGL_NO_CONTEXT;
    self->renderer_egl_surface = EGL_NO_SURFACE;
    g_mutex_init(&self->present_mutex);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

GtkWidget *flutter_gl_area_view_new(void) {
    return g_object_new(FLUTTER_GL_AREA_VIEW_TYPE, NULL);
}

/* Main-thread callback: schedule a redraw.
   Holds a strong reference to the widget taken in present(). */
static gboolean do_queue_draw(gpointer data) {
    FlutterGLAreaView *self = FLUTTER_GL_AREA_VIEW(data);

    g_mutex_lock(&self->present_mutex);
    self->present_scheduled = FALSE;
    g_mutex_unlock(&self->present_mutex);

    gtk_widget_queue_draw(GTK_WIDGET(self));

    g_object_unref(self);
    return G_SOURCE_REMOVE;
}

static void flutter_gl_area_view_present(FlutterGLAreaView *self,
                                          GLuint             texture_id,
                                          GLenum             texture_format G_GNUC_UNUSED,
                                          size_t             width,
                                          size_t             height) {
    g_mutex_lock(&self->present_mutex);

    self->present_texture = texture_id;
    self->present_width   = width;
    self->present_height  = height;
    self->has_frame       = TRUE;

    gboolean was_scheduled = self->present_scheduled;
    if (!was_scheduled) {
        self->present_scheduled = TRUE;
        g_object_ref(self);
    }

    g_mutex_unlock(&self->present_mutex);

    if (!was_scheduled)
        g_main_context_invoke(NULL, do_queue_draw, self);
}

static FlutterBackingStore *
flutter_gl_area_view_create_backing_store(FlutterGLAreaView *self G_GNUC_UNUSED,
                                          size_t             width,
                                          size_t             height) {
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

static void flutter_gl_area_view_collect_backing_store(FlutterGLAreaView   *self G_GNUC_UNUSED,
                                                FlutterBackingStore *backing_store) {
    if (!backing_store)
        return;
    glDeleteTextures(1, &backing_store->texture);
    g_free(backing_store);
}

/* ── FlutterView interface ────────────────────────────────────────────────── */

static FlutterBackingStore *
gl_area_view_iface_create_backing_store(FlutterView *view, size_t width, size_t height) {
    return flutter_gl_area_view_create_backing_store(
        FLUTTER_GL_AREA_VIEW(view), width, height);
}

static void
gl_area_view_iface_collect_backing_store(FlutterView         *view,
                                         FlutterBackingStore *backing_store) {
    flutter_gl_area_view_collect_backing_store(
        FLUTTER_GL_AREA_VIEW(view), backing_store);
}

static void
gl_area_view_iface_present(FlutterView *view, GLuint texture_id,
                            GLenum texture_format, size_t width, size_t height) {
    flutter_gl_area_view_present(FLUTTER_GL_AREA_VIEW(view),
                                 texture_id, texture_format, width, height);
}

static gboolean gl_area_view_iface_make_current(FlutterView *view) {
    FlutterGLAreaView *self = FLUTTER_GL_AREA_VIEW(view);
    return eglMakeCurrent(self->egl_display, self->renderer_egl_surface,
                          self->renderer_egl_surface,
                          self->renderer_egl_context) == EGL_TRUE;
}

static void gl_area_view_iface_clear_current(FlutterView *view) {
    FlutterGLAreaView *self = FLUTTER_GL_AREA_VIEW(view);
    eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
}

static void flutter_gl_area_view_iface_init(FlutterViewInterface *iface) {
    iface->create_backing_store  = gl_area_view_iface_create_backing_store;
    iface->collect_backing_store = gl_area_view_iface_collect_backing_store;
    iface->present               = gl_area_view_iface_present;
    iface->make_current          = gl_area_view_iface_make_current;
    iface->clear_current         = gl_area_view_iface_clear_current;
}
