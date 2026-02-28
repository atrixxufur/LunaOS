#!/usr/bin/env bash
# =============================================================================
# build-compositor.sh — LunaOS full Wayland stack build script
#
# Clones, patches, and builds in order:
#   1. libwayland      (with kqueue event loop patch)
#   2. libxkbcommon    (pure C, no patches needed)
#   3. libinput        (with Darwin evdev-bridge backend)
#   4. pixman          (software renderer dependency)
#   5. wlroots         (with Darwin DRM session patches)
#   6. luna-compositor (the LunaOS compositor itself)
#
# Prerequisites:
#   - macOS host with Xcode + Command Line Tools
#   - Homebrew: brew install cmake meson ninja pkg-config
#   - darwin-wayland built and installed (run this from the repo root)
#   - IODRMShim.kext loaded: sudo kextload /Library/Extensions/IODRMShim.kext
#   - seatd-darwin running: sudo seatd-darwin -g video &
#   - darwin-evdev-bridge running: sudo darwin-evdev-bridge &
#
# Usage:
#   chmod +x scripts/build-compositor.sh
#   ./scripts/build-compositor.sh
# =============================================================================

set -euo pipefail

# ── Config ────────────────────────────────────────────────────────────────────
PREFIX="${PREFIX:-/usr/local}"
BUILD_DIR="${BUILD_DIR:-$(pwd)/build/compositor-deps}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
PATCHES_DIR="$(cd "$(dirname "$0")/.." && pwd)/patches"

# Library versions
WAYLAND_VERSION="1.23.0"
XKBCOMMON_VERSION="2.7.0"
LIBINPUT_VERSION="1.26.0"
PIXMAN_VERSION="0.43.4"
WLROOTS_VERSION="0.18.0"

export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export CFLAGS="-arch x86_64 ${CFLAGS:-}"
export LDFLAGS="-arch x86_64 ${LDFLAGS:-}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}[luna-build]${NC} $*"; }
warn() { echo -e "${YELLOW}[luna-build]${NC} $*"; }
die()  { echo -e "${RED}[luna-build] ERROR:${NC} $*" >&2; exit 1; }

# ── Preflight checks ──────────────────────────────────────────────────────────
log "=== LunaOS Wayland stack build ==="

[[ "$(uname -s)" == "Darwin" ]] || die "Must run on macOS/Darwin"
[[ "$(uname -m)" == "x86_64" ]] || die "Must run on x86_64 (not Apple Silicon)"

for tool in cmake meson ninja pkg-config git; do
    command -v "$tool" &>/dev/null || die "'$tool' not found. Install with: brew install $tool"
done

# Check IODRMShim is loaded
if ! ls /dev/dri/card0 &>/dev/null; then
    warn "/dev/dri/card0 not found — IODRMShim.kext may not be loaded"
    warn "Run: sudo kextload /Library/Extensions/IODRMShim.kext"
    warn "Continuing anyway (needed at runtime, not build time)"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# =============================================================================
# 1. libwayland (with kqueue event loop)
# =============================================================================
log "── [1/6] libwayland ${WAYLAND_VERSION}"

if [[ ! -d wayland ]]; then
    git clone --depth 1 --branch "$WAYLAND_VERSION" \
        https://gitlab.freedesktop.org/wayland/wayland.git
fi
cd wayland

# Apply kqueue event loop patch
if [[ ! -f .kqueue-patched ]]; then
    log "  applying kqueue event loop patch..."
    # Replace wayland-event-loop.c with our Darwin kqueue version
    cp "${PATCHES_DIR}/wlroots/../libwayland/../../../libwayland-darwin/wayland-darwin-event-loop.c" \
       src/wayland-darwin-event-loop.c 2>/dev/null || \
    cp "$(dirname "$PATCHES_DIR")/libwayland-darwin/wayland-darwin-event-loop.c" \
       src/wayland-darwin-event-loop.c

    # Patch meson.build to swap the source file on Darwin
    patch -p1 <<'PATCH'
