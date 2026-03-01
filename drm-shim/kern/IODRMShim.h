/*
 * IODRMShim.h — IOKit KEXT header
 *
 * IODRMShim is an IOKit kernel extension that:
 *   1. Matches on IOFramebuffer providers in the IOService plane
 *   2. Creates a character device at /dev/dri/card0
 *   3. Implements a DRM-compatible ioctl interface over IOFramebuffer APIs
 *   4. Manages dumb buffer allocation in VRAM (or system RAM fallback)
 *   5. Forwards VBL (vertical blank) events as DRM page-flip events
 *
 * Dependencies (all open-source in apple-oss-distributions/macos-262):
 *   - IOGraphicsFamily: IOFramebuffer, IODisplay, IODisplayWrangler
 *   - IOKit/IOService, IOKit/IOUserClient
 *   - xnu: IOMemoryDescriptor, IOBufferMemoryDescriptor, semaphore
 *
 * Build: xcodebuild -target IODRMShim -configuration Release
 * Install: kextload /Library/Extensions/IODRMShim.kext
 * Verify:  ls /dev/dri/   →  card0  renderD128
 */

#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLib.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IOGraphicsInterface.h>
#include <sys/conf.h>
#include <miscfs/devfs/devfs.h>
#include "drm_darwin.h"

/* Maximum concurrent GEM buffer handles per open file descriptor */
#define DRM_DARWIN_MAX_GEM_HANDLES  4096

/* Maximum display modes we report to userspace */
#define DRM_DARWIN_MAX_MODES        64

/* Fixed IDs for our single-CRTC, single-connector setup */
#define DRM_DARWIN_CRTC_ID          1
#define DRM_DARWIN_ENCODER_ID       1
#define DRM_DARWIN_CONNECTOR_ID     1

/* ── GEM buffer descriptor ───────────────────────────────────────────────── */

struct darwin_gem_object {
    uint32_t    handle;
    uint32_t    width, height, pitch, bpp;
    uint64_t    size;
    vm_offset_t kernel_vaddr;   /* kernel VA of backing pages */
    IOBufferMemoryDescriptor *mem;
    uint64_t    mmap_offset;    /* mmap cookie returned to userspace */
    bool        in_use;
};

/* ── IODRMShim: the main IOService ───────────────────────────────────────── */

class IODRMShimUserClient;

class IODRMShim : public IOService {
    OSDeclareDefaultStructors(IODRMShim)

public:
    /* IOService lifecycle */
    bool        init(OSDictionary *properties) override;
    bool        start(IOService *provider)     override;
    void        stop(IOService *provider)      override;
    void        free()                         override;

    /* IOUserClient factory */
    IOReturn    newUserClient(task_t           owningTask,
                              void            *securityID,
                              UInt32           type,
                              IOUserClient   **handler) override;

    /* Display mode query (called from user client ioctl handlers) */
    IOReturn    getModeInfo(uint32_t modeIdx,
                            struct drm_mode_modeinfo *out);
    uint32_t    getModeCount() { return _modeCount; }
    IOReturn    setMode(uint32_t modeIdx);

    /* Current framebuffer geometry */
    uint32_t    getWidth()    { return _fbWidth; }
    uint32_t    getHeight()   { return _fbHeight; }
    uint32_t    getPitch()    { return _fbPitch; }
    uint32_t    getBpp()      { return _fbBpp; }
    uint64_t    getPhysAddr() { return _fbPhysAddr; }

    /* GEM buffer management */
    IOReturn    gemCreate(uint32_t w, uint32_t h, uint32_t bpp,
                          uint32_t *handleOut, uint32_t *pitchOut,
                          uint64_t *sizeOut);
    IOReturn    gemMapOffset(uint32_t handle, uint64_t *offsetOut);
    IOReturn    gemDestroy(uint32_t handle);
    darwin_gem_object *gemLookup(uint32_t handle);

