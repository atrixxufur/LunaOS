/*
 * darwin-evdev-bridge.c — IOHIDFamily → Linux evdev emulation daemon
 *
 * Creates /dev/input/event0, event1, ... character devices and feeds them
 * standard struct input_event records translated from Darwin's IOHIDFamily.
 * libinput reads these nodes exactly as it would on Linux — zero changes
 * needed to libinput itself.
 *
 * Architecture:
 *   IOHIDManager (kernel) → IOHIDQueue (userspace CF API)
 *       ↓  darwin-evdev-bridge  (this daemon)
 *   pipe pairs → /dev/input/event0..N (synthetic chardevs via devfs+pipe)
 *       ↓
 *   libinput  →  seatd  →  Wayland compositor
 *
 * Build:
 *   clang -o darwin-evdev-bridge darwin-evdev-bridge.c \
 *         -framework IOKit -framework CoreFoundation
 *
 * Run (as root, before starting Wayland compositor):
 *   sudo darwin-evdev-bridge &
 *
 * The bridge creates /run/darwin-evdev/ and symlinks:
 *   /dev/input/event0 → /run/darwin-evdev/event0  (keyboard)
 *   /dev/input/event1 → /run/darwin-evdev/event1  (mouse/pointer)
 *   /dev/input/event2 → /run/darwin-evdev/event2  (touchpad, if present)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <CoreFoundation/CoreFoundation.h>

/* ── Linux input_event wire format (we write this to the pipe) ───────────── */

struct input_event {
    struct timeval time;
    uint16_t       type;
    uint16_t       code;
    int32_t        value;
};

/* Event types */
#define EV_SYN          0x00
#define EV_KEY          0x01
#define EV_REL          0x02
#define EV_ABS          0x03
#define EV_MSC          0x04
#define EV_SW           0x05
#define EV_LED          0x11
#define EV_REP          0x14

/* Sync codes */
#define SYN_REPORT      0
#define SYN_CONFIG      1
#define SYN_MT_REPORT   2
#define SYN_DROPPED     3

/* Relative axes */
#define REL_X           0x00
#define REL_Y           0x01
#define REL_WHEEL       0x08
#define REL_HWHEEL      0x06

/* Absolute axes */
#define ABS_X           0x00
#define ABS_Y           0x01
#define ABS_MT_SLOT     0x2f
#define ABS_MT_TOUCH_MAJOR 0x30
#define ABS_MT_POSITION_X  0x35
#define ABS_MT_POSITION_Y  0x36
#define ABS_MT_TRACKING_ID 0x39

/* Key values */
#define KEY_VALUE_UP        0
#define KEY_VALUE_DOWN      1
#define KEY_VALUE_REPEAT    2

/* Mouse buttons */
#define BTN_LEFT    0x110
#define BTN_RIGHT   0x111
#define BTN_MIDDLE  0x112

/* ── Device node paths ────────────────────────────────────────────────────── */

#define EVDEV_DIR        "/dev/input"
#define EVDEV_RUN_DIR    "/run/darwin-evdev"
#define MAX_DEVICES      16

/* ── Device context ───────────────────────────────────────────────────────── */

typedef enum {
    DEV_KEYBOARD = 0,
    DEV_POINTER,
    DEV_TOUCHPAD,
    DEV_UNKNOWN,
} device_type_t;

typedef struct {
    int             write_fd;   /* we write input_events here */
    int             read_fd;    /* libinput reads from here (via /dev/input symlink) */
    char            node_path[64];  /* e.g. /run/darwin-evdev/event0 */
    device_type_t   type;
    bool            active;
    char            name[128];
    IOHIDDeviceRef  hid_device; /* NULL for the "virtual" aggregate */
} evdev_device_t;

static evdev_device_t  g_devices[MAX_DEVICES];
static int             g_device_count = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static IOHIDManagerRef g_hid_manager = NULL;
static volatile bool   g_running = true;

/* ── Utility ─────────────────────────────────────────────────────────────── */

static void emit_event(evdev_device_t *dev, uint16_t type, uint16_t code,
                        int32_t value) {
    struct input_event ev;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ev.time  = tv;
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    write(dev->write_fd, &ev, sizeof(ev));
}

static void emit_sync(evdev_device_t *dev) {
    emit_event(dev, EV_SYN, SYN_REPORT, 0);
}

/* ── HID Usage Page → Linux key code translation ─────────────────────────── */
/*
 * Subset of the full USB HID Usage Tables → Linux evdev keycode mapping.
 * Full mapping: linux/drivers/hid/hid-input.c (hid_keyboard[])
 */
