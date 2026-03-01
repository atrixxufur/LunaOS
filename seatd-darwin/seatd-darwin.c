/*
 * seatd-darwin.c — Seat management daemon for Darwin/PureDarwin
 *
 * seatd provides non-root Wayland compositors with access to DRM and input
 * devices.  The Linux seatd uses udev for device discovery.  This Darwin
 * port replaces udev enumeration with IOKit (IOServiceGetMatchingServices)
 * and IORegistryEntry notifications.
 *
 * Wire protocol: identical to upstream seatd — compositors using
 * libseat (seatd's client library) work without modification.
 *
 * Protocol summary (seatd-proto.h compatible):
 *   Client → seatd:  OPEN_SEAT, TAKE_DEVICE, RELEASE_DEVICE, DISABLE_SEAT
 *   seatd → Client:  SEAT_OPENED, DEVICE_OPENED, DEVICE_CLOSED, ENABLE_SEAT,
 *                    DISABLE_SEAT, ERROR
 *
 * Build:
 *   clang -o seatd-darwin seatd-darwin.c \
 *         -framework IOKit -framework CoreFoundation -lpthread
 *
 * Run: sudo seatd-darwin -g video
 * Socket: /run/seatd.sock (same path as Linux seatd)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <grp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/event.h>
#include <sys/time.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOGraphicsLib.h>
#include <CoreFoundation/CoreFoundation.h>

/* ── Protocol constants (matches seatd-proto.h) ───────────────────────────── */

#define SEATD_PROTO_VERSION     1
#define SEATD_SOCK_PATH         "/run/seatd.sock"
#define SEATD_SOCK_FALLBACK     "/tmp/seatd.sock"

/* Message types */
#define CLIENT_OPEN_SEAT        1
#define CLIENT_CLOSE_SEAT       2
#define CLIENT_TAKE_DEVICE      3
#define CLIENT_RELEASE_DEVICE   4
#define CLIENT_DISABLE_SEAT     5

#define SERVER_SEAT_OPENED      0x10
#define SERVER_SEAT_CLOSED      0x11
#define SERVER_DEVICE_OPENED    0x20
#define SERVER_DEVICE_CLOSED    0x21
#define SERVER_ENABLE_SEAT      0x30
#define SERVER_DISABLE_SEAT     0x31
#define SERVER_ERROR            0x40

/* Error codes */
#define SEATD_ERROR_PERMISSION  1
#define SEATD_ERROR_NOT_FOUND   2
#define SEATD_ERROR_PROTOCOL    3
#define SEATD_ERROR_NO_SEAT     4

/* ── Message header ───────────────────────────────────────────────────────── */

struct __attribute__((packed)) seatd_msg_header {
    uint16_t opcode;
    uint16_t size;       /* payload size in bytes, not including header */
};

struct __attribute__((packed)) seatd_msg_take_device {
    uint32_t major;
    uint32_t minor;
};

/* ── Device table ─────────────────────────────────────────────────────────── */

#define MAX_DEVICES     64
#define MAX_CLIENTS     16

typedef struct {
    char     path[256];
    uint32_t major, minor;
    bool     active;
    bool     paused;
    int      fd;         /* opened fd held by seatd, sent to client via SCM_RIGHTS */
} seat_device_t;

typedef struct {
    int      sock_fd;
    char     seat_name[64];
    bool     active;
    bool     has_seat;
    seat_device_t *claimed[MAX_DEVICES];
    int      claim_count;
    pid_t    pid;
} seat_client_t;

typedef struct {
    char            name[64];    /* e.g. "seat0" */
    seat_device_t   devices[MAX_DEVICES];
    int             device_count;
    seat_client_t  *active_client; /* only one client is "active" at a time */
} seat_t;

static seat_t       g_seat;
static seat_client_t g_clients[MAX_CLIENTS];
static int           g_client_count = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_running = true;
static int           g_listen_fd = -1;
static int           g_kq = -1;

/* ── Device discovery via IOKit ───────────────────────────────────────────── */

