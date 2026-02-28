/*
 * luna-compositor/src/main.c — LunaOS Wayland compositor
 *
 * Entry point. Sets up the wlroots backend (DRM via IODRMShim), initialises
 * all Wayland protocols, and runs the event loop.
 *
 * Build: see compositor/CMakeLists.txt
 * Run:   luna-compositor [--config ~/.config/luna/compositor.ini]
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include "luna-compositor.h"

/* ── Globals ──────────────────────────────────────────────────────────────── */

static struct luna_server *g_server = NULL;

static void sig_handler(int sig) {
    (void)sig;
    if (g_server) wl_display_terminate(g_server->display);
}

/* ── Output handling ──────────────────────────────────────────────────────── */

static void output_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_output *output =
        wl_container_of(listener, output, frame);

    struct wlr_scene_output *scene_output = output->scene_output;
    if (!wlr_scene_output_commit(scene_output, NULL))
        return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
    struct luna_output *output =
        wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_output *output =
        wl_container_of(listener, output, destroy);
    wlr_scene_output_destroy(output->scene_output);
    wl_list_remove(&output->link);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    struct luna_server *server =
        wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    /* Use pixman allocator on Darwin (no GPU) */
    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    /* Pick the preferred mode (highest resolution at native refresh) */
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode) wlr_output_state_set_mode(&state, mode);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    struct luna_output *output = calloc(1, sizeof(*output));
    output->server     = server;
    output->wlr_output = wlr_output;
    wl_list_init(&output->layer_surfaces);

    output->frame.notify          = output_frame;
    output->request_state.notify  = output_request_state;
    output->destroy.notify         = output_destroy;
    wl_signal_add(&wlr_output->events.frame,          &output->frame);
    wl_signal_add(&wlr_output->events.request_state,  &output->request_state);
    wl_signal_add(&wlr_output->events.destroy,         &output->destroy);

    /* Add to output layout at (0,0) — multi-monitor: auto-arrange later */
    struct wlr_output_layout_output *lo =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    output->scene_output =
        wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, lo,
                                       output->scene_output);

    wl_list_insert(&server->outputs, &output->link);

    wlr_log(WLR_INFO, "new output: %s %dx%d",
            wlr_output->name,
            wlr_output->width, wlr_output->height);
}

/* ── View / XDG shell ─────────────────────────────────────────────────────── */

static void view_map(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_view *view = wl_container_of(listener, view, map);
    view->mapped = true;
    wl_list_insert(&view->server->views, &view->link);
    luna_focus_view(view, view->xdg_toplevel->base->surface);
}

static void view_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_view *view = wl_container_of(listener, view, unmap);
    view->mapped = false;
    wl_list_remove(&view->link);
    /* Focus the next view if this was focused */
    if (view->server->focused_view == view) {
        view->server->focused_view = NULL;
        struct luna_view *next;
        wl_list_for_each(next, &view->server->views, link) {
            luna_focus_view(next, next->xdg_toplevel->base->surface);
            break;
        }
    }
}

static void view_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_view *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->set_title.link);
    free(view);
}

static void view_request_maximize(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_view *view = wl_container_of(listener, view, request_maximize);
    luna_view_maximize(view, view->xdg_toplevel->requested.maximized);
}

static void view_request_fullscreen(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_view *view =
        wl_container_of(listener, view, request_fullscreen);
    luna_view_fullscreen(view, view->xdg_toplevel->requested.fullscreen);
}

static void view_request_move(struct wl_listener *listener, void *data) {
    struct luna_view *view = wl_container_of(listener, view, request_move);
    const struct wlr_xdg_toplevel_move_event *event = data;
    luna_begin_move(view, event->serial);
}

