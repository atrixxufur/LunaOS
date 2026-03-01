/*
 * luna-shell-main.c — LunaOS desktop shell entry point
 *
 * Connects to luna-compositor's Wayland socket, binds all protocols,
 * creates panel + wallpaper on each output, runs the event loop.
 *
 * Also implements:
 *   - org.freedesktop.Notifications D-Bus interface (notification daemon)
 *   - Super key → toggle launcher
 *   - Foreign toplevel tracking (window list in panel)
 */

#include "luna-shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

/* ── Global for signal handler ────────────────────────────────────────────── */
static struct luna_shell *g_shell = NULL;
static void sig_handler(int s) { (void)s; if (g_shell) g_shell->running = false; }

/* ── Default config ───────────────────────────────────────────────────────── */
static struct luna_shell_config default_config(void) {
    return (struct luna_shell_config){
        .panel_height         = 36,
        .panel_bg_color       = 0xE6101010,
        .panel_fg_color       = 0xFFEEEEEE,
        .panel_accent_color   = 0xFF4A90D9,
        .font_family          = "sans-serif",
        .font_size            = 13,
        .launcher_columns     = 6,
        .launcher_icon_size   = 64,
        .launcher_bg_color    = 0xE6050510,
        .wallpaper_path       = NULL,
        .wallpaper_color_a    = 0xFF1a1a2e,
        .wallpaper_color_b    = 0xFF16213e,
        .clock_format         = "%H:%M",
        .clock_show_seconds   = false,
        .notif_timeout_ms     = 5000,
        .notif_max_visible    = 3,
    };
}

/* ── Foreign toplevel management ─────────────────────────────────────────── */

static void toplevel_handle_title(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *h, const char *title) {
    (void)h;
    struct luna_toplevel *tl = data;
    strncpy(tl->title, title, sizeof(tl->title) - 1);
    /* Mark all panels dirty */
    struct luna_output *out;
    wl_list_for_each(out, &tl->link, link) { /* iterate via shell */ }
}

static void toplevel_handle_app_id(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *h, const char *app_id) {
    (void)h;
    struct luna_toplevel *tl = data;
    strncpy(tl->app_id, app_id, sizeof(tl->app_id) - 1);
}

static void toplevel_handle_state(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *state) {
    (void)h;
    struct luna_toplevel *tl = data;
    tl->maximized = tl->minimized = tl->activated = false;
    uint32_t *s;
    wl_array_for_each(s, state) {
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED)  tl->maximized = true;
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)  tl->minimized = true;
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)  tl->activated = true;
    }
}

static void toplevel_handle_done(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *h) {
    (void)h; (void)data;
    /* Re-render panels */
}

static void toplevel_handle_closed(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *h) {
    struct luna_toplevel *tl = data;
    wl_list_remove(&tl->link);
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    free(tl);
}

static void toplevel_handle_parent(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *h,
        struct zwlr_foreign_toplevel_handle_v1 *parent) {
    (void)data; (void)h; (void)parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_listener = {
    .title      = toplevel_handle_title,
    .app_id     = toplevel_handle_app_id,
    .output_enter = NULL,
    .output_leave = NULL,
    .state      = toplevel_handle_state,
    .done       = toplevel_handle_done,
    .closed     = toplevel_handle_closed,
    .parent     = toplevel_handle_parent,
};

static void toplevel_mgr_handle_toplevel(void *data,
        struct zwlr_foreign_toplevel_manager_v1 *mgr,
        struct zwlr_foreign_toplevel_handle_v1 *handle) {
    (void)mgr;
    struct luna_shell *shell = data;
    struct luna_toplevel *tl = calloc(1, sizeof(*tl));
    tl->handle = handle;
    wl_list_insert(&shell->toplevels, &tl->link);
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_listener, tl);
}

static void toplevel_mgr_finished(void *data,
        struct zwlr_foreign_toplevel_manager_v1 *mgr) {
    (void)data; (void)mgr;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevel_mgr_listener = {
    .toplevel = toplevel_mgr_handle_toplevel,
    .finished = toplevel_mgr_finished,
};

/* ── Output handling ──────────────────────────────────────────────────────── */

static void output_geometry(void *data, struct wl_output *o,
    int32_t x, int32_t y, int32_t pw, int32_t ph,
    int32_t subpixel, const char *make, const char *model, int32_t transform) {
    (void)o;(void)x;(void)y;(void)pw;(void)ph;
    (void)subpixel;(void)make;(void)model;(void)transform;
}

static void output_mode(void *data, struct wl_output *o,
    uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void)o; (void)refresh;
    struct luna_output *out = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        out->width  = width;
        out->height = height;
    }
}