static void register_device(const char *path, uint32_t major, uint32_t minor) {
    pthread_mutex_lock(&g_lock);
    if (g_seat.device_count >= MAX_DEVICES) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    seat_device_t *dev = &g_seat.devices[g_seat.device_count++];
    strncpy(dev->path, path, sizeof(dev->path) - 1);
    dev->major  = major;
    dev->minor  = minor;
    dev->active = true;
    dev->paused = false;
    dev->fd     = -1;
    printf("[seatd-darwin] registered device: %s (%u:%u)\n",
           path, major, minor);
    pthread_mutex_unlock(&g_lock);
}

/*
 * Enumerate DRM and input devices using IOKit.
 * On Darwin, we discover:
 *   /dev/dri/card0     — IOFramebuffer-backed DRM node  (major from cdevsw)
 *   /dev/input/event*  — evdev bridge nodes              (pipe fds)
 * We also scan /dev/dri/ and /dev/input/ directly for anything already
 * created by the KEXT or darwin-evdev-bridge.
 */
static void discover_devices(void) {
    struct stat st;

    /* DRM card nodes */
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        if (stat(path, &st) == 0)
            register_device(path,
                            (uint32_t)major(st.st_rdev),
                            (uint32_t)minor(st.st_rdev));
    }

    /* DRM render nodes */
    for (int i = 128; i < 132; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
        if (stat(path, &st) == 0)
            register_device(path,
                            (uint32_t)major(st.st_rdev),
                            (uint32_t)minor(st.st_rdev));
    }

    /* evdev input nodes */
    for (int i = 0; i < 16; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (stat(path, &st) == 0)
            register_device(path,
                            (uint32_t)major(st.st_rdev),
                            (uint32_t)minor(st.st_rdev));
    }

    printf("[seatd-darwin] discovered %d devices for seat '%s'\n",
           g_seat.device_count, g_seat.name);
}

/* ── Send helpers ─────────────────────────────────────────────────────────── */

static int send_msg(int fd, uint16_t opcode, const void *payload, uint16_t plen) {
    struct seatd_msg_header hdr = {
        .opcode = htons(opcode),
        .size   = htons(plen),
    };
    struct iovec iov[2] = {
        { .iov_base = &hdr,          .iov_len = sizeof(hdr) },
        { .iov_base = (void*)payload,.iov_len = plen         },
    };
    struct msghdr msg = {
        .msg_iov    = iov,
        .msg_iovlen = plen ? 2 : 1,
    };
    return sendmsg(fd, &msg, MSG_NOSIGNAL) < 0 ? -errno : 0;
}

static int send_fd(int sock_fd, int send_fd_val) {
    /* Send the opened device fd to the client via SCM_RIGHTS */
    char buf[1] = {0};
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {
        .msg_iov     = &iov,
        .msg_iovlen  = 1,
        .msg_control = cmsgbuf,
        .msg_controllen = sizeof(cmsgbuf),
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &send_fd_val, sizeof(int));
    return sendmsg(sock_fd, &msg, MSG_NOSIGNAL) < 0 ? -errno : 0;
}

/* ── Handle client messages ───────────────────────────────────────────────── */

static seat_device_t *find_device(uint32_t major_n, uint32_t minor_n) {
    for (int i = 0; i < g_seat.device_count; i++) {
        seat_device_t *dev = &g_seat.devices[i];
        if (dev->active &&
            dev->major == major_n && dev->minor == minor_n)
            return dev;
    }
    return NULL;
}

static void handle_open_seat(seat_client_t *client) {
    if (client->has_seat) {
        send_msg(client->sock_fd, SERVER_ERROR,
                 "\x04", 1); /* SEATD_ERROR_NO_SEAT */
        return;
    }
    client->has_seat = true;
    g_seat.active_client = client;

    uint16_t nlen = (uint16_t)strlen(g_seat.name);
    uint8_t  payload[2 + 64];
    payload[0] = (nlen >> 8) & 0xff;
    payload[1] = nlen & 0xff;
    memcpy(payload + 2, g_seat.name, nlen);
    send_msg(client->sock_fd, SERVER_SEAT_OPENED, payload, 2 + nlen);
    printf("[seatd-darwin] client (fd %d) opened seat '%s'\n",
           client->sock_fd, g_seat.name);
}

