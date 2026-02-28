#!/usr/bin/env bash
# =============================================================================
# build-iso.sh — LunaOS bootable ISO packager
#
# Takes build/rootfs/ + build/kernel/output/ and produces:
#   LunaOS-0.1.0-x86_64.iso  — bootable ISO (EFI + BIOS hybrid)
#
# Boot chain:
#   BIOS/EFI firmware
#     └── GRUB2 bootloader  (reads grub.cfg, loads mach_kernel)
#           └── mach_kernel  (XNU, Darwin 26)
#                 └── launchd (PID 1)
#                       └── rc.lunaos  (starts KEXTs, seatd, evdev-bridge)
#                             └── luna-compositor (Wayland compositor)
#
# Output: build/LunaOS-0.1.0-x86_64.iso
#
# Requires: xorriso (brew install xorriso)
#           grub (brew install grub)
# =============================================================================

set -euo pipefail

LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ROOTFS="${LUNA_ROOT}/build/rootfs"
KERNEL_OUT="${LUNA_ROOT}/build/kernel/output"
ISO_STAGING="${LUNA_ROOT}/build/iso-staging"
OUTPUT_ISO="${LUNA_ROOT}/build/LunaOS-0.1.0-x86_64.iso"
VERSION="0.1.0"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}[iso]${NC} $*"; }
warn() { echo -e "${YELLOW}[iso]${NC} $*"; }
die()  { echo -e "${RED}[iso] ERROR:${NC} $*" >&2; exit 1; }

log "=== LunaOS ISO builder ==="

# ── Preflight ─────────────────────────────────────────────────────────────────
[[ -d "$ROOTFS" ]]     || die "Rootfs not found: $ROOTFS — run build-userland.sh"
[[ -f "$ROOTFS/System/Library/Kernels/kernel" ]] || \
    die "Kernel not in rootfs — run build-xnu.sh then build-userland.sh"

for tool in xorriso grub-mkrescue; do
    command -v "$tool" &>/dev/null || die \
        "$tool not found. Install with: brew install xorriso grub"
done

# ── Stage the ISO layout ──────────────────────────────────────────────────────
log "staging ISO layout..."
rm -rf "$ISO_STAGING"
mkdir -p "${ISO_STAGING}/boot/grub"

# Copy rootfs into ISO staging
log "  copying rootfs (~$(du -sh "$ROOTFS" | cut -f1))..."
rsync -a --exclude='*.dSYM' "${ROOTFS}/" "${ISO_STAGING}/"

# ── GRUB configuration ────────────────────────────────────────────────────────
log "  writing grub.cfg..."
cat > "${ISO_STAGING}/boot/grub/grub.cfg" <<GRUBCFG
# LunaOS GRUB configuration
set timeout=3
set default=0

insmod all_video
insmod gfxterm
insmod png

terminal_output gfxterm
loadfont unicode

menuentry "LunaOS ${VERSION} (Darwin 26 / XNU)" {
    set root=(hd0,gpt2)
    # XNU boot protocol: load mach_kernel via multiboot-style handoff
    # GRUB's darwin loader sets up the boot args and hands off to XNU
    darwin_kernel /System/Library/Kernels/kernel \
        "csr-active-config=0xFF kext-dev-mode=1 -v pmuflags=1 -no_compat_warning"
    boot
}

menuentry "LunaOS ${VERSION} (verbose boot)" {
    set root=(hd0,gpt2)
    darwin_kernel /System/Library/Kernels/kernel \
        "csr-active-config=0xFF kext-dev-mode=1 -v -s io=0xffffffff"
    boot
}

menuentry "LunaOS ${VERSION} (single user / recovery)" {
    set root=(hd0,gpt2)
    darwin_kernel /System/Library/Kernels/kernel \
        "csr-active-config=0xFF -s -v"
    boot
}
GRUBCFG

# ── Build the ISO ─────────────────────────────────────────────────────────────
log "building ISO with xorriso..."
xorriso \
    -as mkisofs \
    -iso-level 3 \
    -full-iso9660-filenames \
    -volid "LUNAOS_${VERSION//./_}" \
    -publisher "LunaOS Project" \
    -preparer "build-iso.sh" \
    -appid "LunaOS ${VERSION}" \
    --grub2-mbr /usr/local/lib/grub/i386-pc/boot_hybrid.img \
    -partition_offset 16 \
    --mbr-force-bootable \
    -append_partition 2 0xef /usr/local/lib/grub/i386-pc/efi.img \
    -appended_part_as_gpt \
    -iso_mbr_part_type 0x00 \
    -c boot/grub/boot.cat \
    -b boot/grub/i386-pc/eltorito.img \
    -no-emul-boot \
    -boot-load-size 4 \
    -boot-info-table \
    --grub2-boot-info \
    -eltorito-alt-boot \
    -e --interval:appended_partition_2:all:: \
    -no-emul-boot \
    -output "$OUTPUT_ISO" \
    "$ISO_STAGING" \
    2>&1 | grep -v "^$" | tail -20

# ── Summary ───────────────────────────────────────────────────────────────────
ISO_SIZE=$(du -sh "$OUTPUT_ISO" | cut -f1)
echo ""
log "╔══════════════════════════════════════════════════════════╗"
log "║          LunaOS ISO build complete!                     ║"
log "╠══════════════════════════════════════════════════════════╣"
log "║  Output: ${OUTPUT_ISO}"
log "║  Size:   ${ISO_SIZE}"
log "╠══════════════════════════════════════════════════════════╣"
log "║  Test with QEMU:                                         ║"
log "║    qemu-system-x86_64 \\                                 ║"
log "║      -m 2048 \\                                          ║"
log "║      -cdrom LunaOS-${VERSION}-x86_64.iso \\              ║"
log "║      -device virtio-vga \\                               ║"
log "║      -enable-kvm \\                                      ║"
log "║      -boot d                                            ║"
log "╚══════════════════════════════════════════════════════════╝"
