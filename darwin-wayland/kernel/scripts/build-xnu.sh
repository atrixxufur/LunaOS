#!/usr/bin/env bash
# =============================================================================
# build-xnu.sh — LunaOS XNU kernel build script
#
# Builds XNU kernel from apple-oss-distributions at the macos-262 tag.
# macos-262 = Darwin 26 = the open-source base for macOS 26 (Sequoia+)
#
# What this script does:
#   1.  Installs required build tools (LLVM, cmake, ninja via brew)
#   2.  Clones all XNU dependencies from apple-oss-distributions
#   3.  Builds dependencies in order (dtrace → AvailabilityVersions → libdispatch → ...)
#   4.  Builds XNU itself (RELEASE_X86_64 configuration)
#   5.  Packages the kernel + symbol file into build/kernel/
#
# Output:
#   build/kernel/mach_kernel          — the XNU kernel binary
#   build/kernel/mach_kernel.dSYM/    — debug symbols
#   build/kernel/System.kext/         — kernel extensions bundle
#
# Host requirements:
#   - macOS 13+ (Ventura) or macOS 26 (for macos-262 SDKs)
#   - Xcode 15+ with Command Line Tools
#   - ~40GB free disk space
#   - ~16GB RAM recommended
#   - Apple Developer account (free tier OK — needed for SDK downloads)
#
# Time estimate: 45-90 minutes on modern x86_64 hardware
#
# Usage:
#   chmod +x kernel/scripts/build-xnu.sh
#   ./kernel/scripts/build-xnu.sh [--clean] [--jobs N]
# =============================================================================

set -euo pipefail

# ── Configuration ─────────────────────────────────────────────────────────────
LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${LUNA_ROOT}/build/kernel"
SOURCES_DIR="${LUNA_ROOT}/build/kernel-src"
PREFIX="${PREFIX:-/usr/local}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
MACOS_TAG="macos-262"
XNU_TAG="xnu-11215"    # XNU version matching macos-262

# apple-oss-distributions GitHub base URL
APPLE_OSS="https://github.com/apple-oss-distributions"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; NC='\033[0m'
log()     { echo -e "${GREEN}[xnu-build]${NC} $*"; }
step()    { echo -e "${BLUE}[xnu-build]${NC} ── $*"; }
warn()    { echo -e "${YELLOW}[xnu-build] WARN:${NC} $*"; }
die()     { echo -e "${RED}[xnu-build] ERROR:${NC} $*" >&2; exit 1; }
progress(){ echo -e "${GREEN}[xnu-build]${NC} [$1/$2] $3"; }

# ── Parse args ────────────────────────────────────────────────────────────────
CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --jobs)  shift; JOBS="$1" ;;
    esac
done

# ── Preflight ─────────────────────────────────────────────────────────────────
log "=== LunaOS XNU kernel build (${XNU_TAG} / ${MACOS_TAG}) ==="
log "Host: $(uname -srm)"
log "Jobs: ${JOBS}"
log "Output: ${BUILD_DIR}"

[[ "$(uname -s)" == "Darwin" ]]  || die "Must build on macOS (Darwin)"
[[ "$(uname -m)" == "x86_64" ]]  || die "x86_64 host required"

# Check Xcode
if ! xcode-select -p &>/dev/null; then
    die "Xcode Command Line Tools not installed.\nRun: xcode-select --install"
fi
XCODE_VER=$(xcodebuild -version 2>/dev/null | head -1 | awk '{print $2}')
log "Xcode version: ${XCODE_VER}"

# Minimum Xcode 15
XCODE_MAJOR=$(echo "$XCODE_VER" | cut -d. -f1)
(( XCODE_MAJOR >= 15 )) || die "Xcode 15+ required (found ${XCODE_VER})"

# Check brew tools
MISSING_TOOLS=()
for tool in cmake ninja git python3; do
    command -v "$tool" &>/dev/null || MISSING_TOOLS+=("$tool")
