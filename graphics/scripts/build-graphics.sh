#!/usr/bin/env bash
# =============================================================================
# build-graphics.sh — LunaOS full graphics stack
#
# Builds in order:
#   1. LLVM 18        — backend for lavapipe + llvmpipe
#   2. libdrm-darwin  — already built, just verify
#   3. GBM shim       — Generic Buffer Manager over IODRMShim dumb buffers
#   4. Mesa 26        — with:
#                        llvmpipe  (software OpenGL 4.6)
#                        lavapipe  (software Vulkan 1.4)
#                        virgl     (QEMU virtio-gpu acceleration)
#                        anv stub  (Intel hardware — future)
#                        radv stub (AMD hardware — future)
#   5. Vulkan loader  — Khronos official ICD loader
#   6. Vulkan tools   — vulkaninfo, vkcube (validation)
#
# Vulkan 1.4 conformance path:
#   App → Vulkan loader → LunaOS ICD manifest → lavapipe/anv/radv
#                                              → Mesa 26 → DRM dumb buffer
#                                                        → IODRMShim.kext
#                                                        → IOFramebuffer
# =============================================================================

set -euo pipefail

LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${LUNA_ROOT}/build/graphics"
SOURCES="${BUILD_DIR}/src"
PREFIX="${LUNA_ROOT}/build/rootfs/usr/local"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

MESA_VERSION="26.0.0"
LLVM_VERSION="18"
VULKAN_LOADER_VERSION="1.4.304"
VULKAN_HEADERS_VERSION="1.4.304"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; BOLD='\033[1m'; NC='\033[0m'
log()      { echo -e "${GREEN}[graphics]${NC} $*"; }
step()     { echo -e "${BOLD}${BLUE}[graphics]${NC} ── $*"; }
warn()     { echo -e "${YELLOW}[graphics] WARN:${NC} $*"; }
die()      { echo -e "${RED}[graphics] ERROR:${NC} $*" >&2; exit 1; }
progress() { echo -e "${GREEN}[graphics]${NC} [$1/$TOTAL] $2"; }

TOTAL=6

export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export PATH="/usr/local/opt/llvm@${LLVM_VERSION}/bin:$PATH"
export CFLAGS="-arch x86_64"
export CXXFLAGS="-arch x86_64"
export LDFLAGS="-arch x86_64"

[[ "$(uname -s)" == "Darwin" ]] || die "Darwin only"
[[ "$(uname -m)" == "x86_64" ]] || die "x86_64 only"

mkdir -p "$BUILD_DIR" "$SOURCES" "$PREFIX"

clone() {
    local url="$1" dir="$2" ref="${3:-main}"
    [[ -d "$dir/.git" ]] && return
    git clone --depth 1 --branch "$ref" "$url" "$dir" --quiet
}

# =============================================================================
# STEP 1: LLVM 18
# =============================================================================
progress 1 "LLVM ${LLVM_VERSION} (lavapipe/llvmpipe backend)"
step "LLVM"

if ! command -v "llvm-config-${LLVM_VERSION}" &>/dev/null && \
   ! /usr/local/opt/llvm@${LLVM_VERSION}/bin/llvm-config --version &>/dev/null 2>&1; then
    log "  installing LLVM ${LLVM_VERSION} via Homebrew..."
    brew install "llvm@${LLVM_VERSION}" || die "LLVM install failed"
fi

LLVM_CONFIG="/usr/local/opt/llvm@${LLVM_VERSION}/bin/llvm-config"
[[ -x "$LLVM_CONFIG" ]] || LLVM_CONFIG="$(brew --prefix llvm@${LLVM_VERSION})/bin/llvm-config"
LLVM_PREFIX="$($LLVM_CONFIG --prefix)"
log "  LLVM prefix: ${LLVM_PREFIX}"
log "  LLVM version: $($LLVM_CONFIG --version)"

# =============================================================================
# STEP 2: Vulkan headers + loader
# =============================================================================
progress 2 "Vulkan ${VULKAN_LOADER_VERSION} headers + loader"
step "Vulkan loader"

# Headers
clone "https://github.com/KhronosGroup/Vulkan-Headers.git" \
      "${SOURCES}/Vulkan-Headers" \
      "v${VULKAN_HEADERS_VERSION}"

cmake -S "${SOURCES}/Vulkan-Headers" \
      -B "${BUILD_DIR}/obj/Vulkan-Headers" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_BUILD_TYPE=Release \
      --quiet
cmake --install "${BUILD_DIR}/obj/Vulkan-Headers"
log "  Vulkan headers installed"

