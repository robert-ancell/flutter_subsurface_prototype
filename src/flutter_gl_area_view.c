#include "flutter_gl_area_view.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

struct _FlutterGLAreaView {
    GtkGLArea parent_instance;

    EGLDisplay egl_display;
    EGLContext egl_context;

    /* GL resources for the texture blit */
    GLuint gl_program;
    GLuint gl_vbo;
    GLint  gl_position_loc;
    GLint  gl_texture_loc;

    /* Thread-safe pending-present state */
    GMutex   present_mutex;
    gboolean present_scheduled; /* a queue_render has been dispatched */
    gboolean has_frame;         /* render vfunc has something to blit */
    GLuint   present_texture_id;
    GLenum   present_texture_format;
    size_t   present_width;
    size_t   present_height;
};

static void flutter_gl_area_view_iface_init(FlutterViewInterface *iface);

G_DEFINE_TYPE_WITH_CODE(FlutterGLAreaView, flutter_gl_area_view, GTK_TYPE_GL_AREA,
    G_IMPLEMENT_INTERFACE(FLUTTER_TYPE_VIEW, flutter_gl_area_view_iface_init))

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

static gboolean setup_gl(FlutterGLAreaView *self) {
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

static void teardown_gl(FlutterGLAreaView *self) {
    if (self->gl_vbo) {
        glDeleteBuffers(1, &self->gl_vbo);
        self->gl_vbo = 0;
    }
    if (self->gl_program) {
        glDeleteProgram(self->gl_program);
        self->gl_program = 0;
    }
}

/* Blit texture_id to the currently-bound framebuffer.
   Called from within the render vfunc — no eglMakeCurrent or
   eglSwapBuffers; GtkGLArea manages both. */
static void blit_texture(FlutterGLAreaView *self,
                          GLuint texture_id,
                          GLenum texture_format G_GNUC_UNUSED) {
    GtkAllocation alloc;
    gtk_widget_get_allocation(GTK_WIDGET(self), &alloc);
    glViewport(0, 0, alloc.width, alloc.height);

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

/* ── GtkGLArea vfuncs ─────────────────────────────────────────────────────── */

static void flutter_gl_area_view_realize(GtkWidget *widget) {
    GTK_WIDGET_CLASS(flutter_gl_area_view_parent_class)->realize(widget);

    FlutterGLAreaView *self = FLUTTER_GL_AREA_VIEW(widget);
    gtk_gl_area_make_current(GTK_GL_AREA(widget));

    if (gtk_gl_area_get_error(GTK_GL_AREA(widget)) != NULL) {
        g_warning("FlutterGLAreaView: GL context creation failed");
        return;
    }

    /* Capture the EGL display and context created by GDK so the renderer
       can create a sharing EGL context on its own thread. */
    self->egl_display = eglGetCurrentDisplay();
    self->egl_context = eglGetCurrentContext();

    setup_gl(self);
}

static void flutter_gl_area_view_unrealize(GtkWidget *widget) {
    FlutterGLAreaView *self = FLUTTER_GL_AREA_VIEW(widget);

    gtk_gl_area_make_current(GTK_GL_AREA(widget));
    if (gtk_gl_area_get_error(GTK_GL_AREA(widget)) == NULL)
        teardown_gl(self);

    self->egl_display = EGL_NO_DISPLAY;
    self->egl_context = EGL_NO_CONTEXT;

    GTK_WIDGET_CLASS(flutter_gl_area_view_parent_class)->unrealize(widget);
}

/* render vfunc — called by GtkGLArea with the correct FBO bound and the
   GL context already current.  Blits the latest pending frame if one is
   available, otherwise clears to opaque black. */
static gboolean flutter_gl_area_view_render(GtkGLArea    *area,
                                             GdkGLContext *context G_GNUC_UNUSED) {
    FlutterGLAreaView *self = FLUTTER_GL_AREA_VIEW(area);

    g_mutex_lock(&self->present_mutex);
    if (!self->has_frame) {
        g_mutex_unlock(&self->present_mutex);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return TRUE;
    }

    GLuint texture_id     = self->present_texture_id;
    GLenum texture_format = self->present_texture_format;
    self->has_frame = FALSE;
    g_mutex_unlock(&self->present_mutex);

    blit_texture(self, texture_id, texture_format);
    return TRUE;
}

static void flutter_gl_area_view_finalize(GObject *object) {
    FlutterGLAreaView *self = FLUTTER_GL_AREA_VIEW(object);
    g_mutex_clear(&self->present_mutex);
    G_OBJECT_CLASS(flutter_gl_area_view_parent_class)->finalize(object);
}

static void flutter_gl_area_view_class_init(FlutterGLAreaViewClass *klass) {
    GObjectClass   *object_class   = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class   = GTK_WIDGET_CLASS(klass);
    GtkGLAreaClass *gl_area_class  = GTK_GL_AREA_CLASS(klass);

    object_class->finalize   = flutter_gl_area_view_finalize;
    widget_class->realize    = flutter_gl_area_view_realize;
    widget_class->unrealize  = flutter_gl_area_view_unrealize;
    gl_area_class->render    = flutter_gl_area_view_render;
}

static void flutter_gl_area_view_init(FlutterGLAreaView *self) {
    self->egl_display = EGL_NO_DISPLAY;
    self->egl_context = EGL_NO_CONTEXT;
    g_mutex_init(&self->present_mutex);

    /* Request an OpenGL ES 2 context so the shaders and the renderer's
       sharing context use the same API. */
    gtk_gl_area_set_use_es(GTK_GL_AREA(self), TRUE);
    gtk_gl_area_set_required_version(GTK_GL_AREA(self), 2, 0);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

GtkWidget *flutter_gl_area_view_new(void) {
    return g_object_new(FLUTTER_GL_AREA_VIEW_TYPE, NULL);
}

EGLDisplay flutter_gl_area_view_get_egl_display(FlutterGLAreaView *self) {
    return self->egl_display;
}

EGLContext flutter_gl_area_view_get_egl_context(FlutterGLAreaView *self) {
    return self->egl_context;
}

/* Main-thread callback: schedule a GtkGLArea render pass.
   Holds a strong reference to the widget taken in flutter_gl_area_view_present(). */
static gboolean do_queue_render(gpointer data) {
    FlutterGLAreaView *self = FLUTTER_GL_AREA_VIEW(data);

    g_mutex_lock(&self->present_mutex);
    self->present_scheduled = FALSE;
    g_mutex_unlock(&self->present_mutex);

    gtk_gl_area_queue_render(GTK_GL_AREA(self));

    g_object_unref(self);
    return G_SOURCE_REMOVE;
}

static void flutter_gl_area_view_present(FlutterGLAreaView *self,
                                   GLuint             texture_id,
                                   GLenum             texture_format,
                                   size_t             width,
                                   size_t             height) {
    g_mutex_lock(&self->present_mutex);

    self->present_texture_id     = texture_id;
    self->present_texture_format = texture_format;
    self->present_width          = width;
    self->present_height         = height;
    self->has_frame              = TRUE;

    gboolean was_scheduled = self->present_scheduled;
    if (!was_scheduled) {
        self->present_scheduled = TRUE;
        g_object_ref(self);
    }

    g_mutex_unlock(&self->present_mutex);

    if (!was_scheduled)
        g_main_context_invoke(NULL, do_queue_render, self);
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

static void flutter_gl_area_view_iface_init(FlutterViewInterface *iface) {
    iface->create_backing_store  = gl_area_view_iface_create_backing_store;
    iface->collect_backing_store = gl_area_view_iface_collect_backing_store;
    iface->present               = gl_area_view_iface_present;
}
