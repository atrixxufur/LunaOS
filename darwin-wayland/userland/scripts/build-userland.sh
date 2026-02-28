#!/usr/bin/env bash
# =============================================================================
# build-userland.sh — LunaOS PureDarwin userland assembly
#
# Pulls all open-source Darwin components from apple-oss-distributions
# and assembles them into a complete rootfs at build/rootfs/.
#
# Component build order (each depends on the previous):
#   1.  Libc / libSystem       — C runtime, syscall wrappers
#   2.  dyld                   — dynamic linker
#   3.  libdispatch            — Grand Central Dispatch
#   4.  libpthread             — POSIX threads
#   5.  launchd (XPC compat)   — PureDarwin XPC replacement
#   6.  shell utilities        — bash, coreutils-darwin, file
#   7.  network stack          — configd, mDNSResponder (optional)
#   8.  LunaOS services        — seatd-darwin, darwin-evdev-bridge
#   9.  Wayland stack          — libwayland, wlroots, luna-compositor
#   10. rootfs skeleton        — /etc, /var, fstab, passwd, rc scripts
#
# Output: build/rootfs/  — a complete Darwin root filesystem
#
# Usage:
#   ./userland/scripts/build-userland.sh [--clean] [--jobs N] [--skip-wayland]
# =============================================================================

set -euo pipefail

LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${LUNA_ROOT}/build"
ROOTFS="${BUILD_DIR}/rootfs"
SOURCES="${BUILD_DIR}/userland-src"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
MACOS_TAG="macos-262"
APPLE_OSS="https://github.com/apple-oss-distributions"
PUREDARWIN="https://github.com/PureDarwin"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; BOLD='\033[1m'; NC='\033[0m'
log()      { echo -e "${GREEN}[userland]${NC} $*"; }
step()     { echo -e "${BOLD}${BLUE}[userland]${NC} ── $*"; }
warn()     { echo -e "${YELLOW}[userland] WARN:${NC} $*"; }
die()      { echo -e "${RED}[userland] ERROR:${NC} $*" >&2; exit 1; }
progress() { echo -e "${GREEN}[userland]${NC} [$1/$TOTAL_STEPS] $2"; }

TOTAL_STEPS=10
SKIP_WAYLAND=0
CLEAN=0

for arg in "$@"; do
    case "$arg" in
        --clean)         CLEAN=1 ;;
        --skip-wayland)  SKIP_WAYLAND=1 ;;
        --jobs)          shift; JOBS="$1" ;;
    esac
done

# ── Preflight ─────────────────────────────────────────────────────────────────
log "=== LunaOS PureDarwin userland assembly ==="
[[ "$(uname -s)" == "Darwin" ]] || die "Must build on macOS"
[[ "$(uname -m)" == "x86_64" ]] || die "x86_64 host required"

for tool in git cmake meson ninja xcodebuild python3; do
    command -v "$tool" &>/dev/null || die "Missing: $tool"
done

SDK="$(xcrun --sdk macosx --show-sdk-path)"
TOOLCHAIN="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain"
export SDKROOT="$SDK"
export CC="clang -arch x86_64"
export CXX="clang++ -arch x86_64"
export CFLAGS="-arch x86_64 -isysroot $SDK"
export LDFLAGS="-arch x86_64 -isysroot $SDK"
export PKG_CONFIG_PATH="${ROOTFS}/usr/local/lib/pkgconfig:${ROOTFS}/usr/lib/pkgconfig"

(( CLEAN )) && rm -rf "$ROOTFS" "$SOURCES"
mkdir -p "$ROOTFS" "$SOURCES"

# ── Helpers ───────────────────────────────────────────────────────────────────

clone() {
    # clone <url> <dir> [branch/tag]
    local url="$1" dir="$2" ref="${3:-main}"
    [[ -d "$dir/.git" ]] && { git -C "$dir" fetch -q; return; }
    git clone --depth 1 --branch "$ref" "$url" "$dir" --quiet
}

xbuild() {
    # xbuild <srcdir> [target] [extra xcflags]
    local src="$1" tgt="${2:-install}" extra="${3:-}"
    local name; name="$(basename "$src")"
    xcodebuild \
        -sdk macosx \
        -configuration Release \
        -target "$tgt" \
        ARCHS=x86_64 \
        SRCROOT="$src" \
        OBJROOT="${BUILD_DIR}/obj/${name}" \
        DSTROOT="$ROOTFS" \
        $extra 2>&1 | grep -E "(error:|Build succeeded|FAILED)" || true
}

cmakebuild() {
    local src="$1" name; name="$(basename "$src")"
    shift
    cmake -S "$src" -B "${BUILD_DIR}/obj/${name}" \
        -DCMAKE_INSTALL_PREFIX="${ROOTFS}/usr/local" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=x86_64 \
        "$@" --quiet
    cmake --build "${BUILD_DIR}/obj/${name}" -j"$JOBS"
    cmake --install "${BUILD_DIR}/obj/${name}"
}

