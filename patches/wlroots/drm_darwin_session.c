/*
 * drm_darwin_session.c — wlroots session backend for Darwin
 *
 * Replaces the Linux udev+logind session in wlroots with:
 *   - Direct /dev/dri/ scanning for DRM devices (created by IODRMShim.kext)
 *   - Direct /dev/input/ scanning for input nodes (created by darwin-evdev-bridge)
 *   - seatd-darwin socket for privilege escalation (device fd handoff)
 *   - kqueue for device hotplug notification (no udev)
 *
 * Drop this file into wlroots/backend/drm/darwin/ and apply the
 * companion patch 0001-drm-darwin-backend.patch.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

#include <wlr/util/log.h>
#include <wlr/backend/session.h>
#include "drm_darwin_session.h"

/* seatd-darwin socket path (matches seatd-darwin.c) */
#define SEATD_SOCK_PATH     "/run/seatd.sock"
#define SEATD_SOCK_FALLBACK "/tmp/seatd.sock"

/* seatd protocol opcodes (must match seatd-darwin.c) */
#define CLIENT_OPEN_SEAT    1
#define CLIENT_TAKE_DEVICE  3
#define SERVER_SEAT_OPENED  0x10
#define SERVER_DEVICE_OPENED 0x20
#define SERVER_ERROR        0x40

#define MAX_DRM_DEVICES   8
#define MAX_INPUT_DEVICES 32

/* ── Internal session struct ──────────────────────────────────────────────── */

struct darwin_session {
    struct wlr_session base;        /* must be first — wlroots casts freely */

    int   seatd_fd;                 /* Unix socket to seatd-darwin */
    int   kq;                       /* kqueue for hotplug events */

    /* Opened device fds (received from seatd-darwin via SCM_RIGHTS) */
    struct {
        char     path[256];
        uint32_t major, minor;
        int      fd;
        bool     active;
    } devices[MAX_DRM_DEVICES + MAX_INPUT_DEVICES];
    int device_count;

    char seat_name[64];
};

/* ── seatd-darwin client ──────────────────────────────────────────────────── */

struct __attribute__((packed)) seatd_hdr {
    uint16_t opcode;
    uint16_t size;
};

struct __attribute__((packed)) seatd_take_device {
    uint32_t major;
    uint32_t minor;
};

static int seatd_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;

    /* Try primary path first, then fallback */
    strncpy(addr.sun_path, SEATD_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        strncpy(addr.sun_path, SEATD_SOCK_FALLBACK, sizeof(addr.sun_path) - 1);
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            wlr_log(WLR_ERROR, "darwin_session: cannot connect to seatd-darwin "
                    "at %s or %s — is seatd-darwin running?",
                    SEATD_SOCK_PATH, SEATD_SOCK_FALLBACK);
            return -1;
        }
    }
    return fd;
}

static int seatd_send(int fd, uint16_t opcode, const void *payload, uint16_t plen) {
    struct seatd_hdr hdr = { htons(opcode), htons(plen) };
    if (send(fd, &hdr, sizeof(hdr), MSG_NOSIGNAL) < 0) return -1;
    if (plen && send(fd, payload, plen, MSG_NOSIGNAL) < 0) return -1;
    return 0;
}

/* Receive a message header + optional payload; returns opcode or -1 */
static int seatd_recv(int fd, void *payload_buf, uint16_t buf_size) {
    struct seatd_hdr hdr;
    if (recv(fd, &hdr, sizeof(hdr), MSG_WAITALL) != sizeof(hdr)) return -1;
    uint16_t op   = ntohs(hdr.opcode);
    uint16_t size = ntohs(hdr.size);
    if (size > 0 && size <= buf_size)
        recv(fd, payload_buf, size, MSG_WAITALL);
    return op;
}

/* Receive an fd via SCM_RIGHTS after a DEVICE_OPENED message */
static int seatd_recv_fd(int sock) {
    char buf[1];
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov  = { buf, 1 };
    struct msghdr msg = {
        .msg_iov     = &iov, .msg_iovlen = 1,
        .msg_control = cmsgbuf, .msg_controllen = sizeof(cmsgbuf),
    };
    if (recvmsg(sock, &msg, 0) < 0) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) return -1;
    int received_fd;
    memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
    return received_fd;
}

static int seatd_open_seat(int seatd_fd, char *seat_name_out) {
    if (seatd_send(seatd_fd, CLIENT_OPEN_SEAT, NULL, 0) < 0) return -1;
    char buf[128] = {};
    int op = seatd_recv(seatd_fd, buf, sizeof(buf));
    if (op != SERVER_SEAT_OPENED) return -1;
    /* Payload: uint16_t name_len + name bytes */
    uint16_t nlen = ((uint8_t)buf[0] << 8) | (uint8_t)buf[1];
    if (nlen < 64) memcpy(seat_name_out, buf + 2, nlen);
    return 0;
}

