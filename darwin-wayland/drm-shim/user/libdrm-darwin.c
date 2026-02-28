/*
 * libdrm-darwin.c — userspace DRM library for Darwin
 *
 * Provides the same public symbol surface as libdrm (libdrm.so.2) so that
 * Wayland compositors (wlroots, Weston), Mesa, and other DRM clients compile
 * and link without modification.  Internally this opens /dev/dri/card0 and
 * issues ioctls to IODRMShim.kext.
 *
 * Build:
 *   clang -dynamiclib -o libdrm.dylib libdrm-darwin.c \
 *         -I../include -install_name /usr/local/lib/libdrm.dylib
 *
 * Install:
 *   cp libdrm.dylib /usr/local/lib/
 *   ln -sf libdrm.dylib /usr/local/lib/libdrm.2.dylib
 *
 * Then build wlroots/Mesa with:
 *   PKG_CONFIG_PATH=/usr/local/lib/pkgconfig cmake ...
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "drm_darwin.h"
#include "libdrm-darwin.h"

/* ── Internal device open ─────────────────────────────────────────────────── */

int drmOpen(const char *name, const char *busId) {
    (void)name; (void)busId;
    /* Try card0..card3 */
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    return -1;
}

int drmOpenWithType(const char *name, const char *busId, int type) {
    (void)type;
    return drmOpen(name, busId);
}

int drmClose(int fd) {
    return close(fd);
}

/* ── Version ──────────────────────────────────────────────────────────────── */

drmVersionPtr drmGetVersion(int fd) {
    drmVersionPtr v = (drmVersionPtr)calloc(1, sizeof(*v));
    if (!v) return NULL;

    struct drm_version kv = {};
    char name[64] = {}, date[16] = {}, desc[128] = {};
    kv.name     = (uint64_t)(uintptr_t)name;
    kv.name_len = sizeof(name);
    kv.date     = (uint64_t)(uintptr_t)date;
    kv.date_len = sizeof(date);
    kv.desc     = (uint64_t)(uintptr_t)desc;
    kv.desc_len = sizeof(desc);

    if (ioctl(fd, DRM_IOCTL_VERSION, &kv) < 0) {
        free(v);
        return NULL;
    }
    v->version_major    = kv.version_major;
    v->version_minor    = kv.version_minor;
    v->version_patchlevel = kv.version_patchlevel;
    v->name             = strdup(name);
    v->date             = strdup(date);
    v->desc             = strdup(desc);
    v->name_len         = kv.name_len;
    v->date_len         = kv.date_len;
    v->desc_len         = kv.desc_len;
    return v;
}

void drmFreeVersion(drmVersionPtr v) {
    if (!v) return;
    free(v->name);
    free(v->date);
    free(v->desc);
    free(v);
}

/* ── Capabilities ─────────────────────────────────────────────────────────── */

int drmGetCap(int fd, uint64_t capability, uint64_t *value) {
    struct drm_get_cap cap = { .capability = capability };
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0) return -errno;
    *value = cap.value;
    return 0;
}

int drmSetClientCap(int fd, uint64_t capability, uint64_t value) {
    struct drm_set_client_cap c = { .capability = capability, .value = value };
    return ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &c) < 0 ? -errno : 0;
}

/* ── Authentication ───────────────────────────────────────────────────────── */

int drmGetMagic(int fd, drm_magic_t *magic) {
    struct drm_auth auth = {};
    if (ioctl(fd, DRM_IOCTL_GET_MAGIC, &auth) < 0) return -errno;
    *magic = auth.magic;
    return 0;
}

int drmAuthMagic(int fd, drm_magic_t magic) {
    struct drm_auth auth = { .magic = magic };
    return ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &auth) < 0 ? -errno : 0;
}

int drmSetMaster(int fd) {
    return ioctl(fd, DRM_IOCTL_SET_MASTER, NULL) < 0 ? -errno : 0;
}

int drmDropMaster(int fd) {
    return ioctl(fd, DRM_IOCTL_DROP_MASTER, NULL) < 0 ? -errno : 0;
}

/* ── Mode resources ───────────────────────────────────────────────────────── */

drmModeResPtr drmModeGetResources(int fd) {
    struct drm_mode_card_res res = {};

    /* First call: get counts */
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) return NULL;

    /* Allocate arrays */
    uint32_t *crtcs      = (uint32_t *)calloc(res.count_crtcs,      sizeof(uint32_t));
    uint32_t *connectors = (uint32_t *)calloc(res.count_connectors,  sizeof(uint32_t));
    uint32_t *encoders   = (uint32_t *)calloc(res.count_encoders,    sizeof(uint32_t));
    uint32_t *fbs        = (uint32_t *)calloc(res.count_fbs + 1,     sizeof(uint32_t));

    res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
    res.encoder_id_ptr   = (uint64_t)(uintptr_t)encoders;
    res.fb_id_ptr        = (uint64_t)(uintptr_t)fbs;

    /* Second call: fill arrays */
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        free(crtcs); free(connectors); free(encoders); free(fbs);
        return NULL;
    }

    drmModeResPtr r = (drmModeResPtr)calloc(1, sizeof(*r));
    r->count_fbs         = res.count_fbs;
    r->count_crtcs       = res.count_crtcs;
    r->count_connectors  = res.count_connectors;
    r->count_encoders    = res.count_encoders;
    r->min_width         = res.min_width;
    r->max_width         = res.max_width;
    r->min_height        = res.min_height;
    r->max_height        = res.max_height;
    r->fbs               = fbs;
    r->crtcs             = crtcs;
    r->connectors        = connectors;
    r->encoders          = encoders;
    return r;
}

