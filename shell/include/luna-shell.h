/*
 * luna-shell.h — LunaOS desktop shell
 *
 * The shell is a Wayland client that runs on top of luna-compositor.
 * It uses the wlr-layer-shell protocol to anchor itself to screen edges,
 * and wlr-foreign-toplevel-management to track open windows.
 *
 * Components:
 *   luna-panel      — top bar: clock, launcher button, systray, window list
 *   luna-launcher   — full-screen app grid (Super key or panel button)
 *   luna-wallpaper  — background surface (bottom layer)
 *   luna-notifyd    — notification daemon (org.freedesktop.Notifications)
 *   luna-settings   — GTK-free settings app (pure Wayland/EGL)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include <wayland-client.h>
#include <wayland-client-protocol.h>

/* wlroots layer-shell protocol (generated from XML) */
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
/* wlroots foreign toplevel management */
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

/* EGL + OpenGL ES for panel rendering */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

/* ── Forward declarations ─────────────────────────────────────────────────── */
struct luna_shell;
struct luna_panel;
struct luna_launcher;
struct luna_wallpaper;
struct luna_notifyd;
struct luna_output;
struct luna_toplevel;

/* ── Shell config ─────────────────────────────────────────────────────────── */
struct luna_shell_config {
    /* Panel */
    int      panel_height;          /* px, default 36 */
    uint32_t panel_bg_color;        /* ARGB, default 0xE6101010 */
    uint32_t panel_fg_color;        /* ARGB, default 0xFFEEEEEE */
    uint32_t panel_accent_color;    /* ARGB, default 0xFF4A90D9 */
    const char *font_family;        /* default "sans-serif" */
    int      font_size;             /* default 13 */

    /* Launcher */
    int      launcher_columns;      /* default 5 */
    int      launcher_icon_size;    /* default 64 */
    uint32_t launcher_bg_color;     /* ARGB semi-transparent dark */

    /* Wallpaper */
    const char *wallpaper_path;     /* path to image, or NULL for gradient */
    uint32_t    wallpaper_color_a;  /* gradient start, default 0xFF1a1a2e */
    uint32_t    wallpaper_color_b;  /* gradient end,   default 0xFF16213e */

    /* Clock */
    const char *clock_format;       /* strftime format, default "%H:%M" */
    bool        clock_show_seconds;

    /* Notifications */
    int      notif_timeout_ms;      /* default 5000 */
    int      notif_max_visible;     /* default 3 */
};

/* ── Output (monitor) ─────────────────────────────────────────────────────── */
struct luna_output {
    struct wl_list      link;
    struct luna_shell  *shell;
    struct wl_output   *wl_output;
    int32_t             width, height;
    int32_t             scale;
    char                name[64];

    struct luna_panel      *panel;
    struct luna_wallpaper  *wallpaper;
};

/* ── Toplevel window (tracked for panel window list) ─────────────────────── */
struct luna_toplevel {
    struct wl_list  link;
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char            title[256];
    char            app_id[128];
    bool            maximized;
    bool            minimized;
    bool            activated;      /* focused */
};

/* ── App descriptor (for launcher) ───────────────────────────────────────── */
struct luna_app {
    char        name[64];
    char        exec[256];          /* command to launch */
    char        icon_name[64];      /* XDG icon name */
    char        category[32];       /* XDG category */
    uint32_t    accent_color;       /* per-app color for icon bg */
};

/* ── Notification ─────────────────────────────────────────────────────────── */
struct luna_notification {
    struct wl_list  link;
    uint32_t        id;
    char            app_name[64];
    char            summary[128];
    char            body[512];
    int64_t         expire_at_ms;   /* monotonic ms, -1 = persistent */
};

/* ── Main shell struct ────────────────────────────────────────────────────── */
struct luna_shell {
    /* Wayland globals */
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_seat       *seat;
    struct wl_shm        *shm;

    /* Layer shell + toplevel management */
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_foreign_toplevel_manager_v1 *toplevel_mgr;

    /* EGL */
    EGLDisplay   egl_display;
    EGLContext   egl_context;
    EGLConfig    egl_config;

    /* Lists */
    struct wl_list outputs;         /* luna_output */
    struct wl_list toplevels;       /* luna_toplevel */
    struct wl_list notifications;   /* luna_notification */

    /* Components */
    struct luna_launcher  *launcher;
    struct luna_notifyd   *notifyd;

    /* State */
    bool           launcher_visible;
    bool           running;
    uint32_t       next_notif_id;

    /* Config */
    struct luna_shell_config config;

    /* App list (loaded from /usr/local/share/applications/*.desktop) */
    struct luna_app *apps;
    int              app_count;

    /* Input */
    struct wl_keyboard  *keyboard;
    struct wl_pointer   *pointer;
    uint32_t             pointer_x, pointer_y;
};

/* ── Public API ───────────────────────────────────────────────────────────── */
struct luna_shell *luna_shell_create(void);
void               luna_shell_run(struct luna_shell *shell);
void               luna_shell_destroy(struct luna_shell *shell);

/* Panel */
struct luna_panel *luna_panel_create(struct luna_shell *shell,
                                     struct luna_output *output);
void               luna_panel_destroy(struct luna_panel *panel);
void               luna_panel_render(struct luna_panel *panel);

/* Launcher */
struct luna_launcher *luna_launcher_create(struct luna_shell *shell);
void                  luna_launcher_show(struct luna_launcher *launcher);
void                  luna_launcher_hide(struct luna_launcher *launcher);
void                  luna_launcher_destroy(struct luna_launcher *launcher);

/* Wallpaper */
struct luna_wallpaper *luna_wallpaper_create(struct luna_shell *shell,
                                             struct luna_output *output);
void                   luna_wallpaper_destroy(struct luna_wallpaper *wp);

/* Notifications */
void luna_notifyd_start(struct luna_shell *shell);
uint32_t luna_notify(struct luna_shell *shell, const char *app,
                     const char *summary, const char *body, int timeout_ms);
void luna_notifyd_tick(struct luna_shell *shell);

/* Apps */
int  luna_apps_load(struct luna_shell *shell, const char *dir);
void luna_app_launch(struct luna_shell *shell, const struct luna_app *app);