    /* Framebuffer object management */
    IOReturn    fbCreate(uint32_t handle, uint32_t w, uint32_t h,
                         uint32_t pitch, uint32_t bpp, uint32_t depth,
                         uint32_t *fbIdOut);
    IOReturn    fbDestroy(uint32_t fbId);
    IOReturn    setCrtcFB(uint32_t fbId, uint32_t x, uint32_t y,
                          const struct drm_mode_modeinfo *mode);

    /* VBL / page-flip notifications */
    IOReturn    waitVBlank(uint32_t sequence, uint64_t userdata);
    void        vblankInterrupt();

    /* DRM master / auth (stub — single-client model for now) */
    IOReturn    setMaster()  { _isMaster = true;  return kIOReturnSuccess; }
    IOReturn    dropMaster() { _isMaster = false; return kIOReturnSuccess; }
    bool        isMaster()   { return _isMaster; }

private:
    IOFramebuffer           *_framebuffer;   /* our IOFramebuffer provider */
    IOWorkLoop              *_workLoop;
    IOInterruptEventSource  *_vblInterrupt;

    /* Current display mode */
    uint32_t    _fbWidth, _fbHeight, _fbPitch, _fbBpp;
    uint64_t    _fbPhysAddr;
    uint32_t    _currentModeIdx;

    /* Mode table (populated from IOFramebuffer::getDisplayModeCount/ID) */
    struct drm_mode_modeinfo _modes[DRM_DARWIN_MAX_MODES];
    uint32_t                 _modeCount;

    /* GEM handle table (indexed by handle-1, handle 0 is invalid) */
    darwin_gem_object    _gemTable[DRM_DARWIN_MAX_GEM_HANDLES];
    IOLock              *_gemLock;
    uint64_t             _nextMmapOffset;   /* monotonically increasing */

    /* Framebuffer objects (FBO id → gem handle) */
    struct { uint32_t id; uint32_t gemHandle; bool valid; }
                         _fboTable[256];

    /* Character device state */
    dev_t        _devNode;
    void        *_devHandle;   /* devfs handle for cleanup */

    bool         _isMaster;

    /* helpers */
    IOReturn    _populateModes();
    IOReturn    _iofbModeToModeInfo(IODisplayModeID modeId,
                                     IOIndex depth,
                                     struct drm_mode_modeinfo *out);
    void        _installCharDevice();
    void        _removeCharDevice();

    /* Character device switch (static trampolines → IODRMShim methods) */
    static int  _devOpen (dev_t dev, int flag, int devtype, struct proc *p);
    static int  _devClose(dev_t dev, int flag, int devtype, struct proc *p);
    static int  _devIoctl(dev_t dev, u_long cmd, caddr_t data,
                          int flag, struct proc *p);
    static int  _devMmap (dev_t dev, vm_map_offset_t *addrp,
                          vm_size_t size, int prot);

    /* Singleton: one shim per framebuffer */
    static IODRMShim *_instances[8];
    static uint32_t   _instanceCount;
    uint32_t          _instanceIdx;
};

/* ── IODRMShimUserClient ─────────────────────────────────────────────────── */
/*
 * Provides a Mach port connection to the shim for user-space processes
 * that open /dev/dri/card0 via IOServiceOpen(). The character device
 * ioctl path is the primary interface; this client is used for memory
 * mapping and privileged operations that require task-level tracking.
 */

class IODRMShimUserClient : public IOUserClient {
    OSDeclareDefaultStructors(IODRMShimUserClient)

public:
    bool        initWithTask(task_t owningTask, void *securityToken,
                             UInt32 type, OSDictionary *properties) override;
    bool        start(IOService *provider) override;
    IOReturn    clientClose()              override;
    void        free()                     override;

    /* Memory mapping for dumb buffers */
    IOReturn    clientMemoryForType(UInt32 type,
                                    IOOptionBits *options,
                                    IOMemoryDescriptor **memory) override;

    /* External method dispatch table */
    IOReturn    externalMethod(uint32_t selector,
                               IOExternalMethodArguments *args,
                               IOExternalMethodDispatch *dispatch,
                               OSObject *target,
                               void *reference)               override;

private:
    IODRMShim  *_provider;
    task_t      _task;
};
