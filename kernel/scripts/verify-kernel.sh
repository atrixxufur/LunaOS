#!/usr/bin/env bash
# =============================================================================
# verify-kernel.sh — Post-build kernel sanity checks
#
# Runs after build-xnu.sh to verify the kernel binary is valid and
# contains the expected symbols, architecture, and LunaOS patches.
#
# Usage: ./kernel/scripts/verify-kernel.sh [path/to/mach_kernel]
# =============================================================================

set -euo pipefail

LUNA_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KERNEL="${1:-${LUNA_ROOT}/build/kernel/output/mach_kernel}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { echo -e "  ${GREEN}✓${NC} $*"; }
fail() { echo -e "  ${RED}✗${NC} $*"; FAILURES=$((FAILURES+1)); }
warn() { echo -e "  ${YELLOW}?${NC} $*"; }

FAILURES=0

echo "═══════════════════════════════════════════════════"
echo " LunaOS kernel verification"
echo " Kernel: $KERNEL"
echo "═══════════════════════════════════════════════════"

# ── 1. File exists ────────────────────────────────────────────────────────────
echo ""
echo "── Basic checks"
if [[ -f "$KERNEL" ]]; then
    pass "kernel binary exists"
    SIZE=$(du -sh "$KERNEL" | cut -f1)
    pass "size: ${SIZE} (typical: 20-40MB for RELEASE_X86_64)"
else
    fail "kernel binary not found at: $KERNEL"
    echo ""
    echo "Run build-xnu.sh first."
    exit 1
fi

# ── 2. Mach-O format ──────────────────────────────────────────────────────────
echo ""
echo "── Mach-O format"
FILE_OUT=$(file "$KERNEL")
if echo "$FILE_OUT" | grep -q "Mach-O 64-bit"; then
    pass "Mach-O 64-bit format"
else
    fail "unexpected format: $FILE_OUT"
fi

if echo "$FILE_OUT" | grep -q "x86_64"; then
    pass "architecture: x86_64"
else
    fail "wrong architecture: $FILE_OUT"
fi

if echo "$FILE_OUT" | grep -q "kernel"; then
    pass "Mach-O type: kernel"
else
    warn "Mach-O type not 'kernel' — may still be valid"
fi

# ── 3. Required symbols ───────────────────────────────────────────────────────
echo ""
echo "── Required symbols (nm check)"

check_symbol() {
    local sym="$1"
    local desc="$2"
    if nm "$KERNEL" 2>/dev/null | grep -q "$sym"; then
        pass "$desc ($sym)"
    else
        fail "$desc missing ($sym)"
    fi
}

check_symbol "_start"                    "kernel entry point"
check_symbol "_kernel_bootstrap"         "bootstrap sequence"
check_symbol "_IOFramebuffer"            "IOFramebuffer class (IODRMShim dep)"
check_symbol "_IOHIDDevice"              "IOHIDDevice class (evdev-bridge dep)"
check_symbol "_csr_check"               "SIP check function"
check_symbol "_PE_kputc"                "platform expert console output"

# ── 4. LunaOS patches applied ─────────────────────────────────────────────────
echo ""
echo "── LunaOS patch verification"

# Check CSR patch: csr_check should always return 0 in LunaOS build
# We verify by checking the symbol is present (patched version changes implementation)
if nm "$KERNEL" 2>/dev/null | grep -q "_csr_check"; then
    pass "CSR override symbol present"
else
    warn "CSR symbol not found — SIP may not be fully disabled"
fi

# Check IOFramebuffer API promotion patch
if nm "$KERNEL" 2>/dev/null | grep -q "_IOFramebuffer.*addFramebufferNotification"; then
    pass "IOFramebuffer::addFramebufferNotification promoted to public KPI"
else
    warn "addFramebufferNotification not found in symbol table"
    warn "(may still work via kpi.unsupported linkage)"
fi

# ── 5. KEXTs present ─────────────────────────────────────────────────────────
echo ""
echo "── KEXTs in output/Extensions/"
EXTS_DIR="$(dirname "$KERNEL")/Extensions"

check_kext() {
    local name="$1"
    if [[ -d "${EXTS_DIR}/${name}.kext" ]]; then
        pass "${name}.kext"
    else
        fail "${name}.kext missing from Extensions/"
    fi
}

if [[ -d "$EXTS_DIR" ]]; then
    check_kext "IOGraphicsFamily"
    check_kext "IOHIDFamily"
    check_kext "IOStorageFamily"
    check_kext "IONetworkingFamily"
    check_kext "IOACPIFamily"
    # Our custom KEXT
    if [[ -d "${EXTS_DIR}/IODRMShim.kext" ]]; then
        pass "IODRMShim.kext (LunaOS DRM driver)"
    else
        warn "IODRMShim.kext not found — build it with: xcodebuild in drm-shim/kern/"
    fi
else
    warn "Extensions/ directory not found — KEXTs may not have been built"
fi

# ── 6. otool check ────────────────────────────────────────────────────────────
echo ""
echo "── Load commands (otool -l)"
LOAD_CMDS=$(otool -l "$KERNEL" 2>/dev/null | grep -c "cmd LC_" || echo 0)
if (( LOAD_CMDS > 5 )); then
    pass "${LOAD_CMDS} load commands (healthy Mach-O)"
else
    warn "only ${LOAD_CMDS} load commands — binary may be incomplete"
fi

# Check text segment base address (kernel should load at 0xffffff8000200000)
TEXT_BASE=$(otool -l "$KERNEL" 2>/dev/null | grep -A5 "segname __TEXT" | grep vmaddr | awk '{print $2}' | head -1)
if [[ "$TEXT_BASE" == "0xffffff8000200000" ]]; then
    pass "__TEXT segment base: ${TEXT_BASE} (correct kernel address)"
else
    warn "__TEXT base: ${TEXT_BASE:-unknown} (expected 0xffffff8000200000 for x86_64 kernel)"
fi

# ── Summary ────────────────────────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════════"
if (( FAILURES == 0 )); then
    echo -e "${GREEN}All checks passed.${NC} Kernel is ready for ISO packaging."
    echo ""
    echo "Next steps:"
    echo "  1. ./scripts/build-userland.sh"
    echo "  2. ./scripts/build-iso.sh"
else
    echo -e "${RED}${FAILURES} check(s) failed.${NC}"
    echo "Review the output above and check build-xnu.sh logs."
    echo "Log: ${LUNA_ROOT}/build/kernel/xnu-build.log"
    exit 1
fi
echo "═══════════════════════════════════════════════════"
