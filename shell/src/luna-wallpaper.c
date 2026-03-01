/*
 * luna-wallpaper.c — LunaOS wallpaper renderer
 *
 * A wlr-layer-shell BACKGROUND surface that fills each monitor with
 * either a loaded image or a smooth gradient.
 *
 * Gradient: linear interpolation between two ARGB colors, top→bottom.
 * Default: deep navy (#1a1a2e) → dark blue (#16213e) — matches panel.
 *
 * Image loading: PPM format (trivial parser, no dependencies).
 * For PNG/JPEG, link libpng/libjpeg and replace ppm_load().
 */

#include "luna-shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <math.h>

struct luna_wallpaper {
    struct luna_shell  *shell;
    struct luna_output *output;

    struct wl_surface            *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_shm_pool           *shm_pool;
    struct wl_buffer             *buffer;

    uint32_t *pixels;
    int       stride, width, height;
    size_t    size;
    int       shm_fd;
    bool      configured;
};

/* ── Gradient renderer ────────────────────────────────────────────────────── */

static void render_gradient(struct luna_wallpaper *wp,
                             uint32_t color_top, uint32_t color_bot) {
    uint8_t r0 = (color_top >> 16) & 0xFF, g0 = (color_top >> 8) & 0xFF,
            b0 = (color_top      ) & 0xFF;
    uint8_t r1 = (color_bot >> 16) & 0xFF, g1 = (color_bot >> 8) & 0xFF,
            b1 = (color_bot      ) & 0xFF;

    int w = wp->width, h = wp->height;
    for (int y = 0; y < h; y++) {
        float t  = (float)y / (float)(h - 1);
        uint8_t r = (uint8_t)(r0 + t * (r1 - r0));
        uint8_t g = (uint8_t)(g0 + t * (g1 - g0));
        uint8_t b = (uint8_t)(b0 + t * (b1 - b0));
        uint32_t color = 0xFF000000 | ((uint32_t)r << 16) |
                         ((uint32_t)g << 8) | b;
        for (int x = 0; x < w; x++)
            wp->pixels[y * (wp->stride / 4) + x] = color;
    }

    /* Subtle noise for depth (simple dither) */
    unsigned seed = 0x12345678;
    for (int i = 0; i < w * h / 8; i++) {
        seed = seed * 1664525u + 1013904223u;
        int px_i = (int)(seed >> 17) % (w * h);
        uint32_t c = wp->pixels[px_i];
        uint8_t nr = (uint8_t)(((c >> 16) & 0xFF) + (seed & 3) - 1);
        uint8_t ng = (uint8_t)(((c >>  8) & 0xFF) + ((seed>>2) & 3) - 1);
        uint8_t nb = (uint8_t)(((c      ) & 0xFF) + ((seed>>4) & 3) - 1);
        wp->pixels[px_i] = 0xFF000000 | ((uint32_t)nr << 16) |
                           ((uint32_t)ng << 8) | nb;
    }
}

/* ── PPM image loader (portable pixmap — no extra deps) ──────────────────── */

static int ppm_load(const char *path, uint32_t **out_pixels,
                    int *out_w, int *out_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char magic[3]; fread(magic, 1, 3, f);
    if (magic[0] != 'P' || magic[1] != '6') { fclose(f); return -1; }
    int w, h, maxval;
    fscanf(f, " %d %d %d ", &w, &h, &maxval);
    if (w <= 0 || h <= 0 || maxval <= 0) { fclose(f); return -1; }

    uint32_t *px = malloc((size_t)(w * h) * 4);
    if (!px) { fclose(f); return -1; }

    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3]; fread(rgb, 1, 3, f);
        px[i] = 0xFF000000 | ((uint32_t)rgb[0] << 16) |
                ((uint32_t)rgb[1] << 8) | rgb[2];
    }
    fclose(f);
    *out_pixels = px; *out_w = w; *out_h = h;
    return 0;
}

/* Scale source image into destination buffer (nearest-neighbor) */
static void blit_scaled(uint32_t *dst, int dw, int dh,
                         const uint32_t *src, int sw, int sh) {
    for (int dy = 0; dy < dh; dy++) {
        int sy = dy * sh / dh;
        for (int dx = 0; dx < dw; dx++) {
            int sx = dx * sw / dw;
            dst[dy * dw + dx] = src[sy * sw + sx];
        }
    }
}

