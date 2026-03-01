cmake_minimum_required(VERSION 3.18)
project(luna-shell VERSION 0.1.0 LANGUAGES C)

set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "" FORCE)
set(CMAKE_C_STANDARD 11)
add_compile_options(-arch x86_64 -Wall -Wextra -Wno-unused-parameter -lm)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)

# wlr-layer-shell and foreign-toplevel protocols (generated from XML)
add_custom_command(
    OUTPUT wlr-layer-shell-unstable-v1-client-protocol.h
           wlr-layer-shell-unstable-v1-protocol.c
    COMMAND wayland-scanner client-header
            ${CMAKE_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml
            wlr-layer-shell-unstable-v1-client-protocol.h
    COMMAND wayland-scanner private-code
            ${CMAKE_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml
            wlr-layer-shell-unstable-v1-protocol.c
)
add_custom_command(
    OUTPUT wlr-foreign-toplevel-management-unstable-v1-client-protocol.h
           wlr-foreign-toplevel-management-unstable-v1-protocol.c
    COMMAND wayland-scanner client-header
            ${CMAKE_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml
            wlr-foreign-toplevel-management-unstable-v1-client-protocol.h
    COMMAND wayland-scanner private-code
            ${CMAKE_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml
            wlr-foreign-toplevel-management-unstable-v1-protocol.c
)

add_executable(luna-shell
    src/luna-shell-main.c
    src/luna-panel.c
    src/luna-launcher.c
    src/luna-wallpaper.c
    wlr-layer-shell-unstable-v1-protocol.c
    wlr-foreign-toplevel-management-unstable-v1-protocol.c
)
target_include_directories(luna-shell PRIVATE
    include
    ${CMAKE_BINARY_DIR}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
)
target_link_libraries(luna-shell
    ${WAYLAND_CLIENT_LIBRARIES} m
)
install(TARGETS luna-shell RUNTIME DESTINATION bin)