static void view_request_resize(struct wl_listener *listener, void *data) {
    struct luna_view *view = wl_container_of(listener, view, request_resize);
    const struct wlr_xdg_toplevel_resize_event *event = data;
    luna_begin_resize(view, event->edges, event->serial);
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
    struct luna_server *server =
        wl_container_of(listener, server, new_xdg_surface);
    struct wlr_xdg_surface *surface = data;

    /* Popups are managed by the scene tree automatically */
    if (surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
        struct wlr_xdg_surface *parent =
            wlr_xdg_surface_try_from_wlr_surface(surface->popup->parent);
        struct wlr_scene_tree *parent_tree = parent->data;
        surface->data = wlr_scene_xdg_surface_create(parent_tree, surface);
        return;
    }

    /* Toplevel */
    struct luna_view *view = calloc(1, sizeof(*view));
    view->server       = server;
    view->type         = LUNA_VIEW_XDG;
    view->xdg_toplevel = surface->toplevel;
    view->scene_tree   = wlr_scene_xdg_surface_create(
                             server->layer_normal, surface);
    surface->data      = view->scene_tree;
    view->scene_tree->node.data = view;

    view->map.notify              = view_map;
    view->unmap.notify            = view_unmap;
    view->destroy.notify           = view_destroy;
    view->request_maximize.notify  = view_request_maximize;
    view->request_fullscreen.notify= view_request_fullscreen;
    view->request_move.notify      = view_request_move;
    view->request_resize.notify    = view_request_resize;
    view->set_title.notify         = NULL; /* stub */

    wl_signal_add(&surface->surface->events.map,            &view->map);
    wl_signal_add(&surface->surface->events.unmap,          &view->unmap);
    wl_signal_add(&surface->events.destroy,                 &view->destroy);
    wl_signal_add(&surface->toplevel->events.request_maximize,
                  &view->request_maximize);
    wl_signal_add(&surface->toplevel->events.request_fullscreen,
                  &view->request_fullscreen);
    wl_signal_add(&surface->toplevel->events.request_move,  &view->request_move);
    wl_signal_add(&surface->toplevel->events.request_resize,&view->request_resize);
}

/* ── Keyboard ─────────────────────────────────────────────────────────────── */

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_keyboard *kb = wl_container_of(listener, kb, modifiers);
    wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(kb->server->seat,
                                       &kb->wlr_keyboard->modifiers);
}

static bool handle_compositor_keybind(struct luna_server *server,
                                       xkb_keysym_t sym, uint32_t mods) {
    /* Super+Q: close focused window */
    if ((mods & WLR_MODIFIER_LOGO) && sym == XKB_KEY_q) {
        if (server->focused_view)
            wlr_xdg_toplevel_send_close(
                server->focused_view->xdg_toplevel);
        return true;
    }
    /* Super+T: launch terminal (foot) */
    if ((mods & WLR_MODIFIER_LOGO) && sym == XKB_KEY_t) {
        if (fork() == 0) {
            execl("/usr/local/bin/foot", "foot", NULL);
            execl("/usr/bin/xterm", "xterm", NULL);
            _exit(1);
        }
        return true;
    }
    /* Super+Shift+E: exit compositor */
    if ((mods & WLR_MODIFIER_LOGO) && (mods & WLR_MODIFIER_SHIFT)
            && sym == XKB_KEY_e) {
        wl_display_terminate(server->display);
        return true;
    }
    /* Super+M: maximize focused window */
    if ((mods & WLR_MODIFIER_LOGO) && sym == XKB_KEY_m) {
        if (server->focused_view)
            luna_view_maximize(server->focused_view,
                               !server->focused_view->maximized);
        return true;
    }
    return false;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    struct luna_keyboard *kb = wl_container_of(listener, kb, key);
    const struct wlr_keyboard_key_event *event = data;

    wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);

    /* Translate keycode → xkb keysym */
    uint32_t keycode    = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(
        kb->wlr_keyboard->xkb_state, keycode, &syms);
    uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

    bool handled = false;
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++)
            if (handle_compositor_keybind(kb->server, syms[i], mods))
                handled = true;
    }

    if (!handled)
        wlr_seat_keyboard_notify_key(kb->server->seat,
                                     event->time_msec,
                                     event->keycode,
                                     event->state);
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_keyboard *kb = wl_container_of(listener, kb, destroy);
    wl_list_remove(&kb->modifiers.link);
    wl_list_remove(&kb->key.link);
    wl_list_remove(&kb->destroy.link);
    wl_list_remove(&kb->link);
    free(kb);
}

/* ── Cursor / pointer ─────────────────────────────────────────────────────── */

static void process_cursor_motion(struct luna_server *server, uint32_t time) {
    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    struct luna_view *view = luna_view_at(server,
                                          server->cursor->x,
                                          server->cursor->y,
                                          &surface, &sx, &sy);
    if (!view)
        wlr_cursor_set_xcursor(server->cursor, server->xcursor_mgr, "default");

    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(seat);
    }
}

