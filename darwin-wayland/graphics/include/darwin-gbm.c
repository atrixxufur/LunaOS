/*
 * darwin-gbm.c — Generic Buffer Manager for LunaOS/Darwin
 *
 * Implements the GBM API (that Mesa, wlroots, EGL expect) on top of
 * IODRMShim dumb buffer ioctls (/dev/dri/card0).
 *
 * Buffer lifecycle:
 *   gbm_bo_create()
 *     → DRM_IOCTL_MODE_CREATE_DUMB  (allocates IOBufferMemoryDescriptor)
 *     → DRM_IOCTL_MODE_MAP_DUMB     (gets mmap offset)
 *     → mmap()                      (maps into process address space)
 *
 *   gbm_surface_lock_front_buffer()
 *     → returns the bo Mesa just rendered into
 *     → luna-compositor calls drmModeSetCrtc(fb_id) to display it
 *
 *   gbm_bo_destroy()
 *     → munmap()
 *     → DRM_IOCTL_MODE_DESTROY_DUMB
 *     → DRM_IOCTL_GEM_CLOSE
 *
 * Thread safety: GBM objects are not thread-safe. Mesa serialises
 * access internally. Do not call GBM from multiple threads.
 */

#include "gbm.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <assert.h>

/* Our DRM shim ioctl definitions */
#include "drm_darwin.h"

#define GBM_MAX_BOS_PER_SURFACE 4

/* ── Internal structs ─────────────────────────────────────────────────────── */

struct gbm_device {
    int      fd;               /* /dev/dri/card0 fd */
    char     backend[32];      /* "darwin-iodrmshim" */
};

struct gbm_bo {
    struct gbm_device *gbm;
    uint32_t  width, height;
    uint32_t  stride;
    uint32_t  format;
    uint32_t  flags;
    uint64_t  modifier;       /* DRM_FORMAT_MOD_LINEAR always on Darwin */

    /* DRM dumb buffer */
    uint32_t  gem_handle;
    uint32_t  fb_id;          /* set by drmModeAddFB() if scanout bo */
    uint64_t  map_offset;     /* from MODE_MAP_DUMB */
    size_t    size;
    void     *map;            /* CPU virtual address, or NULL if not mapped */

    /* User data */
    void    *user_data;
    void   (*user_data_destroy)(struct gbm_bo *, void *);
};

struct gbm_surface {
    struct gbm_device *gbm;
    uint32_t  width, height;
    uint32_t  format;
    uint32_t  flags;

    /* Double-buffered: front is displayed, back is being rendered */
    struct gbm_bo *bos[GBM_MAX_BOS_PER_SURFACE];
    int            nbo;
    int            front;     /* index of front buffer (-1 = none) */
    int            locked;    /* front buffer locked by compositor */
};

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static uint32_t format_bpp(uint32_t format) {
    switch (format) {
    case GBM_FORMAT_ARGB8888:
    case GBM_FORMAT_XRGB8888:
    case GBM_FORMAT_ABGR8888:
    case GBM_FORMAT_RGBA8888:
    case GBM_FORMAT_ARGB2101010:
    case GBM_FORMAT_XRGB2101010: return 32;
    case GBM_FORMAT_RGB888:
    case GBM_FORMAT_BGR888:      return 24;
    default:                     return 32;
    }
}

