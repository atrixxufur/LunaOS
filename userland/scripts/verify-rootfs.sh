#!/usr/bin/env bash
# =============================================================================
# verify-rootfs.sh — LunaOS rootfs sanity checker
#
# Verifies that build-userland.sh produced a bootable rootfs.
# Run before building the ISO.
#
# Usage: ./userland/scripts/verify-rootfs.sh [path/to/rootfs]
# =============================================================================

set -euo pipefail

LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ROOTFS="${1:-${LUNA_ROOT}/build/rootfs}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass()  { echo -e "  ${GREEN}✓${NC} $*"; }
fail()  { echo -e "  ${RED}✗${NC} $*"; FAILURES=$((FAILURES+1)); }
warn()  { echo -e "  ${YELLOW}?${NC} $*"; WARNINGS=$((WARNINGS+1)); }

FAILURES=0
WARNINGS=0

echo "══════════════════════════════════════════════════"
echo " LunaOS rootfs verification"
echo " Rootfs: $ROOTFS"
echo " Size:   $(du -sh "$ROOTFS" 2>/dev/null | cut -f1)"
echo "══════════════════════════════════════════════════"

check_file()  {
    local f="$1" desc="${2:-$1}"
    [[ -f "${ROOTFS}${f}" ]] && pass "$desc" || fail "$desc missing: $f"
}
check_dir()   {
    local d="$1" desc="${2:-$1}"
    [[ -d "${ROOTFS}${d}" ]] && pass "$desc" || fail "$desc missing: $d"
}
check_binary(){
    local b="$1" desc="${2:-$1}"
    if [[ -f "${ROOTFS}${b}" ]]; then
        local arch; arch=$(file "${ROOTFS}${b}" | grep -o "x86_64" || echo "?")
        [[ "$arch" == "x86_64" ]] && pass "$desc (x86_64)" \
                                  || warn "$desc exists but arch=$arch"
    else
        fail "$desc missing: $b"
    fi
}
check_kext()  {
    local k="$1"
    [[ -d "${ROOTFS}/System/Library/Extensions/${k}.kext" || \
       -d "${ROOTFS}/Library/Extensions/${k}.kext" ]] \
        && pass "${k}.kext" || fail "${k}.kext missing"
}

# ── Kernel ────────────────────────────────────────────────────────────────────
echo ""
echo "── Kernel"
check_file "/System/Library/Kernels/kernel"          "XNU kernel"
if [[ -f "${ROOTFS}/System/Library/Kernels/kernel" ]]; then
    KSIZE=$(du -sh "${ROOTFS}/System/Library/Kernels/kernel" | cut -f1)
    pass "  kernel size: $KSIZE"
fi

# ── C runtime ─────────────────────────────────────────────────────────────────
echo ""
echo "── C runtime"
check_file "/usr/lib/libSystem.B.dylib"              "libSystem.B.dylib"
check_file "/usr/lib/libc.dylib"                     "libc.dylib"
check_file "/usr/lib/libdispatch.dylib"              "libdispatch (GCD)"
check_file "/usr/lib/libpthread.dylib"               "libpthread"

# ── Dynamic linker ────────────────────────────────────────────────────────────
echo ""
echo "── Dynamic linker"
check_file "/usr/lib/dyld"                           "dyld"

# ── Shell utilities ───────────────────────────────────────────────────────────
echo ""
echo "── Shell utilities"
check_binary "/bin/bash"                             "bash"
check_binary "/bin/sh"                               "sh"
check_binary "/bin/ls"                               "ls"
check_binary "/bin/cp"                               "cp"
check_binary "/bin/mv"                               "mv"
check_binary "/bin/rm"                               "rm"
check_binary "/usr/bin/grep"                         "grep"
check_binary "/usr/bin/sed"                          "sed"
check_binary "/usr/bin/awk"                          "awk"
check_binary "/usr/bin/ps"                           "ps"
check_binary "/sbin/reboot"                          "reboot"
check_binary "/sbin/ifconfig"                        "ifconfig"