--- a/src/meson.build
+++ b/src/meson.build
@@ -20,7 +20,12 @@ wayland_server_sources = [
   'wayland-server.c',
   'wayland-shm.c',
   'wayland-util.c',
+]
+if host_machine.system() == 'darwin'
+  wayland_server_sources += 'wayland-darwin-event-loop.c'
+else
+  wayland_server_sources += [
   'wayland-event-loop.c',
-]
+  ]
+endif
PATCH
    touch .kqueue-patched
fi

meson setup build \
    --prefix="$PREFIX" \
    --buildtype=release \
    -Ddocumentation=false \
    -Dtests=false
ninja -C build -j"$JOBS"
sudo ninja -C build install
cd "$BUILD_DIR"

# =============================================================================
# 2. libxkbcommon
# =============================================================================
log "── [2/6] libxkbcommon ${XKBCOMMON_VERSION}"

if [[ ! -d libxkbcommon ]]; then
    git clone --depth 1 --branch "xkbcommon-${XKBCOMMON_VERSION}" \
        https://github.com/xkbcommon/libxkbcommon.git
fi
cd libxkbcommon
# No Darwin patches needed — pure C, no Linux deps
meson setup build \
    --prefix="$PREFIX" \
    --buildtype=release \
    -Denable-docs=false \
    -Denable-tools=false \
    -Denable-x11=false
ninja -C build -j"$JOBS"
sudo ninja -C build install
cd "$BUILD_DIR"

# =============================================================================
# 3. pixman
# =============================================================================
log "── [3/6] pixman ${PIXMAN_VERSION}"

if [[ ! -d pixman ]]; then
    git clone --depth 1 --branch "pixman-${PIXMAN_VERSION}" \
        https://gitlab.freedesktop.org/pixman/pixman.git
fi
cd pixman
meson setup build \
    --prefix="$PREFIX" \
    --buildtype=release \
    -Dgtk=disabled \
    -Dlibpng=disabled \
    -Dtests=disabled \
    -Ddemos=disabled
ninja -C build -j"$JOBS"
sudo ninja -C build install
cd "$BUILD_DIR"

# =============================================================================
# 4. libinput (point it at our evdev bridge nodes)
# =============================================================================
log "── [4/6] libinput ${LIBINPUT_VERSION}"

if [[ ! -d libinput ]]; then
    git clone --depth 1 --branch "$LIBINPUT_VERSION" \
        https://gitlab.freedesktop.org/libinput/libinput.git
fi
cd libinput

# Darwin patch: replace udev_monitor with IOKit-based device scanner
if [[ ! -f .darwin-patched ]]; then
    log "  applying Darwin evdev-bridge patch to libinput..."
    patch -p1 <<'PATCH'
--- a/meson.build
+++ b/meson.build
@@ -1,5 +1,11 @@
+# Darwin: no udev. Device nodes are provided by darwin-evdev-bridge
+# which creates /dev/input/event* pipe-backed nodes.
+if host_machine.system() == 'darwin'
+  add_project_arguments('-DLIBINPUT_DARWIN=1', language: 'c')
+  add_project_arguments('-DHAVE_LIBEVDEV=1', language: 'c')
+endif
+
 project('libinput', 'c',
PATCH

    # Patch udev backend to skip udev on Darwin, use direct path scanning
    patch -p1 <<'PATCH'
--- a/src/udev-seat.c
+++ b/src/udev-seat.c
@@ -1,3 +1,7 @@
+#ifdef LIBINPUT_DARWIN
+/* Darwin: udev replaced by direct evdev node scanning */
+#include "darwin-evdev-seat.c"
+#else
 #include "config.h"
 /* ... original udev-seat.c content ... */
+#endif
PATCH

    # Create Darwin evdev seat implementation
    cat > src/darwin-evdev-seat.c << 'DARWINSEAT'
/*
 * darwin-evdev-seat.c — libinput seat backend for Darwin
 *
 * Replaces udev device discovery with direct scanning of
 * /dev/input/event* nodes created by darwin-evdev-bridge.
 */
#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "libinput-private.h"

static int darwin_open_restricted(const char *path, int flags, void *ud) {
    (void)ud;
    return open(path, flags | O_CLOEXEC);
}
static void darwin_close_restricted(int fd, void *ud) {
    (void)ud; close(fd);
}
static const struct libinput_interface darwin_iface = {
    .open_restricted  = darwin_open_restricted,
    .close_restricted = darwin_close_restricted,
};

struct libinput *libinput_darwin_create_context(void) {
    struct libinput *li =
        libinput_path_create_context(&darwin_iface, NULL);
    if (!li) return NULL;

    /* Scan /dev/input/ for nodes created by darwin-evdev-bridge */
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        fprintf(stderr, "libinput-darwin: /dev/input not found. "
                "Is darwin-evdev-bridge running?\n");
        return li;
    }
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (strncmp(ent->d_name, "event", 5)) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        libinput_path_add_device(li, path);
    }
    closedir(dir);
    return li;
}
DARWINSEAT

    touch .darwin-patched