void drmModeFreeResources(drmModeResPtr res) {
    if (!res) return;
    free(res->fbs);
    free(res->crtcs);
    free(res->connectors);
    free(res->encoders);
    free(res);
}

drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t crtcId) {
    struct drm_mode_crtc crtc = { .crtc_id = crtcId };
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0) return NULL;
    drmModeCrtcPtr c = (drmModeCrtcPtr)calloc(1, sizeof(*c));
    c->crtc_id     = crtc.crtc_id;
    c->buffer_id   = crtc.fb_id;
    c->x = crtc.x; c->y = crtc.y;
    c->mode_valid  = crtc.mode_valid;
    memcpy(&c->mode, &crtc.mode, sizeof(crtc.mode));
    c->gamma_size  = crtc.gamma_size;
    return c;
}

int drmModeSetCrtc(int fd, uint32_t crtcId, uint32_t bufferId,
                   uint32_t x, uint32_t y,
                   uint32_t *connectors, int count,
                   drmModeModeInfoPtr mode) {
    struct drm_mode_crtc crtc = {
        .crtc_id             = crtcId,
        .fb_id               = bufferId,
        .x = x, .y = y,
        .count_connectors    = (uint32_t)count,
        .set_connectors_ptr  = (uint64_t)(uintptr_t)connectors,
        .mode_valid          = mode ? 1 : 0,
    };
    if (mode) memcpy(&crtc.mode, mode, sizeof(*mode));
    return ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0 ? -errno : 0;
}

void drmModeFreeCrtc(drmModeCrtcPtr crtc) { free(crtc); }

/* ── Connector ────────────────────────────────────────────────────────────── */

drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t connId) {
    struct drm_mode_get_connector conn = { .connector_id = connId };

    /* First pass: get counts */
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) return NULL;

    struct drm_mode_modeinfo *modes =
        (struct drm_mode_modeinfo *)calloc(conn.count_modes, sizeof(*modes));
    uint32_t *encoders = (uint32_t *)calloc(conn.count_encoders, sizeof(uint32_t));

    conn.modes_ptr    = (uint64_t)(uintptr_t)modes;
    conn.encoders_ptr = (uint64_t)(uintptr_t)encoders;

    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
        free(modes); free(encoders);
        return NULL;
    }

    drmModeConnectorPtr c = (drmModeConnectorPtr)calloc(1, sizeof(*c));
    c->connector_id       = conn.connector_id;
    c->encoder_id         = conn.encoder_id;
    c->connector_type     = conn.connector_type;
    c->connector_type_id  = conn.connector_type_id;
    c->connection         = (drmModeConnection)conn.connection;
    c->mmWidth            = conn.mm_width;
    c->mmHeight           = conn.mm_height;
    c->subpixel           = (drmModeSubPixel)conn.subpixel;
    c->count_modes        = conn.count_modes;
    c->count_encoders     = conn.count_encoders;
    c->count_props        = 0;
    c->modes              = (drmModeModeInfoPtr)modes;
    c->encoders           = encoders;
    c->props              = NULL;
    c->prop_values        = NULL;
    return c;
}

void drmModeFreeConnector(drmModeConnectorPtr conn) {
    if (!conn) return;
    free(conn->modes);
    free(conn->encoders);
    free(conn->props);
    free(conn->prop_values);
    free(conn);
}

/* ── Encoder ──────────────────────────────────────────────────────────────── */

drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t encId) {
    struct drm_mode_get_encoder enc = { .encoder_id = encId };
    if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0) return NULL;
    drmModeEncoderPtr e = (drmModeEncoderPtr)calloc(1, sizeof(*e));
    e->encoder_id      = enc.encoder_id;
    e->encoder_type    = enc.encoder_type;
    e->crtc_id         = enc.crtc_id;
    e->possible_crtcs  = enc.possible_crtcs;
    e->possible_clones = enc.possible_clones;
    return e;
}

void drmModeFreeEncoder(drmModeEncoderPtr enc) { free(enc); }

/* ── Framebuffer ──────────────────────────────────────────────────────────── */

int drmModeAddFB(int fd, uint32_t width, uint32_t height,
                 uint8_t depth, uint8_t bpp, uint32_t pitch,
                 uint32_t bo_handle, uint32_t *buf_id) {
    struct drm_mode_fb_cmd fb = {
        .width = width, .height = height,
        .pitch = pitch, .bpp = bpp, .depth = depth,
        .handle = bo_handle,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) return -errno;
    *buf_id = fb.fb_id;
    return 0;
}

