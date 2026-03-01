# darwin-wayland — Kernel-Level Wayland Support for PureDarwin

Ports the kernel-level components required to run Wayland compositors
(wlroots, Weston) on PureDarwin / Darwin XNU 26 (macos-262).

---

## What This Implements

```
┌─────────────────────────────────────────────────────────────────┐
│  Wayland compositor (Weston / Sway / custom)                    │
├───────────────┬──────────────┬──────────────────────────────────┤
│  libdrm       │  libinput    │  libwayland-server               │
│  (Darwin shim)│  (unmodified)│  (kqueue event loop)             │
├───────────────┼──────────────┼──────────────────────────────────┤
│  /dev/dri/    │  /dev/input/ │  Unix sockets + kqueue           │
│  card0        │  event0..N   │                                   │
├───────────────┼──────────────┼──────────────────────────────────┤
│ IODRMShim.kext│ darwin-evdev │  wayland-darwin-event-loop.c     │
│ (IOFramebuffer│ -bridge      │  (replaces epoll with kqueue)     │
│  wrapper KEXT)│ (IOHIDFamily)│                                   │
├───────────────┴──────────────┴──────────────────────────────────┤
│                    XNU / IOKit / Darwin 26                       │
│              (macos-262 open-source distribution)               │
└─────────────────────────────────────────────────────────────────┘
```

---

## File Map

```
darwin-wayland/
├── CMakeLists.txt                    ← top-level build system
│
├── drm-shim/
│   ├── include/
│   │   ├── drm_darwin.h              ← DRM ioctl ABI (shared kernel+user)
│   │   └── libdrm-darwin.h           ← public libdrm API surface
│   ├── kern/
│   │   ├── IODRMShim.h               ← KEXT class declarations
│   │   ├── IODRMShim.cpp             ← KEXT implementation
│   │   ├── Info.plist                ← KEXT bundle metadata + IOKit matching
│   │   └── IODRMShim.entitlements    ← codesign entitlements
│   └── user/
│       └── libdrm-darwin.c           ← userspace DRM library (libdrm ABI)
│
├── evdev-bridge/
│   └── darwin-evdev-bridge.c         ← IOHIDFamily → /dev/input/event* daemon
│
├── libwayland-darwin/
│   └── wayland-darwin-event-loop.c   ← kqueue replacement for libwayland epoll
│
├── seatd-darwin/
│   └── seatd-darwin.c                ← seat daemon (IOKit device enum, no udev)
│
└── build/
    ├── libdrm.pc.in                  ← pkg-config template
    └── launchd/
        ├── org.puredarwin.seatd.plist
        └── org.puredarwin.evdev-bridge.plist
```

---

## Build Requirements

- **macOS host** with Xcode (for building KEXTs and Darwin userspace)
- **Kernel Development Kit (KDK)** matching your Darwin kernel version
  - Download from https://developer.apple.com/downloads
  - Install to `/Library/Developer/KDKs/`
- **CMake 3.18+**
- **LLVM/Clang** (ships with Xcode)
- **PureDarwin** target system (or QEMU VM with PureDarwin disk image)

---

## Build Steps

```sh
# 1. Clone darwin-wayland
git clone https://github.com/PureDarwin/darwin-wayland
cd darwin-wayland

# 2. Configure
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local

# 3. Build userspace components
cmake --build build

# 4. Build the KEXT (requires KDK)
xcodebuild -project drm-shim/kern/IODRMShim.xcodeproj -configuration Release

# 5. Sign the KEXT (self-signed for PureDarwin, SIP disabled)
codesign --force --sign "Darwin Development" \
         --entitlements drm-shim/kern/IODRMShim.entitlements \
         build/IODRMShim.kext

# 6. Install
sudo cmake --install build
sudo cp -r build/IODRMShim.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/IODRMShim.kext
```

---

## Runtime Startup Order

Services must start in this exact order:

