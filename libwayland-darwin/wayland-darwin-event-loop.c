/*
 * wayland-darwin-event-loop.c — Darwin event loop backend for libwayland
 *
 * libwayland's event loop (wayland-server-core.h: wl_event_loop) is built
 * on Linux epoll(7). Darwin has no epoll, but does have kqueue(2), which
 * is semantically equivalent (and arguably superior).
 *
 * This file provides a drop-in replacement for libwayland's
 * wayland-event-loop.c that uses kqueue instead of epoll. Build it as
 * part of the libwayland-server static library when targeting Darwin.
 *
 * Differences from Linux epoll:
 *   epoll_create1  →  kqueue()
 *   epoll_ctl ADD  →  kevent(EV_ADD | EV_ENABLE)
 *   epoll_ctl DEL  →  kevent(EV_DELETE)
 *   epoll_ctl MOD  →  kevent(EV_ADD | EV_ENABLE)  (replaces previous)
 *   epoll_wait     →  kevent(nevents>0, timeout)
 *
 * EVFILT_READ  = EPOLLIN
 * EVFILT_WRITE = EPOLLOUT
 * EVFILT_SIGNAL (bonus: native signal delivery without signalfd)
 * EVFILT_TIMER (bonus: native timers without timerfd)
 *
 * Integration: patch libwayland's CMakeLists.txt:
 *   if(APPLE)
 *     list(REMOVE_ITEM SERVER_SOURCES src/wayland-event-loop.c)
 *     list(APPEND SERVER_SOURCES src/wayland-darwin-event-loop.c)
 *   endif()
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/event.h>   /* kqueue, kevent */
#include <sys/time.h>

/* libwayland internal headers (included when building libwayland itself) */
#include "wayland-server-core.h"
#include "wayland-util.h"

/* ── Source types (mirrors libwayland internals) ──────────────────────────── */

#define WL_EVENT_READABLE   0x01
#define WL_EVENT_WRITABLE   0x02
#define WL_EVENT_HANGUP     0x04
#define WL_EVENT_ERROR      0x08

typedef int (*wl_event_loop_fd_func_t)(int fd, uint32_t mask, void *data);
typedef int (*wl_event_loop_timer_func_t)(void *data);
typedef int (*wl_event_loop_signal_func_t)(int sig, void *data);
typedef void (*wl_event_loop_idle_func_t)(void *data);

/* ── Source struct ────────────────────────────────────────────────────────── */

typedef enum {
    SOURCE_FD,
    SOURCE_TIMER,
    SOURCE_SIGNAL,
    SOURCE_IDLE,
} source_type_t;

struct wl_event_source {
    source_type_t type;
    struct wl_event_loop *loop;
    struct wl_list link;
    void *data;

    union {
        struct {
            int fd;
            uint32_t mask;
            wl_event_loop_fd_func_t func;
        } fd;
        struct {
            int fd;                          /* kqueue timer ident */
            wl_event_loop_timer_func_t func;
        } timer;
        struct {
            int signum;
            wl_event_loop_signal_func_t func;
        } signal;
        struct {
            wl_event_loop_idle_func_t func;
        } idle;
    };
};

/* ── Event loop struct ────────────────────────────────────────────────────── */

struct wl_event_loop {
    int          kq;               /* kqueue fd */
    struct wl_list sources;        /* all registered wl_event_source */
    struct wl_list idle_list;
    struct wl_list destroy_list;
    pthread_mutex_t mutex;
    int          pipe_r, pipe_w;   /* self-pipe for wl_event_loop_dispatch() wakeup */
    int          dispatch_depth;
};

/* ── Create / destroy loop ────────────────────────────────────────────────── */

struct wl_event_loop *wl_event_loop_create(void) {
    struct wl_event_loop *loop = calloc(1, sizeof(*loop));
    if (!loop) return NULL;

    loop->kq = kqueue();
    if (loop->kq < 0) { free(loop); return NULL; }
    fcntl(loop->kq, F_SETFD, FD_CLOEXEC);

    int pp[2];
    if (pipe(pp) < 0) { close(loop->kq); free(loop); return NULL; }
    fcntl(pp[0], F_SETFD, FD_CLOEXEC);
    fcntl(pp[1], F_SETFD, FD_CLOEXEC);
    fcntl(pp[0], F_SETFL, O_NONBLOCK);
    fcntl(pp[1], F_SETFL, O_NONBLOCK);
    loop->pipe_r = pp[0];
    loop->pipe_w = pp[1];

    /* Monitor the read end of the self-pipe for wakeups */
    struct kevent kev;
    EV_SET(&kev, loop->pipe_r, EVFILT_READ, EV_ADD|EV_ENABLE, 0, 0, NULL);
    kevent(loop->kq, &kev, 1, NULL, 0, NULL);

