cmake_minimum_required(VERSION 3.18)
project(darwin-wayland
    VERSION 0.1.0
    DESCRIPTION "Wayland kernel-level support layer for Darwin/PureDarwin (x86_64)"
    LANGUAGES C CXX
)

# ── Target architecture: x86_64 only ─────────────────────────────────────────
# TODO_ARM64: To add arm64 support later:
#   1. Add IOMobileFramebuffer KEXT backend (IODRMShimArm64.cpp)
#   2. Change CMAKE_OSX_ARCHITECTURES to "x86_64;arm64"
#   3. Use lipo to split fat binaries when packaging each ISO
set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Build arch" FORCE)

# ── Require Darwin/x86_64 ────────────────────────────────────────────────────
if(NOT APPLE)
    message(FATAL_ERROR "darwin-wayland targets Darwin/XNU only.")
endif()

# Fail fast if someone tries to cross-compile to arm64 by mistake
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    message(FATAL_ERROR
        "arm64 is not yet supported. "
        "See TODO_ARM64 comments throughout the codebase.")
endif()

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)  # required for IOKit C headers

# ── Output directories ────────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ── Find system frameworks ────────────────────────────────────────────────────
find_library(IOKIT_FRAMEWORK     IOKit          REQUIRED)
find_library(COREFOUNDATION_LIB  CoreFoundation REQUIRED)

# ── Compiler flags ────────────────────────────────────────────────────────────
add_compile_options(
    -arch x86_64
    -Wall -Wextra -Wno-unused-parameter
    -fno-strict-aliasing    # required for IOKit C++ compat
)

# KEXT builds: no stdlib, kernel headers, x86_64 only
set(KEXT_COMPILE_FLAGS
    -arch x86_64
    -fno-builtin
    -fno-exceptions
    -fno-rtti
    -mkernel
    # TODO_ARM64: swap -mkernel for -target arm64-apple-macosx-kernel when ready
)

# ── Include dirs ──────────────────────────────────────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/drm-shim/include
)

# ═══════════════════════════════════════════════════════════════════════════════
# 1. libdrm-darwin  (userspace DRM compatibility library)
# ═══════════════════════════════════════════════════════════════════════════════

add_library(drm SHARED
    drm-shim/user/libdrm-darwin.c
)
target_include_directories(drm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/drm-shim/include>
    $<INSTALL_INTERFACE:include/drm>
)
set_target_properties(drm PROPERTIES
    VERSION   2.4.120
    SOVERSION 2
    OUTPUT_NAME drm
    PUBLIC_HEADER "drm-shim/include/drm_darwin.h;drm-shim/include/libdrm-darwin.h"
)
# Symlink compatibility: libdrm.2.dylib → libdrm.dylib
install(TARGETS drm
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include/drm
)

# ═══════════════════════════════════════════════════════════════════════════════
# 2. darwin-evdev-bridge  (IOHIDFamily → /dev/input/eventN daemon)
# ═══════════════════════════════════════════════════════════════════════════════

add_executable(darwin-evdev-bridge
    evdev-bridge/darwin-evdev-bridge.c
)
target_link_libraries(darwin-evdev-bridge
    ${IOKIT_FRAMEWORK}
    ${COREFOUNDATION_LIB}
    pthread
)
install(TARGETS darwin-evdev-bridge RUNTIME DESTINATION sbin)

# ═══════════════════════════════════════════════════════════════════════════════
# 3. seatd-darwin  (seat management daemon)
# ═══════════════════════════════════════════════════════════════════════════════

add_executable(seatd-darwin
    seatd-darwin/seatd-darwin.c
)
target_link_libraries(seatd-darwin
    ${IOKIT_FRAMEWORK}
    ${COREFOUNDATION_LIB}
    pthread
)
install(TARGETS seatd-darwin RUNTIME DESTINATION sbin)

# ═══════════════════════════════════════════════════════════════════════════════
# 4. wayland-event-loop-darwin  (kqueue backend for libwayland-server)
#    — not a standalone binary; compiled as an object to be linked into
#      libwayland-server when building from source with Darwin patches.
# ═══════════════════════════════════════════════════════════════════════════════

add_library(wayland-event-loop-darwin OBJECT
    libwayland-darwin/wayland-darwin-event-loop.c
)
target_compile_options(wayland-event-loop-darwin PRIVATE
    -DWAYLANDSERVER_BUILD   # tells libwayland headers to expose internals
)
# Install the object file so the libwayland build system can pick it up
install(FILES
    $<TARGET_OBJECTS:wayland-event-loop-darwin>
    DESTINATION lib/wayland-darwin
)

