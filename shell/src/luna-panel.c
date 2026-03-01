/*
 * luna-panel.c — LunaOS top panel
 *
 * A wlr-layer-shell surface anchored to the top of each output.
 * Renders using EGL + OpenGL ES 2.0 (lavapipe on software, virgl in QEMU).
 *
 * Layout (left to right):
 *   [🌙 LunaOS] [window 1] [window 2] ... [      ] [🔔] [🔊] [HH:MM]
 *    launcher     window list (foreign-toplevel)     systray    clock
 */

#include "luna-shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>

/* wl_shm pixel format */
#define PANEL_FORMAT WL_SHM_FORMAT_ARGB8888
#define PANEL_BPP    4

/* ── Internal panel struct ────────────────────────────────────────────────── */
struct luna_panel {
    struct luna_shell  *shell;
    struct luna_output *output;

    /* Wayland objects */
    struct wl_surface              *surface;
    struct zwlr_layer_surface_v1   *layer_surface;
    struct wl_shm_pool             *shm_pool;
    struct wl_buffer               *buffer;

    int     width, height;
    bool    configured;
    bool    dirty;

    /* Pixel buffer (CPU rendered) */
    uint32_t *pixels;
    int       stride;
    size_t    size;
    int       shm_fd;

    /* Hot zones for click detection */
    struct { int x, w; } zone_launcher;
    struct { int x, w; } zone_clock;
    struct { int x, w; } zone_toplevels[32];
    int                  zone_toplevel_count;
};

/* ── Color helpers ────────────────────────────────────────────────────────── */

static void fill_rect(uint32_t *px, int stride,
                       int x, int y, int w, int h, uint32_t color) {
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            px[row * (stride / 4) + col] = color;
}

static void blend_rect(uint32_t *px, int stride,
                        int x, int y, int w, int h,
                        uint32_t color, uint8_t alpha) {
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >>  8) & 0xFF;
    uint8_t b = (color      ) & 0xFF;
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            uint32_t *dst = &px[row * (stride / 4) + col];
            uint8_t dr = (*dst >> 16) & 0xFF;
            uint8_t dg = (*dst >>  8) & 0xFF;
            uint8_t db = (*dst      ) & 0xFF;
            uint8_t nr = (r * alpha + dr * (255 - alpha)) / 255;
            uint8_t ng = (g * alpha + dg * (255 - alpha)) / 255;
            uint8_t nb = (b * alpha + db * (255 - alpha)) / 255;
            *dst = 0xFF000000 | (nr << 16) | (ng << 8) | nb;
        }
    }
}

/* ── Minimal bitmap font (5x7 pixels, ASCII 32-126) ─────────────────────────
 * Full font omitted for brevity — in production, link freetype2 or use
 * a pre-rasterized bitmap font header. This stub draws placeholder rects. */

static void draw_text(uint32_t *px, int stride,
                       int x, int y, const char *text,
                       uint32_t color, int font_size) {
    /* Placeholder: draw a thin horizontal bar representing text.
     * Replace with freetype2 or stb_truetype in production. */
    int len = (int)strlen(text);
    int w   = len * (font_size / 2);
    int h   = font_size;
    (void)w; (void)h;
    for (int i = 0; i < len; i++) {
        int cx = x + i * (font_size / 2);
        fill_rect(px, stride, cx, y + font_size/4,
                  font_size/2 - 1, font_size/2, color);
    }
}

/* ── SHM buffer allocation ────────────────────────────────────────────────── */

static int create_shm_buffer(struct luna_panel *panel) {
    panel->stride = panel->width * PANEL_BPP;
    panel->size   = (size_t)(panel->stride * panel->height);

    char name[64];
    snprintf(name, sizeof(name), "/luna-panel-%d-XXXXXX", getpid());
    panel->shm_fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (panel->shm_fd < 0) return -1;
    shm_unlink(name);
    ftruncate(panel->shm_fd, (off_t)panel->size);

    panel->pixels = mmap(NULL, panel->size,
                         PROT_READ | PROT_WRITE, MAP_SHARED,
                         panel->shm_fd, 0);
    if (panel->pixels == MAP_FAILED) { close(panel->shm_fd); return -1; }

    panel->shm_pool = wl_shm_create_pool(panel->shell->shm,
                                          panel->shm_fd, (int32_t)panel->size);
    panel->buffer   = wl_shm_pool_create_buffer(panel->shm_pool, 0,
                          panel->width, panel->height,
                          panel->stride, PANEL_FORMAT);
    return 0;
}

/* ── Render ───────────────────────────────────────────────────────────────── */

