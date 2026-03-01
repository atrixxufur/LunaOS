/*
 * luna-launcher.c — LunaOS full-screen app launcher
 *
 * Activated by: Super key, or clicking "Luna" in the panel.
 * Renders as a wlr-layer-shell OVERLAY surface (above everything).
 * Shows app icons in a grid, launches apps on click.
 *
 * App discovery: reads /usr/local/share/applications/*.desktop files
 * plus a built-in list of bundled LunaOS apps.
 */

#include "luna-shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

/* Built-in app list (shown even before .desktop files are scanned) */
static const struct luna_app BUILTIN_APPS[] = {
    { "Terminal",    "foot",                  "terminal",    "System",  0xFF2D8CFF },
    { "Files",       "luna-files",            "file-manager","Files",   0xFF4CAF50 },
    { "Text Editor", "luna-editor",           "text-editor", "Utility", 0xFFFF9800 },
    { "Browser",     "luna-browser",          "web-browser", "Network", 0xFF9C27B0 },
    { "Settings",    "luna-settings",         "preferences", "System",  0xFF607D8B },
    { "Terminal Alt","xterm",                 "terminal",    "System",  0xFF00BCD4 },
};
#define BUILTIN_APP_COUNT ((int)(sizeof(BUILTIN_APPS)/sizeof(BUILTIN_APPS[0])))

/* ── Internal struct ──────────────────────────────────────────────────────── */
struct luna_launcher {
    struct luna_shell *shell;

    struct wl_surface            *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_shm_pool           *shm_pool;
    struct wl_buffer             *buffer;

    uint32_t *pixels;
    int       stride, width, height;
    size_t    size;
    int       shm_fd;
    bool      configured;
    bool      visible;

    /* Grid layout */
    int  cols, rows;
    int  cell_w, cell_h;
    int  icon_size;
    int  scroll_offset;         /* rows scrolled, for large app lists */

    /* Hover state */
    int  hover_idx;             /* -1 = none */
    int  pointer_x, pointer_y;
};

/* ── Pixel helpers ────────────────────────────────────────────────────────── */

static void fill(uint32_t *px, int s, int x, int y, int w, int h, uint32_t c) {
    for (int r = y; r < y + h; r++)
        for (int col = x; col < x + w; col++)
            px[r * (s/4) + col] = c;
}

static void fill_rounded(uint32_t *px, int s, int x, int y,
                          int w, int h, int radius, uint32_t color) {
    /* Simple rounded rect — fill inner + 4 edge strips */
    fill(px, s, x + radius, y,          w - 2*radius, h,          color);
    fill(px, s, x,          y + radius, radius,        h - 2*radius, color);
    fill(px, s, x + w - radius, y + radius, radius,   h - 2*radius, color);
    /* Corners: approximate with small squares */
    for (int cy = 0; cy < radius; cy++) {
        int cw = (int)(radius - sqrtf((float)(radius*radius - cy*cy)));
        fill(px, s, x + cw,         y + cy,                radius - cw, 1, color);
        fill(px, s, x + cw,         y + h - 1 - cy,        radius - cw, 1, color);
        fill(px, s, x + w - radius, y + cy,                radius - cw, 1, color);
        fill(px, s, x + w - radius, y + h - 1 - cy,        radius - cw, 1, color);
    }
}

static void draw_text_centered(uint32_t *px, int s,
                                int x, int y, int w,
                                const char *text, uint32_t color, int fsize) {
    int len   = (int)strlen(text);
    int tw    = len * (fsize / 2);
    int start = x + (w - tw) / 2;
    for (int i = 0; i < len && i < 20; i++) {
        int cx = start + i * (fsize / 2);
        if (cx < x || cx + fsize/2 >= x + w) continue;
        fill(px, s, cx, y + fsize/4, fsize/2 - 1, fsize/2, color);
    }
}

/* ── Render ───────────────────────────────────────────────────────────────── */

