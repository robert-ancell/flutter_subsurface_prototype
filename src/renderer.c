#include "renderer.h"

#include <GLES2/gl2.h>
#include <gtk/gtk.h>
#include <math.h>

struct _Renderer {
    FlutterView *widget;

    size_t width;
    size_t height;

    /* GL resources — valid only on the render thread after setup_gl() */
    GLuint               fbo;
    FlutterBackingStore *backing_store;
    GLuint                  program;
    GLuint vbo;
    GLint  position_loc;
    GLint  color_loc;
    GLint  angle_loc;

    /* Thread control */
    GThread  *thread;
    GMutex    mutex;
    GCond     cond;
    gboolean  running;

    /* Pending resize, protected by mutex */
    gboolean resize_pending;
    size_t   pending_width;
    size_t   pending_height;
};

/* ── GL helpers ───────────────────────────────────────────────────────────── */

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);

    GLint compile_status;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &compile_status);
    if (compile_status == GL_FALSE) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        g_warning("Renderer shader compile error: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* (Re)create the FBO and backing store at r->width × r->height.
   Any existing FBO and backing store are released first. */
static gboolean create_fbo(Renderer *r) {
    if (r->fbo) { glDeleteFramebuffers(1, &r->fbo); r->fbo = 0; }
    flutter_view_collect_backing_store(FLUTTER_VIEW(r->widget), r->backing_store);

    r->backing_store = flutter_view_create_backing_store(
        FLUTTER_VIEW(r->widget), r->width, r->height);
    if (!r->backing_store)
        return FALSE;

    glGenFramebuffers(1, &r->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, r->backing_store->texture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        g_warning("Renderer FBO incomplete (status 0x%x)", (unsigned)status);
        return FALSE;
    }
    return TRUE;
}

static gboolean setup_gl(Renderer *r) {
    /* Vertex shader: rotate the triangle by u_angle radians around the
       origin, then pass interpolated per-vertex colour to the fragment
       shader. */
    static const char *vert_src =
        "attribute vec2 a_position;\n"
        "attribute vec3 a_color;\n"
        "uniform float u_angle;\n"
        "varying vec3 v_color;\n"
        "void main() {\n"
        "    float c = cos(u_angle);\n"
        "    float s = sin(u_angle);\n"
        "    vec2 p = vec2(a_position.x * c - a_position.y * s,\n"
        "                  a_position.x * s + a_position.y * c);\n"
        "    gl_Position = vec4(p, 0.0, 1.0);\n"
        "    v_color = a_color;\n"
        "}\n";

    static const char *frag_src =
        "precision mediump float;\n"
        "varying vec3 v_color;\n"
        "void main() {\n"
        "    gl_FragColor = vec4(v_color, 1.0);\n"
        "}\n";

    GLuint vert = compile_shader(GL_VERTEX_SHADER,   vert_src);
    if (!vert) return FALSE;
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!frag) { glDeleteShader(vert); return FALSE; }

    r->program = glCreateProgram();
    glAttachShader(r->program, vert);
    glAttachShader(r->program, frag);
    glLinkProgram(r->program);
    glDeleteShader(vert);
    glDeleteShader(frag);

    GLint ok;
    glGetProgramiv(r->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(r->program, sizeof(log), NULL, log);
        g_warning("Renderer shader link error: %s", log);
        glDeleteProgram(r->program);
        r->program = 0;
        return FALSE;
    }

    r->position_loc = glGetAttribLocation (r->program, "a_position");
    r->color_loc    = glGetAttribLocation (r->program, "a_color");
    r->angle_loc    = glGetUniformLocation(r->program, "u_angle");

    /* Interleaved: position (x, y) + colour (r, g, b) — one vertex per row */
    static const GLfloat vertices[] = {
         0.0f,  0.6f,   1.0f, 0.2f, 0.2f,   /* top          – red   */
        -0.6f, -0.4f,   0.2f, 1.0f, 0.2f,   /* bottom-left  – green */
         0.6f, -0.4f,   0.2f, 0.2f, 1.0f,   /* bottom-right – blue  */
    };
    glGenBuffers(1, &r->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return create_fbo(r);
}

static void teardown_gl(Renderer *r) {
    if (r->fbo) { glDeleteFramebuffers(1, &r->fbo); r->fbo = 0; }
    flutter_view_collect_backing_store(FLUTTER_VIEW(r->widget), r->backing_store);
    r->backing_store = NULL;
    if (r->vbo)     { glDeleteBuffers(1, &r->vbo);    r->vbo     = 0; }
    if (r->program) { glDeleteProgram(r->program);    r->program = 0; }
}

static void render_frame(Renderer *r, float angle) {
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glViewport(0, 0, (GLsizei)r->width, (GLsizei)r->height);

    glClearColor(0.08f, 0.08f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(r->program);
    glUniform1f(r->angle_loc, angle);

    const GLsizei stride = 5 * (GLsizei)sizeof(GLfloat);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glEnableVertexAttribArray(r->position_loc);
    glVertexAttribPointer(r->position_loc, 2, GL_FLOAT, GL_FALSE,
                          stride, (void *)0);
    glEnableVertexAttribArray(r->color_loc);
    glVertexAttribPointer(r->color_loc, 3, GL_FLOAT, GL_FALSE,
                          stride, (void *)(2 * sizeof(GLfloat)));

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glDisableVertexAttribArray(r->position_loc);
    glDisableVertexAttribArray(r->color_loc);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Ensure all writes are visible to the widget's context before present. */
    glFinish();
}

/* ── Render thread ────────────────────────────────────────────────────────── */

static gpointer renderer_thread_func(gpointer data) {
    Renderer *r = data;

    if (!flutter_view_make_current(r->widget)) {
        g_warning("Renderer: flutter_view_make_current failed, thread exiting");
        return NULL;
    }

    if (!setup_gl(r)) {
        g_warning("Renderer: GL setup failed, thread exiting");
        return NULL;
    }

    gint64 start = g_get_monotonic_time();

    for (;;) {
        /* Sleep up to ~16 ms (60 fps); wake early on resize or stop. */
        gint64 deadline = g_get_monotonic_time() + 16 * G_TIME_SPAN_MILLISECOND;

        g_mutex_lock(&r->mutex);
        g_cond_wait_until(&r->cond, &r->mutex, deadline);
        gboolean running        = r->running;
        gboolean resize_pending = r->resize_pending;
        size_t   new_width      = r->pending_width;
        size_t   new_height     = r->pending_height;
        if (resize_pending)
            r->resize_pending = FALSE;
        g_mutex_unlock(&r->mutex);

        if (!running)
            break;

        if (resize_pending && new_width > 0 && new_height > 0 &&
            (new_width != r->width || new_height != r->height)) {
            r->width  = new_width;
            r->height = new_height;
            create_fbo(r);
        }

        float elapsed = (float)(g_get_monotonic_time() - start) / (float)G_USEC_PER_SEC;
        float angle   = elapsed * ((float)G_PI * 2.0f / 4.0f); /* one rotation per 4 s */

        render_frame(r, angle);
        flutter_view_present(FLUTTER_VIEW(r->widget),
                             r->backing_store->texture, GL_RGBA,
                             r->backing_store->width,
                             r->backing_store->height);
    }

    teardown_gl(r);
    flutter_view_clear_current(r->widget);
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

Renderer *renderer_new(FlutterView *widget) {
    GtkAllocation alloc;
    gtk_widget_get_allocation(GTK_WIDGET(widget), &alloc);

    gint scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));

    Renderer *r = g_new0(Renderer, 1);
    r->widget  = widget;
    r->width   = (size_t)alloc.width * (size_t)scale;
    r->height  = (size_t)alloc.height * (size_t)scale;
    r->running = TRUE;
    g_mutex_init(&r->mutex);
    g_cond_init(&r->cond);

    r->thread = g_thread_new("renderer", renderer_thread_func, r);

    return r;
}

void renderer_free(Renderer *r) {
    if (!r)
        return;

    /* Signal the thread to stop and wait for it to finish. */
    g_mutex_lock(&r->mutex);
    r->running = FALSE;
    g_cond_signal(&r->cond);
    g_mutex_unlock(&r->mutex);

    g_thread_join(r->thread);

    g_mutex_clear(&r->mutex);
    g_cond_clear(&r->cond);
    g_free(r);
}

void renderer_resize(Renderer *r, size_t width, size_t height, gint scale) {
    g_mutex_lock(&r->mutex);
    r->pending_width  = width * (size_t)scale;
    r->pending_height = height * (size_t)scale;
    r->resize_pending = TRUE;
    g_cond_signal(&r->cond);
    g_mutex_unlock(&r->mutex);
}