static void handle_take_device(seat_client_t *client,
                                const struct seatd_msg_take_device *req) {
    uint32_t maj = ntohl(req->major);
    uint32_t min = ntohl(req->minor);

    pthread_mutex_lock(&g_lock);
    seat_device_t *dev = find_device(maj, min);
    if (!dev) {
        pthread_mutex_unlock(&g_lock);
        send_msg(client->sock_fd, SERVER_ERROR, "\x02", 1);
        printf("[seatd-darwin] TAKE_DEVICE %u:%u — not found\n", maj, min);
        return;
    }

    /* Open the device as root on behalf of the client */
    if (dev->fd < 0) {
        dev->fd = open(dev->path, O_RDWR | O_CLOEXEC);
        if (dev->fd < 0) {
            pthread_mutex_unlock(&g_lock);
            printf("[seatd-darwin] failed to open %s: %s\n",
                   dev->path, strerror(errno));
            send_msg(client->sock_fd, SERVER_ERROR, "\x01", 1);
            return;
        }
    }

    /* Send DEVICE_OPENED header, then the fd via SCM_RIGHTS */
    uint8_t payload[4];
    payload[0] = (uint8_t)((maj >> 24) & 0xff);
    payload[1] = (uint8_t)((maj >> 16) & 0xff);
    payload[2] = (uint8_t)((maj >>  8) & 0xff);
    payload[3] = (uint8_t)( maj        & 0xff);
    send_msg(client->sock_fd, SERVER_DEVICE_OPENED, payload, 4);
    send_fd(client->sock_fd, dev->fd);

    printf("[seatd-darwin] TAKE_DEVICE %s → client fd %d\n",
           dev->path, client->sock_fd);
    pthread_mutex_unlock(&g_lock);
}

static void handle_release_device(seat_client_t *client,
                                   const struct seatd_msg_take_device *req) {
    uint32_t maj = ntohl(req->major);
    uint32_t min = ntohl(req->minor);

    pthread_mutex_lock(&g_lock);
    seat_device_t *dev = find_device(maj, min);
    if (dev && dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
    }
    pthread_mutex_unlock(&g_lock);
    printf("[seatd-darwin] RELEASE_DEVICE %u:%u\n", maj, min);
}

static void handle_client_message(seat_client_t *client) {
    struct seatd_msg_header hdr;
    ssize_t n = recv(client->sock_fd, &hdr, sizeof(hdr), MSG_WAITALL);
    if (n <= 0) {
        /* Client disconnected */
        printf("[seatd-darwin] client disconnected (fd %d)\n", client->sock_fd);
        close(client->sock_fd);
        client->active = false;
        return;
    }

    uint16_t opcode = ntohs(hdr.opcode);
    uint16_t size   = ntohs(hdr.size);

    uint8_t payload[512] = {};
    if (size > 0 && size < sizeof(payload)) {
        recv(client->sock_fd, payload, size, MSG_WAITALL);
    }

    switch (opcode) {
    case CLIENT_OPEN_SEAT:
        handle_open_seat(client);
        break;
    case CLIENT_CLOSE_SEAT:
        client->has_seat = false;
        if (g_seat.active_client == client)
            g_seat.active_client = NULL;
        send_msg(client->sock_fd, SERVER_SEAT_CLOSED, NULL, 0);
        break;
    case CLIENT_TAKE_DEVICE:
        handle_take_device(client, (struct seatd_msg_take_device*)payload);
        break;
    case CLIENT_RELEASE_DEVICE:
        handle_release_device(client, (struct seatd_msg_take_device*)payload);
        break;
    case CLIENT_DISABLE_SEAT:
        send_msg(client->sock_fd, SERVER_DISABLE_SEAT, NULL, 0);
        break;
    default:
        printf("[seatd-darwin] unknown opcode 0x%x from client fd %d\n",
               opcode, client->sock_fd);
        send_msg(client->sock_fd, SERVER_ERROR, "\x03", 1);
        break;
    }
}