static void output_done(void *data, struct wl_output *o) {
    (void)o;
    struct luna_output *out = data;
    if (!out->panel) {
        out->panel     = luna_panel_create(out->shell, out);
        out->wallpaper = luna_wallpaper_create(out->shell, out);
    }
}

static void output_scale(void *data, struct wl_output *o, int32_t factor) {
    (void)o;
    struct luna_output *out = data;
    out->scale = factor;
}

static void output_name(void *data, struct wl_output *o, const char *name) {
    (void)o;
    struct luna_output *out = data;
    strncpy(out->name, name, sizeof(out->name) - 1);
}

static void output_description(void *data, struct wl_output *o, const char *d) {
    (void)data; (void)o; (void)d;
}

static const struct wl_output_listener output_listener = {
    .geometry    = output_geometry,
    .mode        = output_mode,
    .done        = output_done,
    .scale       = output_scale,
    .name        = output_name,
    .description = output_description,
};

/* ── Keyboard input ───────────────────────────────────────────────────────── */

static void kb_keymap(void *data, struct wl_keyboard *kb,
    uint32_t fmt, int32_t fd, uint32_t size) {
    (void)data;(void)kb;(void)fmt;(void)fd;(void)size;
    close(fd);
}
static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t s,
    struct wl_surface *surf, struct wl_array *keys) {
    (void)data;(void)kb;(void)s;(void)surf;(void)keys;
}
static void kb_leave(void *data, struct wl_keyboard *kb,
    uint32_t s, struct wl_surface *surf) {
    (void)data;(void)kb;(void)s;(void)surf;
}
static void kb_key(void *data, struct wl_keyboard *kb,
    uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)kb;(void)serial;(void)time;
    struct luna_shell *shell = data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    /* KEY_LEFTMETA = 125, KEY_RIGHTMETA = 126 (Super key) */
    if (key == 125 || key == 126) {
        if (shell->launcher_visible)
            luna_launcher_hide(shell->launcher);
        else
            luna_launcher_show(shell->launcher);
    }
    /* Escape — close launcher */
    if (key == 1 && shell->launcher_visible)
        luna_launcher_hide(shell->launcher);
}
static void kb_modifiers(void *data, struct wl_keyboard *kb,
    uint32_t s, uint32_t dmods, uint32_t lmods, uint32_t group) {
    (void)data;(void)kb;(void)s;(void)dmods;(void)lmods;(void)group;
}
static void kb_repeat_info(void *data, struct wl_keyboard *kb,
    int32_t rate, int32_t delay) {
    (void)data;(void)kb;(void)rate;(void)delay;
}
static const struct wl_keyboard_listener kb_listener = {
    .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave,
    .key = kb_key, .modifiers = kb_modifiers, .repeat_info = kb_repeat_info,
};

/* ── Seat ─────────────────────────────────────────────────────────────────── */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    struct luna_shell *shell = data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !shell->keyboard) {
        shell->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(shell->keyboard, &kb_listener, shell);
    }
}
static void seat_name(void *data, struct wl_seat *s, const char *name) {
    (void)data;(void)s;(void)name;
}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities, .name = seat_name,
};

/* ── Registry ─────────────────────────────────────────────────────────────── */

static void registry_global(void *data, struct wl_registry *reg,
    uint32_t name, const char *iface, uint32_t version) {
    struct luna_shell *shell = data;

    if (!strcmp(iface, wl_compositor_interface.name))
        shell->compositor = wl_registry_bind(reg, name,
                                &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        shell->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name)) {
        shell->seat = wl_registry_bind(reg, name, &wl_seat_interface,
                                        (version < 7 ? version : 7));
        wl_seat_add_listener(shell->seat, &seat_listener, shell);
    }
    else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        shell->layer_shell = wl_registry_bind(reg, name,
                                &zwlr_layer_shell_v1_interface,
                                (version < 4 ? version : 4));
    else if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
        shell->toplevel_mgr = wl_registry_bind(reg, name,
                                &zwlr_foreign_toplevel_manager_v1_interface, 3);
        zwlr_foreign_toplevel_manager_v1_add_listener(
            shell->toplevel_mgr, &toplevel_mgr_listener, shell);
    }
    else if (!strcmp(iface, wl_output_interface.name)) {
        struct luna_output *out = calloc(1, sizeof(*out));
        out->shell     = shell;
        out->scale     = 1;
        out->wl_output = wl_registry_bind(reg, name, &wl_output_interface,
                                           (version < 4 ? version : 4));
        wl_list_insert(&shell->outputs, &out->link);
        wl_output_add_listener(out->wl_output, &output_listener, out);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg,
    uint32_t name) { (void)data;(void)reg;(void)name; }

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* ── Notification daemon (simple, no D-Bus — uses a UNIX socket) ─────────── */