static int seatd_take_device(int seatd_fd, uint32_t major, uint32_t minor) {
    struct seatd_take_device req = { htonl(major), htonl(minor) };
    if (seatd_send(seatd_fd, CLIENT_TAKE_DEVICE, &req, sizeof(req)) < 0)
        return -1;
    char buf[8];
    int op = seatd_recv(seatd_fd, buf, sizeof(buf));
    if (op != SERVER_DEVICE_OPENED) return -1;
    return seatd_recv_fd(seatd_fd);
}

/* ── Device enumeration ───────────────────────────────────────────────────── */

static void scan_devices(struct darwin_session *s) {
    const char *dirs[]  = { "/dev/dri", "/dev/input", NULL };
    struct stat st;

    for (int d = 0; dirs[d]; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", dirs[d], ent->d_name);
            if (stat(path, &st) < 0) continue;
            if (!S_ISCHR(st.st_mode)) continue;
            if (s->device_count >= MAX_DRM_DEVICES + MAX_INPUT_DEVICES) break;

            auto *dev = &s->devices[s->device_count++];
            strncpy(dev->path, path, sizeof(dev->path) - 1);
            dev->major  = (uint32_t)major(st.st_rdev);
            dev->minor  = (uint32_t)minor(st.st_rdev);
            dev->fd     = -1;
            dev->active = true;
        }
        closedir(dir);
    }
    wlr_log(WLR_INFO, "darwin_session: found %d devices", s->device_count);
}

/* ── wlr_session interface implementation ─────────────────────────────────── */

static int darwin_session_open_device(struct wlr_session *base,
                                       const char *path) {
    struct darwin_session *s = (struct darwin_session *)base;
    struct stat st;
    if (stat(path, &st) < 0) return -errno;

    uint32_t maj = (uint32_t)major(st.st_rdev);
    uint32_t min = (uint32_t)minor(st.st_rdev);

    /* Ask seatd-darwin to open the device and hand us the fd */
    int fd = seatd_take_device(s->seatd_fd, maj, min);
    if (fd < 0) {
        wlr_log(WLR_ERROR, "darwin_session: seatd refused %s", path);
        /* Fallback: try opening directly (only works as root) */
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            wlr_log(WLR_ERROR, "darwin_session: direct open %s failed: %s",
                    path, strerror(errno));
            return -errno;
        }
    }
    wlr_log(WLR_DEBUG, "darwin_session: opened %s → fd %d", path, fd);
    return fd;
}

static void darwin_session_close_device(struct wlr_session *base, int fd) {
    (void)base;
    close(fd);
}

/* Called by wlroots when it wants to scan for DRM GPUs */
static struct wlr_device *darwin_session_find_gpus(struct wlr_session *base,
                                                    size_t ret_len,
                                                    struct wlr_device **ret) {
    struct darwin_session *s = (struct darwin_session *)base;
    size_t found = 0;

    for (int i = 0; i < s->device_count && found < ret_len; i++) {
        /* DRM card nodes only (not renderD*) */
        if (strncmp(s->devices[i].path, "/dev/dri/card", 13) != 0) continue;

        struct wlr_device *dev = calloc(1, sizeof(*dev));
        dev->fd   = darwin_session_open_device(base, s->devices[i].path);
        dev->dev  = makedev(s->devices[i].major, s->devices[i].minor);
        ret[found++] = dev;
        wlr_log(WLR_INFO, "darwin_session: GPU candidate: %s (fd %d)",
                s->devices[i].path, dev->fd);
    }
    return found > 0 ? ret[0] : NULL;
}

static void darwin_session_destroy(struct wlr_session *base) {
    struct darwin_session *s = (struct darwin_session *)base;
    if (s->seatd_fd >= 0) close(s->seatd_fd);
    if (s->kq >= 0) close(s->kq);
    free(s);
}

static const struct wlr_session_impl darwin_session_impl = {
    .open    = darwin_session_open_device,
    .close   = darwin_session_close_device,
    .find_gpus = darwin_session_find_gpus,
    .destroy = darwin_session_destroy,
};

/* ── Public constructor ───────────────────────────────────────────────────── */

struct wlr_session *drm_darwin_session_create(struct wl_event_loop *loop) {
    struct darwin_session *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    /* Connect to seatd-darwin */
    s->seatd_fd = seatd_connect();
    if (s->seatd_fd < 0) {
        wlr_log(WLR_ERROR, "darwin_session: seatd-darwin not available. "
                "Start it with: sudo seatd-darwin -g video");
        free(s);
        return NULL;
    }

    /* Open the seat */
    if (seatd_open_seat(s->seatd_fd, s->seat_name) < 0) {
        wlr_log(WLR_ERROR, "darwin_session: failed to open seat");
        close(s->seatd_fd);
        free(s);
        return NULL;
    }
    wlr_log(WLR_INFO, "darwin_session: seat '%s' opened", s->seat_name);

    /* kqueue for hotplug monitoring */
    s->kq = kqueue();

    /* Scan initial device list */
    scan_devices(s);

    /* Initialise the wlr_session base */
    wlr_session_init(&s->base, &darwin_session_impl, loop);
    strncpy(s->base.seat, s->seat_name, sizeof(s->base.seat) - 1);

    return &s->base;
}