/* ── Layer surface callbacks ─────────────────────────────────────────────── */

static void wp_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                          uint32_t serial, uint32_t width, uint32_t height) {
    struct luna_wallpaper *wp = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);

    if ((int)width == wp->width && (int)height == wp->height
            && wp->configured) return;

    /* Free old buffer */
    if (wp->pixels)  munmap(wp->pixels, wp->size);
    if (wp->buffer)  wl_buffer_destroy(wp->buffer);
    if (wp->shm_pool) wl_shm_pool_destroy(wp->shm_pool);
    if (wp->shm_fd >= 0) close(wp->shm_fd);

    wp->width  = (int)width;
    wp->height = (int)height;
    wp->stride = wp->width * 4;
    wp->size   = (size_t)(wp->stride * wp->height);

    char name[64];
    snprintf(name, sizeof(name), "/luna-wallpaper-%d-%s",
             getpid(), wp->output->name);
    wp->shm_fd = shm_open(name, O_RDWR|O_CREAT|O_EXCL, 0600);
    shm_unlink(name);
    ftruncate(wp->shm_fd, (off_t)wp->size);
    wp->pixels = mmap(NULL, wp->size, PROT_READ|PROT_WRITE,
                      MAP_SHARED, wp->shm_fd, 0);
    wp->shm_pool = wl_shm_create_pool(wp->shell->shm,
                                       wp->shm_fd, (int32_t)wp->size);
    wp->buffer   = wl_shm_pool_create_buffer(wp->shm_pool, 0,
                       wp->width, wp->height, wp->stride,
                       WL_SHM_FORMAT_ARGB8888);

    /* Render wallpaper content */
    const char *path = wp->shell->config.wallpaper_path;
    if (path) {
        uint32_t *img_px = NULL;
        int iw, ih;
        if (ppm_load(path, &img_px, &iw, &ih) == 0) {
            blit_scaled(wp->pixels, wp->width, wp->height, img_px, iw, ih);
            free(img_px);
            goto commit;
        }
        /* Fall through to gradient if image load failed */
    }
    render_gradient(wp, wp->shell->config.wallpaper_color_a,
                        wp->shell->config.wallpaper_color_b);

commit:
    wl_surface_attach(wp->surface, wp->buffer, 0, 0);
    wl_surface_damage_buffer(wp->surface, 0, 0, wp->width, wp->height);
    wl_surface_commit(wp->surface);
    wp->configured = true;
}

static void wp_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    (void)data; (void)ls;
}

static const struct zwlr_layer_surface_v1_listener wp_listener = {
    .configure = wp_configure, .closed = wp_closed,
};

/* ── Constructor / destructor ────────────────────────────────────────────── */

struct luna_wallpaper *luna_wallpaper_create(struct luna_shell *shell,
                                              struct luna_output *output) {
    struct luna_wallpaper *wp = calloc(1, sizeof(*wp));
    wp->shell  = shell;
    wp->output = output;
    wp->shm_fd = -1;

    wp->surface = wl_compositor_create_surface(shell->compositor);
    wp->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        shell->layer_shell, wp->surface, output->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "luna-wallpaper");

    zwlr_layer_surface_v1_set_anchor(wp->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP  | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(wp->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(wp->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    zwlr_layer_surface_v1_add_listener(wp->layer_surface, &wp_listener, wp);
    wl_surface_commit(wp->surface);
    return wp;
}

void luna_wallpaper_destroy(struct luna_wallpaper *wp) {
    if (wp->pixels)  munmap(wp->pixels, wp->size);
    if (wp->buffer)  wl_buffer_destroy(wp->buffer);
    if (wp->shm_pool) wl_shm_pool_destroy(wp->shm_pool);
    if (wp->shm_fd >= 0) close(wp->shm_fd);
    zwlr_layer_surface_v1_destroy(wp->layer_surface);
    wl_surface_destroy(wp->surface);
    free(wp);
}
