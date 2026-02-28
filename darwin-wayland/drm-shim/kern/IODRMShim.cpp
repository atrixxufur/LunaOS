/*
 * IODRMShim.cpp — IOKit KEXT implementation
 *
 * Wraps IOFramebuffer (open-source IOGraphicsFamily) in a DRM-compatible
 * character device. This lets libdrm, wlroots, and Weston treat Darwin
 * exactly like a Linux KMS driver.
 *
 * Key design decisions:
 *   - Dumb buffers: allocated with IOBufferMemoryDescriptor (physically
 *     contiguous if <4MB, scattered otherwise). Not GPU-accelerated —
 *     that requires IOAcceleratorFamily. Sufficient for LLVMpipe/swrast.
 *   - Mode-setting: delegates entirely to IOFramebuffer::setDisplayMode().
 *     We translate IODisplayModeID/IOIndex ↔ drm_mode_modeinfo.
 *   - VBL: hooking IOFramebuffer's VBL notification callback and wiring it
 *     to SIGIO/kevent delivery for Wayland compositor page-flip events.
 *   - Single CRTC + single connector model: matches QEMU virtio-gpu and
 *     IOFramebuffer's one-display-per-IOService model.
 */

#include "IODRMShim.h"
#include <IOKit/graphics/IOGraphicsInterface.h>
#include <IOKit/graphics/IOGraphicsTypesPrivate.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <kern/assert.h>

/* ── Statics ─────────────────────────────────────────────────────────────── */

OSDefineMetaClassAndStructors(IODRMShim, IOService)
OSDefineMetaClassAndStructors(IODRMShimUserClient, IOUserClient)

IODRMShim *IODRMShim::_instances[8]  = {};
uint32_t   IODRMShim::_instanceCount = 0;

/* cdevsw index allocated at kext load time */
static int gDRMCdevsw = -1;

/* Character device switch table */
static struct cdevsw drm_cdevsw = {
    .d_open    = IODRMShim::_devOpen,
    .d_close   = IODRMShim::_devClose,
    .d_read    = eno_rdwrt,
    .d_write   = eno_rdwrt,
    .d_ioctl   = IODRMShim::_devIoctl,
    .d_stop    = eno_stop,
    .d_reset   = eno_reset,
    .d_ttys    = NULL,
    .d_select  = eno_select,
    .d_mmap    = IODRMShim::_devMmap,
    .d_strategy= eno_strat,
    .d_type    = 0,
};

/* ── IOService lifecycle ─────────────────────────────────────────────────── */

bool IODRMShim::init(OSDictionary *props) {
    if (!super::init(props)) return false;

    _framebuffer     = nullptr;
    _workLoop        = nullptr;
    _vblInterrupt    = nullptr;
    _gemLock         = nullptr;
    _isMaster        = false;
    _modeCount       = 0;
    _currentModeIdx  = 0;
    _nextMmapOffset  = 0x100000000ULL; /* start at 4GB: avoids ambiguity */
    _devNode         = 0;
    _devHandle       = nullptr;

    bzero(_gemTable, sizeof(_gemTable));
    bzero(_fboTable, sizeof(_fboTable));
    bzero(_modes,    sizeof(_modes));

    return true;
}