# ── LunaOS services ───────────────────────────────────────────────────────────
echo ""
echo "── LunaOS services"
check_binary "/usr/local/sbin/seatd-darwin"          "seatd-darwin"
check_binary "/usr/local/sbin/darwin-evdev-bridge"   "darwin-evdev-bridge"
check_file   "/usr/local/lib/libdrm.dylib"           "libdrm-darwin.dylib"
check_binary "/usr/local/bin/luna-compositor"        "luna-compositor"

# ── KEXTs ─────────────────────────────────────────────────────────────────────
echo ""
echo "── KEXTs (/System/Library/Extensions + /Library/Extensions)"
check_kext "IOGraphicsFamily"
check_kext "IOHIDFamily"
check_kext "IOStorageFamily"
check_kext "IONetworkingFamily"
check_kext "IODRMShim"

# ── Config files ──────────────────────────────────────────────────────────────
echo ""
echo "── Config files"
check_file "/private/etc/fstab"                      "/etc/fstab"
check_file "/private/etc/passwd"                     "/etc/passwd"
check_file "/private/etc/group"                      "/etc/group"
check_file "/private/etc/hostname"                   "/etc/hostname"
check_file "/private/etc/rc.lunaos"                  "/etc/rc.lunaos"
check_file "/private/etc/profile"                    "/etc/profile"
check_file "/System/Library/CoreServices/SystemVersion.plist" "SystemVersion.plist"
check_file "/Library/Preferences/SystemConfiguration/com.apple.Boot.plist" "Boot.plist"

# ── LaunchDaemons ─────────────────────────────────────────────────────────────
echo ""
echo "── LaunchDaemons"
check_file "/Library/LaunchDaemons/org.puredarwin.seatd.plist"         "seatd launchd plist"
check_file "/Library/LaunchDaemons/org.puredarwin.evdev-bridge.plist"  "evdev-bridge launchd plist"
check_file "/Library/LaunchDaemons/org.lunaos.compositor.plist"        "compositor launchd plist"

# ── Symlinks ──────────────────────────────────────────────────────────────────
echo ""
echo "── Darwin symlinks"
[[ -L "${ROOTFS}/var" || -d "${ROOTFS}/var" ]]  && pass "/var"       || fail "/var missing"
[[ -L "${ROOTFS}/tmp" || -d "${ROOTFS}/tmp" ]]  && pass "/tmp"       || fail "/tmp missing"
[[ -L "${ROOTFS}/etc" || -d "${ROOTFS}/etc" ]]  && pass "/etc"       || fail "/etc missing"
[[ -d "${ROOTFS}/dev" ]]                         && pass "/dev"       || fail "/dev missing"
[[ -d "${ROOTFS}/usr/local/bin" ]]               && pass "/usr/local/bin" || fail "/usr/local/bin missing"

# ── Architecture check ────────────────────────────────────────────────────────
echo ""
echo "── Architecture audit (checking for non-x86_64 binaries)"
BAD_ARCH=0
while IFS= read -r -d '' bin; do
    arch=$(file "$bin" 2>/dev/null | grep -oE "(arm64|arm|i386)" | head -1 || true)
    if [[ -n "$arch" ]]; then
        warn "Wrong arch ($arch): ${bin#"$ROOTFS"}"
        BAD_ARCH=$((BAD_ARCH+1))
    fi
done < <(find "$ROOTFS" \( -path "*/bin/*" -o -path "*/sbin/*" -o -path "*/lib/*.dylib" \) \
         -type f -print0 2>/dev/null)
(( BAD_ARCH == 0 )) && pass "All checked binaries are x86_64"

# ── Summary ────────────────────────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════════════"
if (( FAILURES == 0 && WARNINGS == 0 )); then
    echo -e "${GREEN}All checks passed.${NC} Rootfs is ready for ISO packaging."
    echo ""
    echo "  Next: ./userland/scripts/build-iso.sh"
elif (( FAILURES == 0 )); then
    echo -e "${YELLOW}${WARNINGS} warning(s), 0 failures.${NC} Rootfs is likely bootable."
    echo "  Review warnings above before building ISO."
else
    echo -e "${RED}${FAILURES} failure(s)${NC}, ${WARNINGS} warning(s)."
    echo "  Fix failures before building ISO — the OS will not boot."
    exit 1
fi
echo "══════════════════════════════════════════════════"