    wl_list_init(&loop->sources);
    wl_list_init(&loop->idle_list);
    wl_list_init(&loop->destroy_list);
    pthread_mutex_init(&loop->mutex, NULL);
    return loop;
}

void wl_event_loop_destroy(struct wl_event_loop *loop) {
    struct wl_event_source *src, *tmp;
    wl_list_for_each_safe(src, tmp, &loop->sources, link) {
        wl_event_source_remove(src);
    }
    close(loop->pipe_r);
    close(loop->pipe_w);
    close(loop->kq);
    pthread_mutex_destroy(&loop->mutex);
    free(loop);
}

int wl_event_loop_get_fd(struct wl_event_loop *loop) {
    return loop->kq;
}

/* ── Register fd source ───────────────────────────────────────────────────── */

static struct wl_event_source *
add_fd_source(struct wl_event_loop *loop, int fd, uint32_t mask,
              wl_event_loop_fd_func_t func, void *data,
              source_type_t type) {
    struct wl_event_source *src = calloc(1, sizeof(*src));
    if (!src) return NULL;
    src->type      = type;
    src->loop      = loop;
    src->data      = data;
    src->fd.fd     = fd;
    src->fd.mask   = mask;
    src->fd.func   = func;

    /* Register with kqueue */
    struct kevent changes[2];
    int n = 0;
    if (mask & WL_EVENT_READABLE)
        EV_SET(&changes[n++], fd, EVFILT_READ,  EV_ADD|EV_ENABLE, 0, 0, src);
    if (mask & WL_EVENT_WRITABLE)
        EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_ADD|EV_ENABLE, 0, 0, src);

    if (n > 0) kevent(loop->kq, changes, n, NULL, 0, NULL);

    wl_list_insert(&loop->sources, &src->link);
    return src;
}

struct wl_event_source *
wl_event_loop_add_fd(struct wl_event_loop *loop, int fd, uint32_t mask,
                     wl_event_loop_fd_func_t func, void *data) {
    return add_fd_source(loop, fd, mask, func, data, SOURCE_FD);
}

int wl_event_source_fd_update(struct wl_event_source *src, uint32_t mask) {
    struct wl_event_loop *loop = src->loop;
    int fd = src->fd.fd;

    /* Remove old filters */
    struct kevent changes[4];
    int n = 0;
    EV_SET(&changes[n++], fd, EVFILT_READ,  EV_DELETE, 0, 0, NULL);
    EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(loop->kq, changes, n, NULL, 0, NULL);

    /* Add new filters */
    n = 0;
    if (mask & WL_EVENT_READABLE)
        EV_SET(&changes[n++], fd, EVFILT_READ,  EV_ADD|EV_ENABLE, 0, 0, src);
    if (mask & WL_EVENT_WRITABLE)
        EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_ADD|EV_ENABLE, 0, 0, src);

    if (n > 0) kevent(loop->kq, changes, n, NULL, 0, NULL);
    src->fd.mask = mask;
    return 0;
}

/* ── Timer source (uses kqueue EVFILT_TIMER) ─────────────────────────────── */

struct wl_event_source *
wl_event_loop_add_timer(struct wl_event_loop *loop,
                        wl_event_loop_timer_func_t func, void *data) {
    struct wl_event_source *src = calloc(1, sizeof(*src));
    if (!src) return NULL;
    src->type         = SOURCE_TIMER;
    src->loop         = loop;
    src->data         = data;
    src->timer.func   = func;
    /* Use a unique ident derived from pointer value */
    src->timer.fd     = (int)(uintptr_t)src;

    wl_list_insert(&loop->sources, &src->link);
    return src;
}

