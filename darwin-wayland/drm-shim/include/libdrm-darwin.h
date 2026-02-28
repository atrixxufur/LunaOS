/*
 * libdrm-darwin.h — public header for Darwin DRM userspace library
 *
 * Mirrors the public API of libdrm 2.4.x so that wlroots, Weston, Mesa's
 * DRM backend, and other DRM-dependent software compile without changes.
 *
 * Clients should include this as <xf86drm.h> (symlink or -include).
 */

#pragma once
#include <stdint.h>
#include <stddef.h>
#include "drm_darwin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Types matching libdrm ─────────────────────────────────────────────────── */

typedef unsigned int drm_magic_t;
typedef unsigned int drm_handle_t;
typedef uint32_t     drm_context_t;
typedef uint32_t     drm_drawable_t;

/* ── Version ───────────────────────────────────────────────────────────────── */

typedef struct _drmVersion {
    int         version_major;
    int         version_minor;
    int         version_patchlevel;
    int         name_len;
    char       *name;
    int         date_len;
    char       *date;
    int         desc_len;
    char       *desc;
} drmVersion, *drmVersionPtr;

/* ── Mode types (match libdrm/xf86drmMode.h exactly) ────────────────────────── */

typedef struct drm_mode_modeinfo  drmModeModeInfo, *drmModeModeInfoPtr;

typedef struct {
    uint32_t    count_fbs;
    uint32_t   *fbs;
    uint32_t    count_crtcs;
    uint32_t   *crtcs;
    uint32_t    count_connectors;
    uint32_t   *connectors;
    uint32_t    count_encoders;
    uint32_t   *encoders;
    uint32_t    min_width, max_width;
    uint32_t    min_height, max_height;
} drmModeRes, *drmModeResPtr;

typedef struct {
    uint32_t            crtc_id;
    uint32_t            buffer_id;   /* FB id to scanout */
    uint32_t            x, y;
    uint32_t            width, height;
    int                 mode_valid;
    drmModeModeInfo     mode;
    int                 gamma_size;
} drmModeCrtc, *drmModeCrtcPtr;

typedef enum {
    DRM_MODE_CONNECTED         = 1,
    DRM_MODE_DISCONNECTED      = 2,
    DRM_MODE_UNKNOWNCONNECTION = 3,
} drmModeConnection;

typedef enum {
    DRM_MODE_SUBPIXEL_UNKNOWN        = 1,
    DRM_MODE_SUBPIXEL_HORIZONTAL_RGB = 2,
    DRM_MODE_SUBPIXEL_HORIZONTAL_BGR = 3,
    DRM_MODE_SUBPIXEL_VERTICAL_RGB   = 4,
    DRM_MODE_SUBPIXEL_VERTICAL_BGR   = 5,
    DRM_MODE_SUBPIXEL_NONE           = 6,
} drmModeSubPixel;

typedef struct {
    uint32_t            connector_id;
    uint32_t            encoder_id;
    uint32_t            connector_type;
    uint32_t            connector_type_id;
    drmModeConnection   connection;
    uint32_t            mmWidth, mmHeight;
    drmModeSubPixel     subpixel;
    int                 count_modes;
    drmModeModeInfoPtr  modes;
    int                 count_props;
    uint32_t           *props;
    uint64_t           *prop_values;
    int                 count_encoders;
    uint32_t           *encoders;
} drmModeConnector, *drmModeConnectorPtr;

typedef struct {
    uint32_t    encoder_id;
    uint32_t    encoder_type;
    uint32_t    crtc_id;
    uint32_t    possible_crtcs;
    uint32_t    possible_clones;
} drmModeEncoder, *drmModeEncoderPtr;

typedef struct {
    uint32_t    type;
    uint32_t    sequence;
    long        tval_sec;
    long        tval_usec;
    void       *user_data;
} drmEventContext;