```sh
# Step 1: Load the DRM kernel extension
sudo kextload /Library/Extensions/IODRMShim.kext
# Verify:
ls /dev/dri/      # should show card0 and renderD128

# Step 2: Start the evdev bridge (HID input → /dev/input/eventN)
sudo darwin-evdev-bridge &
# Verify:
ls /dev/input/    # should show event0 (keyboard), event1 (mouse)

# Step 3: Start the seat daemon
sudo seatd-darwin -g video &
# Verify:
ls /run/seatd.sock   # Unix socket should exist

# Step 4: Launch Weston (as a non-root user in the 'video' group)
export WAYLAND_DISPLAY=wayland-0
export XDG_RUNTIME_DIR=/run/user/$(id -u)
mkdir -p $XDG_RUNTIME_DIR
weston --backend=drm-backend.so --seat=seat0
```

On PureDarwin with launchd/XPC (PureDarwin/XPC project), steps 1-3
are handled automatically by the installed launchd plists.

---

## Integrating libwayland (kqueue event loop)

When building libwayland from source on Darwin:

```sh
git clone https://gitlab.freedesktop.org/wayland/wayland.git
cd wayland

# Apply the kqueue patch
patch -p1 < /path/to/darwin-wayland/libwayland-darwin/kqueue-event-loop.patch

# The patch replaces src/wayland-event-loop.c on Darwin with our kqueue version
cmake -B build \
    -DCMAKE_SYSTEM_NAME=Darwin \
    -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build && sudo cmake --install build
```

Or without patching — just replace the compiled object at link time:

```sh
# After building libwayland normally:
clang -c wayland-darwin-event-loop.c -o wayland_event_loop_darwin.o
# Replace the epoll object in the archive:
ar d /usr/local/lib/libwayland-server.a wayland-event-loop.o
ar r /usr/local/lib/libwayland-server.a wayland_event_loop_darwin.o
```

---

## Building wlroots on Darwin

Once libdrm-darwin and the kqueue libwayland are installed:

```sh
git clone https://gitlab.freedesktop.org/wlroots/wlroots.git
cd wlroots

# Apply Darwin compat patches (available in darwin-wayland/patches/wlroots/)
for p in ../darwin-wayland/patches/wlroots/*.patch; do patch -p1 < $p; done

PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
meson setup build \
    -Dbackends=drm,libinput \
    -Drenderers=pixman \
    -Dxwayland=disabled
ninja -C build
sudo ninja -C build install
```

---

## GPU Acceleration

### VM / QEMU (recommended for development)

Use QEMU with virtio-gpu for accelerated rendering:

```sh
qemu-system-x86_64 \
  -m 8192 \
  -cpu Penryn \
  -device virtio-vga-gl \
  -display sdl,gl=on \
  -drive file=puredarwin.img,format=raw \
  ...
```

virtio-gpu appears as an IOFramebuffer in IOKit. IODRMShim matches it
and Mesa's virgl gallium driver provides OpenGL acceleration.

### Real hardware (future)

For native GPU support, the next step is porting Intel's i915 or AMD's
amdgpu DRM driver as a Darwin KEXT, similar to FreeBSD's drm-kmod project.
See: https://github.com/freebsd/drm-kmod for reference.

---

## Key Sources Referenced

- `apple-oss-distributions/IOGraphics` — IOFramebuffer, IODisplay (open source)
- `apple-oss-distributions/IOHIDFamily` — IOHIDDevice, IOHIDManager (open source)
- `linux/include/uapi/drm/drm.h` — DRM ioctl ABI we replicate
- `swaywm/wlroots` — target compositor toolkit
- `kennylevinsen/seatd` — reference seatd implementation
- `PureDarwin/PureDarwin` — build system and base OS
- `PureDarwin/XPC` — launchd replacement

---

## License

Apache License 2.0. DRM ioctl header structures are from the Linux kernel
UAPI headers (GPL-2.0 with syscall exception — compatible with userspace use).
IOKit headers are from Apple's open-source distribution (APSL 2.0).
