#include "subsurface_widget.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gdk/gdkwayland.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>

struct _SubsurfaceWidget {
    GtkWidget parent_instance;

    struct wl_compositor    *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_surface       *surface;
    struct wl_subsurface    *subsurface;

    struct wl_egl_window *egl_window;
    EGLDisplay            egl_display;
    EGLContext            egl_context;
    EGLSurface            egl_surface;

    /* GL resources for the texture blit */
    GLuint gl_program;
    GLuint gl_vbo;
    GLint  gl_position_loc;
    GLint  gl_texture_loc;

    /* Thread-safe pending-present state */
    GMutex   present_mutex;
    gboolean present_scheduled;
    GLuint   present_texture_id;
    GLenum   present_texture_format;
    size_t   present_width;
    size_t   present_height;
};

G_DEFINE_TYPE(SubsurfaceWidget, subsurface_widget, GTK_TYPE_WIDGET)

/* ── Wayland registry ─────────────────────────────────────────────────────── */

static void registry_global(void *data, struct wl_registry *registry,
                             uint32_t name, const char *interface,
                             uint32_t version) {
    SubsurfaceWidget *self = data;
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

static gboolean setup_gl(SubsurfaceWidget *self) {
    /* Vertex shader: maps clip-space position to texture coordinates.
       (0,0) in texture space = bottom-left, matching OpenGL convention. */
    static const char *vert_src =
        "attribute vec2 a_position;\n"
        "varying vec2 v_texcoord;\n"
        "void main() {\n"
        "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
        "    v_texcoord  = a_position * 0.5 + vec2(0.5);\n"
        "}\n";

    static const char *frag_src =
        "precision mediump float;\n"
        "uniform sampler2D u_texture;\n"
        "varying vec2 v_texcoord;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(u_texture, v_texcoord);\n"
        "}\n";

    GLuint vert = compile_shader(GL_VERTEX_SHADER,   vert_src);
    if (!vert) return FALSE;
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!frag) { glDeleteShader(vert); return FALSE; }

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

static void teardown_gl(SubsurfaceWidget *self) {
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

static gboolean setup_egl(SubsurfaceWidget *self, struct wl_display *display,
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
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
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

    self->egl_window = wl_egl_window_create(self->surface, width, height);
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

    eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                   self->egl_context);
    /* Disable frame-callback throttling so eglSwapBuffers never blocks
       waiting for the compositor to ack the previous frame. */
    eglSwapInterval(self->egl_display, 0);
    return setup_gl(self);
}

/* Render a solid clear — used as the initial / resize frame. */
static void render_clear(SubsurfaceWidget *self, size_t width, size_t height) {
    eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                   self->egl_context);
    glViewport(0, 0, width, height);
    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(self->egl_display, self->egl_surface);
}

/* Blit a caller-supplied texture to the full EGL surface.
   texture_format is stored for future use (e.g. YUV, external-OES). */
static void render_texture(SubsurfaceWidget *self,
                            GLuint texture_id,
                            GLenum texture_format G_GNUC_UNUSED,
                            size_t width, size_t height) {
    eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                   self->egl_context);
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

    eglSwapBuffers(self->egl_display, self->egl_surface);
}

/* ── GtkWidget vfuncs ─────────────────────────────────────────────────────── */

