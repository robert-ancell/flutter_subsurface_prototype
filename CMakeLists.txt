cmake_minimum_required(VERSION 3.16)
project(flutter-subsurface-prototype VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)

add_executable(flutter-subsurface-prototype
    src/main.c
    src/subsurface_widget.c
)

target_link_libraries(flutter-subsurface-prototype PRIVATE
    PkgConfig::GTK3
    PkgConfig::WAYLAND
)

target_compile_options(flutter-subsurface-prototype PRIVATE -Wall -Wextra)
