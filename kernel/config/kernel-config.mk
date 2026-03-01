# =============================================================================
# kernel-config.mk — LunaOS XNU kernel build configuration
#
# Included by build-xnu.sh and by the top-level Makefile.
# All variables here can be overridden by environment variables.
# =============================================================================

# ── Version identifiers ───────────────────────────────────────────────────────

# XNU version tag from apple-oss-distributions
# Run: git ls-remote https://github.com/apple-oss-distributions/xnu | grep macos-262
XNU_TAG            := macos-262
XNU_VERSION_STRING := 11215   # XNU-11215 embedded in version.h

# Darwin kernel major version (uname -r will show this)
DARWIN_VERSION     := 26.0.0

# LunaOS release string (appears in uname -v, /System/Library/CoreServices/SystemVersion.plist)
LUNAOS_VERSION     := 0.1.0
LUNAOS_CODENAME    := Meridian

# ── Build configuration ───────────────────────────────────────────────────────

# RELEASE: fully optimised, no debug assertions, stripped symbols
# DEVELOPMENT: assertions enabled, verbose boot logging
KERNEL_CONFIG      := RELEASE

# Target architecture
MACHINE_CONFIG     := X86_64

# Parallel build jobs (override with: make JOBS=16)
JOBS               ?= $(shell sysctl -n hw.ncpu)

# ── Boot arguments (written to com.apple.Boot.plist) ─────────────────────────
#
# These boot-args are set in the EFI boot partition and passed to XNU at boot.
# They configure the kernel for PureDarwin/LunaOS operation.
#
#   csr-active-config=0xFF    — disable SIP entirely (allow unsigned KEXTs)
#   kext-dev-mode=1           — allow developer-signed KEXTs
#   -v                        — verbose boot (show kernel messages on console)
#   debug=0x144               — enable kernel debugging via serial/firewire
#   pmuflags=1                — disable power management restrictions
#   io=0xffffffff             — verbose IOKit logging
#   -no_compat_warning        — suppress version mismatch warnings from KEXTs
#
BOOT_ARGS          := csr-active-config=0xFF kext-dev-mode=1 -v pmuflags=1 -no_compat_warning

# ── Paths ─────────────────────────────────────────────────────────────────────

# Where apple-oss-distributions repos are cloned
SOURCES_DIR        := $(LUNA_ROOT)/build/kernel-src

# Where build artifacts go
BUILD_DIR          := $(LUNA_ROOT)/build/kernel

# Final output directory (what gets packaged into the ISO)
OUTPUT_DIR         := $(BUILD_DIR)/output

# ── Toolchain ─────────────────────────────────────────────────────────────────

# Use Homebrew LLVM for clang — Xcode's clang may lack some builtins XNU uses
LLVM_PREFIX        := /usr/local/opt/llvm
CC                 := $(LLVM_PREFIX)/bin/clang
CXX                := $(LLVM_PREFIX)/bin/clang++
LD                 := $(LLVM_PREFIX)/bin/ld.lld
AR                 := $(LLVM_PREFIX)/bin/llvm-ar

# macOS SDK (must match or be newer than the XNU tag)
SDK_PATH           := $(shell xcrun --sdk macosx --show-sdk-path)
TOOLCHAIN_DIR      := $(shell xcode-select -p)/Toolchains/XcodeDefault.xctoolchain

# ── XNU make flags ────────────────────────────────────────────────────────────
#
# Passed verbatim to: make -C $(SOURCES_DIR)/xnu <XNU_MAKE_FLAGS>

XNU_MAKE_FLAGS := \
    SDKROOT="$(SDK_PATH)"           \
    OBJROOT="$(BUILD_DIR)/obj/xnu"  \
    DSTROOT="$(BUILD_DIR)/dst"      \
    SYMROOT="$(BUILD_DIR)/sym/xnu"  \
    TOOLCHAINDIR="$(TOOLCHAIN_DIR)" \
    KERNEL_CONFIGS="$(KERNEL_CONFIG)" \
    MACHINE_CONFIGS="$(MACHINE_CONFIG)" \
    BUILD_WERROR=0                  \
    PLATFORM=MacOSX                 \
    RC_ARCHS=x86_64                 \
    LUNAOS=1                        \
    -j$(JOBS)

# ── KEXT build flags ──────────────────────────────────────────────────────────

KEXT_MAKE_FLAGS := \
    ARCHS=x86_64                   \
    SDKROOT="$(SDK_PATH)"          \
    DSTROOT="$(BUILD_DIR)/dst"     \
    CONFIGURATION=Release          \
    LUNAOS=1

# ── Dependency list (in build order) ─────────────────────────────────────────
#
# Each entry is "repo-name:git-tag"
# All repos are from github.com/apple-oss-distributions

XNU_DEPS := \
    dtrace:$(XNU_TAG)               \
    AvailabilityVersions:$(XNU_TAG) \
    libplatform:$(XNU_TAG)          \
    libdispatch:$(XNU_TAG)          \
    libpthread:$(XNU_TAG)           \
    Libm:$(XNU_TAG)                 \
    xnu:$(XNU_TAG)

KEXT_DEPS := \
    IOKitUser:$(XNU_TAG)            \
    IOGraphicsFamily:$(XNU_TAG)     \
    IOHIDFamily:$(XNU_TAG)          \
    IOStorageFamily:$(XNU_TAG)      \
    IONetworkingFamily:$(XNU_TAG)   \
    IOACPIFamily:$(XNU_TAG)         \
    AppleACPIPlatform:$(XNU_TAG)