static void subsurface_widget_realize(GtkWidget *widget) {
    SubsurfaceWidget *self = SUBSURFACE_WIDGET(widget);

    GTK_WIDGET_CLASS(subsurface_widget_parent_class)->realize(widget);

    GdkDisplay *gdk_display = gtk_widget_get_display(widget);
    if (!GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        g_warning("SubsurfaceWidget requires a Wayland display");
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

    /* Desync so the subsurface commits independently without waiting for
       the parent GTK surface to commit. */
    wl_subsurface_set_desync(self->subsurface);

    gint x, y;
    gtk_widget_translate_coordinates(widget, toplevel, 0, 0, &x, &y);
    wl_subsurface_set_position(self->subsurface, x, y);

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    if (!setup_egl(self, display, alloc.width, alloc.height))
        return;

    render_clear(self, alloc.width, alloc.height);
}

static void subsurface_widget_unrealize(GtkWidget *widget) {
    SubsurfaceWidget *self = SUBSURFACE_WIDGET(widget);

    if (self->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                       self->egl_context);
        teardown_gl(self);
        eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (self->egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(self->egl_display, self->egl_surface);
        if (self->egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(self->egl_display, self->egl_context);
        eglTerminate(self->egl_display);
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

    GTK_WIDGET_CLASS(subsurface_widget_parent_class)->unrealize(widget);
}

static void subsurface_widget_size_allocate(GtkWidget     *widget,
                                             GtkAllocation *allocation) {
    GTK_WIDGET_CLASS(subsurface_widget_parent_class)
        ->size_allocate(widget, allocation);

    SubsurfaceWidget *self = SUBSURFACE_WIDGET(widget);
    if (!self->subsurface)
        return;

    GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
    gint x, y;
    gtk_widget_translate_coordinates(widget, toplevel, 0, 0, &x, &y);
    wl_subsurface_set_position(self->subsurface, x, y);

    if (self->egl_window) {
        wl_egl_window_resize(self->egl_window,
                             allocation->width, allocation->height, 0, 0);
        render_clear(self, allocation->width, allocation->height);
    }
}

static void subsurface_widget_get_preferred_width(GtkWidget *widget G_GNUC_UNUSED,
                                                   gint *minimum, gint *natural) {
    *minimum = 1;
    *natural = 400;
}

static void subsurface_widget_get_preferred_height(GtkWidget *widget G_GNUC_UNUSED,
                                                    gint *minimum, gint *natural) {
    *minimum = 1;
    *natural = 300;
}

static void subsurface_widget_finalize(GObject *object) {
    SubsurfaceWidget *self = SUBSURFACE_WIDGET(object);

    g_mutex_clear(&self->present_mutex);

    if (self->subcompositor) {
        wl_subcompositor_destroy(self->subcompositor);
        self->subcompositor = NULL;
    }
    if (self->compositor) {
        wl_compositor_destroy(self->compositor);
        self->compositor = NULL;
    }

    G_OBJECT_CLASS(subsurface_widget_parent_class)->finalize(object);
}

static void subsurface_widget_class_init(SubsurfaceWidgetClass *klass) {
    GObjectClass    *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass  *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->finalize = subsurface_widget_finalize;

    widget_class->realize              = subsurface_widget_realize;
    widget_class->unrealize            = subsurface_widget_unrealize;
    widget_class->size_allocate        = subsurface_widget_size_allocate;
    widget_class->get_preferred_width  = subsurface_widget_get_preferred_width;
    widget_class->get_preferred_height = subsurface_widget_get_preferred_height;
}

static void subsurface_widget_init(SubsurfaceWidget *self) {
    gtk_widget_set_has_window(GTK_WIDGET(self), FALSE);
    self->egl_display = EGL_NO_DISPLAY;
    self->egl_context = EGL_NO_CONTEXT;
    self->egl_surface = EGL_NO_SURFACE;
    g_mutex_init(&self->present_mutex);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

GtkWidget *subsurface_widget_new(void) {
    return g_object_new(SUBSURFACE_WIDGET_TYPE, NULL);
}

EGLDisplay subsurface_widget_get_egl_display(SubsurfaceWidget *self) {
    return self->egl_display;
}

EGLContext subsurface_widget_get_egl_context(SubsurfaceWidget *self) {
    return self->egl_context;
}

/* Main-thread callback that performs the actual render.
   Holds a strong reference to the widget (taken in subsurface_widget_present)
   so it is safe even if the widget is destroyed before the idle runs. */
static gboolean do_present(gpointer data) {
    SubsurfaceWidget *self = SUBSURFACE_WIDGET(data);

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
    }

    g_object_unref(self);
    return G_SOURCE_REMOVE;
}

void subsurface_widget_present(SubsurfaceWidget *self,
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

SubsurfaceBackingStore *
subsurface_widget_create_backing_store(SubsurfaceWidget *self G_GNUC_UNUSED,
                                       size_t            width,
                                       size_t            height) {
    SubsurfaceBackingStore *store = g_new0(SubsurfaceBackingStore, 1);
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

void subsurface_widget_collect_backing_store(SubsurfaceWidget       *self G_GNUC_UNUSED,
                                             SubsurfaceBackingStore *backing_store) {
    if (!backing_store)
        return;
    glDeleteTextures(1, &backing_store->texture);
    g_free(backing_store);
}