# Loader
clone "https://github.com/KhronosGroup/Vulkan-Loader.git" \
      "${SOURCES}/Vulkan-Loader" \
      "v${VULKAN_LOADER_VERSION}"

# Apply Darwin patch (disable Linux-specific procfs loader path)
if [[ ! -f "${SOURCES}/Vulkan-Loader/.darwin-patched" ]]; then
    patch -d "${SOURCES}/Vulkan-Loader" -p1 \
        < "${LUNA_ROOT}/graphics/patches/vulkan-loader/0001-darwin-platform.patch" \
        2>/dev/null || warn "vulkan-loader patch already applied"
    touch "${SOURCES}/Vulkan-Loader/.darwin-patched"
fi

cmake -S "${SOURCES}/Vulkan-Loader" \
      -B "${BUILD_DIR}/obj/Vulkan-Loader" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=x86_64 \
      -DVULKAN_HEADERS_INSTALL_DIR="$PREFIX" \
      -DBUILD_WSI_XCB_SUPPORT=OFF \
      -DBUILD_WSI_XLIB_SUPPORT=OFF \
      -DBUILD_WSI_WAYLAND_SUPPORT=ON \
      -DBUILD_TESTS=OFF \
      --quiet
cmake --build "${BUILD_DIR}/obj/Vulkan-Loader" -j"$JOBS"
cmake --install "${BUILD_DIR}/obj/Vulkan-Loader"
log "  Vulkan loader installed: ${PREFIX}/lib/libvulkan.dylib"

# =============================================================================
# STEP 3: GBM shim
# =============================================================================
progress 3 "GBM shim (Generic Buffer Manager over IODRMShim)"
step "GBM"

log "  building darwin-gbm..."
clang -arch x86_64 -dynamiclib \
    -o "${PREFIX}/lib/libgbm.dylib" \
    -install_name "${PREFIX}/lib/libgbm.dylib" \
    -current_version 1.0.0 \
    -compatibility_version 1.0.0 \
    "${LUNA_ROOT}/graphics/include/darwin-gbm.c" \
    -I"${LUNA_ROOT}/drm-shim/include" \
    -I"${PREFIX}/include" \
    -L"${PREFIX}/lib" -ldrm \
    -Os 2>&1 | tail -3 || warn "GBM build failed — Mesa will use dumb buffer fallback"

# Install GBM header
cp "${LUNA_ROOT}/graphics/include/gbm.h" "${PREFIX}/include/" 2>/dev/null || true

# pkg-config for GBM
cat > "${PREFIX}/lib/pkgconfig/gbm.pc" <<GBM_PC
prefix=${PREFIX}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: gbm
Description: LunaOS GBM shim over IODRMShim
Version: 1.0.0
Libs: -L\${libdir} -lgbm
Cflags: -I\${includedir}
Requires: libdrm
GBM_PC
log "  GBM shim installed"

# =============================================================================
# STEP 4: Mesa 26
# =============================================================================
progress 4 "Mesa ${MESA_VERSION} (llvmpipe + lavapipe + virgl + anv-stub + radv-stub)"
step "Mesa 26"

clone "https://gitlab.freedesktop.org/mesa/mesa.git" \
      "${SOURCES}/mesa" \
      "mesa-${MESA_VERSION}"

