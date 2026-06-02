#pragma once

#include <GLES2/gl2.h>
#include <stddef.h>

typedef struct {
    GLuint texture;
    size_t width;
    size_t height;
} FlutterBackingStore;