static struct gbm_bo *alloc_bo(struct gbm_device *gbm,
                                uint32_t width, uint32_t height,
                                uint32_t format, uint32_t flags) {
    struct gbm_bo *bo = calloc(1, sizeof(*bo));
    if (!bo) return NULL;

    bo->gbm      = gbm;
    bo->width    = width;
    bo->height   = height;
    bo->format   = format;
    bo->flags    = flags;
    bo->modifier = DRM_FORMAT_MOD_LINEAR;
    bo->front    = -1;

    /* Create dumb buffer via IODRMShim */
    struct drm_mode_create_dumb create = {
        .width  = width,
        .height = height,
        .bpp    = format_bpp(format),
    };
    if (ioctl(gbm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        fprintf(stderr, "gbm: CREATE_DUMB failed: %s\n", strerror(errno));
        free(bo);
        return NULL;
    }
    bo->gem_handle = create.handle;
    bo->stride     = create.pitch;
    bo->size       = create.size;

    /* Get mmap offset */
    struct drm_mode_map_dumb map_req = { .handle = bo->gem_handle };
    if (ioctl(gbm->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
        fprintf(stderr, "gbm: MAP_DUMB failed: %s\n", strerror(errno));
        /* Non-fatal — bo can be used without CPU mapping */
    } else {
        bo->map_offset = map_req.offset;
    }

    return bo;
}

/* ── Device ───────────────────────────────────────────────────────────────── */

struct gbm_device *gbm_create_device(int fd) {
    struct gbm_device *gbm = calloc(1, sizeof(*gbm));
    if (!gbm) return NULL;
    gbm->fd = fd;
    strncpy(gbm->backend, "darwin-iodrmshim", sizeof(gbm->backend) - 1);
    return gbm;
}

void gbm_device_destroy(struct gbm_device *gbm) {
    free(gbm);
}

int gbm_device_get_fd(struct gbm_device *gbm) {
    return gbm->fd;
}

const char *gbm_device_get_backend_name(struct gbm_device *gbm) {
    return gbm->backend;
}

int gbm_device_is_format_supported(struct gbm_device *gbm,
                                    uint32_t format, uint32_t usage) {
    (void)gbm; (void)usage;
    /* We support ARGB/XRGB 32bpp natively. Others via CPU conversion. */
    switch (format) {
    case GBM_FORMAT_ARGB8888:
    case GBM_FORMAT_XRGB8888:
    case GBM_FORMAT_ABGR8888:
    case GBM_FORMAT_RGBA8888:
        return 1;
    default:
        return 0;
    }
}

int gbm_device_get_format_modifier_plane_count(struct gbm_device *gbm,
                                                uint32_t format,
                                                uint64_t modifier) {
    (void)gbm; (void)format; (void)modifier;
    return 1; /* always single-plane (no YUV multiplane via dumb buffers) */
}

/* ── Buffer object ────────────────────────────────────────────────────────── */

struct gbm_bo *gbm_bo_create(struct gbm_device *gbm,
                              uint32_t width, uint32_t height,
                              uint32_t format, uint32_t flags) {
    return alloc_bo(gbm, width, height, format, flags);
}

struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *gbm,
                   uint32_t width, uint32_t height, uint32_t format,
                   const uint64_t *modifiers, const unsigned int count) {
    /* We only support LINEAR — ignore other modifiers */
    for (unsigned i = 0; i < count; i++)
        if (modifiers[i] == DRM_FORMAT_MOD_LINEAR)
            return alloc_bo(gbm, width, height, format, GBM_BO_USE_RENDERING);
    /* If LINEAR not in list, still try — IODRMShim only does linear */
    return alloc_bo(gbm, width, height, format, GBM_BO_USE_RENDERING);
}

struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *gbm,
                   uint32_t width, uint32_t height, uint32_t format,
                   const uint64_t *modifiers, const unsigned int count,
                   uint32_t flags) {
    (void)flags;
    return gbm_bo_create_with_modifiers(gbm, width, height, format,
                                        modifiers, count);
}

struct gbm_bo *gbm_bo_import(struct gbm_device *gbm, uint32_t type,
                              void *buffer, uint32_t usage) {
    (void)gbm; (void)type; (void)buffer; (void)usage;
    /* PRIME import: future — requires IODRMShim PRIME_FD_TO_HANDLE ioctl */
    fprintf(stderr, "gbm_bo_import: not yet implemented on Darwin\n");
    return NULL;
}

void gbm_bo_destroy(struct gbm_bo *bo) {
    if (!bo) return;
    if (bo->user_data && bo->user_data_destroy)
        bo->user_data_destroy(bo, bo->user_data);
    if (bo->map && bo->map != MAP_FAILED)
        munmap(bo->map, bo->size);
    /* Destroy dumb buffer */
    struct drm_mode_destroy_dumb destroy = { .handle = bo->gem_handle };
    ioctl(bo->gbm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    /* Close GEM handle */
    struct drm_gem_close gem_close = { .handle = bo->gem_handle };
    ioctl(bo->gbm->fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
    free(bo);
}

uint32_t gbm_bo_get_width(struct gbm_bo *bo)   { return bo->width; }
uint32_t gbm_bo_get_height(struct gbm_bo *bo)  { return bo->height; }
uint32_t gbm_bo_get_stride(struct gbm_bo *bo)  { return bo->stride; }
uint32_t gbm_bo_get_format(struct gbm_bo *bo)  { return bo->format; }
uint64_t gbm_bo_get_modifier(struct gbm_bo *bo){ return bo->modifier; }
int      gbm_bo_get_plane_count(struct gbm_bo *bo) { (void)bo; return 1; }

uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane) {
    (void)plane; return bo->stride;
}
uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int plane) {
    (void)bo; (void)plane; return 0;
}
uint32_t gbm_bo_get_bpp(struct gbm_bo *bo) {
    return format_bpp(bo->format);
}

