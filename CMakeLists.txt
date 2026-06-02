cmake_minimum_required(VERSION 3.16)
project(flutter-subsurface-prototype VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_EGL REQUIRED IMPORTED_TARGET wayland-egl)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLESV2 REQUIRED IMPORTED_TARGET glesv2)

add_executable(flutter-subsurface-prototype
    src/main.c
    src/renderer.c
    src/flutter_renderer.c
    src/flutter_gl_renderer.c
    src/flutter_subsurface_renderer.c
    src/flutter_software_renderer.c
    src/flutter_view_resize.c
    src/flutter_gl_compositor.c
    src/flutter_subsurface.c
)

target_link_libraries(flutter-subsurface-prototype PRIVATE
    PkgConfig::GTK3
    PkgConfig::WAYLAND
    PkgConfig::WAYLAND_EGL
    PkgConfig::EGL
    PkgConfig::GLESV2
    m
)

target_compile_options(flutter-subsurface-prototype PRIVATE -Wall -Wextra)
