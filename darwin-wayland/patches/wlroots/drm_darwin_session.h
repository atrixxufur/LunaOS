/*
 * drm_darwin_session.h — Darwin session backend for wlroots
 * Drop into wlroots/backend/drm/darwin/
 */
#pragma once

#ifdef __APPLE__

#include <wlr/backend/session.h>
#include <wayland-server-core.h>

/* Creates a wlr_session backed by seatd-darwin + IOKit device scanning.
 * Called from wlr_session_create() on Darwin instead of udev_session_create(). */
struct wlr_session *drm_darwin_session_create(struct wl_event_loop *loop);

#endif /* __APPLE__ */