fi

meson setup build \
    --prefix="$PREFIX" \
    --buildtype=release \
    -Ddocumentation=false \
    -Dtests=false \
    -Dlibwacom=false \
    -Ddebug-gui=false \
    -Dudev=false \
    -Dzshcompletiondir=no
ninja -C build -j"$JOBS"
sudo ninja -C build install
cd "$BUILD_DIR"

# =============================================================================
# 5. wlroots (with Darwin DRM session + pixman-only renderer patches)
# =============================================================================
log "── [5/6] wlroots ${WLROOTS_VERSION}"

if [[ ! -d wlroots ]]; then
    git clone --depth 1 --branch "$WLROOTS_VERSION" \
        https://gitlab.freedesktop.org/wlroots/wlroots.git
fi
cd wlroots

if [[ ! -f .darwin-patched ]]; then
    log "  applying Darwin DRM backend patches..."

    # Copy our Darwin session files into wlroots
    mkdir -p backend/drm/darwin
    cp "${PATCHES_DIR}/wlroots/drm_darwin_session.c" backend/drm/darwin/
    cp "${PATCHES_DIR}/wlroots/drm_darwin_session.h" backend/drm/darwin/

    # Apply the main patches
    git apply "${PATCHES_DIR}/wlroots/0001-drm-darwin-backend.patch" || \
        warn "patch 0001 failed — may already be applied"
    git apply "${PATCHES_DIR}/wlroots/0002-pixman-renderer-darwin.patch" || \
        warn "patch 0002 failed — may already be applied"

    touch .darwin-patched
fi

meson setup build \
    --prefix="$PREFIX" \
    --buildtype=release \
    -Dbackends=drm,libinput \
    -Drenderers=pixman \
    -Dallocators=drm_dumb \
    -Dxwayland=disabled \
    -Dexamples=false \
    -Dtests=false \
    -Dwerror=false
ninja -C build -j"$JOBS"
sudo ninja -C build install
cd "$BUILD_DIR"

# =============================================================================
# 6. luna-compositor
# =============================================================================
log "── [6/6] luna-compositor"

COMPOSITOR_SRC="$(dirname "$BUILD_DIR")/../compositor"
# Resolve relative to script location
COMPOSITOR_SRC="$(cd "$(dirname "$0")/../compositor" && pwd)"

cmake -B luna-compositor-build \
    -S "$COMPOSITOR_SRC" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build luna-compositor-build -j"$JOBS"
sudo cmake --install luna-compositor-build

# =============================================================================
# Done
# =============================================================================
log ""
log "╔══════════════════════════════════════════════════════════╗"
log "║        LunaOS compositor stack build complete!          ║"
log "╠══════════════════════════════════════════════════════════╣"
log "║  Binary: ${PREFIX}/bin/luna-compositor                  ║"
log "╠══════════════════════════════════════════════════════════╣"
log "║  Runtime startup order:                                  ║"
log "║  1. sudo kextload /Library/Extensions/IODRMShim.kext     ║"
log "║  2. sudo darwin-evdev-bridge &                           ║"
log "║  3. sudo seatd-darwin -g video &                         ║"
log "║  4. luna-compositor                                      ║"
log "╠══════════════════════════════════════════════════════════╣"
log "║  Keybindings:                                            ║"
log "║    Super+T         → open terminal (foot/xterm)          ║"
log "║    Super+Q         → close focused window                ║"
log "║    Super+M         → maximize/restore window             ║"
log "║    Super+Shift+E   → exit compositor                     ║"
log "╚══════════════════════════════════════════════════════════╝"