bool IODRMShim::start(IOService *provider) {
    IOReturn ret;

    if (!super::start(provider)) return false;

    /* Must match on IOFramebuffer */
    _framebuffer = OSDynamicCast(IOFramebuffer, provider);
    if (!_framebuffer) {
        IOLog("IODRMShim: provider is not IOFramebuffer\n");
        return false;
    }

    /* Allocate locks */
    _gemLock = IOLockAlloc();
    if (!_gemLock) return false;

    /* Work loop for VBL events */
    _workLoop = IOWorkLoop::workLoop();
    if (!_workLoop) return false;

    /* Register for VBL notifications from IOFramebuffer */
    /* IOFramebuffer::addFramebufferNotification is the public API */
    ret = _framebuffer->addFramebufferNotification(
        [](void *self, IOFramebuffer *fb, IOIndex event, void *info) -> IOReturn {
            if (event == kIOFBNotifyVBLMultiplier)
                static_cast<IODRMShim*>(self)->vblankInterrupt();
            return kIOReturnSuccess;
        },
        this, nullptr, kIOFBNotifyEvent_VBL, 0
    );
    if (ret != kIOReturnSuccess) {
        IOLog("IODRMShim: warning: could not register VBL notifier (%x)\n", ret);
        /* non-fatal: compositors can poll instead */
    }

    /* Populate display modes from IOFramebuffer */
    ret = _populateModes();
    if (ret != kIOReturnSuccess) {
        IOLog("IODRMShim: failed to populate modes (%x)\n", ret);
        return false;
    }

    /* Query current framebuffer geometry */
    IODisplayModeID  curMode;
    IOIndex          curDepth;
    IOPixelInformation pixInfo;

    _framebuffer->getCurrentDisplayMode(&curMode, &curDepth);
    _framebuffer->getPixelInformation(curMode, curDepth,
                                       kIOFBSystemAperture, &pixInfo);
    _fbWidth    = pixInfo.activeWidth;
    _fbHeight   = pixInfo.activeHeight;
    _fbPitch    = (uint32_t)pixInfo.bytesPerRow;
    _fbBpp      = pixInfo.bitsPerPixel;

    /* Get physical framebuffer base address from IODeviceMemory */
    IODeviceMemory *vramMem = _framebuffer->getVRAMRange();
    _fbPhysAddr = vramMem ? vramMem->getPhysicalAddress() : 0;

    /* Register cdevsw once (all instances share same major) */
    if (gDRMCdevsw < 0) {
        gDRMCdevsw = cdevsw_add(-1, &drm_cdevsw);
        if (gDRMCdevsw < 0) {
            IOLog("IODRMShim: cdevsw_add failed\n");
            return false;
        }
    }

    /* Install our character device under /dev/dri/ */
    _instanceIdx = _instanceCount;
    _instances[_instanceCount++] = this;
    _installCharDevice();

    IOLog("IODRMShim: started on %s → /dev/dri/card%u  %ux%u @%ubpp\n",
          provider->getName(), _instanceIdx, _fbWidth, _fbHeight, _fbBpp);
    return true;
}

void IODRMShim::stop(IOService *provider) {
    _removeCharDevice();
    if (_vblInterrupt) { _workLoop->removeEventSource(_vblInterrupt); }
    if (_workLoop)     { _workLoop->release(); }
    if (_gemLock)      { IOLockFree(_gemLock); }

    /* Free any leaked GEM objects */
    IOLockLock(_gemLock);
    for (int i = 0; i < DRM_DARWIN_MAX_GEM_HANDLES; i++) {
        if (_gemTable[i].in_use && _gemTable[i].mem) {
            _gemTable[i].mem->release();
        }
    }
    IOLockUnlock(_gemLock);

    super::stop(provider);
}

void IODRMShim::free() { super::free(); }

/* ── Character device install/remove ─────────────────────────────────────── */

void IODRMShim::_installCharDevice() {
    /* Create /dev/dri/ directory if needed (devfs_make_node creates parents) */
    /* card0 = primary node (mode-setting + rendering) */
    _devNode   = makedev(gDRMCdevsw, _instanceIdx);
    _devHandle = devfs_make_node(
        _devNode,
        DEVFS_CHAR,
        UID_ROOT, GID_WHEEL,
        0660,                /* owner=rw, group=rw — seatd grants access */
        "dri/card%d", _instanceIdx
    );
    /* renderD128 = render-only node (no mode-setting, unprivileged) */
    devfs_make_node(
        makedev(gDRMCdevsw, _instanceIdx + 128),
        DEVFS_CHAR,
        UID_ROOT, GID_VIDEO,
        0666,
        "dri/renderD%d", 128 + _instanceIdx
    );
}

void IODRMShim::_removeCharDevice() {
    if (_devHandle) {
        devfs_remove(_devHandle);
        _devHandle = nullptr;
    }
}

/* ── Mode population ─────────────────────────────────────────────────────── */