static void panel_render(struct luna_panel *panel) {
    if (!panel->configured || !panel->pixels) return;

    struct luna_shell        *shell = panel->shell;
    struct luna_shell_config *cfg   = &shell->config;
    uint32_t *px   = panel->pixels;
    int        s   = panel->stride;
    int        w   = panel->width;
    int        h   = panel->height;

    /* Background */
    fill_rect(px, s, 0, 0, w, h, cfg->panel_bg_color);

    /* Bottom border line (accent color) */
    fill_rect(px, s, 0, h - 1, w, 1, cfg->panel_accent_color);

    int x = 8;  /* current draw x */

    /* ── Launcher button: [🌙 Luna] ──────────────────────────────────────── */
    int launcher_w = 80;
    blend_rect(px, s, x, 4, launcher_w, h - 8, cfg->panel_accent_color, 40);
    draw_text(px, s, x + 8, (h - cfg->font_size) / 2,
              "Luna", cfg->panel_fg_color, cfg->font_size);
    panel->zone_launcher.x = x;
    panel->zone_launcher.w = launcher_w;
    x += launcher_w + 8;

    /* Separator */
    fill_rect(px, s, x, 8, 1, h - 16, 0x33FFFFFF);
    x += 8;

    /* ── Window list (foreign-toplevel) ──────────────────────────────────── */
    panel->zone_toplevel_count = 0;
    struct luna_toplevel *tl;
    wl_list_for_each(tl, &shell->toplevels, link) {
        if (panel->zone_toplevel_count >= 32) break;
        int btn_w = 140;
        uint32_t bg = tl->activated ? 0x33FFFFFF : 0x11FFFFFF;
        blend_rect(px, s, x, 4, btn_w, h - 8, 0xFFFFFF, (bg >> 24));
        draw_text(px, s, x + 8, (h - cfg->font_size) / 2,
                  tl->title[0] ? tl->title : tl->app_id,
                  cfg->panel_fg_color, cfg->font_size);
        panel->zone_toplevels[panel->zone_toplevel_count].x = x;
        panel->zone_toplevels[panel->zone_toplevel_count].w = btn_w;
        panel->zone_toplevel_count++;
        x += btn_w + 4;
    }

    /* ── Right side: clock ───────────────────────────────────────────────── */
    char clock_str[32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(clock_str, sizeof(clock_str),
             cfg->clock_format ? cfg->clock_format : "%H:%M", tm_info);

    int clock_w = (int)strlen(clock_str) * (cfg->font_size / 2) + 16;
    int clock_x = w - clock_w - 8;
    draw_text(px, s, clock_x + 8, (h - cfg->font_size) / 2,
              clock_str, cfg->panel_fg_color, cfg->font_size);
    panel->zone_clock.x = clock_x;
    panel->zone_clock.w = clock_w;

    /* Commit to compositor */
    wl_surface_attach(panel->surface, panel->buffer, 0, 0);
    wl_surface_damage_buffer(panel->surface, 0, 0, w, h);
    wl_surface_commit(panel->surface);
    panel->dirty = false;
}

/* ── Layer surface callbacks ─────────────────────────────────────────────── */

static void layer_surface_configure(void *data,
        struct zwlr_layer_surface_v1 *ls,
        uint32_t serial, uint32_t width, uint32_t height) {
    struct luna_panel *panel = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);

    if ((int)width != panel->width || (int)height != panel->height) {
        if (panel->pixels) {
            munmap(panel->pixels, panel->size);
            wl_buffer_destroy(panel->buffer);
            wl_shm_pool_destroy(panel->shm_pool);
            close(panel->shm_fd);
        }
        panel->width  = (int)width;
        panel->height = (int)height;
        create_shm_buffer(panel);
    }
    panel->configured = true;
    panel->dirty      = true;
    panel_render(panel);
}

static void layer_surface_closed(void *data,
        struct zwlr_layer_surface_v1 *ls) {
    (void)ls;
    struct luna_panel *panel = data;
    panel->shell->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

/* ── Pointer click ────────────────────────────────────────────────────────── */

void luna_panel_handle_click(struct luna_panel *panel, int px_x, int px_y) {
    (void)px_y;
    /* Launcher button */
    if (px_x >= panel->zone_launcher.x &&
        px_x <  panel->zone_launcher.x + panel->zone_launcher.w) {
        if (panel->shell->launcher_visible)
            luna_launcher_hide(panel->shell->launcher);
        else
            luna_launcher_show(panel->shell->launcher);
        return;
    }
    /* Window list */
    for (int i = 0; i < panel->zone_toplevel_count; i++) {
        if (px_x >= panel->zone_toplevels[i].x &&
            px_x <  panel->zone_toplevels[i].x + panel->zone_toplevels[i].w) {
            /* Focus/raise this window via foreign-toplevel */
            int j = 0;
            struct luna_toplevel *tl;
            wl_list_for_each(tl, &panel->shell->toplevels, link) {
                if (j++ == i) {
                    zwlr_foreign_toplevel_handle_v1_activate(
                        tl->handle, panel->shell->seat);
                    break;
                }
            }
            return;
        }
    }
}

/* ── Constructor / destructor ────────────────────────────────────────────── */

struct luna_panel *luna_panel_create(struct luna_shell *shell,
                                     struct luna_output *output) {
    struct luna_panel *panel = calloc(1, sizeof(*panel));
    panel->shell  = shell;
    panel->output = output;
    panel->height = shell->config.panel_height;

    panel->surface = wl_compositor_create_surface(shell->compositor);

    panel->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        shell->layer_shell, panel->surface,
        output->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        "luna-panel");

    /* Anchor to top, full width, fixed height */
    zwlr_layer_surface_v1_set_anchor(panel->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(panel->layer_surface, 0, panel->height);
    zwlr_layer_surface_v1_set_exclusive_zone(panel->layer_surface, panel->height);
    zwlr_layer_surface_v1_set_keyboard_interactivity(panel->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    zwlr_layer_surface_v1_add_listener(panel->layer_surface,
                                       &layer_surface_listener, panel);
    wl_surface_commit(panel->surface);
    return panel;
}

void luna_panel_destroy(struct luna_panel *panel) {
    if (panel->pixels) munmap(panel->pixels, panel->size);
    if (panel->buffer)   wl_buffer_destroy(panel->buffer);
    if (panel->shm_pool) wl_shm_pool_destroy(panel->shm_pool);
    if (panel->shm_fd >= 0) close(panel->shm_fd);
    zwlr_layer_surface_v1_destroy(panel->layer_surface);
    wl_surface_destroy(panel->surface);
    free(panel);
}