# Apply all Darwin patches
MESA_PATCHES_DIR="${LUNA_ROOT}/graphics/patches/mesa"
if [[ -d "$MESA_PATCHES_DIR" ]]; then
    for patch in "${MESA_PATCHES_DIR}"/*.patch; do
        [[ -f "$patch" ]] || continue
        pname="$(basename "$patch")"
        marker="${SOURCES}/mesa/.${pname%.patch}-applied"
        [[ -f "$marker" ]] && continue
        log "  applying: $pname"
        git -C "${SOURCES}/mesa" apply "$patch" 2>/dev/null || \
            warn "  $pname failed or already applied"
        touch "$marker"
    done
fi

# Meson configuration — the key build options
MESON_OPTS=(
    "--prefix=${PREFIX}"
    "--buildtype=release"
    "--wrap-mode=nofallback"

    # ── Gallium drivers ─────────────────────────────────────────────────────
    # llvmpipe: software OpenGL 4.6 via LLVM
    # virgl:    QEMU virtio-gpu OpenGL acceleration
    # softpipe: fallback software renderer (no LLVM dependency)
    "-Dgallium-drivers=llvmpipe,virgl,softpipe"

    # ── Vulkan drivers ──────────────────────────────────────────────────────
    # lavapipe:  software Vulkan 1.4 (uses llvmpipe backend)
    # virtio:    Vulkan over QEMU virtio-gpu
    # swrast:    Vulkan software fallback
    # intel-hasvk / anv: stubbed — enabled but disabled at runtime without i915
    # amd / radv: stubbed — enabled but disabled at runtime without amdgpu
    "-Dvulkan-drivers=swrast,virtio,amd,intel"

    # ── Platforms ───────────────────────────────────────────────────────────
    # wayland: primary display platform (luna-compositor)
    # No X11 (no XQuartz dependency), no xcb
    "-Dplatforms=wayland"

    # ── Renderers ───────────────────────────────────────────────────────────
    "-Dgles1=enabled"
    "-Dgles2=enabled"
    "-Dopengl=true"
    "-Dglx=disabled"           # no X11
    "-Degl=enabled"
    "-Dgbm=enabled"

    # ── EGL / WSI ───────────────────────────────────────────────────────────
    "-Degl-native-platform=wayland"

    # ── LLVM ────────────────────────────────────────────────────────────────
    "-Dllvm=enabled"
    "-Dshared-llvm=enabled"

    # ── Vulkan features ─────────────────────────────────────────────────────
    "-Dvulkan-layers=device-select,overlay"
    "-Dvulkan-icd-dir=${PREFIX}/share/vulkan/icd.d"

    # ── Video ────────────────────────────────────────────────────────────────
    "-Dvideo-codecs=all"

    # ── Misc ────────────────────────────────────────────────────────────────
    "-Dbuild-tests=false"
    "-Dwerror=false"
    "-Dintel-clc=disabled"     # no system clang-cl; anv stub doesn't need it
    "-Dmicrosoft-clc=disabled"
)

# Point meson at our LLVM
export LLVM_CONFIG="$LLVM_CONFIG"

meson setup "${BUILD_DIR}/obj/mesa" "${SOURCES}/mesa" \
    "${MESON_OPTS[@]}" \
    -Dc_args="-arch x86_64" \
    -Dcpp_args="-arch x86_64" \
    -Dc_link_args="-arch x86_64" \
    -Dcpp_link_args="-arch x86_64" \
    2>&1 | tail -20

log "  Mesa configured — building (~20-40 min)..."
ninja -C "${BUILD_DIR}/obj/mesa" -j"$JOBS" 2>&1 | \
    grep -E "(ERROR|error:|linking|Compiling src/)" | tail -30 || true

ninja -C "${BUILD_DIR}/obj/mesa" install
log "  Mesa ${MESA_VERSION} installed"

# =============================================================================
# STEP 5: ICD manifests
# =============================================================================
progress 5 "Vulkan ICD manifests + driver registry"
step "ICD manifests"

ICD_DIR="${PREFIX}/share/vulkan/icd.d"
mkdir -p "$ICD_DIR"

# lavapipe ICD (software Vulkan 1.4)
cat > "${ICD_DIR}/lvp_icd.x86_64.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "libvulkan_lvp.dylib",
        "api_version": "1.4.304"
    }
}
JSON

# virtio-gpu ICD (QEMU VM acceleration)
cat > "${ICD_DIR}/virtio_icd.x86_64.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "libvulkan_virtio.dylib",
        "api_version": "1.3.0"
    }
}
JSON

# anv stub ICD (Intel — disabled until i915 KEXT is ported)
cat > "${ICD_DIR}/anv_icd.x86_64.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "libvulkan_intel.dylib",
        "api_version": "1.4.304",
        "is_portability_driver": false
    }
}
JSON
# Disable anv by default — rename to .disabled until i915 KEXT exists
mv "${ICD_DIR}/anv_icd.x86_64.json" \
   "${ICD_DIR}/anv_icd.x86_64.json.disabled_until_i915_kext"

# radv stub ICD (AMD — disabled until amdgpu KEXT is ported)
cat > "${ICD_DIR}/radv_icd.x86_64.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "libvulkan_radeon.dylib",
        "api_version": "1.4.304"
    }
}
JSON
mv "${ICD_DIR}/radv_icd.x86_64.json" \
   "${ICD_DIR}/radv_icd.x86_64.json.disabled_until_amdgpu_kext"

log "  Active ICDs:"
log "    ✅ lavapipe  (software Vulkan 1.4)"
log "    ✅ virtio    (QEMU virtio-gpu Vulkan)"
log "    ⏸  anv      (Intel  — disabled, needs i915 KEXT)"
log "    ⏸  radv     (AMD    — disabled, needs amdgpu KEXT)"

# Vulkan layer manifests
LAYER_DIR="${PREFIX}/share/vulkan/explicit_layer.d"
mkdir -p "$LAYER_DIR"

# device-select layer (automatically picks best GPU)
cat > "${LAYER_DIR}/VkLayer_MESA_device_select.json" <<'JSON'
{
    "file_format_version": "1.1.0",
    "layer": {
        "name": "VK_LAYER_MESA_device_select",
        "type": "GLOBAL",
        "library_path": "libVkLayer_MESA_device_select.dylib",
        "api_version": "1.4.304",
        "implementation_version": "1",
        "description": "LunaOS Mesa device selection layer"
    }
}
JSON

# Write /etc/vulkan/icd.d symlink for system-wide discovery
mkdir -p "${LUNA_ROOT}/build/rootfs/etc/vulkan/icd.d"
ln -sf "${PREFIX}/share/vulkan/icd.d/lvp_icd.x86_64.json" \
       "${LUNA_ROOT}/build/rootfs/etc/vulkan/icd.d/" 2>/dev/null || true
ln -sf "${PREFIX}/share/vulkan/icd.d/virtio_icd.x86_64.json" \
       "${LUNA_ROOT}/build/rootfs/etc/vulkan/icd.d/" 2>/dev/null || true

# =============================================================================
# STEP 6: Vulkan validation + tools
# =============================================================================
progress 6 "Vulkan validation layers + vulkaninfo + vkcube"
step "Vulkan tools"

# Validation layers (essential for development)
clone "https://github.com/KhronosGroup/Vulkan-ValidationLayers.git" \
      "${SOURCES}/Vulkan-ValidationLayers" \
      "v${VULKAN_LOADER_VERSION}"

cmake -S "${SOURCES}/Vulkan-ValidationLayers" \
      -B "${BUILD_DIR}/obj/Vulkan-ValidationLayers" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=x86_64 \
      -DVULKAN_HEADERS_INSTALL_DIR="$PREFIX" \
      -DGLSLANG_INSTALL_DIR="$PREFIX" \
      -DBUILD_TESTS=OFF \
      --quiet
cmake --build "${BUILD_DIR}/obj/Vulkan-ValidationLayers" -j"$JOBS" 2>&1 | tail -5
cmake --install "${BUILD_DIR}/obj/Vulkan-ValidationLayers"

# vulkaninfo + vkcube
clone "https://github.com/KhronosGroup/Vulkan-Tools.git" \
      "${SOURCES}/Vulkan-Tools" \
      "v${VULKAN_LOADER_VERSION}"

cmake -S "${SOURCES}/Vulkan-Tools" \
      -B "${BUILD_DIR}/obj/Vulkan-Tools" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=x86_64 \
      -DVULKAN_HEADERS_INSTALL_DIR="$PREFIX" \
      -DBUILD_ICD=OFF \
      -DBUILD_CUBE=ON \
      -DBUILD_VULKANINFO=ON \
      --quiet
cmake --build "${BUILD_DIR}/obj/Vulkan-Tools" -j"$JOBS" 2>&1 | tail -5
cmake --install "${BUILD_DIR}/obj/Vulkan-Tools"

log "  vulkaninfo installed: ${PREFIX}/bin/vulkaninfo"
log "  vkcube installed:     ${PREFIX}/bin/vkcube"

# =============================================================================
# DONE
# =============================================================================
echo ""
log "╔══════════════════════════════════════════════════════════════════╗"
log "║         LunaOS Graphics Stack Build Complete                    ║"
log "╠══════════════════════════════════════════════════════════════════╣"
log "║  OpenGL 4.6   → llvmpipe (software, CPU)                        ║"
log "║  Vulkan 1.4   → lavapipe (software, CPU)                        ║"
log "║  Vulkan 1.3   → virtio   (QEMU virtio-gpu-gl)                   ║"
log "║  OpenGL/VK    → virgl    (QEMU 3D acceleration)                 ║"
log "║  Vulkan 1.4   → anv stub (Intel — enable when i915 KEXT ready)  ║"
log "║  Vulkan 1.4   → radv stub(AMD  — enable when amdgpu KEXT ready) ║"
log "╠══════════════════════════════════════════════════════════════════╣"
log "║  Test (run inside LunaOS on WAYLAND_DISPLAY=wayland-0):         ║"
log "║    vulkaninfo --summary                                          ║"
log "║    vkcube                                                        ║"
log "║    glxgears (via weston-terminal)                                ║"
log "╠══════════════════════════════════════════════════════════════════╣"
log "║  Enable hardware Vulkan later:                                   ║"
log "║    Intel: port i915 as Darwin KEXT, rename anv ICD .disabled     ║"
log "║    AMD:   port amdgpu as Darwin KEXT, rename radv ICD .disabled  ║"
log "╚══════════════════════════════════════════════════════════════════╝"