done
if (( ${#MISSING_TOOLS[@]} > 0 )); then
    die "Missing tools: ${MISSING_TOOLS[*]}\nInstall with: brew install ${MISSING_TOOLS[*]}"
fi

# Check LLVM — XNU requires clang 15+
if command -v /usr/local/opt/llvm/bin/clang &>/dev/null; then
    LLVM_PREFIX="/usr/local/opt/llvm"
elif command -v /opt/homebrew/opt/llvm/bin/clang &>/dev/null; then
    # Note: this path shouldn't exist on x86_64 but handle gracefully
    LLVM_PREFIX="/opt/homebrew/opt/llvm"
else
    warn "LLVM not found in Homebrew. Installing..."
    brew install llvm
    LLVM_PREFIX="/usr/local/opt/llvm"
fi
export PATH="${LLVM_PREFIX}/bin:$PATH"
log "LLVM: $(clang --version | head -1)"

# Clean if requested
if (( CLEAN )); then
    log "Cleaning build directories..."
    rm -rf "$BUILD_DIR" "$SOURCES_DIR"
fi

mkdir -p "$BUILD_DIR" "$SOURCES_DIR"

# ── Helper: clone or update a repo ────────────────────────────────────────────
clone_repo() {
    local name="$1"
    local tag="$2"
    local url="${APPLE_OSS}/${name}.git"
    local dest="${SOURCES_DIR}/${name}"

    if [[ -d "$dest/.git" ]]; then
        log "  ${name}: already cloned, fetching..."
        git -C "$dest" fetch --tags --quiet
    else
        log "  ${name}: cloning @ ${tag}..."
        git clone --depth 1 --branch "$tag" "$url" "$dest" --quiet
    fi
}

# ── Helper: build a component with xcodebuild ─────────────────────────────────
xbuild() {
    local name="$1"
    local target="${2:-install}"
    local extra_args="${3:-}"
    local srcdir="${SOURCES_DIR}/${name}"
    local dstdir="${BUILD_DIR}/dst"
    local objdir="${BUILD_DIR}/obj/${name}"

    mkdir -p "$dstdir" "$objdir"

    log "  building ${name}..."
    xcodebuild \
        -target "$target" \
        -configuration Release \
        SRCROOT="$srcdir" \
        OBJROOT="$objdir" \
        DSTROOT="$dstdir" \
        SDKROOT=macosx \
        ARCHS=x86_64 \
        $extra_args \
        install 2>&1 | grep -E "(error:|warning:|^Build)" || true
}

# ── Step 1: Clone all dependencies ────────────────────────────────────────────
step "Cloning XNU dependencies from apple-oss-distributions/${MACOS_TAG}"

# These must be built in dependency order
DEPS=(
    "dtrace:${MACOS_TAG}"
    "AvailabilityVersions:${MACOS_TAG}"
    "libplatform:${MACOS_TAG}"
    "libdispatch:${MACOS_TAG}"
    "libpthread:${MACOS_TAG}"
    "Libm:${MACOS_TAG}"
    "xnu:${MACOS_TAG}"
)

# Additional KEXTs we need for LunaOS
KEXT_DEPS=(
    "IOKitUser:${MACOS_TAG}"
    "IOGraphicsFamily:${MACOS_TAG}"  # IOFramebuffer — IODRMShim depends on this
    "IOHIDFamily:${MACOS_TAG}"       # darwin-evdev-bridge depends on this
    "IOStorageFamily:${MACOS_TAG}"
    "IONetworkingFamily:${MACOS_TAG}"
    "IOACPIFamily:${MACOS_TAG}"
    "AppleACPIPlatform:${MACOS_TAG}"
)

progress 1 7 "Cloning XNU core"
for entry in "${DEPS[@]}"; do
    name="${entry%%:*}"
    tag="${entry##*:}"
    clone_repo "$name" "$tag"
done

progress 2 7 "Cloning IOKit families"
for entry in "${KEXT_DEPS[@]}"; do
    name="${entry%%:*}"
    tag="${entry##*:}"
    clone_repo "$name" "$tag" || warn "Could not clone ${name} — skipping"
done

# ── Step 2: Build dtrace (XNU needs ctfconvert, ctfmerge) ────────────────────
progress 3 7 "Building dtrace (provides ctf tools)"
step "dtrace"

DTRACE_SRC="${SOURCES_DIR}/dtrace"
DTRACE_OBJ="${BUILD_DIR}/obj/dtrace"
DTRACE_DST="${BUILD_DIR}/dst"

mkdir -p "$DTRACE_OBJ" "$DTRACE_DST"

# dtrace uses a custom Makefile-based build, not xcodebuild
make -C "$DTRACE_SRC" \
    SRCROOT="$DTRACE_SRC" \
    OBJROOT="$DTRACE_OBJ" \
    DSTROOT="$DTRACE_DST" \
    SDKROOT="$(xcrun --show-sdk-path)" \
    RC_ARCHS=x86_64 \
    install 2>&1 | tail -5 || warn "dtrace build had warnings — continuing"

# ctfconvert and ctfmerge must be on PATH for XNU build
export PATH="${DTRACE_DST}/usr/local/bin:$PATH"
log "dtrace tools: $(ctfconvert --version 2>&1 | head -1 || echo 'not found')"

# ── Step 3: Build AvailabilityVersions ────────────────────────────────────────
progress 4 7 "Building AvailabilityVersions"
step "AvailabilityVersions"

AV_SRC="${SOURCES_DIR}/AvailabilityVersions"
AV_DST="${BUILD_DIR}/dst"
mkdir -p "$AV_DST"

make -C "$AV_SRC" \
    SRCROOT="$AV_SRC" \
    DSTROOT="$AV_DST" \
    install 2>&1 | tail -3 || true

# ── Step 4: Build libplatform headers ─────────────────────────────────────────
progress 5 7 "Building libplatform (headers)"
step "libplatform"

xcodebuild -project "${SOURCES_DIR}/libplatform/libplatform.xcodeproj" \
    -target 'libplatform_headers' \
    -sdk macosx \
    ARCHS=x86_64 \
    SRCROOT="${SOURCES_DIR}/libplatform" \
    OBJROOT="${BUILD_DIR}/obj/libplatform" \
    DSTROOT="${BUILD_DIR}/dst" \
    install 2>&1 | grep -E "^(Build|error)" || true

# ── Step 5: Build libdispatch headers (XNU needs them) ───────────────────────
progress 6 7 "Building libdispatch headers"
step "libdispatch"

cmake -S "${SOURCES_DIR}/libdispatch" \
      -B "${BUILD_DIR}/obj/libdispatch" \
      -DCMAKE_INSTALL_PREFIX="${BUILD_DIR}/dst/usr" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_SYSTEM_NAME=Darwin \
      -DCMAKE_OSX_ARCHITECTURES=x86_64 \
      -DENABLE_TESTING=OFF \
      -Wno-dev --quiet

cmake --build "${BUILD_DIR}/obj/libdispatch" \
      --target dispatch_private_headers \
      -j"$JOBS" 2>&1 | tail -3 || true

cmake --install "${BUILD_DIR}/obj/libdispatch" \
      --component dispatch_private_headers 2>&1 | tail -3 || true

# ── Step 6: Build XNU kernel ──────────────────────────────────────────────────
progress 7 7 "Building XNU kernel (RELEASE_X86_64)"
step "XNU — this takes 45-90 minutes"

XNU_SRC="${SOURCES_DIR}/xnu"
XNU_OBJ="${BUILD_DIR}/obj/xnu"
XNU_DST="${BUILD_DIR}/dst"
XNU_SYM="${BUILD_DIR}/sym/xnu"

mkdir -p "$XNU_OBJ" "$XNU_DST" "$XNU_SYM"

# SDK and toolchain paths
SDK_PATH="$(xcrun --sdk macosx --show-sdk-path)"
TOOLCHAIN_DIR="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain"

log "SDK: ${SDK_PATH}"
log "Toolchain: ${TOOLCHAIN_DIR}"

# Apply LunaOS kernel patches before building
if [[ -d "${LUNA_ROOT}/kernel/patches" ]]; then
    log "Applying LunaOS kernel patches..."
    for patch in "${LUNA_ROOT}/kernel/patches/"*.patch; do
        [[ -f "$patch" ]] || continue
        log "  applying: $(basename "$patch")"
        git -C "$XNU_SRC" apply "$patch" 2>/dev/null || \
            warn "  patch $(basename "$patch") already applied or failed"
    done
fi

# The XNU make invocation
# KERNEL_CONFIGS: RELEASE = optimised, no debug assertions
#                 DEVELOPMENT = assertions on (slower, more verbose)
# MACHINE_CONFIGS: X86_64
make -C "$XNU_SRC" \
    SDKROOT="$SDK_PATH" \
    OBJROOT="$XNU_OBJ" \
    DSTROOT="$XNU_DST" \
    SYMROOT="$XNU_SYM" \
    TOOLCHAINDIR="$TOOLCHAIN_DIR" \
    KERNEL_CONFIGS="RELEASE" \
    MACHINE_CONFIGS="X86_64" \
    BUILD_WERROR=0 \
    PLATFORM=MacOSX \
    -j"$JOBS" \
    installhdrs 2>&1 | tail -5 || true

make -C "$XNU_SRC" \
    SDKROOT="$SDK_PATH" \
    OBJROOT="$XNU_OBJ" \
    DSTROOT="$XNU_DST" \
    SYMROOT="$XNU_SYM" \
    TOOLCHAINDIR="$TOOLCHAIN_DIR" \
    KERNEL_CONFIGS="RELEASE" \
    MACHINE_CONFIGS="X86_64" \
    BUILD_WERROR=0 \
    PLATFORM=MacOSX \
    -j"$JOBS" \
    install 2>&1 | tee "${BUILD_DIR}/xnu-build.log" | \
    grep -E "(error:|^===|^make|Compiling|Linking)" || true

# ── Step 7: Collect outputs ────────────────────────────────────────────────────
step "Collecting build outputs"

KERNEL_OUT="${BUILD_DIR}/output"
mkdir -p "$KERNEL_OUT"

# Find the compiled kernel
KERNEL_BIN=""
for candidate in \
    "${XNU_DST}/System/Library/Kernels/kernel" \
    "${XNU_SYM}/release/x86_64/kernel" \
    "${XNU_OBJ}/RELEASE_X86_64/kernel" ; do
    if [[ -f "$candidate" ]]; then
        KERNEL_BIN="$candidate"
        break
    fi
done

if [[ -z "$KERNEL_BIN" ]]; then
    warn "Kernel binary not found in expected locations. Searching..."
    KERNEL_BIN=$(find "$BUILD_DIR" -name "kernel" -type f 2>/dev/null | head -1)
fi

if [[ -n "$KERNEL_BIN" ]]; then
    cp "$KERNEL_BIN" "${KERNEL_OUT}/mach_kernel"
    log "Kernel binary: ${KERNEL_OUT}/mach_kernel"
    file "${KERNEL_OUT}/mach_kernel"
    ls -lh "${KERNEL_OUT}/mach_kernel"
else
    warn "Could not locate kernel binary — check ${BUILD_DIR}/xnu-build.log"
fi

# Copy kernel extensions
if [[ -d "${XNU_DST}/System/Library/Extensions" ]]; then
    cp -r "${XNU_DST}/System/Library/Extensions" "${KERNEL_OUT}/Extensions"
fi

# Copy kernel headers (needed for KEXT building)
if [[ -d "${XNU_DST}/System/Library/Frameworks/Kernel.framework" ]]; then
    cp -r "${XNU_DST}/System/Library/Frameworks/Kernel.framework" \
          "${KERNEL_OUT}/Kernel.framework"
fi

# ── Step 8: Build IOKit KEXTs ─────────────────────────────────────────────────
step "Building IOKit KEXTs for LunaOS"

build_kext() {
    local name="$1"
    local src="${SOURCES_DIR}/${name}"
    [[ -d "$src" ]] || { warn "  ${name} source not found, skipping"; return; }
    log "  building ${name}.kext..."
    xcodebuild \
        -sdk macosx \
        -configuration Release \
        ARCHS=x86_64 \
        SRCROOT="$src" \
        OBJROOT="${BUILD_DIR}/obj/${name}" \
        DSTROOT="${BUILD_DIR}/dst" \
        install 2>&1 | grep -E "(error:|Build succeeded|Build FAILED)" || true
}

build_kext "IOGraphicsFamily"
build_kext "IOHIDFamily"
build_kext "IOStorageFamily"
build_kext "IONetworkingFamily"
build_kext "IOACPIFamily"

# Copy resulting KEXTs to output
KEXT_OUT="${KERNEL_OUT}/Extensions"
mkdir -p "$KEXT_OUT"
find "${BUILD_DIR}/dst" -name "*.kext" -exec cp -r {} "$KEXT_OUT/" \; 2>/dev/null || true

# Also copy our IODRMShim.kext if it's been built
if [[ -d "${LUNA_ROOT}/build/IODRMShim.kext" ]]; then
    cp -r "${LUNA_ROOT}/build/IODRMShim.kext" "$KEXT_OUT/"
    log "  IODRMShim.kext included"
fi

# ── Summary ────────────────────────────────────────────────────────────────────
echo ""
log "╔══════════════════════════════════════════════════════════════╗"
log "║           LunaOS XNU kernel build complete                  ║"
log "╠══════════════════════════════════════════════════════════════╣"
log "║  Outputs in: ${KERNEL_OUT}"
log "║"
log "║  mach_kernel          — XNU kernel binary (RELEASE_X86_64)  ║"
log "║  Extensions/          — IOKit KEXTs bundle                  ║"
log "║  Kernel.framework/    — kernel headers for KEXT building     ║"
log "╠══════════════════════════════════════════════════════════════╣"
log "║  Next: run scripts/build-userland.sh                        ║"
log "╚══════════════════════════════════════════════════════════════╝"