static void launcher_render(struct luna_launcher *l) {
    if (!l->configured || !l->pixels) return;

    struct luna_shell *shell = l->shell;
    uint32_t *px = l->pixels;
    int s = l->stride, w = l->width, h = l->height;

    /* Semi-transparent dark background with blur effect (simulated) */
    fill(px, s, 0, 0, w, h, 0xE6050510);

    /* Title */
    int title_y = 24;
    draw_text_centered(px, s, 0, title_y, w, "Applications",
                       0xFFEEEEEE, 18);

    /* Search bar placeholder */
    int sb_w = 320, sb_h = 32;
    int sb_x = (w - sb_w) / 2;
    int sb_y = title_y + 32;
    fill_rounded(px, s, sb_x, sb_y, sb_w, sb_h, 8, 0x33FFFFFF);
    draw_text_centered(px, s, sb_x, sb_y + (sb_h - 13) / 2, sb_w,
                       "Search apps...", 0x88EEEEEE, 13);

    /* Grid */
    int grid_y    = sb_y + sb_h + 24;
    int grid_x    = (w - l->cols * l->cell_w) / 2;
    int total_apps = shell->app_count + BUILTIN_APP_COUNT;

    for (int i = 0; i < total_apps; i++) {
        const struct luna_app *app;
        struct luna_app builtin_copy;
        if (i < BUILTIN_APP_COUNT) {
            app = &BUILTIN_APPS[i];
        } else {
            app = &shell->apps[i - BUILTIN_APP_COUNT];
        }

        int col = i % l->cols;
        int row = i / l->cols - l->scroll_offset;
        if (row < 0 || grid_y + row * l->cell_h + l->cell_h > h) continue;

        int cx = grid_x + col * l->cell_w + (l->cell_w - l->icon_size) / 2;
        int cy = grid_y + row * l->cell_h;

        bool hovered = (l->hover_idx == i);

        /* Icon background */
        uint32_t icon_bg = app->accent_color;
        if (hovered) {
            /* lighten on hover */
            uint8_t r = ((icon_bg >> 16) & 0xFF);
            uint8_t g = ((icon_bg >>  8) & 0xFF);
            uint8_t b = ((icon_bg      ) & 0xFF);
            icon_bg = 0xFF000000 |
                      ((uint32_t)((r + 40 > 255 ? 255 : r + 40)) << 16) |
                      ((uint32_t)((g + 40 > 255 ? 255 : g + 40)) <<  8) |
                      ((uint32_t)((b + 40 > 255 ? 255 : b + 40)));
        }
        fill_rounded(px, s, cx, cy, l->icon_size, l->icon_size, 12, icon_bg);

        /* App initial letter as placeholder icon */
        char initial[2] = { app->name[0], 0 };
        draw_text_centered(px, s, cx, cy + l->icon_size/4,
                           l->icon_size, initial, 0xFFFFFFFF, 24);

        /* App name below icon */
        draw_text_centered(px, s, cx - 8, cy + l->icon_size + 6,
                           l->icon_size + 16, app->name,
                           0xFFDDDDDD, 11);
    }

    /* Close hint */
    draw_text_centered(px, s, 0, h - 24, w,
                       "Press Super or Esc to close",
                       0x55EEEEEE, 11);

    wl_surface_attach(l->surface, l->buffer, 0, 0);
    wl_surface_damage_buffer(l->surface, 0, 0, w, h);
    wl_surface_commit(l->surface);
}

/* ── Hit test ─────────────────────────────────────────────────────────────── */

static int launcher_app_at(struct luna_launcher *l, int mx, int my) {
    int grid_y = 24 + 32 + 32 + 24;
    int grid_x = (l->width - l->cols * l->cell_w) / 2;
    int total  = l->shell->app_count + BUILTIN_APP_COUNT;

    for (int i = 0; i < total; i++) {
        int col = i % l->cols;
        int row = i / l->cols - l->scroll_offset;
        if (row < 0) continue;
        int cx = grid_x + col * l->cell_w;
        int cy = grid_y + row * l->cell_h;
        if (mx >= cx && mx < cx + l->cell_w &&
            my >= cy && my < cy + l->cell_h)
            return i;
    }
    return -1;
}

/* ── Layer surface callbacks ─────────────────────────────────────────────── */

static void launcher_ls_configure(void *data,
        struct zwlr_layer_surface_v1 *ls, uint32_t serial,
        uint32_t width, uint32_t height) {
    struct luna_launcher *l = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    l->width  = (int)width;
    l->height = (int)height;
    l->configured = true;

    /* (Re)allocate SHM buffer */
    if (l->pixels) munmap(l->pixels, l->size);
    if (l->buffer)   wl_buffer_destroy(l->buffer);
    if (l->shm_pool) wl_shm_pool_destroy(l->shm_pool);
    if (l->shm_fd >= 0) close(l->shm_fd);

    l->stride = width * 4;
    l->size   = (size_t)(l->stride * (int)height);
    char name[64]; snprintf(name, sizeof(name), "/luna-launcher-%d", getpid());
    l->shm_fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    shm_unlink(name);
    ftruncate(l->shm_fd, (off_t)l->size);
    l->pixels = mmap(NULL, l->size, PROT_READ|PROT_WRITE,
                     MAP_SHARED, l->shm_fd, 0);
    l->shm_pool = wl_shm_create_pool(l->shell->shm, l->shm_fd, (int32_t)l->size);
    l->buffer   = wl_shm_pool_create_buffer(l->shm_pool, 0,
                      (int)width, (int)height, l->stride, WL_SHM_FORMAT_ARGB8888);

    /* Compute grid layout */
    l->icon_size = l->shell->config.launcher_icon_size;
    l->cell_w    = l->icon_size + 32;
    l->cell_h    = l->icon_size + 40;
    l->cols      = (l->width - 64) / l->cell_w;
    if (l->cols < 1) l->cols = 1;

    launcher_render(l);
}