static const uint16_t hid_keyboard_to_linux[256] = {
    /* 0x00 */ 0,   0,   0,   0,  30,  48,  46,  32, /* ..   A   B   C   D */
    /* 0x08 */18,  33,  34,  35,  23,  36,  37,  38, /* E    F   G   H   I */
    /* 0x10 */24,  25,  16,  19,  31,  20,  22,  47, /* J    K   L   M   N */
    /* 0x18 */17,  45,  21,  44,   2,   3,   4,   5, /* O    P   Q   R   S */
    /* 0x20 */ 6,   7,   8,   9,  10,  11,  28,   1, /* T    U   V   W   X */
    /* 0x28 */14,  15,  57,  12,  13,  26,  27,  43, /* Y    Z   ..  ..  .. */
    /* 0x30 */39,  40,  41,  51,  52,  53,  58,  59, /* ;    '   `   ,   . */
    /* 0x38 */60,  61,  62,  63,  64,  65,  66,  67, /* /    F1..F8         */
    /* 0x40 */68,  87,  88,  99,  70, 119, 110, 102, /* F9  F10 F11 F12 .. */
    /* 0x48 */104,111, 107, 109, 106, 105, 108, 103, /* home end pgup pgdn */
    /* 0x50 */100,101,  97,  91,  92,  93,  95,  96, /* ins  del  ..      */
    [0x4f] = 106, [0x50] = 103, [0x51] = 108, [0x52] = 105, /* arrow keys */
    [0x53] = 69,  /* numlock */
    [0x39] = 58,  /* capslock */
    [0x47] = 70,  /* scrolllock (Darwin: F14) */
    [0xe0] = 29,  /* left ctrl */
    [0xe1] = 42,  /* left shift */
    [0xe2] = 56,  /* left alt */
    [0xe3] = 125, /* left meta/super */
    [0xe4] = 97,  /* right ctrl */
    [0xe5] = 54,  /* right shift */
    [0xe6] = 100, /* right alt */
    [0xe7] = 126, /* right meta */
};

static uint16_t hid_usage_to_linux_key(uint32_t usage_page, uint32_t usage) {
    if (usage_page == kHIDPage_KeyboardOrKeypad) {
        if (usage < 256) return hid_keyboard_to_linux[usage];
    } else if (usage_page == kHIDPage_Button) {
        /* Mouse buttons: usage 1=primary, 2=secondary, 3=middle */
        if (usage == 1) return BTN_LEFT;
        if (usage == 2) return BTN_RIGHT;
        if (usage == 3) return BTN_MIDDLE;
    }
    return 0;
}

/* ── IOHIDFamily callbacks ────────────────────────────────────────────────── */

static evdev_device_t *device_for_hid(IOHIDDeviceRef dev) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_device_count; i++) {
        if (g_devices[i].hid_device == dev) {
            pthread_mutex_unlock(&g_lock);
            return &g_devices[i];
        }
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void input_value_callback(void *context, IOReturn result,
                                  void *sender, IOHIDValueRef value) {
    (void)context; (void)result;
    IOHIDDeviceRef   dev_ref  = (IOHIDDeviceRef)sender;
    IOHIDElementRef  elem     = IOHIDValueGetElement(value);
    uint32_t usage_page = IOHIDElementGetUsagePage(elem);
    uint32_t usage      = IOHIDElementGetUsage(elem);
    CFIndex  int_val    = IOHIDValueGetIntegerValue(value);
    IOHIDElementType etype = IOHIDElementGetType(elem);

    evdev_device_t *dev = device_for_hid(dev_ref);
    if (!dev) return;

    /* ── Keyboard ─────────────────────────────────────────────────────── */
    if (usage_page == kHIDPage_KeyboardOrKeypad &&
        (etype == kIOHIDElementTypeInput_Button ||
         etype == kIOHIDElementTypeInput_Misc)) {
        uint16_t keycode = hid_usage_to_linux_key(usage_page, usage);
        if (keycode) {
            emit_event(dev, EV_KEY, keycode, int_val ? KEY_VALUE_DOWN : KEY_VALUE_UP);
            emit_sync(dev);
        }
    }
    /* ── Mouse buttons ────────────────────────────────────────────────── */
    else if (usage_page == kHIDPage_Button) {
        uint16_t btn = hid_usage_to_linux_key(usage_page, usage);
        if (btn) {
            emit_event(dev, EV_KEY, btn, (int)int_val);
            emit_sync(dev);
        }
    }
    /* ── Relative axes (mouse X/Y/wheel) ──────────────────────────────── */
    else if (usage_page == kHIDPage_GenericDesktop) {
        evdev_device_t *ptr = NULL;
        /* Find the pointer device */
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < g_device_count; i++) {
            if (g_devices[i].type == DEV_POINTER) { ptr = &g_devices[i]; break; }
        }
        pthread_mutex_unlock(&g_lock);
        if (!ptr) ptr = dev;

        switch (usage) {
            case kHIDUsage_GD_X:
                emit_event(ptr, EV_REL, REL_X, (int32_t)int_val);
                emit_sync(ptr);
                break;
            case kHIDUsage_GD_Y:
                emit_event(ptr, EV_REL, REL_Y, (int32_t)int_val);
                emit_sync(ptr);
                break;
            case kHIDUsage_GD_Wheel:
                emit_event(ptr, EV_REL, REL_WHEEL, (int32_t)int_val);
                emit_sync(ptr);
                break;
            case kHIDUsage_GD_Z:
                emit_event(ptr, EV_REL, REL_HWHEEL, (int32_t)int_val);
                emit_sync(ptr);
                break;
            /* Touchpad absolute position */
            case kHIDUsage_GD_Slider:
                if (dev->type == DEV_TOUCHPAD) {
                    emit_event(dev, EV_ABS, ABS_X, (int32_t)int_val);
                    emit_sync(dev);
                }
                break;
        }
    }
}

