#include "flutter_subsurface_view.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gdk/gdkwayland.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>

/* GLES3 constants not in GLES2 headers */
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif

typedef void (GL_APIENTRY *PFNGLBLITFRAMEBUFFERPROC)(
    GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
    GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
    GLbitfield mask, GLenum filter);

struct _FlutterSubsurfaceView {
    GtkWidget parent_instance;

    gint scale;

    struct wl_compositor    *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_surface       *surface;
    struct wl_subsurface    *subsurface;

    struct wl_egl_window *egl_window;
    EGLDisplay            egl_display;
    EGLContext            egl_context;
    EGLSurface            egl_surface;

    /* Shared context + 1×1 pbuffer for the renderer thread */
    EGLContext renderer_egl_context;
    EGLSurface renderer_egl_surface;

    /* GL resources for the texture blit (shader fallback) */
    GLuint gl_program;
    GLuint gl_vbo;
    GLint  gl_position_loc;
    GLint  gl_texture_loc;

    /* glBlitFramebuffer path (preferred when available) */
    PFNGLBLITFRAMEBUFFERPROC p_glBlitFramebuffer;
    GLuint                   blit_read_fbo;

    /* Resize synchronization: freeze the toplevel window until the
       renderer delivers a frame at the expected size. */
    gboolean has_presented;  /* TRUE after the first frame is shown */
    gboolean awaiting_frame;
    size_t   expected_width;
    size_t   expected_height;

    /* Thread-safe pending-present state */
    GMutex   present_mutex;
    gboolean present_scheduled;
    GLuint   present_texture_id;
    GLenum   present_texture_format;
    size_t   present_width;
    size_t   present_height;
};

static void flutter_subsurface_view_iface_init(FlutterViewInterface *iface);

G_DEFINE_TYPE_WITH_CODE(FlutterSubsurfaceView, flutter_subsurface_view, GTK_TYPE_WIDGET,
    G_IMPLEMENT_INTERFACE(FLUTTER_TYPE_VIEW, flutter_subsurface_view_iface_init))

/* ── Wayland registry ─────────────────────────────────────────────────────── */