IOReturn IODRMShim::_populateModes() {
    IOReturn ret;
    IODisplayModeID *modeIds = nullptr;
    IOItemCount      modeCount = 0;

    ret = _framebuffer->getDisplayModeCount(&modeCount);
    if (ret != kIOReturnSuccess) return ret;

    modeIds = (IODisplayModeID *)IOMalloc(sizeof(IODisplayModeID) * modeCount);
    if (!modeIds) return kIOReturnNoMemory;

    ret = _framebuffer->getDisplayModes(modeIds);
    if (ret != kIOReturnSuccess) { IOFree(modeIds, sizeof(IODisplayModeID)*modeCount); return ret; }

    _modeCount = 0;
    for (IOItemCount i = 0; i < modeCount && _modeCount < DRM_DARWIN_MAX_MODES; i++) {
        IODisplayModeInformation modeInfo = {};
        if (_framebuffer->getInformationForDisplayMode(modeIds[i], &modeInfo)
                != kIOReturnSuccess) continue;

        /* Use the highest depth (index 0 is highest in IOKit convention) */
        IOPixelInformation pixInfo = {};
        if (_framebuffer->getPixelInformation(modeIds[i], 0,
                kIOFBSystemAperture, &pixInfo) != kIOReturnSuccess) continue;

        struct drm_mode_modeinfo *m = &_modes[_modeCount];
        bzero(m, sizeof(*m));

        m->hdisplay   = (uint16_t)modeInfo.nominalWidth;
        m->vdisplay   = (uint16_t)modeInfo.nominalHeight;
        m->vrefresh   = modeInfo.refreshRate >> 16; /* IOKit: 16.16 fixed-pt Hz */
        m->flags      = 0;
        m->type       = (i == 0) ? 0x48 : 0; /* DRM_MODE_TYPE_PREFERRED|DRIVER */

        /* Synthesise blanking timings using GTF approximation.
         * Real compositors only care about hdisplay/vdisplay/vrefresh. */
        uint32_t hz    = m->vrefresh ? m->vrefresh : 60;
        uint32_t hb    = (m->hdisplay * 30) / 100;  /* ~30% blanking */
        uint32_t vb    = (m->vdisplay * 5)  / 100;
        m->htotal      = m->hdisplay + hb;
        m->hsync_start = m->hdisplay + hb / 4;
        m->hsync_end   = m->hdisplay + hb / 2;
        m->hskew       = 0;
        m->vtotal      = m->vdisplay + vb;
        m->vsync_start = m->vdisplay + vb / 4;
        m->vsync_end   = m->vdisplay + vb / 2;
        m->vscan       = 0;
        m->clock       = (m->htotal * m->vtotal * hz) / 1000; /* kHz */

        snprintf(m->name, DRM_DISPLAY_MODE_LEN, "%dx%d",
                 m->hdisplay, m->vdisplay);
        _modeCount++;
    }

    IOFree(modeIds, sizeof(IODisplayModeID) * modeCount);
    return kIOReturnSuccess;
}

/* ── GEM buffer management ───────────────────────────────────────────────── */