union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo) {
    union gbm_bo_handle h; h.u32 = bo->gem_handle; return h;
}
union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane) {
    (void)plane; return gbm_bo_get_handle(bo);
}

int gbm_bo_get_fd(struct gbm_bo *bo) {
    /* Export as PRIME fd — requires IODRMShim PRIME_HANDLE_TO_FD */
    struct drm_prime_handle prime = {
        .handle = bo->gem_handle,
        .flags  = 0,
    };
    if (ioctl(bo->gbm->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0)
        return -1;
    return prime.fd;
}

int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane) {
    (void)plane; return gbm_bo_get_fd(bo);
}

void *gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y,
                 uint32_t width, uint32_t height, uint32_t flags,
                 uint32_t *stride_out, void **map_data) {
    (void)x; (void)y; (void)width; (void)height; (void)flags;

    if (!bo->map) {
        bo->map = mmap(NULL, bo->size,
                       PROT_READ | PROT_WRITE, MAP_SHARED,
                       bo->gbm->fd, (off_t)bo->map_offset);
        if (bo->map == MAP_FAILED) {
            fprintf(stderr, "gbm: mmap failed: %s\n", strerror(errno));
            bo->map = NULL;
            return NULL;
        }
    }
    *stride_out = bo->stride;
    *map_data   = bo->map;
    return bo->map;
}

void gbm_bo_unmap(struct gbm_bo *bo, void *map_data) {
    (void)bo; (void)map_data;
    /* We keep the buffer mapped for the lifetime of the bo — no-op here */
}

void gbm_bo_set_user_data(struct gbm_bo *bo, void *data,
                           void (*destroy)(struct gbm_bo *, void *)) {
    bo->user_data         = data;
    bo->user_data_destroy = destroy;
}
void *gbm_bo_get_user_data(struct gbm_bo *bo) { return bo->user_data; }

/* ── Surface ──────────────────────────────────────────────────────────────── */

struct gbm_surface *gbm_surface_create(struct gbm_device *gbm,
                                        uint32_t width, uint32_t height,
                                        uint32_t format, uint32_t flags) {
    struct gbm_surface *surf = calloc(1, sizeof(*surf));
    if (!surf) return NULL;
    surf->gbm    = gbm;
    surf->width  = width;
    surf->height = height;
    surf->format = format;
    surf->flags  = flags;
    surf->front  = -1;

    /* Pre-allocate double buffer */
    for (int i = 0; i < 2; i++) {
        surf->bos[i] = alloc_bo(gbm, width, height, format, flags);
        if (!surf->bos[i]) {
            for (int j = 0; j < i; j++) gbm_bo_destroy(surf->bos[j]);
            free(surf);
            return NULL;
        }
        surf->nbo++;
    }
    return surf;
}

struct gbm_surface *gbm_surface_create_with_modifiers(struct gbm_device *gbm,
                        uint32_t w, uint32_t h, uint32_t fmt,
                        const uint64_t *mods, const unsigned int cnt) {
    (void)mods; (void)cnt;
    return gbm_surface_create(gbm, w, h, fmt, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
}

struct gbm_surface *gbm_surface_create_with_modifiers2(struct gbm_device *gbm,
                        uint32_t w, uint32_t h, uint32_t fmt,
                        const uint64_t *mods, const unsigned int cnt,
                        uint32_t flags) {
    (void)flags;
    return gbm_surface_create_with_modifiers(gbm, w, h, fmt, mods, cnt);
}

struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *surf) {
    /* Return whichever buffer Mesa just finished rendering into */
    int back = (surf->front == -1) ? 0 : 1 - surf->front;
    surf->front  = back;
    surf->locked = 1;
    return surf->bos[back];
}

void gbm_surface_release_buffer(struct gbm_surface *surf, struct gbm_bo *bo) {
    (void)bo;
    surf->locked = 0;
}

int gbm_surface_has_free_buffers(struct gbm_surface *surf) {
    return !surf->locked;
}

void gbm_surface_destroy(struct gbm_surface *surf) {
    for (int i = 0; i < surf->nbo; i++)
        if (surf->bos[i]) gbm_bo_destroy(surf->bos[i]);
    free(surf);
}
