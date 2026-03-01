# ARM64 Porting Guide

This guide provides comprehensive documentation for ARM64 porting of the LunaOS. It covers the essential aspects of the porting process, including build configuration, DRM shim architecture changes, and kernel modifications.

## Table of Contents
- [Build Configuration](#build-configuration)
- [DRM Shim Architecture Changes](#drm-shim-architecture-changes)
- [Kernel Modifications](#kernel-modifications)
- [Step-by-Step Porting Instructions](#step-by-step-porting-instructions)

## Build Configuration
### Toolchain Setup
Begin by installing the necessary ARM64 toolchain. For example, you can use `gcc-aarch64-linux-gnu`:
```bash
sudo apt-get install gcc-aarch64-linux-gnu
```
### Makefile Adjustments
Ensure that your Makefile is set up to use the ARM64 architecture:
```makefile
ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu-
```

## DRM Shim Architecture Changes
### Overview
Changes have been made to the DRM shim architecture to support ARM64 functionalities. Key aspects include:
- Integration with `libdrm`
- Modifications in the driver interface

### Specific Changes
Please refer to the [changes in src/drm](https://github.com/atrixxufur/LunaOS/tree/arm64-port/src/drm) for detailed file references.

## Kernel Modifications
### Necessary Patches
1. Update the kernel headers for ARM64 support: Ensure that the `arm64/` directory reflects the latest changes.
2. Add support for new devices in `arch/arm64/boot/dts/`

### Example Patches
For example, see `0001-arm64-add-support-for-new-device.patch`. This patch includes...

## Step-by-Step Porting Instructions
### Step 1: Initial Setup
Create a new branch for your port:
```bash
git checkout -b arm64-port
```
### Step 2: Update Dependencies
Ensure all dependencies are updated:
```bash
sudo apt-get update && sudo apt-get upgrade
```
### Step 3: Build the System
Compile the system using the provided Makefile configurations.

For detailed instructions, refer to the original build scripts located [here](https://github.com/atrixxufur/LunaOS/tree/arm64-port).

### Additional Resources
- [LunaOS Repository](https://github.com/atrixxufur/LunaOS)
- [ARM Developer Documentation](https://developer.arm.com/documentation)

## Conclusion
This guide serves as a starting point for developers looking to port LunaOS to ARM64. Further contributions and feedback are welcome!