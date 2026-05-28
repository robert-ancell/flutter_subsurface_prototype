#include "renderer.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <gtk/gtk.h>
#include <math.h>

struct _Renderer {
    SubsurfaceWidget *widget;

    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface; /* 1×1 pbuffer — keeps the context current */

    size_t width;
    size_t height;

    /* GL resources — valid only on the render thread after setup_gl() */
    GLuint fbo;
    GLuint texture;
    GLuint program;
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

    GLint ok;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        g_warning("Renderer shader compile error: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* (Re)create the FBO and backing texture at r->width × r->height.
   Any existing texture and FBO are deleted first. */
static gboolean create_fbo(Renderer *r) {
    if (r->fbo)     { glDeleteFramebuffers(1, &r->fbo);  r->fbo     = 0; }
    if (r->texture) { glDeleteTextures    (1, &r->texture); r->texture = 0; }

    glGenTextures(1, &r->texture);
    glBindTexture(GL_TEXTURE_2D, r->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 (GLsizei)r->width, (GLsizei)r->height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &r->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, r->texture, 0);
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
    if (r->fbo)     { glDeleteFramebuffers(1, &r->fbo);  r->fbo     = 0; }
    if (r->texture) { glDeleteTextures    (1, &r->texture); r->texture = 0; }
    if (r->vbo)     { glDeleteBuffers     (1, &r->vbo);  r->vbo     = 0; }
    if (r->program) { glDeleteProgram(r->program);        r->program = 0; }
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

    eglMakeCurrent(r->egl_display, r->egl_surface, r->egl_surface,
                   r->egl_context);

    if (!setup_gl(r)) {
        g_warning("Renderer: GL setup failed, thread exiting");
        return NULL;
    }

    gint64 start = g_get_monotonic_time();

    for (;;) {
        /* Sleep up to 10 ms; wake early if renderer_free() signals the cond. */
        gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_MILLISECOND;

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
        subsurface_widget_present(r->widget, r->texture, GL_RGBA,
                                  r->width, r->height);
    }

    teardown_gl(r);
    eglMakeCurrent(r->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

Renderer *renderer_new(SubsurfaceWidget *widget) {
    EGLDisplay egl_display   = subsurface_widget_get_egl_display(widget);
    EGLContext share_context = subsurface_widget_get_egl_context(widget);

    if (egl_display == EGL_NO_DISPLAY || share_context == EGL_NO_CONTEXT) {
        g_warning("Renderer: widget has no EGL context (not on Wayland?)");
        return NULL;
    }

    /* Choose a config that supports pbuffer surfaces for the offscreen
       render surface used to keep the context current on this thread. */
    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE,
    };
    EGLConfig config;
    EGLint    num_configs;
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(egl_display, config_attribs, &config, 1,
                         &num_configs) || num_configs == 0) {
        g_warning("Renderer: failed to choose EGL config");
        return NULL;
    }

    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    EGLContext egl_context = eglCreateContext(egl_display, config,
                                              share_context, context_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        g_warning("Renderer: failed to create EGL context");
        return NULL;
    }

    static const EGLint pbuffer_attribs[] = {
        EGL_WIDTH,  1,
        EGL_HEIGHT, 1,
        EGL_NONE,
    };
    EGLSurface egl_surface = eglCreatePbufferSurface(egl_display, config,
                                                     pbuffer_attribs);
    if (egl_surface == EGL_NO_SURFACE) {
        g_warning("Renderer: failed to create EGL pbuffer surface");
        eglDestroyContext(egl_display, egl_context);
        return NULL;
    }

    GtkAllocation alloc;
    gtk_widget_get_allocation(GTK_WIDGET(widget), &alloc);

    Renderer *r = g_new0(Renderer, 1);
    r->widget      = widget;
    r->egl_display = egl_display;
    r->egl_context = egl_context;
    r->egl_surface = egl_surface;
    r->width       = (size_t)alloc.width;
    r->height      = (size_t)alloc.height;
    r->running     = TRUE;
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

    /* The thread has already done eglMakeCurrent(NO_CONTEXT), so it is safe
       to destroy the EGL objects from this thread. */
    if (r->egl_surface != EGL_NO_SURFACE)
        eglDestroySurface(r->egl_display, r->egl_surface);
    if (r->egl_context != EGL_NO_CONTEXT)
        eglDestroyContext(r->egl_display, r->egl_context);

    g_mutex_clear(&r->mutex);
    g_cond_clear(&r->cond);
    g_free(r);
}

void renderer_resize(Renderer *r, size_t width, size_t height) {
    g_mutex_lock(&r->mutex);
    r->pending_width  = width;
    r->pending_height = height;
    r->resize_pending = TRUE;
    g_mutex_unlock(&r->mutex);
}