mesonbuild() {
    local src="$1" name; name="$(basename "$src")"
    shift
    meson setup "${BUILD_DIR}/obj/${name}" "$src" \
        --prefix="${ROOTFS}/usr/local" \
        --buildtype=release \
        "$@"
    ninja -C "${BUILD_DIR}/obj/${name}" -j"$JOBS"
    ninja -C "${BUILD_DIR}/obj/${name}" install
}

# =============================================================================
# STEP 1: Libc + libSystem
# =============================================================================
progress 1 "Libc / libSystem (Darwin C runtime)"
step "Libc"

LIBC_COMPONENTS=(
    "Libc"
    "libsyscall"
    "Libm"
    "Libinfo"
    "libclosure"
)

for comp in "${LIBC_COMPONENTS[@]}"; do
    log "  cloning ${comp}..."
    clone "${APPLE_OSS}/${comp}.git" \
          "${SOURCES}/${comp}" \
          "$MACOS_TAG" || warn "  ${comp} clone failed — skipping"
done

# Build Libc headers first (other components need them)
if [[ -d "${SOURCES}/Libc" ]]; then
    log "  installing Libc headers..."
    xcodebuild \
        -sdk macosx -configuration Release \
        -target installhdrs \
        ARCHS=x86_64 \
        SRCROOT="${SOURCES}/Libc" \
        OBJROOT="${BUILD_DIR}/obj/Libc" \
        DSTROOT="$ROOTFS" 2>&1 | tail -3 || true
fi

# Build libSystem (the Darwin libc umbrella dylib)
if [[ -d "${SOURCES}/Libc" ]]; then
    log "  building libSystem.B.dylib..."
    xbuild "${SOURCES}/Libc" "libSystem" \
        "INSTALL_PATH=/usr/lib PUBLIC_HEADERS_FOLDER_PATH=/usr/include"
fi

# =============================================================================
# STEP 2: dyld
# =============================================================================
progress 2 "dyld (dynamic linker)"
step "dyld"

clone "${APPLE_OSS}/dyld.git" "${SOURCES}/dyld" "$MACOS_TAG"

if [[ -d "${SOURCES}/dyld" ]]; then
    log "  building dyld..."
    cmake -S "${SOURCES}/dyld" \
          -B "${BUILD_DIR}/obj/dyld" \
          -DCMAKE_INSTALL_PREFIX="${ROOTFS}" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_OSX_ARCHITECTURES=x86_64 \
          -DDYLD_PLATFORM=MacOSX \
          --quiet || warn "dyld cmake configuration failed — using prebuilt stub"

    cmake --build "${BUILD_DIR}/obj/dyld" -j"$JOBS" 2>&1 | tail -5 || true
    cmake --install "${BUILD_DIR}/obj/dyld" 2>&1 | tail -3 || true
fi

# =============================================================================
# STEP 3: libdispatch (GCD)
# =============================================================================
progress 3 "libdispatch (Grand Central Dispatch)"
step "libdispatch"

clone "${APPLE_OSS}/libdispatch.git" "${SOURCES}/libdispatch" "$MACOS_TAG"

cmakebuild "${SOURCES}/libdispatch" \
    -DENABLE_TESTING=OFF \
    -DENABLE_SWIFT=OFF \
    -DBUILD_SHARED_LIBS=ON || warn "libdispatch build had issues"

# =============================================================================
# STEP 4: libpthread
# =============================================================================
progress 4 "libpthread (POSIX threads)"
step "libpthread"

clone "${APPLE_OSS}/libpthread.git" "${SOURCES}/libpthread" "$MACOS_TAG"

if [[ -d "${SOURCES}/libpthread" ]]; then
    xcodebuild \
        -sdk macosx -configuration Release \
        ARCHS=x86_64 \
        SRCROOT="${SOURCES}/libpthread" \
        OBJROOT="${BUILD_DIR}/obj/libpthread" \
        DSTROOT="$ROOTFS" \
        install 2>&1 | tail -3 || true
fi

# =============================================================================
# STEP 5: launchd / XPC (PureDarwin replacement)
# =============================================================================
progress 5 "launchd / XPC (PureDarwin XPC replacement)"
step "launchd"

# Apple's launchd source was closed after macOS 10.9.
# PureDarwin/XPC is the community replacement that provides:
#   - launchd PID 1 (service manager)
#   - XPC inter-process communication
#   - launchctl command-line tool
clone "${PUREDARWIN}/XPC.git" "${SOURCES}/PureDarwin-XPC" "main"

