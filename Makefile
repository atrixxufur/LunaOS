# =============================================================================
# Makefile — LunaOS top-level build orchestrator
#
# Usage:
#   make all          — build everything in order
#   make kernel       — XNU kernel only
#   make compositor   — Wayland compositor only
#   make graphics     — Mesa 26 + Vulkan 1.4
#   make shell        — desktop shell only
#   make apps         — terminal + browser + file manager
#   make userland     — PureDarwin userland assembly
#   make iso          — package everything into LunaOS-0.1.0-x86_64.iso
#   make verify       — run all verification scripts
#   make qemu         — boot the ISO in QEMU (requires qemu-system-x86_64)
#   make clean        — remove all build artifacts
#   make status       — show what's built and what's missing
# =============================================================================

LUNA_ROOT := $(shell pwd)
JOBS      ?= $(shell sysctl -n hw.ncpu)
VERSION   := 0.1.0

BUILD_DIR    := $(LUNA_ROOT)/build
ROOTFS       := $(BUILD_DIR)/rootfs
ISO_OUTPUT   := $(BUILD_DIR)/LunaOS-$(VERSION)-x86_64.iso

export LUNA_ROOT JOBS BUILD_DIR ROOTFS

# ── Colors ────────────────────────────────────────────────────────────────────
RED    := \033[0;31m
GREEN  := \033[0;32m
YELLOW := \033[1;33m
BLUE   := \033[0;34m
BOLD   := \033[1m
NC     := \033[0m

define log
	@echo -e "$(GREEN)[lunaos]$(NC) $(1)"
endef
define step
	@echo -e "$(BOLD)$(BLUE)━━━ $(1) ━━━$(NC)"
endef

# ── Top-level targets ─────────────────────────────────────────────────────────

.PHONY: all kernel compositor graphics shell apps userland iso verify qemu clean status help

all: kernel compositor graphics shell apps userland iso
	$(call step,LunaOS $(VERSION) build complete)
	@echo -e "$(GREEN)ISO: $(ISO_OUTPUT)$(NC)"
	@echo "Run: make qemu"

# ── 1. XNU Kernel ─────────────────────────────────────────────────────────────
kernel: $(BUILD_DIR)/kernel/output/mach_kernel

$(BUILD_DIR)/kernel/output/mach_kernel:
	$(call step,Building XNU kernel)
	bash $(LUNA_ROOT)/kernel/scripts/build-xnu.sh
	bash $(LUNA_ROOT)/kernel/scripts/verify-kernel.sh
	$(call log,Kernel OK)

# ── 2. Darwin-Wayland glue layer ──────────────────────────────────────────────
glue: $(ROOTFS)/usr/local/lib/libdrm.dylib

$(ROOTFS)/usr/local/lib/libdrm.dylib:
	$(call step,Building darwin-wayland glue)
	cmake -B $(BUILD_DIR)/obj/darwin-wayland \
	      -S $(LUNA_ROOT) \
	      -DCMAKE_INSTALL_PREFIX=$(ROOTFS)/usr/local \
	      -DCMAKE_BUILD_TYPE=Release \
	      -DCMAKE_OSX_ARCHITECTURES=x86_64 \
	      --quiet
	cmake --build $(BUILD_DIR)/obj/darwin-wayland -j$(JOBS)
	cmake --install $(BUILD_DIR)/obj/darwin-wayland
	$(call log,Glue layer installed)

# ── 3. Wayland compositor ─────────────────────────────────────────────────────
compositor: glue $(ROOTFS)/usr/local/bin/luna-compositor

$(ROOTFS)/usr/local/bin/luna-compositor:
	$(call step,Building Wayland compositor stack)
	BUILD_DIR=$(BUILD_DIR)/compositor-deps \
	PREFIX=$(ROOTFS)/usr/local \
	JOBS=$(JOBS) \
	bash $(LUNA_ROOT)/scripts/build-compositor.sh
	$(call log,Compositor OK)

# ── 4. Mesa 26 + Vulkan 1.4 ───────────────────────────────────────────────────
graphics: compositor $(ROOTFS)/usr/local/lib/libvulkan_lvp.dylib

$(ROOTFS)/usr/local/lib/libvulkan_lvp.dylib:
	$(call step,Building Mesa 26 + Vulkan 1.4)
	JOBS=$(JOBS) bash $(LUNA_ROOT)/graphics/scripts/build-graphics.sh
	bash $(LUNA_ROOT)/graphics/scripts/verify-graphics.sh
	$(call log,Graphics stack OK)

# ── 5. Desktop shell ──────────────────────────────────────────────────────────
shell: compositor $(ROOTFS)/usr/local/bin/luna-shell

$(ROOTFS)/usr/local/bin/luna-shell:
	$(call step,Building desktop shell)
	# Download wlr protocol XMLs needed by wayland-scanner
	@mkdir -p $(LUNA_ROOT)/shell/protocols
	@if [ ! -f $(LUNA_ROOT)/shell/protocols/wlr-layer-shell-unstable-v1.xml ]; then \
	    curl -sL https://raw.githubusercontent.com/swaywm/wlr-protocols/master/unstable/wlr-layer-shell-unstable-v1.xml \
	         -o $(LUNA_ROOT)/shell/protocols/wlr-layer-shell-unstable-v1.xml; \
	fi
	@if [ ! -f $(LUNA_ROOT)/shell/protocols/wlr-foreign-toplevel-management-unstable-v1.xml ]; then \
	    curl -sL https://raw.githubusercontent.com/swaywm/wlr-protocols/master/unstable/wlr-foreign-toplevel-management-unstable-v1.xml \
	         -o $(LUNA_ROOT)/shell/protocols/wlr-foreign-toplevel-management-unstable-v1.xml; \
	fi
	cmake -B $(BUILD_DIR)/obj/luna-shell \
	      -S $(LUNA_ROOT)/shell \
	      -DCMAKE_INSTALL_PREFIX=$(ROOTFS)/usr/local \
	      -DCMAKE_BUILD_TYPE=Release \
	      -DCMAKE_OSX_ARCHITECTURES=x86_64 \
	      --quiet
	cmake --build $(BUILD_DIR)/obj/luna-shell -j$(JOBS)
	cmake --install $(BUILD_DIR)/obj/luna-shell
	$(call log,Shell installed: $(ROOTFS)/usr/local/bin/luna-shell)

# ── 6. Apps ───────────────────────────────────────────────────────────────────
apps: $(ROOTFS)/usr/local/bin/foot \
      $(ROOTFS)/usr/local/bin/luna-files \
      $(ROOTFS)/usr/local/bin/luna-editor

$(ROOTFS)/usr/local/bin/foot:
	$(call step,Building foot terminal emulator)
	JOBS=$(JOBS) bash $(LUNA_ROOT)/apps/terminal/build-foot.sh
	$(call log,foot terminal OK)

$(ROOTFS)/usr/local/bin/luna-files:
	$(call step,Building luna-files file manager)
	cmake -B $(BUILD_DIR)/obj/luna-files \
	      -S $(LUNA_ROOT)/apps/files \
	      -DCMAKE_INSTALL_PREFIX=$(ROOTFS)/usr/local \
	      -DCMAKE_BUILD_TYPE=Release --quiet
	cmake --build $(BUILD_DIR)/obj/luna-files -j$(JOBS)
	cmake --install $(BUILD_DIR)/obj/luna-files

$(ROOTFS)/usr/local/bin/luna-editor:
	$(call step,Building luna-editor text editor)
	cmake -B $(BUILD_DIR)/obj/luna-editor \
	      -S $(LUNA_ROOT)/apps/text-editor \
	      -DCMAKE_INSTALL_PREFIX=$(ROOTFS)/usr/local \
	      -DCMAKE_BUILD_TYPE=Release --quiet
	cmake --build $(BUILD_DIR)/obj/luna-editor -j$(JOBS)
	cmake --install $(BUILD_DIR)/obj/luna-editor

# Install .desktop files for the launcher
	@mkdir -p $(ROOTFS)/usr/local/share/applications
	@cp $(LUNA_ROOT)/apps/*/app.desktop \
	    $(ROOTFS)/usr/local/share/applications/ 2>/dev/null || true
	$(call log,Apps installed)

# ── 7. Userland assembly ──────────────────────────────────────────────────────
userland: kernel glue compositor graphics shell apps
	$(call step,Assembling PureDarwin userland)
	JOBS=$(JOBS) bash $(LUNA_ROOT)/userland/scripts/build-userland.sh --skip-wayland
	bash $(LUNA_ROOT)/userland/scripts/verify-rootfs.sh
	$(call log,Rootfs assembled and verified)

# ── 8. ISO ────────────────────────────────────────────────────────────────────
iso: userland
	$(call step,Packaging ISO)
	bash $(LUNA_ROOT)/userland/scripts/build-iso.sh
	@ls -lh $(ISO_OUTPUT)
	$(call log,ISO ready: $(ISO_OUTPUT))

# ── Verify all components ─────────────────────────────────────────────────────
verify:
	$(call step,Verification)
	@bash $(LUNA_ROOT)/kernel/scripts/verify-kernel.sh     && echo "kernel: OK"     || echo "kernel: FAIL"
	@bash $(LUNA_ROOT)/graphics/scripts/verify-graphics.sh && echo "graphics: OK"   || echo "graphics: FAIL"
	@bash $(LUNA_ROOT)/userland/scripts/verify-rootfs.sh   && echo "rootfs: OK"     || echo "rootfs: FAIL"

# ── QEMU boot ─────────────────────────────────────────────────────────────────
qemu: iso
	$(call step,Booting LunaOS in QEMU)
	@command -v qemu-system-x86_64 >/dev/null || \
	    (echo "Install QEMU: brew install qemu" && exit 1)
	qemu-system-x86_64 \
	    -name "LunaOS $(VERSION)" \
	    -m 2048 \
	    -smp $(shell sysctl -n hw.ncpu) \
	    -cdrom $(ISO_OUTPUT) \
	    -boot d \
	    -device virtio-vga-gl \
	    -display sdl,gl=on \
	    -device virtio-tablet-pci \
	    -device virtio-keyboard-pci \
	    -device e1000,netdev=net0 \
	    -netdev user,id=net0 \
	    -enable-kvm 2>/dev/null || \
	qemu-system-x86_64 \
	    -name "LunaOS $(VERSION)" \
	    -m 2048 \
	    -cdrom $(ISO_OUTPUT) \
	    -boot d \
	    -device virtio-vga \
	    -device virtio-tablet-pci \
	    -device virtio-keyboard-pci

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	$(call step,Cleaning build artifacts)
	rm -rf $(BUILD_DIR)/obj $(BUILD_DIR)/compositor-deps
	rm -f  $(ISO_OUTPUT)
	$(call log,Cleaned — rootfs and kernel preserved. Use clean-all to remove everything.)

clean-all:
	rm -rf $(BUILD_DIR)

# ── Status ────────────────────────────────────────────────────────────────────
status:
	@echo ""
	@echo -e "$(BOLD)LunaOS $(VERSION) build status$(NC)"
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@_check() { \
	    if [ -e "$$1" ]; then \
	        echo -e "  $(GREEN)✓$(NC) $$2"; \
	    else \
	        echo -e "  $(RED)✗$(NC) $$2 — not built yet"; \
	    fi \
	}; \
	_check $(BUILD_DIR)/kernel/output/mach_kernel    "XNU kernel (RELEASE_X86_64)"; \
	_check $(ROOTFS)/usr/local/lib/libdrm.dylib      "libdrm-darwin (DRM shim)"; \
	_check $(ROOTFS)/usr/local/bin/luna-compositor   "luna-compositor (Wayland)"; \
	_check $(ROOTFS)/usr/local/lib/libvulkan_lvp.dylib "Mesa 26 + Vulkan 1.4 (lavapipe)"; \
	_check $(ROOTFS)/usr/local/bin/luna-shell        "Desktop shell (panel + launcher)"; \
	_check $(ROOTFS)/usr/local/bin/foot              "foot terminal emulator"; \
	_check $(ROOTFS)/usr/local/bin/luna-files        "luna-files file manager"; \
	_check $(ROOTFS)/usr/local/bin/luna-editor       "luna-editor text editor"; \
	_check $(ROOTFS)/System/Library/Kernels/kernel   "Rootfs assembled"; \
	_check $(ISO_OUTPUT)                             "ISO: LunaOS-$(VERSION)-x86_64.iso"
	@echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
	@echo ""

# ── Help ──────────────────────────────────────────────────────────────────────
help:
	@echo ""
	@echo -e "$(BOLD)LunaOS $(VERSION) build system$(NC)"
	@echo ""
	@echo "  make all          Build everything → ISO"
	@echo "  make kernel       XNU kernel only"
	@echo "  make compositor   Wayland compositor + glue"
	@echo "  make graphics     Mesa 26 + Vulkan 1.4"
	@echo "  make shell        Desktop shell"
	@echo "  make apps         Terminal + file manager + editor"
	@echo "  make userland     PureDarwin rootfs assembly"
	@echo "  make iso          Package into bootable ISO"
	@echo "  make verify       Run all verification checks"
	@echo "  make qemu         Boot ISO in QEMU"
	@echo "  make status       Show what's built"
	@echo "  make clean        Remove obj/ dirs and ISO"
	@echo "  make clean-all    Remove entire build/ tree"
	@echo ""
	@echo "  JOBS=N make all   Parallel build with N jobs (default: ncpu)"
	@echo ""
