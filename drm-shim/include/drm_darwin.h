/*
 * drm_darwin.h — DRM/KMS compatibility header for Darwin/XNU (x86_64)
 *
 * Target: x86_64 Darwin / PureDarwin running on Intel hardware or QEMU x86_64
 *
 * Defines the ioctl interface, structs, and capability flags that libdrm and
 * wlroots expect to find at /dev/dri/card0.  User-space tools built against
 * standard libdrm will work without modification.
 *
 * Architecture:
 *   libdrm (unchanged) → libdrm-darwin.dylib (translation shim)
 *                             ↓  ioctl()
 *              /dev/dri/card0  (IODRMShim character device)
 *                             ↓  IOKit calls
 *              IOFramebuffer / IOGraphicsFamily (Apple open-source)
 *
 * Kernel component: IODRMShim.kext  (x86_64 IOKit KEXT)
 * User component:   libdrm-darwin   (x86_64 dylib, same symbols as libdrm)
 */

#pragma once

/* Enforce x86_64 target */
#if defined(__arm64__) || defined(__aarch64__)
#  error "drm_darwin.h is x86_64 only. arm64 support is not yet implemented."
#endif

#include <stdint.h>
#include <sys/ioccom.h>   /* Darwin x86_64 ioctl macros: _IOW, _IOR, _IOWR */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Character device paths ─────────────────────────────────────────────── */
#define DRM_DARWIN_DIR          "/dev/dri"
#define DRM_DARWIN_CARD_FMT     "/dev/dri/card%d"
#define DRM_DARWIN_RENDER_FMT   "/dev/dri/renderD%d"
#define DRM_DARWIN_CARD_BASE    128    /* renderD128 */

/* ── DRM ioctl magic byte (matches Linux) ────────────────────────────────── */
#define DRM_IOCTL_BASE  'd'
#define DRM_IO(nr)          _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOR(nr,type)    _IOR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOW(nr,type)    _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr,type)   _IOWR(DRM_IOCTL_BASE, nr, type)

/* ── Driver capability flags ─────────────────────────────────────────────── */
#define DRM_CAP_DUMB_BUFFER         0x1
#define DRM_CAP_VBLANK_HIGH_CRTC    0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH 0x3
#define DRM_CAP_DUMB_PREFER_SHADOW  0x4
#define DRM_CAP_PRIME               0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC 0x6
#define DRM_CAP_ASYNC_PAGE_FLIP     0x7
#define DRM_CAP_CURSOR_WIDTH        0x8
#define DRM_CAP_CURSOR_HEIGHT       0x9
#define DRM_CAP_ADDFB2_MODIFIERS    0x10
#define DRM_CAP_PAGE_FLIP_TARGET    0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT 0x12
#define DRM_CAP_SYNCOBJ             0x13
#define DRM_CAP_SYNCOBJ_TIMELINE    0x14

/* Client capabilities (SET) */
#define DRM_CLIENT_CAP_STEREO_3D            1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES     2
#define DRM_CLIENT_CAP_ATOMIC               3
#define DRM_CLIENT_CAP_ASPECT_RATIO         4
#define DRM_CLIENT_CAP_WRITEBACK_CONNECTORS 5

/* ── Core structs ────────────────────────────────────────────────────────── */

struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    uint64_t name_len;
    uint64_t name;          /* ptr: char* */
    uint64_t date_len;
    uint64_t date;          /* ptr: char* */
    uint64_t desc_len;
    uint64_t desc;          /* ptr: char* */
};

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_set_client_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_auth {
    unsigned int magic;
};

/* ── Mode/KMS structs ────────────────────────────────────────────────────── */

#define DRM_DISPLAY_MODE_LEN    32
#define DRM_PROP_NAME_LEN       32

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char     name[DRM_DISPLAY_MODE_LEN];
};

struct drm_mode_card_res {
    uint64_t fb_id_ptr;         /* ptr to uint32_t array */
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr;      /* ptr to uint32_t array */
    uint64_t modes_ptr;         /* ptr to drm_mode_modeinfo array */
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;        /* 1=connected, 2=disconnected, 3=unknown */
    uint32_t mm_width, mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

/* connector_type values */
#define DRM_MODE_CONNECTOR_Unknown      0
#define DRM_MODE_CONNECTOR_VGA          1
#define DRM_MODE_CONNECTOR_DVII         2
#define DRM_MODE_CONNECTOR_DVID         3
#define DRM_MODE_CONNECTOR_DVIA         4
#define DRM_MODE_CONNECTOR_Composite    5
#define DRM_MODE_CONNECTOR_SVIDEO       6
#define DRM_MODE_CONNECTOR_LVDS         7
#define DRM_MODE_CONNECTOR_DisplayPort  10
#define DRM_MODE_CONNECTOR_HDMIA        11
#define DRM_MODE_CONNECTOR_HDMIB        12
#define DRM_MODE_CONNECTOR_eDP          14
#define DRM_MODE_CONNECTOR_Virtual      15

struct drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width, height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id;
    uint32_t width, height;
    uint32_t pixel_format;     /* fourcc */
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};

/* ── Dumb buffer (scanout-capable, no GPU accel needed) ──────────────────── */