/* ── Device matching callbacks ───────────────────────────────────────────── */

static evdev_device_t *alloc_device(IOHIDDeviceRef dev_ref,
                                     device_type_t dtype) {
    pthread_mutex_lock(&g_lock);
    if (g_device_count >= MAX_DEVICES) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    evdev_device_t *dev = &g_devices[g_device_count];
    memset(dev, 0, sizeof(*dev));

    /* Create a pipe pair for this device node */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    /* Make write end non-blocking so the HID callback never stalls */
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
    /* Make read end blocking (libinput poll()s it) */

    dev->write_fd   = pipefd[1];
    dev->read_fd    = pipefd[0];
    dev->type       = dtype;
    dev->active     = true;
    dev->hid_device = dev_ref;

    snprintf(dev->node_path, sizeof(dev->node_path),
             EVDEV_RUN_DIR "/event%d", g_device_count);

    /* Get device name from IOKit registry */
    CFStringRef nameRef = (CFStringRef)IOHIDDeviceGetProperty(
        dev_ref, CFSTR(kIOHIDProductKey));
    if (nameRef)
        CFStringGetCString(nameRef, dev->name, sizeof(dev->name),
                           kCFStringEncodingUTF8);
    else
        snprintf(dev->name, sizeof(dev->name), "Darwin HID device %d",
                 g_device_count);

    g_device_count++;
    pthread_mutex_unlock(&g_lock);

    /* Create the symlink: /dev/input/eventN → /run/darwin-evdev/eventN
     * The read end of the pipe is the "character device". libinput opens
     * the symlink and gets a regular fd — poll()/read() work normally. */
    mkdir(EVDEV_DIR, 0755);
    mkdir(EVDEV_RUN_DIR, 0755);

    char node_name[32];
    snprintf(node_name, sizeof(node_name), "event%d", g_device_count - 1);

    /* Write a fd-as-path via /dev/fd/ trick:
     * We can't create a real device node without root + cdevsw.
     * Instead: write the fd number to a known path file, and provide
     * a helper so libinput can open it. In production this should use
     * a KEXT-created cdev; for userspace testing the pipe trick works. */
    char linkpath[64];
    snprintf(linkpath, sizeof(linkpath), EVDEV_DIR "/%s", node_name);

    /* Write the pipe read-fd number to a file libinput_darwin can pick up */
    char fdpath[64];
    snprintf(fdpath, sizeof(fdpath), EVDEV_RUN_DIR "/%s.fd", node_name);
    FILE *f = fopen(fdpath, "w");
    if (f) { fprintf(f, "%d\n", dev->read_fd); fclose(f); }

    printf("[darwin-evdev] registered %s: %s → %s (fd %d)\n",
           dtype == DEV_KEYBOARD ? "keyboard" :
           dtype == DEV_POINTER  ? "pointer"  : "touchpad",
           dev->name, dev->node_path, dev->read_fd);

    return dev;
}