int wl_event_source_timer_update(struct wl_event_source *src, int ms_delay) {
    struct kevent kev;
    if (ms_delay == 0) {
        /* Disarm */
        EV_SET(&kev, src->timer.fd, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
        kevent(src->loop->kq, &kev, 1, NULL, 0, NULL);
    } else {
        /* NOTE_MSECONDS: timer fires once after ms_delay ms */
        EV_SET(&kev, src->timer.fd, EVFILT_TIMER,
               EV_ADD|EV_ENABLE|EV_ONESHOT,
               NOTE_MSECONDS, ms_delay, src);
        kevent(src->loop->kq, &kev, 1, NULL, 0, NULL);
    }
    return 0;
}

/* ── Signal source (uses kqueue EVFILT_SIGNAL) ───────────────────────────── */

struct wl_event_source *
wl_event_loop_add_signal(struct wl_event_loop *loop, int signal_number,
                         wl_event_loop_signal_func_t func, void *data) {
    struct wl_event_source *src = calloc(1, sizeof(*src));
    if (!src) return NULL;
    src->type           = SOURCE_SIGNAL;
    src->loop           = loop;
    src->data           = data;
    src->signal.signum  = signal_number;
    src->signal.func    = func;

    /* Suppress default signal delivery so kqueue can intercept it */
    signal(signal_number, SIG_IGN);

    struct kevent kev;
    EV_SET(&kev, signal_number, EVFILT_SIGNAL, EV_ADD|EV_ENABLE, 0, 0, src);
    kevent(loop->kq, &kev, 1, NULL, 0, NULL);

    wl_list_insert(&loop->sources, &src->link);
    return src;
}

/* ── Idle source ─────────────────────────────────────────────────────────── */

struct wl_event_source *
wl_event_loop_add_idle(struct wl_event_loop *loop,
                       wl_event_loop_idle_func_t func, void *data) {
    struct wl_event_source *src = calloc(1, sizeof(*src));
    if (!src) return NULL;
    src->type        = SOURCE_IDLE;
    src->loop        = loop;
    src->data        = data;
    src->idle.func   = func;

    wl_list_insert(&loop->idle_list, &src->link);
    /* Prod the kqueue loop to notice immediately */
    char c = 0;
    write(loop->pipe_w, &c, 1);
    return src;
}

/* ── Remove source ────────────────────────────────────────────────────────── */

int wl_event_source_remove(struct wl_event_source *src) {
    struct wl_event_loop *loop = src->loop;
    struct kevent kev;

    switch (src->type) {
    case SOURCE_FD:
        EV_SET(&kev, src->fd.fd, EVFILT_READ,   EV_DELETE, 0, 0, NULL);
        kevent(loop->kq, &kev, 1, NULL, 0, NULL);
        EV_SET(&kev, src->fd.fd, EVFILT_WRITE,  EV_DELETE, 0, 0, NULL);
        kevent(loop->kq, &kev, 1, NULL, 0, NULL);
        break;
    case SOURCE_TIMER:
        EV_SET(&kev, src->timer.fd, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
        kevent(loop->kq, &kev, 1, NULL, 0, NULL);
        break;
    case SOURCE_SIGNAL:
        EV_SET(&kev, src->signal.signum, EVFILT_SIGNAL, EV_DELETE, 0, 0, NULL);
        kevent(loop->kq, &kev, 1, NULL, 0, NULL);
        signal(src->signal.signum, SIG_DFL);
        break;
    case SOURCE_IDLE:
        wl_list_remove(&src->link);
        free(src);
        return 0;
    }

    wl_list_remove(&src->link);
    free(src);
    return 0;
}

/* ── Main dispatch ────────────────────────────────────────────────────────── */

int wl_event_loop_dispatch(struct wl_event_loop *loop, int timeout_ms) {
    /* Run idle handlers first */
    struct wl_event_source *idle, *tmp;
    wl_list_for_each_safe(idle, tmp, &loop->idle_list, link) {
        idle->idle.func(idle->data);
        wl_list_remove(&idle->link);
        free(idle);
    }

    /* Convert ms timeout to timespec for kevent */
    struct timespec ts, *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec  = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    /* Collect up to 32 events per dispatch cycle */
    struct kevent events[32];
    int nev = kevent(loop->kq, NULL, 0, events, 32, tsp);
    if (nev < 0) {
        if (errno == EINTR) return 0;
        return -errno;
    }

    for (int i = 0; i < nev; i++) {
        struct kevent *kev = &events[i];
        struct wl_event_source *src = (struct wl_event_source *)kev->udata;

        /* Self-pipe wakeup */
        if (!src) {
            char buf[64];
            read(loop->pipe_r, buf, sizeof(buf));
            continue;
        }

        switch (src->type) {
        case SOURCE_FD: {
            uint32_t mask = 0;
            if (kev->filter == EVFILT_READ)  mask |= WL_EVENT_READABLE;
            if (kev->filter == EVFILT_WRITE) mask |= WL_EVENT_WRITABLE;
            if (kev->flags & EV_EOF)         mask |= WL_EVENT_HANGUP;
            if (kev->flags & EV_ERROR)       mask |= WL_EVENT_ERROR;
            src->fd.func(src->fd.fd, mask, src->data);
            break;
        }
        case SOURCE_TIMER:
            src->timer.func(src->data);
            break;
        case SOURCE_SIGNAL:
            src->signal.func((int)kev->ident, src->data);
            break;
        case SOURCE_IDLE:
            /* Already handled above */
            break;
        }
    }
    return 0;
}

void wl_event_loop_dispatch_idle(struct wl_event_loop *loop) {
    struct wl_event_source *idle, *tmp;
    wl_list_for_each_safe(idle, tmp, &loop->idle_list, link) {
        idle->idle.func(idle->data);
        wl_list_remove(&idle->link);
        free(idle);
    }
}

void wl_event_loop_wakeup(struct wl_event_loop *loop) {
    char c = 0;
    write(loop->pipe_w, &c, 1);
}