struct drm_mode_create_dumb {
    uint32_t height, width, bpp;
    uint32_t flags;
    uint32_t handle;   /* out */
    uint32_t pitch;    /* out */
    uint64_t size;     /* out */
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;   /* out: mmap offset */
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

/* ── Page flip / vblank ──────────────────────────────────────────────────── */

#define DRM_MODE_PAGE_FLIP_EVENT    0x01
#define DRM_MODE_PAGE_FLIP_ASYNC    0x02

struct drm_mode_crtc_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
};

struct drm_wait_vblank_request {
    uint32_t type;
    uint32_t sequence;
    uint64_t signal;
};

struct drm_wait_vblank_reply {
    uint32_t type;
    uint32_t sequence;
    int64_t  tval_sec;
    int64_t  tval_usec;
};

union drm_wait_vblank {
    struct drm_wait_vblank_request request;
    struct drm_wait_vblank_reply   reply;
};

/* ── GEM (buffer handle management) ─────────────────────────────────────── */

struct drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};

struct drm_gem_flink {
    uint32_t handle;
    uint32_t name;
};

struct drm_gem_open {
    uint32_t name;
    uint32_t handle;  /* out */
    uint64_t size;    /* out */
};

/* ── PRIME (DMA-BUF fd sharing) ──────────────────────────────────────────── */

struct drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t  fd;
};

/* ── ioctl numbers (generated by _IOR/_IOW/_IOWR on x86_64 Darwin) ───────── */

/* Core */
#define DRM_IOCTL_VERSION           DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GET_MAGIC         DRM_IOR( 0x02, struct drm_auth)
#define DRM_IOCTL_AUTH_MAGIC        DRM_IOW( 0x11, struct drm_auth)
#define DRM_IOCTL_SET_MASTER        DRM_IO(  0x1e)
#define DRM_IOCTL_DROP_MASTER       DRM_IO(  0x1f)
#define DRM_IOCTL_GET_CAP           DRM_IOWR(0x0c, struct drm_get_cap)
#define DRM_IOCTL_SET_CLIENT_CAP    DRM_IOW( 0x0d, struct drm_set_client_cap)

/* GEM */
#define DRM_IOCTL_GEM_CLOSE         DRM_IOW( 0x09, struct drm_gem_close)
#define DRM_IOCTL_GEM_FLINK         DRM_IOWR(0x0a, struct drm_gem_flink)
#define DRM_IOCTL_GEM_OPEN          DRM_IOWR(0x0b, struct drm_gem_open)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD DRM_IOWR(0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE DRM_IOWR(0x2e, struct drm_prime_handle)

/* VBlank */
#define DRM_IOCTL_WAIT_VBLANK       DRM_IOWR(0x3a, union drm_wait_vblank)

/* Mode/KMS */
#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xa0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC      DRM_IOWR(0xa1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC      DRM_IOWR(0xa2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER   DRM_IOWR(0xa6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xa7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB        DRM_IOWR(0xae, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_ADDFB2       DRM_IOWR(0xb8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_MODE_RMFB         DRM_IOWR(0xaf, unsigned int)
#define DRM_IOCTL_MODE_PAGE_FLIP    DRM_IOWR(0xb0, struct drm_mode_crtc_page_flip)
#define DRM_IOCTL_MODE_CREATE_DUMB  DRM_IOWR(0xb2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB     DRM_IOWR(0xb3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB DRM_IOWR(0xb4, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_GETPROPERTY  DRM_IOWR(0xaa, struct drm_mode_get_property)
#define DRM_IOCTL_MODE_SETPROPERTY  DRM_IOWR(0xab, struct drm_mode_connector_set_property)
#define DRM_IOCTL_MODE_GETPROPBLOB  DRM_IOWR(0xac, struct drm_mode_get_blob)
#define DRM_IOCTL_MODE_GETPLANERESOURCES DRM_IOWR(0xb5, struct drm_mode_get_plane_res)
#define DRM_IOCTL_MODE_GETPLANE     DRM_IOWR(0xb6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_SETPLANE     DRM_IOWR(0xb7, struct drm_mode_set_plane)
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES DRM_IOWR(0xb9, struct drm_mode_obj_get_properties)
#define DRM_IOCTL_MODE_OBJ_SETPROPERTY   DRM_IOWR(0xba, struct drm_mode_obj_set_property)
#define DRM_IOCTL_MODE_CREATEPROPBLOB    DRM_IOWR(0xbd, struct drm_mode_create_blob)
#define DRM_IOCTL_MODE_DESTROYPROPBLOB   DRM_IOWR(0xbe, struct drm_mode_destroy_blob)
#define DRM_IOCTL_MODE_ATOMIC            DRM_IOWR(0xbc, struct drm_mode_atomic)

/* ── Darwin vendor extension ioctls (nr=0xe0..0xff) ─────────────────────── */
/*
 * TODO_ARM64: When adding arm64 support, the backend ioctl (0xe0) will need
 * to route to IOMobileFramebuffer instead of IOFramebuffer.  The struct and
 * ioctl number stay the same — only the KEXT dispatch changes.
 */
struct drm_darwin_ioframebuffer_info {
    uint64_t fb_phys_addr;  /* physical base of VRAM aperture */
    uint32_t width, height;
    uint32_t rowbytes;
    uint32_t pixel_format;  /* IOKit kIO32ARGBPixelFormat etc. */
    uint32_t depth;
    uint32_t _pad;          /* reserved, must be zero — pad for future use */
};

#define DRM_IOCTL_DARWIN_GET_FBINFO \
    DRM_IOR(0xe0, struct drm_darwin_ioframebuffer_info)

#ifdef __cplusplus
}
#endif