IOReturn IODRMShim::gemCreate(uint32_t w, uint32_t h, uint32_t bpp,
                               uint32_t *hOut, uint32_t *pitchOut, uint64_t *sizeOut) {
    uint32_t pitch = ((w * bpp / 8) + 63) & ~63;  /* 64-byte aligned */
    uint64_t size  = (uint64_t)pitch * h;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    IOLockLock(_gemLock);
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < DRM_DARWIN_MAX_GEM_HANDLES; i++) {
        if (!_gemTable[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { IOLockUnlock(_gemLock); return kIOReturnNoResources; }

    /* Allocate physically contiguous wired memory.
     * For large buffers (>4MB) we use kIOMemoryPhysicallyContiguous=0
     * and rely on scatter-gather → acceptable for software rendering.
     * Hardware scanout would need kIOMemoryPhysicallyContiguous. */
    IOOptionBits memFlags = kIOMemoryKernelUserShared | kIOMemoryMapperNone;
    if (size <= 4 * 1024 * 1024) memFlags |= kIOMemoryPhysicallyContiguous;

    IOBufferMemoryDescriptor *mem =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            memFlags,
            size,
            0xFFFFF000ULL  /* 32-bit physical mask for safe DMA */
        );
    if (!mem) { IOLockUnlock(_gemLock); return kIOReturnNoMemory; }
    mem->prepare();

    darwin_gem_object *obj    = &_gemTable[slot];
    obj->handle               = (uint32_t)(slot + 1); /* 1-based */
    obj->width                = w;
    obj->height               = h;
    obj->pitch                = pitch;
    obj->bpp                  = bpp;
    obj->size                 = size;
    obj->mem                  = mem;
    obj->kernel_vaddr         = (vm_offset_t)mem->getBytesNoCopy();
    obj->mmap_offset          = _nextMmapOffset;
    obj->in_use               = true;
    _nextMmapOffset          += size;

    *hOut     = obj->handle;
    *pitchOut = pitch;
    *sizeOut  = size;

    IOLockUnlock(_gemLock);
    return kIOReturnSuccess;
}

IOReturn IODRMShim::gemMapOffset(uint32_t handle, uint64_t *offsetOut) {
    darwin_gem_object *obj = gemLookup(handle);
    if (!obj) return kIOReturnBadArgument;
    *offsetOut = obj->mmap_offset;
    return kIOReturnSuccess;
}

IOReturn IODRMShim::gemDestroy(uint32_t handle) {
    IOLockLock(_gemLock);
    darwin_gem_object *obj = gemLookup(handle);
    if (!obj) { IOLockUnlock(_gemLock); return kIOReturnBadArgument; }
    obj->mem->complete();
    obj->mem->release();
    bzero(obj, sizeof(*obj));
    IOLockUnlock(_gemLock);
    return kIOReturnSuccess;
}

darwin_gem_object *IODRMShim::gemLookup(uint32_t handle) {
    if (handle == 0 || handle > DRM_DARWIN_MAX_GEM_HANDLES) return nullptr;
    darwin_gem_object *obj = &_gemTable[handle - 1];
    return obj->in_use ? obj : nullptr;
}

/* ── Framebuffer object management ──────────────────────────────────────── */

IOReturn IODRMShim::fbCreate(uint32_t handle, uint32_t w, uint32_t h,
                              uint32_t pitch, uint32_t bpp, uint32_t depth,
                              uint32_t *fbIdOut) {
    /* Validate GEM handle */
    darwin_gem_object *obj = gemLookup(handle);
    if (!obj) return kIOReturnBadArgument;

    /* Find free FBO slot */
    for (int i = 0; i < 256; i++) {
        if (!_fboTable[i].valid) {
            _fboTable[i].id        = (uint32_t)(i + 1);
            _fboTable[i].gemHandle = handle;
            _fboTable[i].valid     = true;
            *fbIdOut               = _fboTable[i].id;
            return kIOReturnSuccess;
        }
    }
    return kIOReturnNoResources;
}

IOReturn IODRMShim::fbDestroy(uint32_t fbId) {
    for (int i = 0; i < 256; i++) {
        if (_fboTable[i].valid && _fboTable[i].id == fbId) {
            bzero(&_fboTable[i], sizeof(_fboTable[i]));
            return kIOReturnSuccess;
        }
    }
    return kIOReturnBadArgument;
}

IOReturn IODRMShim::setCrtcFB(uint32_t fbId, uint32_t x, uint32_t y,
                               const struct drm_mode_modeinfo *mode) {
    /* Look up the GEM object behind this framebuffer */
    darwin_gem_object *obj = nullptr;
    for (int i = 0; i < 256; i++) {
        if (_fboTable[i].valid && _fboTable[i].id == fbId) {
            obj = gemLookup(_fboTable[i].gemHandle);
            break;
        }
    }
    if (!obj) return kIOReturnBadArgument;

    /* Find the IODisplayModeID matching the requested mode */
    IODisplayModeID  targetModeId = 0;
    IOIndex          targetDepth  = 0;
    for (uint32_t i = 0; i < _modeCount; i++) {
        if (_modes[i].hdisplay == mode->hdisplay &&
            _modes[i].vdisplay == mode->vdisplay) {
            /* Map back to IOKit mode — store the index in the modeinfo
             * type field during _populateModes() for round-tripping. */
            targetModeId = (IODisplayModeID)(i + 1);
            targetDepth  = 0;
            break;
        }
    }

    if (targetModeId == 0) return kIOReturnUnsupported;

    IOReturn ret = _framebuffer->setDisplayMode(targetModeId, targetDepth);
    if (ret != kIOReturnSuccess) {
        IOLog("IODRMShim: setDisplayMode failed: %x\n", ret);
        return ret;
    }

    /* Blit the GEM buffer to the live framebuffer.
     * IOFramebuffer::setupForCurrentConfig() refreshes its internal state.
     * We then copy the GEM buffer content to the real VRAM aperture.
     * This is a CPU blit — acceptable for dumb-buffer software rendering. */
    IODeviceMemory *vram = _framebuffer->getVRAMRange();
    if (vram && obj->kernel_vaddr) {
        void *vramPtr = (void *)vram->getVirtualAddress();
        if (vramPtr)
            memcpy(vramPtr, (void *)obj->kernel_vaddr,
                   MIN(obj->size, (uint64_t)obj->pitch * obj->height));
    }

    return kIOReturnSuccess;
}

/* ── VBlank ──────────────────────────────────────────────────────────────── */

void IODRMShim::vblankInterrupt() {
    /* TODO: signal kqueue watchers / SIGIO for registered page-flip waiters.
     * For now, counting is sufficient for wlroots which polls with a
     * DRM_IOCTL_WAIT_VBLANK timeout. */
    _workLoop->runAction(^IOReturn {
        /* Future: iterate pending page-flip requests and deliver events */
        return kIOReturnSuccess;
    });
}

/* ── ioctl dispatcher ────────────────────────────────────────────────────── */

/*
 * The character device ioctl handler. All DRM ioctls from libdrm/wlroots
 * arrive here. We dispatch to the appropriate method.
 *
 * Note: 'data' is already copyin'd by the kernel for IOWR ioctls.
 */
int IODRMShim::_devIoctl(dev_t dev, u_long cmd, caddr_t data,
                          int flag, struct proc *p) {
    uint32_t unit = minor(dev) & 0x7f;
    if (unit >= _instanceCount || !_instances[unit]) return ENXIO;
    IODRMShim *self = _instances[unit];

    switch (cmd) {

    /* ── Version ────────────────────────────────────────────────────────── */
    case DRM_IOCTL_VERSION: {
        struct drm_version *v = (struct drm_version *)data;
        /* Only fill in lengths; pointers are user-space strings handled by libdrm */
        v->version_major = 1;
        v->version_minor = 6;
        v->version_patchlevel = 0;
        static const char name[] = "darwin-drm";
        static const char date[] = "20260101";
        static const char desc[] = "IODRMShim — Darwin DRM/KMS via IOFramebuffer";
        /* If the user passed non-null name/date/desc pointers, copyout strings */
        if (v->name && v->name_len >= sizeof(name))
            copyout(name, (user_addr_t)v->name, sizeof(name));
        if (v->date && v->date_len >= sizeof(date))
            copyout(date, (user_addr_t)v->date, sizeof(date));
        if (v->desc && v->desc_len >= sizeof(desc))
            copyout(desc, (user_addr_t)v->desc, sizeof(desc));
        v->name_len = sizeof(name) - 1;
        v->date_len = sizeof(date) - 1;
        v->desc_len = sizeof(desc) - 1;
        return 0;
    }

    /* ── Capabilities ───────────────────────────────────────────────────── */
    case DRM_IOCTL_GET_CAP: {
        struct drm_get_cap *cap = (struct drm_get_cap *)data;
        switch (cap->capability) {
            case DRM_CAP_DUMB_BUFFER:           cap->value = 1; break;
            case DRM_CAP_VBLANK_HIGH_CRTC:      cap->value = 0; break;
            case DRM_CAP_DUMB_PREFERRED_DEPTH:  cap->value = 32; break;
            case DRM_CAP_DUMB_PREFER_SHADOW:    cap->value = 1; break;
            case DRM_CAP_PRIME:                 cap->value = 0; break;
            case DRM_CAP_TIMESTAMP_MONOTONIC:   cap->value = 1; break;
            case DRM_CAP_CURSOR_WIDTH:          cap->value = 64; break;
            case DRM_CAP_CURSOR_HEIGHT:         cap->value = 64; break;
            case DRM_CAP_ADDFB2_MODIFIERS:      cap->value = 0; break;
            default:                            return EINVAL;
        }
        return 0;
    }

    case DRM_IOCTL_SET_CLIENT_CAP:
        /* Accept all client caps (atomic, universal planes etc.) */
        return 0;

    /* ── Master ─────────────────────────────────────────────────────────── */
    case DRM_IOCTL_SET_MASTER:
        return self->setMaster()  == kIOReturnSuccess ? 0 : EACCES;
    case DRM_IOCTL_DROP_MASTER:
        return self->dropMaster() == kIOReturnSuccess ? 0 : EACCES;

    /* ── Auth (stub: single-process model) ──────────────────────────────── */
    case DRM_IOCTL_GET_MAGIC:
        ((struct drm_auth*)data)->magic = 1;
        return 0;
    case DRM_IOCTL_AUTH_MAGIC:
        return 0;

    /* ── Mode resources ─────────────────────────────────────────────────── */
    case DRM_IOCTL_MODE_GETRESOURCES: {
        struct drm_mode_card_res *res = (struct drm_mode_card_res *)data;
        uint32_t crtc_id   = DRM_DARWIN_CRTC_ID;
        uint32_t conn_id   = DRM_DARWIN_CONNECTOR_ID;
        uint32_t enc_id    = DRM_DARWIN_ENCODER_ID;

        if (res->count_crtcs >= 1 && res->crtc_id_ptr)
            copyout(&crtc_id, (user_addr_t)res->crtc_id_ptr, sizeof(uint32_t));
        if (res->count_connectors >= 1 && res->connector_id_ptr)
            copyout(&conn_id, (user_addr_t)res->connector_id_ptr, sizeof(uint32_t));
        if (res->count_encoders >= 1 && res->encoder_id_ptr)
            copyout(&enc_id, (user_addr_t)res->encoder_id_ptr, sizeof(uint32_t));

        res->count_crtcs      = 1;
        res->count_connectors = 1;
        res->count_encoders   = 1;
        res->count_fbs        = 0;
        res->min_width  = 1;    res->max_width  = 16384;
        res->min_height = 1;    res->max_height = 16384;
        return 0;
    }

    case DRM_IOCTL_MODE_GETCRTC: {
        struct drm_mode_crtc *crtc = (struct drm_mode_crtc *)data;
        if (crtc->crtc_id != DRM_DARWIN_CRTC_ID) return EINVAL;
        crtc->x = crtc->y = 0;
        crtc->mode_valid = 1;
        /* Return current mode */
        if (self->_modeCount > 0)
            crtc->mode = self->_modes[self->_currentModeIdx];
        return 0;
    }

    case DRM_IOCTL_MODE_SETCRTC: {
        struct drm_mode_crtc *crtc = (struct drm_mode_crtc *)data;
        if (crtc->crtc_id != DRM_DARWIN_CRTC_ID) return EINVAL;
        if (crtc->fb_id == 0) return 0; /* disable: no-op for now */

        IOReturn ret = self->setCrtcFB(crtc->fb_id, crtc->x, crtc->y,
                                        &crtc->mode);
        return ret == kIOReturnSuccess ? 0 : EIO;
    }

    case DRM_IOCTL_MODE_GETCONNECTOR: {
        struct drm_mode_get_connector *conn =
            (struct drm_mode_get_connector *)data;
        if (conn->connector_id != DRM_DARWIN_CONNECTOR_ID) return EINVAL;

        /* Connector metadata */
        conn->encoder_id       = DRM_DARWIN_ENCODER_ID;
        conn->connector_type   = DRM_MODE_CONNECTOR_Virtual;
        conn->connector_type_id = 1;
        conn->connection       = 1; /* always connected */
        conn->mm_width         = (self->_fbWidth  * 254) / 10000; /* ~96dpi */
        conn->mm_height        = (self->_fbHeight * 254) / 10000;
        conn->subpixel         = 1; /* SubPixelHorizontalRGB */

        /* Copy mode list */
        uint32_t copyCount = MIN(conn->count_modes, self->_modeCount);
        if (copyCount && conn->modes_ptr)
            copyout(self->_modes, (user_addr_t)conn->modes_ptr,
                    sizeof(struct drm_mode_modeinfo) * copyCount);
        conn->count_modes    = self->_modeCount;
        conn->count_encoders = 1;
        conn->count_props    = 0;

        if (conn->count_encoders >= 1 && conn->encoders_ptr) {
            uint32_t enc_id = DRM_DARWIN_ENCODER_ID;
            copyout(&enc_id, (user_addr_t)conn->encoders_ptr, sizeof(uint32_t));
        }
        return 0;
    }

    case DRM_IOCTL_MODE_GETENCODER: {
        struct drm_mode_get_encoder *enc =
            (struct drm_mode_get_encoder *)data;
        if (enc->encoder_id != DRM_DARWIN_ENCODER_ID) return EINVAL;
        enc->encoder_type    = 2; /* DRM_MODE_ENCODER_TMDS */
        enc->crtc_id         = DRM_DARWIN_CRTC_ID;
        enc->possible_crtcs  = 1;
        enc->possible_clones = 0;
        return 0;
    }

    /* ── Framebuffer objects ─────────────────────────────────────────────── */
    case DRM_IOCTL_MODE_ADDFB: {
        struct drm_mode_fb_cmd *fb = (struct drm_mode_fb_cmd *)data;
        IOReturn ret = self->fbCreate(fb->handle, fb->width, fb->height,
                                       fb->pitch, fb->bpp, fb->depth,
                                       &fb->fb_id);
        return ret == kIOReturnSuccess ? 0 : ENOMEM;
    }

    case DRM_IOCTL_MODE_ADDFB2: {
        struct drm_mode_fb_cmd2 *fb = (struct drm_mode_fb_cmd2 *)data;
        /* Extract bpp from fourcc: we support XRGB8888 (32bpp) and RGB565 */
        uint32_t bpp = (fb->pixel_format == 0x34325258 /*XR24*/) ? 32 : 16;
        uint32_t depth = (bpp == 32) ? 24 : 16;
        IOReturn ret = self->fbCreate(fb->handles[0], fb->width, fb->height,
                                       fb->pitches[0], bpp, depth, &fb->fb_id);
        return ret == kIOReturnSuccess ? 0 : ENOMEM;
    }

    case DRM_IOCTL_MODE_RMFB: {
        uint32_t fbId = *(uint32_t *)data;
        IOReturn ret  = self->fbDestroy(fbId);
        return ret == kIOReturnSuccess ? 0 : EINVAL;
    }

    /* ── Dumb buffers ────────────────────────────────────────────────────── */
    case DRM_IOCTL_MODE_CREATE_DUMB: {
        struct drm_mode_create_dumb *d = (struct drm_mode_create_dumb *)data;
        IOReturn ret = self->gemCreate(d->width, d->height, d->bpp,
                                        &d->handle, &d->pitch, &d->size);
        return ret == kIOReturnSuccess ? 0 : ENOMEM;
    }

    case DRM_IOCTL_MODE_MAP_DUMB: {
        struct drm_mode_map_dumb *d = (struct drm_mode_map_dumb *)data;
        IOReturn ret = self->gemMapOffset(d->handle, &d->offset);
        return ret == kIOReturnSuccess ? 0 : EINVAL;
    }

    case DRM_IOCTL_MODE_DESTROY_DUMB: {
        struct drm_mode_destroy_dumb *d = (struct drm_mode_destroy_dumb *)data;
        IOReturn ret = self->gemDestroy(d->handle);
        return ret == kIOReturnSuccess ? 0 : EINVAL;
    }

    /* ── GEM close ───────────────────────────────────────────────────────── */
    case DRM_IOCTL_GEM_CLOSE: {
        struct drm_gem_close *g = (struct drm_gem_close *)data;
        IOReturn ret = self->gemDestroy(g->handle);
        return ret == kIOReturnSuccess ? 0 : EINVAL;
    }

    /* ── VBlank wait ─────────────────────────────────────────────────────── */
    case DRM_IOCTL_WAIT_VBLANK: {
        /* Minimal implementation: sleep for one frame period (16ms @ 60hz)
         * then return. wlroots uses this for pacing. A proper implementation
         * would block on a semaphore signalled from vblankInterrupt(). */
        IOSleep(16);
        union drm_wait_vblank *vbl = (union drm_wait_vblank *)data;
        struct timespec ts;
        clock_get_calendar_nanotime((uint32_t*)&ts.tv_sec, (uint32_t*)&ts.tv_nsec);
        vbl->reply.tval_sec  = ts.tv_sec;
        vbl->reply.tval_usec = ts.tv_nsec / 1000;
        return 0;
    }

    /* ── Darwin extension: raw IOFramebuffer info ────────────────────────── */
    case DRM_IOCTL_DARWIN_GET_FBINFO: {
        struct drm_darwin_ioframebuffer_info *info =
            (struct drm_darwin_ioframebuffer_info *)data;
        info->fb_phys_addr  = self->_fbPhysAddr;
        info->width         = self->_fbWidth;
        info->height        = self->_fbHeight;
        info->rowbytes      = self->_fbPitch;
        info->pixel_format  = 0x00000008; /* kIO32ARGBPixelFormat */
        info->depth         = self->_fbBpp;
        return 0;
    }

    default:
        IOLog("IODRMShim: unhandled ioctl 0x%lx\n", cmd);
        return ENOTTY;
    }
}

/* ── mmap: map GEM buffer into user process ─────────────────────────────── */

int IODRMShim::_devMmap(dev_t dev, vm_map_offset_t *addrp,
                         vm_size_t size, int prot) {
    uint32_t unit = minor(dev) & 0x7f;
    if (unit >= _instanceCount || !_instances[unit]) return ENXIO;
    IODRMShim *self = _instances[unit];

    /* Find GEM object by mmap_offset (passed via MAP_SHARED offset) */
    IOLockLock(self->_gemLock);
    for (int i = 0; i < DRM_DARWIN_MAX_GEM_HANDLES; i++) {
        darwin_gem_object *obj = &self->_gemTable[i];
        if (!obj->in_use) continue;
        if (obj->mmap_offset == (uint64_t)*addrp) {
            /* Map the IOBufferMemoryDescriptor into user task */
            IOMemoryMap *mapping = obj->mem->createMappingInTask(
                current_task(), 0,
                kIOMapAnywhere | kIOMapReadOnly /* TODO: prot translation */,
                0, obj->size
            );
            if (!mapping) {
                IOLockUnlock(self->_gemLock);
                return ENOMEM;
            }
            *addrp = mapping->getAddress();
            /* mapping is retained by kernel; will be released on unmap */
            IOLockUnlock(self->_gemLock);
            return 0;
        }
    }
    IOLockUnlock(self->_gemLock);
    return EINVAL;
}

/* ── open/close (simple ref counting) ───────────────────────────────────── */

int IODRMShim::_devOpen(dev_t dev, int flag, int devtype, struct proc *p) {
    uint32_t unit = minor(dev) & 0x7f;
    if (unit >= _instanceCount || !_instances[unit]) return ENXIO;
    return 0;
}

int IODRMShim::_devClose(dev_t dev, int flag, int devtype, struct proc *p) {
    return 0;
}

/* ── IOUserClient (for IOServiceOpen path) ──────────────────────────────── */

IOReturn IODRMShim::newUserClient(task_t owningTask, void *securityID,
                                   UInt32 type, IOUserClient **handler) {
    IODRMShimUserClient *client = OSTypeAlloc(IODRMShimUserClient);
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type, nullptr)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->attach(this) || !client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }
    *handler = client;
    return kIOReturnSuccess;
}