static void cursor_motion(struct wl_listener *listener, void *data) {
    struct luna_server *server = wl_container_of(listener, server, cursor_motion);
    const struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base,
                    event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct luna_server *server =
        wl_container_of(listener, server, cursor_motion_absolute);
    const struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
                             event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

static void cursor_button(struct wl_listener *listener, void *data) {
    struct luna_server *server = wl_container_of(listener, server, cursor_button);
    const struct wlr_pointer_button_event *event = data;
    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                   event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        double sx, sy;
        struct wlr_surface *surface = NULL;
        struct luna_view *view = luna_view_at(server,
                                              server->cursor->x,
                                              server->cursor->y,
                                              &surface, &sx, &sy);
        if (view) luna_focus_view(view, surface);
    }
}

static void cursor_axis(struct wl_listener *listener, void *data) {
    struct luna_server *server = wl_container_of(listener, server, cursor_axis);
    const struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
                                 event->orientation, event->delta,
                                 event->delta_discrete, event->source,
                                 event->relative_direction);
}

static void cursor_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct luna_server *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void request_set_cursor(struct wl_listener *listener, void *data) {
    struct luna_server *server =
        wl_container_of(listener, server, request_set_cursor);
    const struct wlr_seat_pointer_request_set_cursor_event *event = data;
    if (server->seat->pointer_state.focused_client == event->seat_client)
        wlr_cursor_set_surface(server->cursor, event->surface,
                               event->hotspot_x, event->hotspot_y);
}

static void request_set_selection(struct wl_listener *listener, void *data) {
    struct luna_server *server =
        wl_container_of(listener, server, request_set_selection);
    const struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* ── New input device ─────────────────────────────────────────────────────── */

static void server_new_input(struct wl_listener *listener, void *data) {
    struct luna_server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD: {
        struct luna_keyboard *kb = calloc(1, sizeof(*kb));
        kb->server       = server;
        kb->wlr_keyboard = wlr_keyboard_from_input_device(device);

        /* Set xkb keymap — use system default */
        struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap  *map = xkb_keymap_new_from_names(ctx, NULL,
                                      XKB_KEYMAP_COMPILE_NO_FLAGS);
        wlr_keyboard_set_keymap(kb->wlr_keyboard, map);
        xkb_keymap_unref(map);
        xkb_context_unref(ctx);
        wlr_keyboard_set_repeat_info(kb->wlr_keyboard, 25, 600);

        kb->modifiers.notify = keyboard_handle_modifiers;
        kb->key.notify       = keyboard_handle_key;
        kb->destroy.notify   = keyboard_destroy;
        wl_signal_add(&kb->wlr_keyboard->events.modifiers, &kb->modifiers);
        wl_signal_add(&kb->wlr_keyboard->events.key,       &kb->key);
        wl_signal_add(&device->events.destroy,              &kb->destroy);

        wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
        wl_list_insert(&server->keyboards, &kb->link);
        break;
    }
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(server->cursor, device);
        break;
    default:
        break;
    }

    /* Tell the seat what capabilities we have */
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards))
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, caps);
}

/* ── Focus ────────────────────────────────────────────────────────────────── */

void luna_focus_view(struct luna_view *view, struct wlr_surface *surface) {
    if (!view) return;
    struct luna_server *server = view->server;
    struct wlr_seat    *seat   = server->seat;

    /* Unfocus the previously focused view */
    if (server->focused_view && server->focused_view != view) {
        wlr_xdg_toplevel_set_activated(
            server->focused_view->xdg_toplevel, false);
    }

    /* Raise to top of stacking order */
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);

    wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);
    server->focused_view = view;

    struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
    if (kb) wlr_seat_keyboard_notify_enter(seat, surface,
                                            kb->keycodes, kb->num_keycodes,
                                            &kb->modifiers);
}

/* ── View hit-test ────────────────────────────────────────────────────────── */

