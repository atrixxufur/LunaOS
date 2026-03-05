#!/usr/bin/env python3
"""
fix-build-xnu-x86.py — Fixes for LunaOS OpenDarXNU build-xnu.sh
Run from: /Users/atrixxu/Desktop/LunaOS OpenDarXNU/darwin-wayland/
"""

import os
import re
import stat
import subprocess
from pathlib import Path

REPO_ROOT = Path.home() / "Desktop" / "LunaOS OpenDarXNU" / "darwin-wayland"
SCRIPT    = REPO_ROOT / "kernel" / "scripts" / "build-xnu.sh"

if not SCRIPT.exists():
    print(f"ERROR: {SCRIPT} not found")
    exit(1)

src = SCRIPT.read_text()
orig = src
fixes = []

# ── Fix 1: Replace hardcoded macos-262 tag with dynamic Darwin version detection
# The macos-262 tag doesn't exist on apple-oss-distributions.
# Real tags look like: xnu-11417.140.69, dtrace-413, etc.

OLD_CONFIG = '''MACOS_TAG="macos-262"
MACOS_TAG="macos-262"
XNU_TAG="xnu-11215"    # XNU version matching macos-262'''

# More flexible match
if 'MACOS_TAG="macos-262"' in src:
    # Replace the static tag block with dynamic detection
    new_detection = '''# Dynamic XNU tag detection based on running Darwin version
DARWIN_VER="$(uname -r)"   # e.g. 25.4.0
DARWIN_MAJOR="${DARWIN_VER%%.*}"

# Map Darwin major → XNU tag prefix
declare -A DARWIN_XNU_MAP=(
    [25]="xnu-11417"
    [24]="xnu-10063"
    [23]="xnu-8796"
)
XNU_PREFIX="${DARWIN_XNU_MAP[$DARWIN_MAJOR]:-xnu-11417}"

# Find the latest real tag for each dependency via git ls-remote
find_latest_tag() {
    local repo="$1"
    local prefix="$2"
    local url="${APPLE_OSS}/${repo}.git"
    GIT_TERMINAL_PROMPT=0 git ls-remote --tags "$url" 2>/dev/null \\
        | awk '{print $2}' \\
        | grep "refs/tags/${prefix}" \\
        | sed 's|refs/tags/||' \\
        | sed 's/\\^{}//' \\
        | grep -v '\\^' \\
        | sort -V | tail -1
}

log "Detecting tags for Darwin ${DARWIN_VER}..."
XNU_TAG=$(find_latest_tag "xnu" "${XNU_PREFIX}")
DTRACE_TAG=$(find_latest_tag "dtrace" "dtrace-")
AVAIL_TAG=$(find_latest_tag "AvailabilityVersions" "AvailabilityVersions-")
LIBPLATFORM_TAG=$(find_latest_tag "libplatform" "libplatform-")
LIBDISPATCH_TAG=$(find_latest_tag "libdispatch" "libdispatch-")
LIBPTHREAD_TAG=$(find_latest_tag "libpthread" "libpthread-")

# Fallbacks
XNU_TAG="${XNU_TAG:-xnu-11417.140.69}"
DTRACE_TAG="${DTRACE_TAG:-dtrace-413}"
AVAIL_TAG="${AVAIL_TAG:-AvailabilityVersions-155}"
LIBPLATFORM_TAG="${LIBPLATFORM_TAG:-libplatform-359.80.2}"
LIBDISPATCH_TAG="${LIBDISPATCH_TAG:-libdispatch-1542.0.4}"
LIBPTHREAD_TAG="${LIBPTHREAD_TAG:-libpthread-539.80.3}"

log "  xnu:                  ${XNU_TAG}"
log "  dtrace:               ${DTRACE_TAG}"
log "  AvailabilityVersions: ${AVAIL_TAG}"
log "  libplatform:          ${LIBPLATFORM_TAG}"
log "  libdispatch:          ${LIBDISPATCH_TAG}"
log "  libpthread:           ${LIBPTHREAD_TAG}"
'''
    # Remove the old static tag lines
    src = re.sub(
        r'MACOS_TAG="macos-262"\s*\n.*?XNU_TAG="xnu-11215".*?\n',
        new_detection + '\n',
        src,
        flags=re.DOTALL
    )
    fixes.append("Fix 1: replaced hardcoded macos-262 tag with dynamic detection")

# ── Fix 2: Replace hardcoded $MACOS_TAG references in DEPS array with dynamic tags
if '"dtrace:${MACOS_TAG}"' in src:
    src = src.replace('"dtrace:${MACOS_TAG}"', '"dtrace:${DTRACE_TAG}"')
    src = src.replace('"AvailabilityVersions:${MACOS_TAG}"', '"AvailabilityVersions:${AVAIL_TAG}"')
    src = src.replace('"libplatform:${MACOS_TAG}"', '"libplatform:${LIBPLATFORM_TAG}"')
    src = src.replace('"libdispatch:${MACOS_TAG}"', '"libdispatch:${LIBDISPATCH_TAG}"')
    src = src.replace('"libpthread:${MACOS_TAG}"', '"libpthread:${LIBPTHREAD_TAG}"')
    src = src.replace('"Libm:${MACOS_TAG}"', '"Libm:${LIBPLATFORM_TAG}"')
    src = src.replace('"xnu:${MACOS_TAG}"', '"xnu:${XNU_TAG}"')
    fixes.append("Fix 2: replaced MACOS_TAG references in DEPS array with dynamic tags")