uint32_t luna_notify(struct luna_shell *shell, const char *app,
                     const char *summary, const char *body, int timeout_ms) {
    struct luna_notification *n = calloc(1, sizeof(*n));
    n->id = ++shell->next_notif_id;
    strncpy(n->app_name, app     ? app     : "System",  63);
    strncpy(n->summary,  summary ? summary : "",        127);
    strncpy(n->body,     body    ? body    : "",        511);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    n->expire_at_ms = (timeout_ms > 0) ? now_ms + timeout_ms : -1;

    wl_list_insert(&shell->notifications, &n->link);
    fprintf(stderr, "[notify] %s: %s\n", n->app_name, n->summary);
    return n->id;
}

void luna_notifyd_tick(struct luna_shell *shell) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    struct luna_notification *n, *tmp;
    wl_list_for_each_safe(n, tmp, &shell->notifications, link) {
        if (n->expire_at_ms >= 0 && now >= n->expire_at_ms) {
            wl_list_remove(&n->link);
            free(n);
        }
    }
}

/* ── Shell create / run / destroy ─────────────────────────────────────────── */

struct luna_shell *luna_shell_create(void) {
    struct luna_shell *shell = calloc(1, sizeof(*shell));
    shell->config  = default_config();
    shell->running = true;
    wl_list_init(&shell->outputs);
    wl_list_init(&shell->toplevels);
    wl_list_init(&shell->notifications);

    shell->display = wl_display_connect(NULL);
    if (!shell->display) {
        fprintf(stderr, "luna-shell: cannot connect to Wayland display.\n"
                "Is luna-compositor running? Check WAYLAND_DISPLAY env.\n");
        free(shell); return NULL;
    }
    shell->registry = wl_display_get_registry(shell->display);
    wl_registry_add_listener(shell->registry, &registry_listener, shell);
    wl_display_roundtrip(shell->display);
    wl_display_roundtrip(shell->display); /* second roundtrip for outputs */

    if (!shell->compositor || !shell->shm || !shell->layer_shell) {
        fprintf(stderr, "luna-shell: compositor missing required protocols.\n"
                "wl_compositor=%p wl_shm=%p layer_shell=%p\n",
                (void*)shell->compositor, (void*)shell->shm,
                (void*)shell->layer_shell);
        wl_display_disconnect(shell->display);
        free(shell); return NULL;
    }

    /* Load apps */
    luna_apps_load(shell, "/usr/local/share/applications");

    /* Create launcher (hidden initially) */
    shell->launcher = luna_launcher_create(shell);

    /* Startup notification */
    luna_notify(shell, "LunaOS", "Welcome to LunaOS",
                "Press Super to open the launcher. Super+T for terminal.",
                shell->config.notif_timeout_ms);

    return shell;
}

void luna_shell_run(struct luna_shell *shell) {
    wl_display_flush(shell->display);

    while (shell->running) {
        if (wl_display_dispatch(shell->display) < 0) break;

        /* Tick clock every second — re-render panels */
        static time_t last_tick = 0;
        time_t now = time(NULL);
        if (now != last_tick) {
            last_tick = now;
            struct luna_output *out;
            wl_list_for_each(out, &shell->outputs, link) {
                if (out->panel) luna_panel_render(out->panel);
            }
            luna_notifyd_tick(shell);
        }
    }
}

void luna_shell_destroy(struct luna_shell *shell) {
    struct luna_output *out, *tmp_out;
    wl_list_for_each_safe(out, tmp_out, &shell->outputs, link) {
        if (out->panel)     luna_panel_destroy(out->panel);
        if (out->wallpaper) luna_wallpaper_destroy(out->wallpaper);
        wl_output_destroy(out->wl_output);
        free(out);
    }
    if (shell->launcher) luna_launcher_destroy(shell->launcher);
    if (shell->apps)     free(shell->apps);
    wl_display_disconnect(shell->display);
    free(shell);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *wallpaper = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wallpaper") && i+1 < argc)
            wallpaper = argv[++i];
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGCHLD, SIG_IGN);

    struct luna_shell *shell = luna_shell_create();
    if (!shell) return 1;
    if (wallpaper) shell->config.wallpaper_path = wallpaper;
    g_shell = shell;

    fprintf(stderr, "luna-shell: running. Super=launcher, Super+T=terminal\n");
    luna_shell_run(shell);
    luna_shell_destroy(shell);
    return 0;
}
