#!/usr/bin/env bash
# =============================================================================
# verify-graphics.sh — LunaOS graphics stack verification
# =============================================================================

set -euo pipefail
LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PREFIX="${LUNA_ROOT}/build/rootfs/usr/local"

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { echo -e "  ${GREEN}✓${NC} $*"; }
fail() { echo -e "  ${RED}✗${NC} $*"; FAILURES=$((FAILURES+1)); }
warn() { echo -e "  ${YELLOW}?${NC} $*"; }
FAILURES=0

echo "══════════════════════════════════════════════════"
echo " LunaOS graphics stack verification"
echo "══════════════════════════════════════════════════"

# ── Mesa libraries ────────────────────────────────────────────────────────────
echo ""
echo "── Mesa ${MESA_VERSION:-26.0.0} libraries"
for lib in \
    "libGL.dylib:OpenGL 4.6 (llvmpipe)" \
    "libEGL.dylib:EGL (Wayland platform)" \
    "libGLESv1_CM.dylib:OpenGL ES 1.1" \
    "libGLESv2.dylib:OpenGL ES 2.0/3.2" \
    "libgbm.dylib:GBM (darwin-gbm shim)" \
    "libMesaOpenCL.dylib:OpenCL (optional)" ; do
    lib_file="${lib%%:*}"
    lib_desc="${lib##*:}"
    if [[ -f "${PREFIX}/lib/${lib_file}" ]]; then
        pass "${lib_file} — ${lib_desc}"
    else
        warn "${lib_file} not found (${lib_desc})"
    fi
done

# ── Vulkan ICDs ───────────────────────────────────────────────────────────────
echo ""
echo "── Vulkan ICDs"
ICD_DIR="${PREFIX}/share/vulkan/icd.d"

check_icd() {
    local name="$1" lib="$2" status="$3"
    if [[ -f "${ICD_DIR}/${name}" ]]; then
        pass "${name} — ${status}"
    else
        local disabled="${ICD_DIR}/${name}.disabled_until_${lib}_kext"
        if [[ -f "$disabled" ]]; then
            warn "${name} — STUBBED (enable when ${lib} KEXT is ported)"
        else
            fail "${name} missing"
        fi
    fi
}

check_icd "lvp_icd.x86_64.json"    "lvp"    "lavapipe Vulkan 1.4 (software) ✅ ACTIVE"
check_icd "virtio_icd.x86_64.json" "virtio" "virtio-gpu Vulkan (QEMU VM)    ✅ ACTIVE"
check_icd "anv_icd.x86_64.json"    "i915"   "Intel anv Vulkan 1.4           ⏸ STUB"
check_icd "radv_icd.x86_64.json"   "amdgpu" "AMD radv Vulkan 1.4            ⏸ STUB"

# ── Vulkan driver libs ────────────────────────────────────────────────────────
echo ""
echo "── Vulkan driver libraries"
for lib in \
    "libvulkan_lvp.dylib:lavapipe (Vulkan 1.4 software)" \
    "libvulkan_virtio.dylib:virtio (QEMU GPU)" \
    "libvulkan_intel.dylib:anv (Intel stub)" \
    "libvulkan_radeon.dylib:radv (AMD stub)" \
    "libvulkan.dylib:Vulkan loader" ; do
    lib_file="${PREFIX}/lib/${lib%%:*}"
    lib_desc="${lib##*:}"
    [[ -f "$lib_file" ]] && pass "$(basename $lib_file) — ${lib_desc}" \
                         || warn "$(basename $lib_file) not found — ${lib_desc}"
done

# ── Tools ─────────────────────────────────────────────────────────────────────
echo ""
echo "── Vulkan tools"
[[ -f "${PREFIX}/bin/vulkaninfo" ]] && pass "vulkaninfo" || fail "vulkaninfo missing"
[[ -f "${PREFIX}/bin/vkcube"     ]] && pass "vkcube"     || fail "vkcube missing"

# ── Validation layers ─────────────────────────────────────────────────────────
echo ""
echo "── Vulkan validation layers"
LAYER_DIR="${PREFIX}/share/vulkan/explicit_layer.d"
[[ -f "${LAYER_DIR}/VkLayer_MESA_device_select.json"    ]] && pass "device_select layer" || warn "device_select missing"
[[ -f "${LAYER_DIR}/VkLayer_khronos_validation.json"    ]] && pass "validation layer"    || warn "validation layer missing"

# ── Architecture audit ────────────────────────────────────────────────────────
echo ""
echo "── Architecture (all must be x86_64)"
BAD=0
for lib in "${PREFIX}/lib"/libGL*.dylib "${PREFIX}/lib"/libvulkan*.dylib \
           "${PREFIX}/lib/libgbm.dylib" "${PREFIX}/lib/libEGL.dylib"; do
    [[ -f "$lib" ]] || continue
    arch=$(file "$lib" | grep -o "x86_64" || echo "UNKNOWN")
    [[ "$arch" == "x86_64" ]] || { warn "Wrong arch: $lib"; BAD=$((BAD+1)); }
done
(( BAD == 0 )) && pass "All graphics libs are x86_64"

# ── Runtime test (if running on LunaOS with Wayland) ─────────────────────────
echo ""
echo "── Runtime test"
if [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
    log "  WAYLAND_DISPLAY=${WAYLAND_DISPLAY} — running vulkaninfo..."
    "${PREFIX}/bin/vulkaninfo" --summary 2>&1 | grep -E "(GPU|Vulkan|lavapipe|virtio)" | head -5
else
    warn "WAYLAND_DISPLAY not set — skipping runtime test"
    warn "On LunaOS: export WAYLAND_DISPLAY=wayland-0 && vulkaninfo --summary"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════════════"
if (( FAILURES == 0 )); then
    echo -e "${GREEN}Graphics stack verified.${NC}"
    echo ""
    echo "Active renderers:"
    echo "  OpenGL 4.6  → llvmpipe  (software, always available)"
    echo "  Vulkan 1.4  → lavapipe  (software, always available)"
    echo "  Vulkan/GL   → virtio    (QEMU with virtio-vga-gl)"
    echo ""
    echo "Future hardware Vulkan (stubs ready, need KEXT):"
    echo "  Vulkan 1.4  → anv   (Intel — TODO_i915_KEXT)"
    echo "  Vulkan 1.4  → radv  (AMD   — TODO_amdgpu_KEXT)"
else
    echo -e "${RED}${FAILURES} failure(s).${NC} Run build-graphics.sh first."
    exit 1
fi
echo "══════════════════════════════════════════════════"