# Replace KEXT deps too
src = src.replace(':${MACOS_TAG}"', ':${XNU_TAG}"')
if ':${MACOS_TAG}"' not in src:
    fixes.append("Fix 2b: replaced remaining MACOS_TAG references")

# ── Fix 3: Skip dtrace xcodebuild (macosx.internal SDK) — use stubs instead
# The x86 script uses make not xcodebuild for dtrace, but it will still fail.
# Replace the dtrace build block with a stub setup.
dtrace_stub = '''# dtrace requires Apple-internal SDK (macosx.internal) — skip, use stubs
step "dtrace (skipped — using no-op stubs for open-source build)"

DTRACE_STUB_DIR="${BUILD_DIR}/ctf-stubs"
mkdir -p "$DTRACE_STUB_DIR"
printf '#!/bin/sh\\nexit 0\\n' > "${DTRACE_STUB_DIR}/ctfconvert"
printf '#!/bin/sh\\nexit 0\\n' > "${DTRACE_STUB_DIR}/ctfmerge"
chmod +x "${DTRACE_STUB_DIR}/ctfconvert" "${DTRACE_STUB_DIR}/ctfmerge"
export PATH="${DTRACE_STUB_DIR}:$PATH"
log "ctf stubs: ${DTRACE_STUB_DIR}/ctfconvert"
'''

# Find the dtrace build block and replace it
dtrace_block_re = re.compile(
    r'(# ── Step 2: Build dtrace.*?^# ── Step 3:)',
    re.DOTALL | re.MULTILINE
)
match = dtrace_block_re.search(src)
if match:
    src = src[:match.start()] + dtrace_stub + "\n# ── Step 3:" + src[match.end():]
    fixes.append("Fix 3: replaced dtrace build with no-op stubs")

# ── Fix 4: Add ARCH_CONFIGS and RC_DARWIN_KERNEL_VERSION to XNU make invocations
# x86 needs ARCH_CONFIGS="X86_64" explicitly
for make_block in [
    'MACHINE_CONFIGS="X86_64" \\\n    BUILD_WERROR=0 \\\n    PLATFORM=MacOSX \\\n    -j"$JOBS" \\\n    installhdrs',
    'MACHINE_CONFIGS="X86_64" \\\n    BUILD_WERROR=0 \\\n    PLATFORM=MacOSX \\\n    -j"$JOBS" \\\n    install',
]:
    new_block = make_block.replace(
        'MACHINE_CONFIGS="X86_64" \\',
        'MACHINE_CONFIGS="X86_64" \\\n    ARCH_CONFIGS="X86_64" \\\n    RC_DARWIN_KERNEL_VERSION="${DARWIN_VER}" \\'
    )
    if make_block in src:
        src = src.replace(make_block, new_block)
        fixes.append(f"Fix 4: added ARCH_CONFIGS + RC_DARWIN_KERNEL_VERSION to make invocation")

# ── Fix 5: Add GIT_TERMINAL_PROMPT=0 to clone_repo to avoid hanging on private repos
src = src.replace(
    'git clone --depth 1 --branch "$tag" "$url" "$dest" --quiet',
    'GIT_TERMINAL_PROMPT=0 git clone --depth 1 --branch "$tag" "$url" "$dest" --quiet'
)
src = src.replace(
    'git -C "$dest" fetch --tags --quiet',
    'GIT_TERMINAL_PROMPT=0 git -C "$dest" fetch --tags --quiet'
)
fixes.append("Fix 5: added GIT_TERMINAL_PROMPT=0 to prevent credential prompts")

# ── Write fixed script ────────────────────────────────────────────────────────
if src != orig:
    SCRIPT.write_text(src)
    print(f"✓ Wrote fixed script: {SCRIPT}")
    for i, f in enumerate(fixes, 1):
        print(f"  {f}")
else:
    print("No changes made — patterns may not have matched.")
    print("Check the script manually.")

# ── Create TrustCache and firehose header stubs ───────────────────────────────
SOURCES = REPO_ROOT / "build" / "kernel-src" / "xnu" / "EXTERNAL_HEADERS"

stubs = {
    "TrustCache/API.h": """\
#pragma once
#include <stdint.h>
#include <stddef.h>
#define kUUIDSize 16
#define kTCEntryHashSize 20
typedef struct { uint8_t uuid[kUUIDSize]; } TrustCache_t;
typedef struct { uint32_t version; } TrustCacheRuntime_t;
typedef struct { uint32_t token; } TrustCacheQueryToken_t;
typedef uint32_t TCCapabilities_t;
typedef uint32_t TCQueryType_t;
""",
    "CodeSignature/Entitlements.h": """\
#pragma once
#include <stdint.h>
typedef uint32_t cs_entitlement_flags_t;
#define kCSWebBrowserNetworkEntitlement "com.apple.security.network.client"
#define kCSNetworkClientEntitlement "com.apple.security.network.client"
""",
}

for rel, content in stubs.items():
    full = SOURCES / rel
    full.parent.mkdir(parents=True, exist_ok=True)
    full.write_text(content)
    print(f"✓ Wrote stub: {full}")

print("\nDone. Now run:")
print("  cd '/Users/atrixxu/Desktop/LunaOS OpenDarXNU/darwin-wayland'")
print("  export PATH='/opt/homebrew/opt/llvm/bin:$PATH'")
print("  make kernel")
