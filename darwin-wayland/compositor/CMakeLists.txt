cmake_minimum_required(VERSION 3.18)
project(luna-compositor VERSION 0.1.0 LANGUAGES C)

# ── x86_64 only ───────────────────────────────────────────────────────────────
set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "" FORCE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

add_compile_options(-arch x86_64 -Wall -Wextra -Wno-unused-parameter)

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)

# wlroots — must be built from source with Darwin patches applied
pkg_check_modules(WLROOTS REQUIRED wlroots)

# libwayland-server — must be built with kqueue event loop patch
pkg_check_modules(WAYLAND_SERVER REQUIRED wayland-server)

# libdrm-darwin — our DRM shim
find_library(LIBDRM drm HINTS /usr/local/lib REQUIRED)

# xkbcommon — for keyboard handling
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)

# pixman — software renderer
pkg_check_modules(PIXMAN REQUIRED pixman-1)

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(luna-compositor
    src/main.c
)

target_include_directories(luna-compositor PRIVATE
    include
    ${WLROOTS_INCLUDE_DIRS}
    ${WAYLAND_SERVER_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${PIXMAN_INCLUDE_DIRS}
)

target_link_libraries(luna-compositor
    ${WLROOTS_LIBRARIES}
    ${WAYLAND_SERVER_LIBRARIES}
    ${LIBDRM}
    ${XKBCOMMON_LIBRARIES}
    ${PIXMAN_LIBRARIES}
)

install(TARGETS luna-compositor RUNTIME DESTINATION bin)