struct luna_view *luna_view_at(struct luna_server *server,
                                double lx, double ly,
                                struct wlr_surface **surface,
                                double *sx, double *sy) {
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER)
        return NULL;

    struct wlr_scene_buffer *sbuf = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *ssurface =
        wlr_scene_surface_try_from_buffer(sbuf);
    if (!ssurface) return NULL;

    *surface = ssurface->surface;
    struct wlr_scene_tree *tree = node->parent;
    while (tree && !tree->node.data)
        tree = tree->node.parent;
    return tree ? tree->node.data : NULL;
}

/* ── Window management ────────────────────────────────────────────────────── */

void luna_view_maximize(struct luna_view *view, bool maximize) {
    if (view->maximized == maximize) return;

    if (maximize) {
        /* Save current geometry */
        struct wlr_box geom;
        wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geom);
        view->saved_x = geom.x;
        view->saved_y = geom.y;
        view->saved_w = geom.width;
        view->saved_h = geom.height;

        /* Use first output dimensions */
        struct luna_output *out;
        wl_list_for_each(out, &view->server->outputs, link) {
            wlr_xdg_toplevel_set_size(view->xdg_toplevel,
                                      out->wlr_output->width,
                                      out->wlr_output->height);
            wlr_scene_node_set_position(&view->scene_tree->node, 0, 0);
            break;
        }
    } else {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel,
                                  view->saved_w, view->saved_h);
        wlr_scene_node_set_position(&view->scene_tree->node,
                                    view->saved_x, view->saved_y);
    }
    wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, maximize);
    view->maximized = maximize;
}

void luna_view_fullscreen(struct luna_view *view, bool fullscreen) {
    wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, fullscreen);
    view->fullscreen = fullscreen;
}

void luna_begin_move(struct luna_view *view, uint32_t serial) {
    (void)serial;
    /* Interactive move: track in cursor motion handler */
    /* Simplified: move the scene node with cursor delta */
    wlr_log(WLR_DEBUG, "begin_move: %s",
            view->xdg_toplevel->title ? view->xdg_toplevel->title : "?");
}

void luna_begin_resize(struct luna_view *view, uint32_t edges, uint32_t serial) {
    (void)edges; (void)serial;
    wlr_log(WLR_DEBUG, "begin_resize: %s",
            view->xdg_toplevel->title ? view->xdg_toplevel->title : "?");
}

/* ── Server create ────────────────────────────────────────────────────────── */

struct luna_server *luna_server_create(void) {
    struct luna_server *server = calloc(1, sizeof(*server));

    server->display    = wl_display_create();
    server->event_loop = wl_display_get_event_loop(server->display);
    wl_list_init(&server->outputs);
    wl_list_init(&server->views);
    wl_list_init(&server->keyboards);

    /* Backend: auto-detects DRM on Darwin via our session backend */
    server->backend = wlr_backend_autocreate(server->event_loop, NULL);
    if (!server->backend) {
        wlr_log(WLR_ERROR, "failed to create backend — "
                "is IODRMShim.kext loaded? (/dev/dri/card0 must exist)");
        goto err;
    }

    /* Renderer: pixman (software, no GPU needed) */
    server->renderer  = wlr_pixman_renderer_create();
    if (!server->renderer) goto err;
    wlr_renderer_init_wl_display(server->renderer, server->display);

    server->allocator = wlr_allocator_autocreate(server->backend,
                                                  server->renderer);
    if (!server->allocator) goto err;

    /* Scene graph */
    server->scene        = wlr_scene_create();
    server->output_layout = wlr_output_layout_create(server->display);
    server->scene_layout  =
        wlr_scene_attach_output_layout(server->scene, server->output_layout);

    /* Scene layers (z-order) */
    server->layer_background = wlr_scene_tree_create(&server->scene->tree);
    server->layer_bottom     = wlr_scene_tree_create(&server->scene->tree);
    server->layer_normal     = wlr_scene_tree_create(&server->scene->tree);
    server->layer_top        = wlr_scene_tree_create(&server->scene->tree);
    server->layer_overlay    = wlr_scene_tree_create(&server->scene->tree);

