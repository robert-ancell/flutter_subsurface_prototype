#pragma once

#include <GLES2/gl2.h>
#include <stddef.h>

typedef enum {
    FLUTTER_BACKING_STORE_TYPE_OPENGL,
    FLUTTER_BACKING_STORE_TYPE_SOFTWARE,
} FlutterBackingStoreType;

typedef struct {
    GLuint texture;
} FlutterOpenGLBackingStore;

typedef struct {
    void *buffer;
} FlutterSoftwareBackingStore;

typedef struct {
    FlutterBackingStoreType type;
    size_t width;
    size_t height;
    union {
        FlutterOpenGLBackingStore   opengl;
        FlutterSoftwareBackingStore software;
    };
} FlutterBackingStore;