static void launcher_ls_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    (void)ls; (void)data;
}

static const struct zwlr_layer_surface_v1_listener launcher_ls_listener = {
    .configure = launcher_ls_configure,
    .closed    = launcher_ls_closed,
};

/* ── Click handler ────────────────────────────────────────────────────────── */

void luna_launcher_handle_click(struct luna_launcher *l, int mx, int my) {
    int idx = launcher_app_at(l, mx, my);
    if (idx < 0) { luna_launcher_hide(l); return; }

    const struct luna_app *app;
    if (idx < BUILTIN_APP_COUNT)
        app = &BUILTIN_APPS[idx];
    else
        app = &l->shell->apps[idx - BUILTIN_APP_COUNT];

    luna_launcher_hide(l);
    luna_app_launch(l->shell, app);
}

/* ── App launch ───────────────────────────────────────────────────────────── */

void luna_app_launch(struct luna_shell *shell, const struct luna_app *app) {
    (void)shell;
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: set env and exec */
        setenv("WAYLAND_DISPLAY", getenv("WAYLAND_DISPLAY") ?: "wayland-0", 1);
        setenv("XDG_RUNTIME_DIR", getenv("XDG_RUNTIME_DIR") ?: "/run/user/501", 1);
        /* Use shell to parse the exec string */
        execl("/bin/sh", "sh", "-c", app->exec, NULL);
        _exit(1);
    }
    /* Parent: don't wait — let it run independently */
}

/* ── App loading from .desktop files ─────────────────────────────────────── */

int luna_apps_load(struct luna_shell *shell, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;

    struct luna_app *apps = NULL;
    int count = 0, cap = 16;
    apps = calloc(cap, sizeof(*apps));

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strstr(ent->d_name, ".desktop")) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        if (count >= cap) {
            cap *= 2;
            apps = realloc(apps, cap * sizeof(*apps));
        }
        struct luna_app *a = &apps[count];
        memset(a, 0, sizeof(*a));
        a->accent_color = 0xFF607D8B; /* default grey */

        char line[512];
        bool in_desktop_entry = false;
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = 0;
            if (!strcmp(line, "[Desktop Entry]")) { in_desktop_entry = true; continue; }
            if (line[0] == '[') { in_desktop_entry = false; continue; }
            if (!in_desktop_entry) continue;
            if (!strncmp(line, "Name=",     5)) strncpy(a->name,     line+5, 63);
            if (!strncmp(line, "Exec=",     5)) strncpy(a->exec,     line+5, 255);
            if (!strncmp(line, "Icon=",     5)) strncpy(a->icon_name, line+5, 63);
            if (!strncmp(line, "Categories=",11)) strncpy(a->category, line+11, 31);
        }
        fclose(f);

        /* Remove %U %f etc from Exec */
        char *pct = strchr(a->exec, '%');
        if (pct) *(pct - 1) = 0;

        if (a->name[0] && a->exec[0]) count++;
    }
    closedir(d);

    shell->apps      = apps;
    shell->app_count = count;
    return count;
}

/* ── Public show/hide ─────────────────────────────────────────────────────── */

void luna_launcher_show(struct luna_launcher *l) {
    if (l->visible) return;
    l->visible = true;
    l->shell->launcher_visible = true;
    wl_surface_commit(l->surface);  /* triggers configure */
    launcher_render(l);
}

void luna_launcher_hide(struct luna_launcher *l) {
    if (!l->visible) return;
    l->visible = false;
    l->shell->launcher_visible = false;
    /* Detach buffer to hide surface */
    wl_surface_attach(l->surface, NULL, 0, 0);
    wl_surface_commit(l->surface);
}

/* ── Constructor / destructor ────────────────────────────────────────────── */

struct luna_launcher *luna_launcher_create(struct luna_shell *shell) {
    struct luna_launcher *l = calloc(1, sizeof(*l));
    l->shell    = shell;
    l->shm_fd   = -1;
    l->hover_idx = -1;

    l->surface = wl_compositor_create_surface(shell->compositor);
    l->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        shell->layer_shell, l->surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "luna-launcher");

    zwlr_layer_surface_v1_set_anchor(l->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP  | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(l->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(l->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);

    zwlr_layer_surface_v1_add_listener(l->layer_surface,
                                       &launcher_ls_listener, l);
    /* Don't commit yet — show only when requested */
    return l;
}

void luna_launcher_destroy(struct luna_launcher *l) {
    if (l->pixels) munmap(l->pixels, l->size);
    if (l->buffer)   wl_buffer_destroy(l->buffer);
    if (l->shm_pool) wl_shm_pool_destroy(l->shm_pool);
    if (l->shm_fd >= 0) close(l->shm_fd);
    zwlr_layer_surface_v1_destroy(l->layer_surface);
    wl_surface_destroy(l->surface);
    free(l);
}