# ═══════════════════════════════════════════════════════════════════════════════
# 5. IODRMShim KEXT  (kernel extension — requires KDK + Xcode)
#    This target is optional: it only builds when the KDK is present.
# ═══════════════════════════════════════════════════════════════════════════════

if(EXISTS "/Library/Developer/KDKs")
    # Find the newest KDK
    file(GLOB KDK_DIRS "/Library/Developer/KDKs/KDK_*.kdk")
    list(SORT KDK_DIRS ORDER DESCENDING)
    list(GET KDK_DIRS 0 LATEST_KDK)
    message(STATUS "Building IODRMShim.kext with KDK: ${LATEST_KDK}")

    set(KEXT_HEADERS
        ${LATEST_KDK}/System/Library/Frameworks/Kernel.framework/Headers
        ${LATEST_KDK}/System/Library/Frameworks/IOKit.framework/Headers
        ${LATEST_KDK}/System/Library/Frameworks/IOKit.framework/Headers/graphics
    )

    add_library(IODRMShim_objects OBJECT
        drm-shim/kern/IODRMShim.cpp
    )
    target_compile_options(IODRMShim_objects PRIVATE
        ${KEXT_COMPILE_FLAGS}
    )
    target_include_directories(IODRMShim_objects PRIVATE
        ${KEXT_HEADERS}
        ${CMAKE_SOURCE_DIR}/drm-shim/include
    )

    # Bundle the KEXT (manual step — CMake can't create full .kext bundles)
    # After cmake build, run: make-kext.sh
    message(STATUS "IODRMShim: kernel extension objects built.")
    message(STATUS "Run scripts/package-kext.sh to create IODRMShim.kext bundle.")
else()
    message(WARNING
        "Kernel Development Kit (KDK) not found at /Library/Developer/KDKs.\n"
        "IODRMShim.kext will not be built.  Install the KDK matching your\n"
        "Darwin kernel version from developer.apple.com/downloads.")
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# 6. pkg-config files  (so wlroots/Mesa find libdrm via pkg-config)
# ═══════════════════════════════════════════════════════════════════════════════

configure_file(
    ${CMAKE_SOURCE_DIR}/build/libdrm.pc.in
    ${CMAKE_BINARY_DIR}/libdrm.pc
    @ONLY
)
install(FILES ${CMAKE_BINARY_DIR}/libdrm.pc
    DESTINATION lib/pkgconfig
)

# ═══════════════════════════════════════════════════════════════════════════════
# 7. Install launchd plists (PureDarwin XPC / launchd-compat)
# ═══════════════════════════════════════════════════════════════════════════════

install(FILES
    build/launchd/org.puredarwin.seatd.plist
    build/launchd/org.puredarwin.evdev-bridge.plist
    DESTINATION /Library/LaunchDaemons
)

# ═══════════════════════════════════════════════════════════════════════════════
# 8. Summary
# ═══════════════════════════════════════════════════════════════════════════════

message(STATUS "")
message(STATUS "════════════════════════════════════════════════")
message(STATUS " darwin-wayland  [x86_64 only]")
message(STATUS "════════════════════════════════════════════════")
message(STATUS " Arch:                  x86_64")
message(STATUS " libdrm.dylib           → DRM userspace shim")
message(STATUS " darwin-evdev-bridge    → HID → /dev/input/")
message(STATUS " seatd-darwin           → seat management")
message(STATUS " wayland-event-loop     → kqueue event loop")
if(EXISTS "/Library/Developer/KDKs")
message(STATUS " IODRMShim.kext         → DRM kernel extension (KDK found)")
else()
message(STATUS " IODRMShim.kext         → SKIPPED (install KDK)")
endif()
message(STATUS "────────────────────────────────────────────────")
message(STATUS " Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS " TODO_ARM64: see drm_darwin.h and CMakeLists.txt")
message(STATUS "════════════════════════════════════════════════")
message(STATUS "")
message(STATUS "Next steps:")
message(STATUS " 1. cmake --build . && sudo cmake --install .")
message(STATUS " 2. sudo kextload /Library/Extensions/IODRMShim.kext")
message(STATUS " 3. sudo darwin-evdev-bridge &")
message(STATUS " 4. sudo seatd-darwin -g video &")
message(STATUS " 5. export WAYLAND_DISPLAY=wayland-0")
message(STATUS "    weston --backend=drm-backend.so")