/* VBlank struct (matches union drm_wait_vblank layout) */
typedef struct {
    struct { uint32_t type; uint32_t sequence; uint64_t signal; } request;
    struct { uint32_t type; uint32_t sequence; long tval_sec; long tval_usec; } reply;
} drmVBlank, *drmVBlankPtr;

/* ── Darwin extension ─────────────────────────────────────────────────────── */

typedef struct {
    uint64_t phys_addr;
    uint32_t width, height, pitch, bpp;
    uint32_t pixel_format;
} drm_darwin_fb_info_t;

/* ── Function declarations ───────────────────────────────────────────────── */

/* Device */
int              drmOpen(const char *name, const char *busId);
int              drmOpenWithType(const char *name, const char *busId, int type);
int              drmClose(int fd);
int              drmAvailable(void);
int              drmIsKMS(int fd);
int              drmIoctl(int fd, unsigned long request, void *arg);

/* Version */
drmVersionPtr    drmGetVersion(int fd);
void             drmFreeVersion(drmVersionPtr v);

/* Capabilities */
int              drmGetCap(int fd, uint64_t capability, uint64_t *value);
int              drmSetClientCap(int fd, uint64_t capability, uint64_t value);

/* Auth / master */
int              drmGetMagic(int fd, drm_magic_t *magic);
int              drmAuthMagic(int fd, drm_magic_t magic);
int              drmSetMaster(int fd);
int              drmDropMaster(int fd);

/* Resources */
drmModeResPtr       drmModeGetResources(int fd);
void                drmModeFreeResources(drmModeResPtr res);

/* CRTC */
drmModeCrtcPtr      drmModeGetCrtc(int fd, uint32_t crtcId);
int                 drmModeSetCrtc(int fd, uint32_t crtcId, uint32_t bufferId,
                                   uint32_t x, uint32_t y,
                                   uint32_t *connectors, int count,
                                   drmModeModeInfoPtr mode);
void                drmModeFreeCrtc(drmModeCrtcPtr crtc);

/* Connector */
drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t connId);
void                drmModeFreeConnector(drmModeConnectorPtr conn);

/* Encoder */
drmModeEncoderPtr   drmModeGetEncoder(int fd, uint32_t encId);
void                drmModeFreeEncoder(drmModeEncoderPtr enc);

/* Framebuffer */
int                 drmModeAddFB(int fd, uint32_t width, uint32_t height,
                                 uint8_t depth, uint8_t bpp, uint32_t pitch,
                                 uint32_t bo_handle, uint32_t *buf_id);
int                 drmModeAddFB2(int fd, uint32_t width, uint32_t height,
                                  uint32_t pixel_format,
                                  const uint32_t bo_handles[4],
                                  const uint32_t pitches[4],
                                  const uint32_t offsets[4],
                                  uint32_t *buf_id, uint32_t flags);
int                 drmModeRmFB(int fd, uint32_t bufferId);

/* Dumb buffers */
int                 drmModeCreateDumbBuffer(int fd, uint32_t width, uint32_t height,
                                            uint32_t bpp, uint32_t flags,
                                            uint32_t *handle, uint32_t *pitch,
                                            uint64_t *size);
int                 drmModeMapDumbBuffer(int fd, uint32_t handle, uint64_t *offset);
int                 drmModeDestroyDumbBuffer(int fd, uint32_t handle);
void               *drmModeMapDumb(int fd, uint32_t handle, size_t size);

/* Page flip */
int                 drmModePageFlip(int fd, uint32_t crtcId, uint32_t fbId,
                                    uint32_t flags, void *user_data);

/* VBlank */
int                 drmWaitVBlank(int fd, drmVBlankPtr vbl);

/* GEM */
int                 drmGemClose(int fd, uint32_t handle);

/* Darwin extension */
int                 drmDarwinGetFramebufferInfo(int fd, drm_darwin_fb_info_t *info);

#ifdef __cplusplus
}
#endif
