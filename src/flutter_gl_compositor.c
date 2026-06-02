#include "flutter_gl_compositor.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
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

struct _FlutterGLCompositor {
    gboolean is_subsurface;

    EGLDisplay egl_display;
    EGLContext egl_context;   /* main compositing context (subsurface mode) */
    EGLSurface egl_surface;   /* window surface (subsurface mode) */

    struct wl_egl_window *egl_window;   /* subsurface mode */
    struct wl_surface    *wl_surface;   /* borrowed, subsurface mode */

    /* GL blit resources (subsurface mode) */
    GLuint gl_program;
    GLuint gl_vbo;
    GLint  gl_position_loc;
    GLint  gl_texture_loc;

    /* glBlitFramebuffer path (preferred when available) */
    PFNGLBLITFRAMEBUFFERPROC p_glBlitFramebuffer;
    GLuint                   blit_read_fbo;

    /* Renderer thread context (both modes) */
    EGLContext renderer_egl_context;
    EGLSurface renderer_egl_surface;
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

static gboolean setup_gl(FlutterGLCompositor *self) {
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

static void teardown_gl(FlutterGLCompositor *self) {
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

/* ── EGL setup (subsurface mode) ──────────────────────────────────────────── */

static gboolean setup_egl_subsurface(FlutterGLCompositor *self,
                                      struct wl_display *display,
                                      struct wl_surface *surface,
                                      size_t width, size_t height, gint scale) {
    self->wl_surface = surface;

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
                                                   self->egl_context,
                                                   context_attribs);
    if (self->renderer_egl_context == EGL_NO_CONTEXT) {
        g_warning("Failed to create renderer EGL context");
        return FALSE;
    }
    static const EGLint pbuffer_attribs[] = {
        EGL_WIDTH,  1,
        EGL_HEIGHT, 1,
        EGL_NONE,
    };
    self->renderer_egl_surface = eglCreatePbufferSurface(self->egl_display,
                                                          config,
                                                          pbuffer_attribs);
    if (self->renderer_egl_surface == EGL_NO_SURFACE) {
        g_warning("Failed to create renderer EGL pbuffer surface");
        eglDestroyContext(self->egl_display, self->renderer_egl_context);
        self->renderer_egl_context = EGL_NO_CONTEXT;
        return FALSE;
    }

    self->egl_window = wl_egl_window_create(surface,
                                             width * scale,
                                             height * scale);
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

    wl_surface_set_buffer_scale(surface, scale);

    eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                   self->egl_context);
    eglSwapInterval(self->egl_display, 0);

    gboolean ok = setup_gl(self);

    /* Release context so the renderer thread can use it later. */
    eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    return ok;
}

/* ── EGL setup (GDK mode) ─────────────────────────────────────────────────── */

static gboolean setup_egl_gdk(FlutterGLCompositor *self,
                               EGLDisplay egl_display,
                               EGLContext share_context) {
    self->egl_display = egl_display;

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
    if (!eglChooseConfig(self->egl_display, config_attribs,
                         &config, 1, &num_configs) || num_configs == 0) {
        g_warning("FlutterGLCompositor: failed to choose EGL config");
        return FALSE;
    }

    self->renderer_egl_context = eglCreateContext(
        self->egl_display, config, share_context, context_attribs);
    if (self->renderer_egl_context == EGL_NO_CONTEXT) {
        g_warning("FlutterGLCompositor: failed to create renderer EGL context");
        return FALSE;
    }

    self->renderer_egl_surface = eglCreatePbufferSurface(
        self->egl_display, config, pbuffer_attribs);
    if (self->renderer_egl_surface == EGL_NO_SURFACE) {
        g_warning("FlutterGLCompositor: failed to create renderer pbuffer");
        eglDestroyContext(self->egl_display, self->renderer_egl_context);
        self->renderer_egl_context = EGL_NO_CONTEXT;
        return FALSE;
    }

    return TRUE;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

FlutterGLCompositor *flutter_gl_compositor_new_subsurface(
    struct wl_display *wl_display,
    struct wl_surface *wl_surface,
    size_t width, size_t height, gint scale) {

    FlutterGLCompositor *self = g_new0(FlutterGLCompositor, 1);
    self->is_subsurface        = TRUE;
    self->egl_display          = EGL_NO_DISPLAY;
    self->egl_context          = EGL_NO_CONTEXT;
    self->egl_surface          = EGL_NO_SURFACE;
    self->renderer_egl_context = EGL_NO_CONTEXT;
    self->renderer_egl_surface = EGL_NO_SURFACE;

    if (!setup_egl_subsurface(self, wl_display, wl_surface,
                               width, height, scale)) {
        flutter_gl_compositor_free(self);
        return NULL;
    }
    return self;
}

FlutterGLCompositor *flutter_gl_compositor_new_gdk(EGLDisplay egl_display,
                                                    EGLContext share_context) {
    FlutterGLCompositor *self = g_new0(FlutterGLCompositor, 1);
    self->is_subsurface        = FALSE;
    self->egl_display          = EGL_NO_DISPLAY;
    self->egl_context          = EGL_NO_CONTEXT;
    self->egl_surface          = EGL_NO_SURFACE;
    self->renderer_egl_context = EGL_NO_CONTEXT;
    self->renderer_egl_surface = EGL_NO_SURFACE;

    if (!setup_egl_gdk(self, egl_display, share_context)) {
        flutter_gl_compositor_free(self);
        return NULL;
    }
    return self;
}

void flutter_gl_compositor_free(FlutterGLCompositor *self) {
    if (!self)
        return;

    if (self->is_subsurface && self->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                       self->egl_context);
        teardown_gl(self);
        eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
    }

    if (self->renderer_egl_surface != EGL_NO_SURFACE)
        eglDestroySurface(self->egl_display, self->renderer_egl_surface);
    if (self->renderer_egl_context != EGL_NO_CONTEXT)
        eglDestroyContext(self->egl_display, self->renderer_egl_context);

    if (self->is_subsurface) {
        if (self->egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(self->egl_display, self->egl_surface);
        if (self->egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(self->egl_display, self->egl_context);
        eglTerminate(self->egl_display);

        if (self->egl_window) {
            wl_egl_window_destroy(self->egl_window);
            self->egl_window = NULL;
        }
    }

    g_free(self);
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

void flutter_gl_compositor_present(FlutterGLCompositor *self,
                                    GLuint texture_id, GLenum texture_format,
                                    size_t width, size_t height) {
    if (!self->is_subsurface)
        return;

    if (self->egl_display == EGL_NO_DISPLAY ||
        self->egl_surface == EGL_NO_SURFACE)
        return;

    EGLint cur_w, cur_h;
    eglQuerySurface(self->egl_display, self->egl_surface, EGL_WIDTH,  &cur_w);
    eglQuerySurface(self->egl_display, self->egl_surface, EGL_HEIGHT, &cur_h);
    if ((size_t)cur_w != width || (size_t)cur_h != height)
        wl_egl_window_resize(self->egl_window, width, height, 0, 0);

    /* Blit the texture to the EGL surface. */
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
        glVertexAttribPointer(self->gl_position_loc, 2, GL_FLOAT,
                              GL_FALSE, 0, 0);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glDisableVertexAttribArray(self->gl_position_loc);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    }

    (void)texture_format;
    eglSwapBuffers(self->egl_display, self->egl_surface);

    /* Restore the renderer context. */
    eglMakeCurrent(self->egl_display, self->renderer_egl_surface,
                   self->renderer_egl_surface, self->renderer_egl_context);
}

void flutter_gl_compositor_resize(FlutterGLCompositor *self,
                                   size_t width, size_t height) {
    if (!self->is_subsurface || !self->egl_window)
        return;
    wl_egl_window_resize(self->egl_window, width, height, 0, 0);
}

void flutter_gl_compositor_render_clear(FlutterGLCompositor *self,
                                         size_t width, size_t height) {
    if (!self->is_subsurface)
        return;
    eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface,
                   self->egl_context);
    glViewport(0, 0, width, height);
    glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(self->egl_display, self->egl_surface);
    eglMakeCurrent(self->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
}
