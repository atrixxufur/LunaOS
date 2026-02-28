/*
 * luna-compositor.h — LunaOS Wayland compositor
 *
 * A wlroots-based Wayland compositor targeting PureDarwin x86_64.
 * Uses our darwin-wayland glue layer for display and input.
 *
 * Feature set (Phase 1 — software rendering):
 *   - DRM/KMS output via IODRMShim.kext + libdrm-darwin
 *   - Input via darwin-evdev-bridge + libinput
 *   - Seat management via seatd-darwin
 *   - Pixman software renderer (no GPU required)
 *   - XDG shell (xdg_toplevel, xdg_popup)
 *   - Layer shell (for panels, wallpapers, overlays)
 *   - Basic window management: tiling + floating
 *   - XWayland support stub (disabled by default, enable with -DLUNA_XWAYLAND)
 */

#pragma once

#include <stdbool.h>
#include <wayland-server-core.h>

/* wlroots headers — all included from the installed wlroots prefix */
#include <wlr/backend.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/util/log.h>

/* ── Forward declarations ─────────────────────────────────────────────────── */

struct luna_server;
struct luna_output;
struct luna_view;
struct luna_keyboard;
struct luna_pointer;
struct luna_layer_surface;

/* ── Server ───────────────────────────────────────────────────────────────── */

struct luna_server {
    struct wl_display        *display;
    struct wl_event_loop     *event_loop;

    /* wlroots core */
    struct wlr_backend       *backend;
    struct wlr_renderer      *renderer;
    struct wlr_allocator     *allocator;
    struct wlr_scene         *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_output_layout *output_layout;

    /* Wayland protocols */
    struct wlr_compositor    *compositor;
    struct wlr_subcompositor *subcompositor;
    struct wlr_data_device_manager *data_device_mgr;
    struct wlr_xdg_shell     *xdg_shell;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wlr_screencopy_manager_v1 *screencopy;
    struct wlr_viewporter    *viewporter;
    struct wlr_presentation  *presentation;
    struct wlr_gamma_control_manager_v1 *gamma_ctrl;
    struct wlr_single_pixel_buffer_manager_v1 *single_pixel;
    struct wlr_xdg_output_manager_v1 *xdg_output_mgr;

    /* Seat / input */
    struct wlr_seat          *seat;
    struct wlr_cursor        *cursor;
    struct wlr_xcursor_manager *xcursor_mgr;

    /* Lists */
    struct wl_list outputs;     /* luna_output */
    struct wl_list views;       /* luna_view (mapped, in focus order) */
    struct wl_list keyboards;   /* luna_keyboard */

    /* Focus tracking */
    struct luna_view         *focused_view;

    /* Scene tree layers (bottom → top) */
    struct wlr_scene_tree    *layer_background;
    struct wlr_scene_tree    *layer_bottom;
    struct wlr_scene_tree    *layer_normal;   /* regular windows */
    struct wlr_scene_tree    *layer_top;
    struct wlr_scene_tree    *layer_overlay;

    /* Listeners for global events */
    struct wl_listener new_output;
    struct wl_listener new_input;
    struct wl_listener new_xdg_surface;
    struct wl_listener new_layer_surface;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_listener request_set_cursor;
    struct wl_listener request_set_selection;

    /* Config */
    struct luna_config       *config;
};

/* ── Output ───────────────────────────────────────────────────────────────── */

struct luna_output {
    struct wl_list           link;       /* luna_server.outputs */
    struct luna_server       *server;
    struct wlr_output        *wlr_output;
    struct wlr_scene_output  *scene_output;

    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;

    /* Layer surfaces on this output */
    struct wl_list layer_surfaces;      /* luna_layer_surface */
};

/* ── View (toplevel window) ───────────────────────────────────────────────── */

typedef enum {
    LUNA_VIEW_XDG,       /* standard xdg_toplevel */
    /* LUNA_VIEW_XWAYLAND — future */
} luna_view_type;

struct luna_view {
    struct wl_list           link;       /* luna_server.views */
    struct luna_server       *server;
    luna_view_type            type;

    struct wlr_xdg_toplevel  *xdg_toplevel;
    struct wlr_scene_tree    *scene_tree;

    /* State */
    bool     mapped;
    bool     maximized;
    bool     fullscreen;
    int      saved_x, saved_y;
    int      saved_w, saved_h;

    /* Listeners */
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener set_title;
};

/* ── Input devices ────────────────────────────────────────────────────────── */

struct luna_keyboard {
    struct wl_list       link;
    struct luna_server  *server;
    struct wlr_keyboard *wlr_keyboard;

    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

/* ── Layer surface ────────────────────────────────────────────────────────── */

struct luna_layer_surface {
    struct wl_list            link;
    struct luna_output       *output;
    struct wlr_layer_surface_v1 *wlr_layer_surface;
    struct wlr_scene_tree    *scene_tree;
    struct wl_listener        map;
    struct wl_listener        unmap;
    struct wl_listener        destroy;
    struct wl_listener        surface_commit;
};

/* ── Config ───────────────────────────────────────────────────────────────── */

struct luna_config {
    const char  *seat_name;        /* default: "seat0" */
    const char  *cursor_theme;     /* default: NULL (system default) */
    int          cursor_size;      /* default: 24 */
    bool         xwayland;         /* default: false */
    int          border_width;     /* px, default: 2 */
    uint32_t     border_color_focused;   /* ARGB, default: 0xFF4A90D9 */
    uint32_t     border_color_unfocused; /* ARGB, default: 0xFF333333 */
    /* TODO: keybinding table, tiling layout, etc. */
};

/* ── Public API ───────────────────────────────────────────────────────────── */

struct luna_server *luna_server_create(void);
void                luna_server_run(struct luna_server *server);
void                luna_server_destroy(struct luna_server *server);

/* Focus */
void luna_focus_view(struct luna_view *view, struct wlr_surface *surface);
struct luna_view *luna_view_at(struct luna_server *server,
                               double lx, double ly,
                               struct wlr_surface **surface,
                               double *sx, double *sy);

/* Window management */
void luna_view_maximize(struct luna_view *view, bool maximize);
void luna_view_fullscreen(struct luna_view *view, bool fullscreen);
void luna_begin_move(struct luna_view *view, uint32_t serial);
void luna_begin_resize(struct luna_view *view, uint32_t edges, uint32_t serial);

/* Config */
struct luna_config *luna_config_load(const char *path);
struct luna_config *luna_config_default(void);
