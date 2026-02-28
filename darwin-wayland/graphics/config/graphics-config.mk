# =============================================================================
# graphics-config.mk — LunaOS graphics stack build configuration
# =============================================================================

MESA_VERSION           := 26.0.0
LLVM_VERSION           := 18
VULKAN_LOADER_VERSION  := 1.4.304
VULKAN_HEADERS_VERSION := 1.4.304

# ── Driver matrix ─────────────────────────────────────────────────────────────
#
# Gallium drivers (OpenGL + OpenCL + VAAPI):
#   llvmpipe  — software OpenGL 4.6 via LLVM (always active)
#   virgl     — QEMU virtio-gpu OpenGL (active in VMs with virtio-vga-gl)
#   softpipe  — pure software fallback (no LLVM, slower than llvmpipe)
#
# Vulkan drivers:
#   lavapipe  — software Vulkan 1.4 via llvmpipe   [ACTIVE]
#   virtio    — QEMU virtio-gpu Vulkan             [ACTIVE in VM]
#   anv       — Intel hardware Vulkan              [STUB — needs i915 KEXT]
#   radv      — AMD hardware Vulkan                [STUB — needs amdgpu KEXT]
#
GALLIUM_DRIVERS := llvmpipe,virgl,softpipe
VULKAN_DRIVERS  := swrast,virtio,amd,intel

# ── Platforms ─────────────────────────────────────────────────────────────────
# Wayland only — no X11 on LunaOS
MESA_PLATFORMS  := wayland

# ── EGL configuration ─────────────────────────────────────────────────────────
EGL_NATIVE_PLATFORM := wayland

# ── LLVM path ─────────────────────────────────────────────────────────────────
LLVM_PREFIX  := /usr/local/opt/llvm@$(LLVM_VERSION)
LLVM_CONFIG  := $(LLVM_PREFIX)/bin/llvm-config

# ── Install prefix ─────────────────────────────────────────────────────────────
GRAPHICS_PREFIX := $(LUNA_ROOT)/build/rootfs/usr/local

# ── Enabling hardware Vulkan (future) ─────────────────────────────────────────
#
# Intel (anv):
#   1. Port i915 kernel driver to Darwin as IOKit KEXT
#      - Reference: linux/drivers/gpu/drm/i915
#      - IODRMShim already provides the DRM ioctl layer
#      - Need: i915-specific GEM, GuC firmware loading, display engine
#   2. Rename: anv_icd.x86_64.json.disabled_until_i915_kext
#            → anv_icd.x86_64.json
#   3. Done — Vulkan loader picks it up automatically
#
# AMD (radv):
#   1. Port amdgpu kernel driver to Darwin as IOKit KEXT
#      - Reference: FreeBSD drm-kmod (significant prior art for BSD kernels)
#      - Reference: linux/drivers/gpu/drm/amd
#   2. Rename: radv_icd.x86_64.json.disabled_until_amdgpu_kext
#            → radv_icd.x86_64.json
#   3. Done — Vulkan loader picks it up automatically
#
# QEMU virtio-gpu (virgl/virtio — works NOW):
#   Run QEMU with: -device virtio-vga-gl -display sdl,gl=on
#   The virtio ICD is already active — no changes needed.