    /* Wayland protocols */
    server->compositor   = wlr_compositor_create(server->display, 6,
                                                  server->renderer);
    server->subcompositor= wlr_subcompositor_create(server->display);
    server->data_device_mgr =
        wlr_data_device_manager_create(server->display);
    server->xdg_shell    = wlr_xdg_shell_create(server->display, 6);
    server->layer_shell  = wlr_layer_shell_v1_create(server->display, 4);
    server->screencopy   =
        wlr_screencopy_manager_v1_create(server->display);
    server->viewporter   = wlr_viewporter_create(server->display);
    server->presentation =
        wlr_presentation_create(server->display, server->backend);
    server->gamma_ctrl   =
        wlr_gamma_control_manager_v1_create(server->display);
    server->single_pixel =
        wlr_single_pixel_buffer_manager_v1_create(server->display);
    server->xdg_output_mgr =
        wlr_xdg_output_manager_v1_create(server->display,
                                          server->output_layout);

    /* Seat */
    server->seat = wlr_seat_create(server->display, "seat0");
    server->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    server->xcursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    wlr_xcursor_manager_load(server->xcursor_mgr, 1.0f);

    /* Wire up listeners */
    server->new_output.notify          = server_new_output;
    server->new_input.notify           = server_new_input;
    server->new_xdg_surface.notify     = server_new_xdg_surface;
    server->cursor_motion.notify       = cursor_motion;
    server->cursor_motion_absolute.notify = cursor_motion_absolute;
    server->cursor_button.notify       = cursor_button;
    server->cursor_axis.notify         = cursor_axis;
    server->cursor_frame.notify        = cursor_frame;
    server->request_set_cursor.notify  = request_set_cursor;
    server->request_set_selection.notify = request_set_selection;

    wl_signal_add(&server->backend->events.new_output,  &server->new_output);
    wl_signal_add(&server->backend->events.new_input,   &server->new_input);
    wl_signal_add(&server->xdg_shell->events.new_surface,
                  &server->new_xdg_surface);
    wl_signal_add(&server->cursor->events.motion,       &server->cursor_motion);
    wl_signal_add(&server->cursor->events.motion_absolute,
                  &server->cursor_motion_absolute);
    wl_signal_add(&server->cursor->events.button,       &server->cursor_button);
    wl_signal_add(&server->cursor->events.axis,         &server->cursor_axis);
    wl_signal_add(&server->cursor->events.frame,        &server->cursor_frame);
    wl_signal_add(&server->seat->events.request_set_cursor,
                  &server->request_set_cursor);
    wl_signal_add(&server->seat->events.request_set_selection,
                  &server->request_set_selection);

    return server;
err:
    free(server);
    return NULL;
}

/* ── Server run / destroy ─────────────────────────────────────────────────── */

void luna_server_run(struct luna_server *server) {
    /* Set WAYLAND_DISPLAY so clients know where to connect */
    const char *socket = wl_display_add_socket_auto(server->display);
    if (!socket) {
        wlr_log(WLR_ERROR, "failed to create Wayland socket");
        return;
    }
    setenv("WAYLAND_DISPLAY", socket, 1);
    wlr_log(WLR_INFO, "LunaOS compositor running on WAYLAND_DISPLAY=%s", socket);
    wlr_log(WLR_INFO, "Keybindings: Super+T=terminal  Super+Q=close  Super+M=maximize  Super+Shift+E=exit");

    if (!wlr_backend_start(server->backend)) {
        wlr_log(WLR_ERROR, "failed to start backend");
        return;
    }
    wl_display_run(server->display);
}

void luna_server_destroy(struct luna_server *server) {
    wl_display_destroy_clients(server->display);
    wlr_scene_node_destroy(&server->scene->tree.node);
    wlr_xcursor_manager_destroy(server->xcursor_mgr);
    wlr_cursor_destroy(server->cursor);
    wlr_allocator_destroy(server->allocator);
    wlr_renderer_destroy(server->renderer);
    wlr_backend_destroy(server->backend);
    wl_display_destroy(server->display);
    free(server);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    wlr_log_init(WLR_DEBUG, NULL);

    const char *config_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            config_path = argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            printf("luna-compositor [--config PATH]\n"
                   "  LunaOS Wayland compositor for PureDarwin x86_64\n");
            return 0;
        }
    }
    (void)config_path; /* config loading: future */

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGCHLD, SIG_IGN);  /* don't zombie fork'd terminals */

    struct luna_server *server = luna_server_create();
    if (!server) {
        fprintf(stderr, "luna-compositor: failed to create server\n");
        return 1;
    }
    g_server = server;

    luna_server_run(server);
    luna_server_destroy(server);
    return 0;
}