static void registry_global(void *data, struct wl_registry *registry,
                             uint32_t name, const char *interface,
                             uint32_t version) {
    FlutterSubsurfaceView *self = data;
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
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* ── GL helpers ───────────────────────────────────────────────────────────── */

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint compile_status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);
    if (compile_status == GL_FALSE) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        g_warning("Shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static gboolean setup_gl(FlutterSubsurfaceView *self) {
    /* Try to get glBlitFramebuffer (available in GLES 3.0+). */
    self->p_glBlitFramebuffer = (PFNGLBLITFRAMEBUFFERPROC)
        eglGetProcAddress("glBlitFramebuffer");
    if (self->p_glBlitFramebuffer) {
        glGenFramebuffers(1, &self->blit_read_fbo);
        return TRUE;
    }

    /* Fallback: compile a shader program for full-screen texture blit. */
    static const char *vert_src =
        "attribute vec2 a_position;\n"
        "varying vec2 v_uv;\n"
        "void main() {\n"
        "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
        "    v_uv = a_position * 0.5 + vec2(0.5);\n"
        "}\n";

    static const char *frag_src =
        "precision mediump float;\n"
        "uniform sampler2D u_texture;\n"
        "varying vec2 v_uv;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(u_texture, v_uv);\n"
        "}\n";

    GLuint vert = compile_shader(GL_VERTEX_SHADER,   vert_src);
    if (vert == 0) return FALSE;
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (frag == 0) { glDeleteShader(vert); return FALSE; }

    self->gl_program = glCreateProgram();
    glAttachShader(self->gl_program, vert);
    glAttachShader(self->gl_program, frag);
    glLinkProgram(self->gl_program);
    glDeleteShader(vert);
    glDeleteShader(frag);

    GLint link_status;
    glGetProgramiv(self->gl_program, GL_LINK_STATUS, &link_status);
    if (link_status == GL_FALSE) {
        char log[512];
        glGetProgramInfoLog(self->gl_program, sizeof(log), NULL, log);
        g_warning("Shader link error: %s", log);
        glDeleteProgram(self->gl_program);
        self->gl_program = 0;
        return FALSE;
    }

    self->gl_position_loc = glGetAttribLocation (self->gl_program, "a_position");
    self->gl_texture_loc  = glGetUniformLocation(self->gl_program, "u_texture");

    /* Full-screen quad as a triangle strip */
    static const GLfloat vertices[] = {
        -1.0f,  1.0f,
        -1.0f, -1.0f,
         1.0f,  1.0f,
         1.0f, -1.0f,
    };
    glGenBuffers(1, &self->gl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, self->gl_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return TRUE;
}

static void teardown_gl(FlutterSubsurfaceView *self) {
    if (self->blit_read_fbo) {
        glDeleteFramebuffers(1, &self->blit_read_fbo);
        self->blit_read_fbo = 0;
    }
    if (self->gl_vbo) {
        glDeleteBuffers(1, &self->gl_vbo);
        self->gl_vbo = 0;
    }
    if (self->gl_program) {
        glDeleteProgram(self->gl_program);
        self->gl_program = 0;
    }
}

/* ── EGL helpers ──────────────────────────────────────────────────────────── */

static gboolean setup_egl(FlutterSubsurfaceView *self, struct wl_display *display,
                           size_t width, size_t height) {
    self->egl_display = eglGetDisplay((EGLNativeDisplayType)display);
    if (self->egl_display == EGL_NO_DISPLAY) {
        g_warning("Failed to get EGL display");
        return FALSE;
    }
    if (!eglInitialize(self->egl_display, NULL, NULL)) {
        g_warning("Failed to initialize EGL");
        return FALSE;
    }

    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE,
    };
    EGLConfig config;
    EGLint    num_configs;
    if (!eglChooseConfig(self->egl_display, config_attribs, &config, 1,
                         &num_configs) || num_configs == 0) {
        g_warning("Failed to choose EGL config");
        return FALSE;
    }

    eglBindAPI(EGL_OPENGL_ES_API);

    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    self->egl_context = eglCreateContext(self->egl_display, config,
                                         EGL_NO_CONTEXT, context_attribs);
    if (self->egl_context == EGL_NO_CONTEXT) {
        g_warning("Failed to create EGL context");
        return FALSE;
    }

    /* Create a renderer context sharing objects with the main context, plus a
       1×1 pbuffer to keep it current on the renderer thread. */
    self->renderer_egl_context = eglCreateContext(self->egl_display, config,
                                                   self->egl_context, context_attribs);
    if (self->renderer_egl_context == EGL_NO_CONTEXT) {
        g_warning("Failed to create renderer EGL context");
        return FALSE;
    }
    static const EGLint pbuffer_attribs[] = {
        EGL_WIDTH,  1,
        EGL_HEIGHT, 1,
        EGL_NONE,
    };
    self->renderer_egl_surface = eglCreatePbufferSurface(self->egl_display, config,
                                                          pbuffer_attribs);
    if (self->renderer_egl_surface == EGL_NO_SURFACE) {
        g_warning("Failed to create renderer EGL pbuffer surface");
        eglDestroyContext(self->egl_display, self->renderer_egl_context);
        self->renderer_egl_context = EGL_NO_CONTEXT;
        return FALSE;
    }

    self->egl_window = wl_egl_window_create(self->surface,
                                             width * self->scale,
                                             height * self->scale);
    if (!self->egl_window) {
        g_warning("Failed to create wl_egl_window");
        return FALSE;
    }

    self->egl_surface = eglCreateWindowSurface(
        self->egl_display, config,
        (EGLNativeWindowType)self->egl_window, NULL);
    if (self->egl_surface == EGL_NO_SURFACE) {
        g_warning("Failed to create EGL window surface");
        return FALSE;
    }

    wl_surface_set_buffer_scale(self->surface, self->scale);

    eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                   self->egl_context);
    /* Disable frame-callback throttling so eglSwapBuffers never blocks
       waiting for the compositor to ack the previous frame. */
    eglSwapInterval(self->egl_display, 0);
    return setup_gl(self);
}

/* Render a solid clear — used as the initial / resize frame. */
static void render_clear(FlutterSubsurfaceView *self, size_t width, size_t height) {
    eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                   self->egl_context);
    glViewport(0, 0, width, height);
    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(self->egl_display, self->egl_surface);
}

/* Blit a caller-supplied texture to the full EGL surface.
   Uses glBlitFramebuffer when available, otherwise falls back to a shader. */