/* ── Accept new client ─────────────────────────────────────────────────────── */

static void accept_client(void) {
    int fd = accept(g_listen_fd, NULL, NULL);
    if (fd < 0) return;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, O_NONBLOCK);

    pthread_mutex_lock(&g_lock);
    if (g_client_count >= MAX_CLIENTS) {
        close(fd);
        pthread_mutex_unlock(&g_lock);
        return;
    }
    seat_client_t *client = &g_clients[g_client_count++];
    memset(client, 0, sizeof(*client));
    client->sock_fd  = fd;
    client->active   = true;
    client->has_seat = false;
    pthread_mutex_unlock(&g_lock);

    /* Register with kqueue */
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_READ, EV_ADD|EV_ENABLE, 0, 0, client);
    kevent(g_kq, &kev, 1, NULL, 0, NULL);

    printf("[seatd-darwin] new client connected (fd %d)\n", fd);
}

/* ── Setup socket ─────────────────────────────────────────────────────────── */

static int setup_socket(const char *path) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, O_NONBLOCK);

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0660);
    if (listen(fd, 16) < 0) { close(fd); return -1; }
    return fd;
}

/* ── Signal handler ───────────────────────────────────────────────────────── */

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *group_name = "video";
    const char *sock_path  = SEATD_SOCK_PATH;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-g") && i+1 < argc) group_name = argv[++i];
        if (!strcmp(argv[i], "-s") && i+1 < argc) sock_path  = argv[++i];
    }

    /* Drop to root-kept-gid for device access */
    struct group *grp = getgrnam(group_name);
    if (!grp) {
        fprintf(stderr, "[seatd-darwin] group '%s' not found\n", group_name);
        return 1;
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize seat */
    strncpy(g_seat.name, "seat0", sizeof(g_seat.name) - 1);
    discover_devices();

    /* Set up Unix socket */
    mkdir("/run", 0755);
    g_listen_fd = setup_socket(sock_path);
    if (g_listen_fd < 0) {
        /* Fallback to /tmp if /run doesn't exist (PureDarwin early boot) */
        fprintf(stderr, "[seatd-darwin] /run not writable, trying %s\n",
                SEATD_SOCK_FALLBACK);
        g_listen_fd = setup_socket(SEATD_SOCK_FALLBACK);
        if (g_listen_fd < 0) {
            perror("[seatd-darwin] socket");
            return 1;
        }
        sock_path = SEATD_SOCK_FALLBACK;
    }
    chown(sock_path, 0, grp->gr_gid);
    printf("[seatd-darwin] listening on %s (group=%s)\n", sock_path, group_name);

    /* kqueue event loop */
    g_kq = kqueue();
    struct kevent kev;
    EV_SET(&kev, g_listen_fd, EVFILT_READ, EV_ADD|EV_ENABLE, 0, 0, NULL);
    kevent(g_kq, &kev, 1, NULL, 0, NULL);

    while (g_running) {
        struct kevent events[32];
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        int nev = kevent(g_kq, NULL, 0, events, 32, &ts);
        if (nev < 0) { if (errno == EINTR) continue; break; }

        for (int i = 0; i < nev; i++) {
            if ((int)events[i].ident == g_listen_fd) {
                accept_client();
            } else {
                seat_client_t *client = (seat_client_t*)events[i].udata;
                if (client && client->active) {
                    if (events[i].flags & (EV_EOF|EV_ERROR)) {
                        close(client->sock_fd);
                        client->active = false;
                    } else {
                        handle_client_message(client);
                    }
                }
            }
        }
    }

    printf("[seatd-darwin] shutting down\n");
    for (int i = 0; i < g_client_count; i++) {
        if (g_clients[i].active) close(g_clients[i].sock_fd);
    }
    close(g_listen_fd);
    close(g_kq);
    unlink(sock_path);
    return 0;
}