/* ── IODRMShimUserClient methods ─────────────────────────────────────────── */

bool IODRMShimUserClient::initWithTask(task_t task, void *security,
                                        UInt32 type, OSDictionary *props) {
    if (!super::initWithTask(task, security, type, props)) return false;
    _task = task;
    return true;
}

bool IODRMShimUserClient::start(IOService *provider) {
    if (!super::start(provider)) return false;
    _provider = OSDynamicCast(IODRMShim, provider);
    return _provider != nullptr;
}

IOReturn IODRMShimUserClient::clientClose() {
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

void IODRMShimUserClient::free() { super::free(); }

IOReturn IODRMShimUserClient::clientMemoryForType(UInt32 type,
                                                   IOOptionBits *opts,
                                                   IOMemoryDescriptor **mem) {
    /* type = GEM handle, map the corresponding buffer */
    darwin_gem_object *obj = _provider->gemLookup(type);
    if (!obj) return kIOReturnBadArgument;
    obj->mem->retain();
    *mem  = obj->mem;
    *opts = kIOMapAnywhere;
    return kIOReturnSuccess;
}

IOReturn IODRMShimUserClient::externalMethod(uint32_t selector,
                                              IOExternalMethodArguments *args,
                                              IOExternalMethodDispatch *,
                                              OSObject *, void *) {
    /* Future: expose gem_create / gem_destroy via external method for
     * processes that prefer the IOKit MIG path over the cdev ioctl path. */
    return kIOReturnUnsupported;
}