int drmModeAddFB2(int fd, uint32_t width, uint32_t height,
                  uint32_t pixel_format,
                  const uint32_t bo_handles[4],
                  const uint32_t pitches[4],
                  const uint32_t offsets[4],
                  uint32_t *buf_id, uint32_t flags) {
    struct drm_mode_fb_cmd2 fb = {
        .width = width, .height = height,
        .pixel_format = pixel_format,
        .flags = flags,
    };
    memcpy(fb.handles, bo_handles, 4 * sizeof(uint32_t));
    memcpy(fb.pitches, pitches,    4 * sizeof(uint32_t));
    memcpy(fb.offsets, offsets,    4 * sizeof(uint32_t));
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) return -errno;
    *buf_id = fb.fb_id;
    return 0;
}

int drmModeRmFB(int fd, uint32_t bufferId) {
    return ioctl(fd, DRM_IOCTL_MODE_RMFB, &bufferId) < 0 ? -errno : 0;
}

/* ── Dumb buffer ──────────────────────────────────────────────────────────── */

int drmModeCreateDumbBuffer(int fd, uint32_t width, uint32_t height,
                             uint32_t bpp, uint32_t flags,
                             uint32_t *handle, uint32_t *pitch, uint64_t *size) {
    struct drm_mode_create_dumb d = {
        .width = width, .height = height, .bpp = bpp, .flags = flags
    };
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &d) < 0) return -errno;
    *handle = d.handle;
    *pitch  = d.pitch;
    *size   = d.size;
    return 0;
}

int drmModeMapDumbBuffer(int fd, uint32_t handle, uint64_t *offset) {
    struct drm_mode_map_dumb d = { .handle = handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &d) < 0) return -errno;
    *offset = d.offset;
    return 0;
}

int drmModeDestroyDumbBuffer(int fd, uint32_t handle) {
    struct drm_mode_destroy_dumb d = { .handle = handle };
    return ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d) < 0 ? -errno : 0;
}

/* ── mmap a dumb buffer into process address space ────────────────────────── */

void *drmModeMapDumb(int fd, uint32_t handle, size_t size) {
    uint64_t offset = 0;
    if (drmModeMapDumbBuffer(fd, handle, &offset) < 0) return MAP_FAILED;
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)offset);
}

/* ── Page flip ────────────────────────────────────────────────────────────── */

int drmModePageFlip(int fd, uint32_t crtcId, uint32_t fbId,
                    uint32_t flags, void *user_data) {
    struct drm_mode_crtc_page_flip flip = {
        .crtc_id  = crtcId,
        .fb_id    = fbId,
        .flags    = flags,
        .user_data = (uint64_t)(uintptr_t)user_data,
    };
    return ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0 ? -errno : 0;
}

/* ── VBlank wait ──────────────────────────────────────────────────────────── */

int drmWaitVBlank(int fd, drmVBlankPtr vbl) {
    union drm_wait_vblank kv = {};
    kv.request.type     = vbl->request.type;
    kv.request.sequence = vbl->request.sequence;
    kv.request.signal   = vbl->request.signal;
    if (ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &kv) < 0) return -errno;
    vbl->reply.type     = kv.reply.type;
    vbl->reply.sequence = kv.reply.sequence;
    vbl->reply.tval_sec = (long)kv.reply.tval_sec;
    vbl->reply.tval_usec = (long)kv.reply.tval_usec;
    return 0;
}

/* ── GEM ──────────────────────────────────────────────────────────────────── */

int drmIoctl(int fd, unsigned long request, void *arg) {
    return ioctl(fd, request, arg);
}

int drmGemClose(int fd, uint32_t handle) {
    struct drm_gem_close g = { .handle = handle };
    return ioctl(fd, DRM_IOCTL_GEM_CLOSE, &g) < 0 ? -errno : 0;
}

/* ── Device node helpers ──────────────────────────────────────────────────── */

int drmIsKMS(int fd) {
    uint64_t value = 0;
    return drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &value) == 0 && value != 0;
}

/* Return true if /dev/dri/ directory exists (IODRMShim is loaded) */
int drmAvailable(void) {
    struct stat st;
    return stat("/dev/dri", &st) == 0;
}

/* ── Darwin extension: get raw IOFramebuffer info ─────────────────────────── */

int drmDarwinGetFramebufferInfo(int fd, drm_darwin_fb_info_t *info) {
    struct drm_darwin_ioframebuffer_info i = {};
    if (ioctl(fd, DRM_IOCTL_DARWIN_GET_FBINFO, &i) < 0) return -errno;
    info->phys_addr    = i.fb_phys_addr;
    info->width        = i.width;
    info->height       = i.height;
    info->pitch        = i.rowbytes;
    info->bpp          = i.depth;
    info->pixel_format = i.pixel_format;
    return 0;
}