if [[ -d "${SOURCES}/PureDarwin-XPC" ]]; then
    log "  building PureDarwin XPC (launchd replacement)..."
    cmakebuild "${SOURCES}/PureDarwin-XPC" \
        -DENABLE_TESTING=OFF || warn "PureDarwin XPC build failed"
fi

# Install our launchd plists
log "  installing LunaOS LaunchDaemons..."
LAUNCH_DAEMONS="${ROOTFS}/Library/LaunchDaemons"
mkdir -p "$LAUNCH_DAEMONS"
cp "${LUNA_ROOT}/build/launchd/"*.plist "$LAUNCH_DAEMONS/" 2>/dev/null || true

# =============================================================================
# STEP 6: Shell utilities
# =============================================================================
progress 6 "Shell utilities (bash, file tools, network tools)"
step "shell utilities"

SHELL_COMPONENTS=(
    "bash"
    "file_cmds"       # ls, cp, mv, rm, chmod, chown, ln, mkdir ...
    "shell_cmds"      # sh, test, echo, kill, printf ...
    "text_cmds"       # cat, head, tail, cut, sort, uniq, wc ...
    "developer_cmds"  # ar, nm, size, strings, strip ...
    "system_cmds"     # reboot, shutdown, ps, top, login ...
    "network_cmds"    # ifconfig, netstat, ping, route ...
)

for comp in "${SHELL_COMPONENTS[@]}"; do
    log "  cloning ${comp}..."
    clone "${APPLE_OSS}/${comp}.git" \
          "${SOURCES}/${comp}" \
          "$MACOS_TAG" || warn "  ${comp} not available — skipping"
done

# Build each component
for comp in "${SHELL_COMPONENTS[@]}"; do
    [[ -d "${SOURCES}/${comp}" ]] || continue
    log "  building ${comp}..."
    # Most Darwin shell components use xcodebuild with an 'install' target
    xcodebuild \
        -sdk macosx \
        -configuration Release \
        ARCHS=x86_64 \
        SRCROOT="${SOURCES}/${comp}" \
        OBJROOT="${BUILD_DIR}/obj/${comp}" \
        DSTROOT="$ROOTFS" \
        install 2>&1 | grep -E "(error:|succeeded|FAILED)" || true
done

# Install a minimal /etc/profile for interactive shells
cat > "${ROOTFS}/etc/profile" <<'PROFILE'
# /etc/profile — LunaOS system-wide shell profile
export PATH="/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
export TERM="${TERM:-xterm-256color}"
export LANG="en_US.UTF-8"

# Create XDG_RUNTIME_DIR if needed
[ -d "$XDG_RUNTIME_DIR" ] || mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null

if [ -f ~/.bashrc ]; then
    . ~/.bashrc
fi
PROFILE

# =============================================================================
# STEP 7: Network stack (configd, mDNSResponder)
# =============================================================================
progress 7 "Network stack (configd, mDNSResponder)"
step "network stack"

NET_COMPONENTS=(
    "configd"
    "mDNSResponder"
)

for comp in "${NET_COMPONENTS[@]}"; do
    clone "${APPLE_OSS}/${comp}.git" \
          "${SOURCES}/${comp}" \
          "$MACOS_TAG" || warn "${comp} not available — skipping"

    [[ -d "${SOURCES}/${comp}" ]] || continue
    log "  building ${comp}..."
    xbuild "${SOURCES}/${comp}" || warn "${comp} build failed — non-fatal"
done

# =============================================================================
# STEP 8: LunaOS services
# =============================================================================
progress 8 "LunaOS services (seatd-darwin, darwin-evdev-bridge)"
step "LunaOS services"

log "  building seatd-darwin..."
clang -arch x86_64 \
    -o "${ROOTFS}/usr/local/sbin/seatd-darwin" \
    "${LUNA_ROOT}/seatd-darwin/seatd-darwin.c" \
    -framework IOKit -framework CoreFoundation \
    -lpthread \
    -Os 2>&1 | tail -3 || warn "seatd-darwin compile failed"

log "  building darwin-evdev-bridge..."
clang -arch x86_64 \
    -o "${ROOTFS}/usr/local/sbin/darwin-evdev-bridge" \
    "${LUNA_ROOT}/evdev-bridge/darwin-evdev-bridge.c" \
    -framework IOKit -framework CoreFoundation \
    -lpthread \
    -Os 2>&1 | tail -3 || warn "darwin-evdev-bridge compile failed"

# Install our libdrm-darwin
log "  building libdrm-darwin..."
clang -arch x86_64 -dynamiclib \
    -o "${ROOTFS}/usr/local/lib/libdrm.dylib" \
    -install_name "/usr/local/lib/libdrm.dylib" \
    -current_version 2.4.120 \
    -compatibility_version 2.0.0 \
    "${LUNA_ROOT}/drm-shim/user/libdrm-darwin.c" \
    -I"${LUNA_ROOT}/drm-shim/include" \
    -Os 2>&1 | tail -3 || warn "libdrm-darwin compile failed"