static void render_texture(FlutterSubsurfaceView *self,
                            GLuint texture_id,
                            GLenum texture_format G_GNUC_UNUSED,
                            size_t width, size_t height) {
    eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                   self->egl_context);

    if (self->p_glBlitFramebuffer) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, self->blit_read_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, texture_id, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        self->p_glBlitFramebuffer(0, 0, (GLint)width, (GLint)height,
                                  0, 0, (GLint)width, (GLint)height,
                                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    } else {
        glViewport(0, 0, width, height);

        glUseProgram(self->gl_program);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_id);
        glUniform1i(self->gl_texture_loc, 0);

        glBindBuffer(GL_ARRAY_BUFFER, self->gl_vbo);
        glEnableVertexAttribArray(self->gl_position_loc);
        glVertexAttribPointer(self->gl_position_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glDisableVertexAttribArray(self->gl_position_loc);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    }

    eglSwapBuffers(self->egl_display, self->egl_surface);
}

/* ── GtkWidget vfuncs ─────────────────────────────────────────────────────── */

static void flutter_subsurface_view_realize(GtkWidget *widget) {
    FlutterSubsurfaceView *self = FLUTTER_SUBSURFACE_VIEW(widget);

    GTK_WIDGET_CLASS(flutter_subsurface_view_parent_class)->realize(widget);

    GdkDisplay *gdk_display = gtk_widget_get_display(widget);
    if (!GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        g_warning("FlutterSubsurfaceView requires a Wayland display");
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

    GtkWidget *toplevel   = gtk_widget_get_toplevel(widget);
    GdkWindow *gdk_window = gtk_widget_get_window(toplevel);
    struct wl_surface *parent_surface =
        gdk_wayland_window_get_wl_surface(gdk_window);

    self->surface   = wl_compositor_create_surface(self->compositor);
    self->subsurface = wl_subcompositor_get_subsurface(
        self->subcompositor, self->surface, parent_surface);

    /* Sync mode: subsurface commits are cached and applied atomically when
       the parent GTK window surface commits.  After each eglSwapBuffers we
       call gtk_widget_queue_draw() to drive a GTK frame cycle, which causes
       GDK to commit the parent surface and flush our pending subsurface
       commit to the compositor. */
    wl_subsurface_set_sync(self->subsurface);

    gint x, y;
    gtk_widget_translate_coordinates(widget, toplevel, 0, 0, &x, &y);
    wl_subsurface_set_position(self->subsurface, x, y);

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    self->scale = gtk_widget_get_scale_factor(widget);
    if (!setup_egl(self, display, alloc.width, alloc.height))
        return;

    render_clear(self, (size_t)alloc.width * self->scale,
                       (size_t)alloc.height * self->scale);
}

static void flutter_subsurface_view_unrealize(GtkWidget *widget) {
    FlutterSubsurfaceView *self = FLUTTER_SUBSURFACE_VIEW(widget);

    if (self->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                       self->egl_context);
        teardown_gl(self);
        eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (self->renderer_egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(self->egl_display, self->renderer_egl_surface);
        if (self->renderer_egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(self->egl_display, self->renderer_egl_context);
        if (self->egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(self->egl_display, self->egl_surface);
        if (self->egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(self->egl_display, self->egl_context);
        eglTerminate(self->egl_display);
        self->renderer_egl_surface = EGL_NO_SURFACE;
        self->renderer_egl_context = EGL_NO_CONTEXT;
        self->egl_surface = EGL_NO_SURFACE;
        self->egl_context = EGL_NO_CONTEXT;
        self->egl_display = EGL_NO_DISPLAY;
    }

    if (self->egl_window) {
        wl_egl_window_destroy(self->egl_window);
        self->egl_window = NULL;
    }

    if (self->subsurface) {
        wl_subsurface_destroy(self->subsurface);
        self->subsurface = NULL;
    }
    if (self->surface) {
        wl_surface_destroy(self->surface);
        self->surface = NULL;
    }

    GTK_WIDGET_CLASS(flutter_subsurface_view_parent_class)->unrealize(widget);
}

static void flutter_subsurface_view_size_allocate(GtkWidget     *widget,
                                             GtkAllocation *allocation) {
    GTK_WIDGET_CLASS(flutter_subsurface_view_parent_class)
        ->size_allocate(widget, allocation);

    FlutterSubsurfaceView *self = FLUTTER_SUBSURFACE_VIEW(widget);
    if (!self->subsurface)
        return;

    GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
    gint x, y;
    gtk_widget_translate_coordinates(widget, toplevel, 0, 0, &x, &y);
    wl_subsurface_set_position(self->subsurface, x, y);

    if (self->egl_window) {
        size_t pw = (size_t)allocation->width * self->scale;
        size_t ph = (size_t)allocation->height * self->scale;
        wl_egl_window_resize(self->egl_window, pw, ph, 0, 0);

        /* Freeze the toplevel window so GDK won't commit the parent
           surface until we have a subsurface frame at the new size.
           Skip this before the first frame (window hasn't shown yet). */
        if (self->has_presented && !self->awaiting_frame) {
            GdkWindow *gdk_window = gtk_widget_get_window(toplevel);
            if (gdk_window)
                gdk_window_freeze_updates(gdk_window);
            self->awaiting_frame = TRUE;
        }
        self->expected_width  = pw;
        self->expected_height = ph;
    }
}

static void flutter_subsurface_view_get_preferred_width(GtkWidget *widget G_GNUC_UNUSED,
                                                   gint *minimum, gint *natural) {
    *minimum = 1;
    *natural = 400;
}

static void flutter_subsurface_view_get_preferred_height(GtkWidget *widget G_GNUC_UNUSED,
                                                    gint *minimum, gint *natural) {
    *minimum = 1;
    *natural = 300;
}

static void flutter_subsurface_view_finalize(GObject *object) {
    FlutterSubsurfaceView *self = FLUTTER_SUBSURFACE_VIEW(object);

    g_mutex_clear(&self->present_mutex);

    if (self->subcompositor) {
        wl_subcompositor_destroy(self->subcompositor);
        self->subcompositor = NULL;
    }
    if (self->compositor) {
        wl_compositor_destroy(self->compositor);
        self->compositor = NULL;
    }

    G_OBJECT_CLASS(flutter_subsurface_view_parent_class)->finalize(object);
}

static void flutter_subsurface_view_class_init(FlutterSubsurfaceViewClass *klass) {
    GObjectClass    *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass  *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->finalize = flutter_subsurface_view_finalize;

    widget_class->realize              = flutter_subsurface_view_realize;
    widget_class->unrealize            = flutter_subsurface_view_unrealize;
    widget_class->size_allocate        = flutter_subsurface_view_size_allocate;
    widget_class->get_preferred_width  = flutter_subsurface_view_get_preferred_width;
    widget_class->get_preferred_height = flutter_subsurface_view_get_preferred_height;
}

static void flutter_subsurface_view_init(FlutterSubsurfaceView *self) {
    gtk_widget_set_has_window(GTK_WIDGET(self), FALSE);
    self->egl_display          = EGL_NO_DISPLAY;
    self->egl_context          = EGL_NO_CONTEXT;
    self->egl_surface          = EGL_NO_SURFACE;
    self->renderer_egl_context = EGL_NO_CONTEXT;
    self->renderer_egl_surface = EGL_NO_SURFACE;
    g_mutex_init(&self->present_mutex);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

GtkWidget *flutter_subsurface_view_new(void) {
    return g_object_new(FLUTTER_SUBSURFACE_VIEW_TYPE, NULL);
}

EGLDisplay flutter_subsurface_view_get_egl_display(FlutterSubsurfaceView *self) {
    return self->egl_display;
}

EGLContext flutter_subsurface_view_get_egl_context(FlutterSubsurfaceView *self) {
    return self->egl_context;
}

/* Main-thread callback that performs the actual render.
   Holds a strong reference to the widget (taken in flutter_subsurface_view_present)
   so it is safe even if the widget is destroyed before the idle runs. */
static gboolean do_present(gpointer data) {
    FlutterSubsurfaceView *self = FLUTTER_SUBSURFACE_VIEW(data);

    /* Snapshot the latest frame under the lock, then release before rendering
       so callers on other threads are never blocked by GPU work. */
    g_mutex_lock(&self->present_mutex);
    GLuint texture_id     = self->present_texture_id;
    GLenum texture_format = self->present_texture_format;
    size_t width          = self->present_width;
    size_t height         = self->present_height;
    self->present_scheduled = FALSE;
    g_mutex_unlock(&self->present_mutex);

    if (self->egl_display != EGL_NO_DISPLAY &&
        self->egl_surface != EGL_NO_SURFACE) {
        /* Resize the EGL surface if the texture dimensions changed. */
        EGLint cur_w, cur_h;
        eglQuerySurface(self->egl_display, self->egl_surface, EGL_WIDTH,  &cur_w);
        eglQuerySurface(self->egl_display, self->egl_surface, EGL_HEIGHT, &cur_h);
        if ((size_t)cur_w != width || (size_t)cur_h != height)
            wl_egl_window_resize(self->egl_window, width, height, 0, 0);

        render_texture(self, texture_id, texture_format, width, height);

        self->has_presented = TRUE;

        /* If we were waiting for a correctly-sized frame after a resize,
           thaw the toplevel window so GTK can commit the parent surface
           atomically with our new subsurface content. */
        if (self->awaiting_frame &&
            width == self->expected_width && height == self->expected_height) {
            self->awaiting_frame = FALSE;
            GtkWidget *toplevel = gtk_widget_get_toplevel(GTK_WIDGET(self));
            GdkWindow *gdk_window = gtk_widget_get_window(toplevel);
            if (gdk_window)
                gdk_window_thaw_updates(gdk_window);
        }

        /* Drive a parent-surface commit so the compositor applies our
           cached subsurface commit atomically (sync mode). */
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }

    g_object_unref(self);
    return G_SOURCE_REMOVE;
}

static void flutter_subsurface_view_present(FlutterSubsurfaceView *self,
                               GLuint            texture_id,
                               GLenum            texture_format,
                               size_t            width,
                               size_t            height) {
    g_mutex_lock(&self->present_mutex);

    self->present_texture_id     = texture_id;
    self->present_texture_format = texture_format;
    self->present_width          = width;
    self->present_height         = height;

    /* Only schedule one dispatch at a time; later calls before it fires
       just update the stored frame so the freshest data is always used. */
    gboolean was_scheduled = self->present_scheduled;
    if (!was_scheduled) {
        self->present_scheduled = TRUE;
        g_object_ref(self);     /* balanced by g_object_unref in do_present */
    }

    g_mutex_unlock(&self->present_mutex);

    if (!was_scheduled)
        g_main_context_invoke(NULL, do_present, self);
}

static FlutterBackingStore *
flutter_subsurface_view_create_backing_store(FlutterSubsurfaceView *self G_GNUC_UNUSED,
                                             size_t                 width,
                                             size_t                 height) {
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

static void flutter_subsurface_view_collect_backing_store(FlutterSubsurfaceView *self G_GNUC_UNUSED,
                                                   FlutterBackingStore   *backing_store) {
    if (!backing_store)
        return;
    glDeleteTextures(1, &backing_store->texture);
    g_free(backing_store);
}

/* ── FlutterView interface ────────────────────────────────────────────────── */

static FlutterBackingStore *
subsurface_view_iface_create_backing_store(FlutterView *view, size_t width, size_t height) {
    return flutter_subsurface_view_create_backing_store(
        FLUTTER_SUBSURFACE_VIEW(view), width, height);
}

static void
subsurface_view_iface_collect_backing_store(FlutterView         *view,
                                            FlutterBackingStore *backing_store) {
    flutter_subsurface_view_collect_backing_store(
        FLUTTER_SUBSURFACE_VIEW(view), backing_store);
}

static void
subsurface_view_iface_present(FlutterView *view, GLuint texture_id,
                               GLenum texture_format, size_t width, size_t height) {
    flutter_subsurface_view_present(FLUTTER_SUBSURFACE_VIEW(view),
                                    texture_id, texture_format, width, height);
}

static gboolean subsurface_view_iface_make_current(FlutterView *view) {
    FlutterSubsurfaceView *self = FLUTTER_SUBSURFACE_VIEW(view);
    return eglMakeCurrent(self->egl_display, self->renderer_egl_surface,
                          self->renderer_egl_surface,
                          self->renderer_egl_context) == EGL_TRUE;
}

static void subsurface_view_iface_clear_current(FlutterView *view) {
    FlutterSubsurfaceView *self = FLUTTER_SUBSURFACE_VIEW(view);
    eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
}

static void flutter_subsurface_view_iface_init(FlutterViewInterface *iface) {
    iface->create_backing_store  = subsurface_view_iface_create_backing_store;
    iface->collect_backing_store = subsurface_view_iface_collect_backing_store;
    iface->present               = subsurface_view_iface_present;
    iface->make_current          = subsurface_view_iface_make_current;
    iface->clear_current         = subsurface_view_iface_clear_current;
}