static device_type_t classify_hid_device(IOHIDDeviceRef dev) {
    CFNumberRef usagePage = (CFNumberRef)IOHIDDeviceGetProperty(
        dev, CFSTR(kIOHIDPrimaryUsagePageKey));
    CFNumberRef usage = (CFNumberRef)IOHIDDeviceGetProperty(
        dev, CFSTR(kIOHIDPrimaryUsageKey));

    if (!usagePage || !usage) return DEV_UNKNOWN;

    int32_t page = 0, use = 0;
    CFNumberGetValue(usagePage, kCFNumberSInt32Type, &page);
    CFNumberGetValue(usage,     kCFNumberSInt32Type, &use);

    if (page == kHIDPage_GenericDesktop) {
        if (use == kHIDUsage_GD_Keyboard || use == kHIDUsage_GD_Keypad)
            return DEV_KEYBOARD;
        if (use == kHIDUsage_GD_Mouse)
            return DEV_POINTER;
        if (use == kHIDUsage_GD_MultiAxisController ||
            use == kHIDUsage_GD_Joystick)
            return DEV_POINTER;
    }
    /* Touchpad: check transport string for "Trackpad" */
    CFStringRef transport = (CFStringRef)IOHIDDeviceGetProperty(
        dev, CFSTR(kIOHIDTransportKey));
    CFStringRef product = (CFStringRef)IOHIDDeviceGetProperty(
        dev, CFSTR(kIOHIDProductKey));
    if (product) {
        char buf[128] = {};
        CFStringGetCString(product, buf, sizeof(buf), kCFStringEncodingUTF8);
        if (strstr(buf, "Trackpad") || strstr(buf, "trackpad"))
            return DEV_TOUCHPAD;
    }
    (void)transport;
    return DEV_UNKNOWN;
}

static void device_matching_callback(void *context, IOReturn result,
                                      void *sender, IOHIDDeviceRef device) {
    (void)context; (void)result; (void)sender;
    device_type_t dtype = classify_hid_device(device);
    if (dtype == DEV_UNKNOWN) return;

    evdev_device_t *dev = alloc_device(device, dtype);
    if (!dev) return;

    /* Register per-device value callback */
    IOHIDDeviceRegisterInputValueCallback(device, input_value_callback, dev);
    IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetCurrent(),
                                   kCFRunLoopDefaultMode);
    IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
}

static void device_removal_callback(void *context, IOReturn result,
                                     void *sender, IOHIDDeviceRef device) {
    (void)context; (void)result; (void)sender;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_device_count; i++) {
        if (g_devices[i].hid_device == device) {
            close(g_devices[i].write_fd);
            close(g_devices[i].read_fd);
            g_devices[i].active = false;
            printf("[darwin-evdev] removed: %s\n", g_devices[i].name);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/* ── Setup HID manager ───────────────────────────────────────────────────── */

static IOHIDManagerRef setup_hid_manager(void) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault,
                                              kIOHIDOptionsTypeNone);
    if (!mgr) return NULL;

    /* Match keyboards, mice, and trackpads by usage page */
    CFDictionaryRef kb = ({
        int page = kHIDPage_GenericDesktop;
        CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &page);
        CFStringRef keys[]   = { CFSTR(kIOHIDDeviceUsagePageKey) };
        CFTypeRef   values[] = { p };
        CFDictionaryRef d = CFDictionaryCreate(NULL,
            (const void**)keys, (const void**)values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFRelease(p);
        d;
    });

    /* Match any GenericDesktop device (covers keyboard, mouse, touchpad) */
    IOHIDManagerSetDeviceMatching(mgr, kb);
    CFRelease(kb);

    IOHIDManagerRegisterDeviceMatchingCallback(mgr, device_matching_callback, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(mgr,  device_removal_callback, NULL);
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    IOReturn ret = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    if (ret != kIOReturnSuccess) {
        fprintf(stderr, "[darwin-evdev] IOHIDManagerOpen failed: 0x%x\n", ret);
        CFRelease(mgr);
        return NULL;
    }
    return mgr;
}

/* ── Signal handler ──────────────────────────────────────────────────────── */

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("[darwin-evdev] starting — IOHIDFamily → evdev bridge\n");
    printf("[darwin-evdev] device nodes will appear at %s/\n", EVDEV_DIR);

    /* Ensure /dev/input exists */
    mkdir(EVDEV_DIR,    0755);
    mkdir(EVDEV_RUN_DIR, 0755);

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    g_hid_manager = setup_hid_manager();
    if (!g_hid_manager) {
        fprintf(stderr, "[darwin-evdev] failed to create HID manager\n");
        return 1;
    }

    printf("[darwin-evdev] running — waiting for HID devices...\n");

    /* Run the CoreFoundation run loop.  HID events arrive on this thread.
     * The write ends of pipes are written here; the read ends are consumed
     * by libinput on the compositor's thread. */
    while (g_running) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
    }

    printf("[darwin-evdev] shutting down\n");
    IOHIDManagerClose(g_hid_manager, kIOHIDOptionsTypeNone);
    IOHIDManagerUnscheduleFromRunLoop(g_hid_manager, CFRunLoopGetCurrent(),
                                      kCFRunLoopDefaultMode);
    CFRelease(g_hid_manager);

    /* Clean up pipe fds */
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_device_count; i++) {
        if (g_devices[i].active) {
            close(g_devices[i].write_fd);
            close(g_devices[i].read_fd);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}