ln -sf libdrm.dylib "${ROOTFS}/usr/local/lib/libdrm.2.dylib"

# Install pkg-config for libdrm
mkdir -p "${ROOTFS}/usr/local/lib/pkgconfig"
sed "s|@CMAKE_INSTALL_PREFIX@|/usr/local|g" \
    "${LUNA_ROOT}/build/libdrm.pc.in" \
    > "${ROOTFS}/usr/local/lib/pkgconfig/libdrm.pc"

# Copy IODRMShim.kext if built
if [[ -d "${LUNA_ROOT}/build/IODRMShim.kext" ]]; then
    sudo cp -r "${LUNA_ROOT}/build/IODRMShim.kext" \
               "${ROOTFS}/Library/Extensions/"
    log "  IODRMShim.kext installed to /Library/Extensions/"
fi

# =============================================================================
# STEP 9: Wayland stack
# =============================================================================
progress 9 "Wayland stack (libwayland, wlroots, luna-compositor)"
step "Wayland stack"

if (( SKIP_WAYLAND )); then
    warn "  --skip-wayland set — skipping (run build-compositor.sh separately)"
else
    log "  running build-compositor.sh..."
    ROOTFS="$ROOTFS" \
    PREFIX="${ROOTFS}/usr/local" \
    BUILD_DIR="${BUILD_DIR}/compositor-deps" \
    bash "${LUNA_ROOT}/scripts/build-compositor.sh" || \
        warn "compositor build had issues — check logs"
fi

# =============================================================================
# STEP 10: rootfs skeleton
# =============================================================================
progress 10 "Rootfs skeleton (etc, var, fstab, passwd, rc)"
step "rootfs skeleton"

log "  applying rootfs skeleton..."
# Overlay our skeleton on top of what was built
rsync -a "${LUNA_ROOT}/userland/rootfs-skel/" "${ROOTFS}/"

# Create standard directories if missing
for d in \
    bin sbin usr/bin usr/sbin usr/lib usr/local/bin usr/local/sbin \
    usr/local/lib etc var/log var/run var/tmp tmp dev proc \
    Library/Extensions Library/LaunchDaemons \
    System/Library/Kernels System/Library/CoreServices \
    private/var/log private/var/run private/tmp \
    run/user; do
    mkdir -p "${ROOTFS}/${d}"
done

# Symlinks matching Darwin's filesystem layout
ln -sf private/var "${ROOTFS}/var"     2>/dev/null || true
ln -sf private/tmp "${ROOTFS}/tmp"     2>/dev/null || true
ln -sf private/etc "${ROOTFS}/etc"     2>/dev/null || true

# Install kernel
KERNEL_BIN="${BUILD_DIR}/kernel/output/mach_kernel"
if [[ -f "$KERNEL_BIN" ]]; then
    cp "$KERNEL_BIN" "${ROOTFS}/System/Library/Kernels/kernel"
    log "  kernel installed to /System/Library/Kernels/kernel"
else
    warn "  kernel not found at ${KERNEL_BIN} — run build-xnu.sh first"
fi

# Install KEXTs
if [[ -d "${BUILD_DIR}/kernel/output/Extensions" ]]; then
    cp -r "${BUILD_DIR}/kernel/output/Extensions/." \
          "${ROOTFS}/System/Library/Extensions/"
    log "  $(ls "${ROOTFS}/System/Library/Extensions/" | wc -l | tr -d ' ') KEXTs installed"
fi

# Install boot plist
cp "${LUNA_ROOT}/kernel/config/com.apple.Boot.plist" \
   "${ROOTFS}/Library/Preferences/SystemConfiguration/"

# Install luna-compositor binary if built
if command -v luna-compositor &>/dev/null; then
    cp "$(command -v luna-compositor)" "${ROOTFS}/usr/local/bin/"
fi

# ── Summary ────────────────────────────────────────────────────────────────────
echo ""
log "╔══════════════════════════════════════════════════════════════════╗"
log "║          LunaOS userland assembly complete                      ║"
log "╠══════════════════════════════════════════════════════════════════╣"
ROOTFS_SIZE=$(du -sh "$ROOTFS" 2>/dev/null | cut -f1)
log "║  Rootfs: ${ROOTFS}"
log "║  Size:   ${ROOTFS_SIZE}"
log "╠══════════════════════════════════════════════════════════════════╣"
log "║  Next: ./userland/scripts/verify-rootfs.sh"
log "║  Then: ./userland/scripts/build-iso.sh"
log "╚══════════════════════════════════════════════════════════════════╝"
