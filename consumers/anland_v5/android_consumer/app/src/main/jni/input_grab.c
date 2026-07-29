/*
 * input_grab — root-side exclusive input grabber. See input_grab.h for the
 * architecture and wire format. Built as libinputgrab.so (an executable named
 * lib*.so so the packager extracts it with +x), launched by the app via `su -c`.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/input.h>

#include "input_grab.h"

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AnlandGrab", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AnlandGrab", __VA_ARGS__)
#else
#define LOGI(...) fprintf(stderr, __VA_ARGS__)
#define LOGE(...) fprintf(stderr, __VA_ARGS__)
#endif

#define BITS_PER_LONG ((int)(sizeof(unsigned long) * 8))
#define NBITS(x) ((((x) - 1) / BITS_PER_LONG) + 1)
#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))

#define IGRAB_CONNECT_TIMEOUT_MS 3000
#define IGRAB_STARTUP_SEND_MS    2000
#define IGRAB_READ_BATCH         64

static int test_bit(int bit, const unsigned long *arr)
{
    return (arr[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL;
}

struct dev {
    int fd;
    char path[128];
    uint32_t kind;
    int forward;
    int grabbed;
    int has_ev_key;
    int has_exit;
    int wire_index;
    int32_t abs_x_min, abs_x_max, abs_y_min, abs_y_max;
    int32_t slot_min, slot_max, slot_current;
};

struct pending_batch {
    int count;
    struct input_event events[IGRAB_READ_BATCH];
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_fd(int fd, short events, int timeout_ms)
{
    struct pollfd p = { .fd = fd, .events = events };
    for (;;) {
        int r = poll(&p, 1, timeout_ms);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return r;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;
        return (p.revents & events) ? 1 : 0;
    }
}

static int connect_bridge(const char *path)
{
    if (!path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0)
        return fd;
    if (errno != EINPROGRESS && errno != EAGAIN) {
        close(fd);
        return -1;
    }

    if (wait_fd(fd, POLLOUT, IGRAB_CONNECT_TIMEOUT_MS) != 1) {
        close(fd);
        errno = ETIMEDOUT;
        return -1;
    }
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
        if (err != 0)
            errno = err;
        close(fd);
        return -1;
    }
    return fd;
}

static int send_exact_timeout(int fd, const void *buf, size_t len, int timeout_ms)
{
    const unsigned char *p = buf;
    size_t sent = 0;
    int64_t deadline = now_ms() + timeout_ms;
    while (sent < len) {
        int64_t left = deadline - now_ms();
        if (left <= 0)
            return -1;
        int r = wait_fd(fd, POLLOUT, left > INT32_MAX ? INT32_MAX : (int)left);
        if (r != 1)
            return -1;
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return -1;
    }
    return 0;
}

/* One event record is deliberately all-or-nothing. A short stream write would leave
 * the reader with a partial frame, so treat it as fatal and release the grab. */
static int send_rec_nb(int sock, const struct igrab_rec *rec)
{
    ssize_t n = send(sock, rec, sizeof *rec, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n == (ssize_t)sizeof *rec)
        return 1;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    return -1;
}

static void send_failed_hello(int sock, int reason)
{
    if (sock < 0)
        return;
    struct igrab_hello hello = {
        .magic = IGRAB_MAGIC,
        .version = IGRAB_PROTOCOL_VERSION,
        .status = IGRAB_STATUS_FAILED,
        .reason = (uint32_t)reason,
        .dev_count = 0,
    };
    (void)send_exact_timeout(sock, &hello, sizeof hello, IGRAB_STARTUP_SEND_MS);
}

static int send_success_hello(int sock, struct dev *devs, int n, int forward_count)
{
    struct igrab_hello hello = {
        .magic = IGRAB_MAGIC,
        .version = IGRAB_PROTOCOL_VERSION,
        .status = IGRAB_STATUS_OK,
        .reason = IGRAB_REASON_NONE,
        .dev_count = (uint32_t)forward_count,
    };
    if (send_exact_timeout(sock, &hello, sizeof hello, IGRAB_STARTUP_SEND_MS) < 0)
        return -1;

    for (int i = 0; i < n; i++) {
        struct dev *d = &devs[i];
        if (!d->forward)
            continue;
        struct igrab_dev gd = {
            .index = (uint32_t)d->wire_index,
            .kind = d->kind,
            .abs_x_min = d->abs_x_min,
            .abs_x_max = d->abs_x_max,
            .abs_y_min = d->abs_y_min,
            .abs_y_max = d->abs_y_max,
            .abs_slot_min = d->slot_min,
            .abs_slot_max = d->slot_max,
        };
        if (send_exact_timeout(sock, &gd, sizeof gd, IGRAB_STARTUP_SEND_MS) < 0)
            return -1;
    }

    /* EVIOCGABS reports the device-global current Type-B slot. A newly opened evdev
     * client is not guaranteed to receive ABS_MT_SLOT before the next contact when
     * the driver reuses that same slot, so seed the app reader explicitly. */
    for (int i = 0; i < n; i++) {
        struct dev *d = &devs[i];
        if (!d->forward || d->kind != IGRAB_KIND_TOUCH)
            continue;
        struct igrab_rec slot = {
            .index = (uint32_t)d->wire_index,
            .type = EV_ABS,
            .code = ABS_MT_SLOT,
            .value = d->slot_current,
        };
        if (send_exact_timeout(sock, &slot, sizeof slot, IGRAB_STARTUP_SEND_MS) < 0)
            return -1;
    }
    return 0;
}

static int is_external_bus(uint16_t bustype)
{
    return bustype == BUS_USB || bustype == BUS_BLUETOOTH;
}

static int any_key_except_reserved(const unsigned long *key)
{
    for (int code = 1; code < KEY_CNT; code++) {
        if (test_bit(code, key))
            return 1;
    }
    return 0;
}

/* Returns 1 to retain the fd, 0 to skip it, -1 for a startup-fatal capability
 * mismatch. Pointer devices are handled by Android pointer capture, but an exit key
 * on one is retained watch-only so the root-side escape remains independent. */
static int classify_device(int fd, int exit_code, struct dev *d, int *reason)
{
    unsigned long ev[NBITS(EV_CNT)];
    unsigned long prop[NBITS(INPUT_PROP_CNT)];
    unsigned long key[NBITS(KEY_CNT)];
    unsigned long abs[NBITS(ABS_CNT)];
    unsigned long rel[NBITS(REL_CNT)];
    struct input_id id;
    memset(ev, 0, sizeof ev);
    memset(prop, 0, sizeof prop);
    memset(key, 0, sizeof key);
    memset(abs, 0, sizeof abs);
    memset(rel, 0, sizeof rel);
    memset(&id, 0, sizeof id);

    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0 ||
        ioctl(fd, EVIOCGPROP(sizeof prop), prop) < 0 ||
        ioctl(fd, EVIOCGID, &id) < 0) {
        *reason = IGRAB_REASON_STARTUP_IO_ERROR;
        return -1;
    }

    d->has_ev_key = test_bit(EV_KEY, ev);
    if (d->has_ev_key && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key) < 0) {
        *reason = IGRAB_REASON_STARTUP_IO_ERROR;
        return -1;
    }
    if (test_bit(EV_ABS, ev) && ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs) < 0) {
        *reason = IGRAB_REASON_STARTUP_IO_ERROR;
        return -1;
    }
    if (test_bit(EV_REL, ev) && ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel) < 0) {
        *reason = IGRAB_REASON_STARTUP_IO_ERROR;
        return -1;
    }

    d->has_exit = d->has_ev_key && exit_code > 0 && exit_code < KEY_CNT &&
                  test_bit(exit_code, key);
    int direct = test_bit(INPUT_PROP_DIRECT, prop);
    int mt = test_bit(ABS_MT_POSITION_X, abs) && test_bit(ABS_MT_POSITION_Y, abs);
    int pointer_buttons = 0;
    if (d->has_ev_key) {
        for (int code = BTN_MOUSE; code < BTN_JOYSTICK; code++) {
            if (test_bit(code, key)) {
                pointer_buttons = 1;
                break;
            }
        }
    }
    int relative_pointer = test_bit(REL_X, rel) || test_bit(REL_Y, rel);
    int pointer = test_bit(INPUT_PROP_POINTER, prop) || relative_pointer ||
                  pointer_buttons || test_bit(INPUT_PROP_BUTTONPAD, prop) ||
                  test_bit(INPUT_PROP_SEMI_MT, prop) || (!direct && mt);
    int power = d->has_ev_key && test_bit(KEY_POWER, key);
    int protected_power = power && !is_external_bus(id.bustype);

    int type_b = mt && test_bit(ABS_MT_SLOT, abs) && test_bit(ABS_MT_TRACKING_ID, abs);
    if (!pointer && mt) {
        if (!type_b) {
            *reason = IGRAB_REASON_STARTUP_IO_ERROR;
            return -1;
        }
        struct input_absinfo ax, ay, as;
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) < 0 ||
            ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) < 0 ||
            ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &as) < 0 ||
            ax.maximum <= ax.minimum || ay.maximum <= ay.minimum ||
            as.maximum < as.minimum ||
            as.value < as.minimum || as.value > as.maximum ||
            (int64_t)as.maximum - as.minimum + 1 > IGRAB_MAX_SLOTS) {
            *reason = IGRAB_REASON_STARTUP_IO_ERROR;
            return -1;
        }
        if (protected_power) {
            *reason = IGRAB_REASON_GRAB_FAILED;
            return -1;
        }
        d->kind = IGRAB_KIND_TOUCH;
        d->forward = 1;
        d->abs_x_min = ax.minimum; d->abs_x_max = ax.maximum;
        d->abs_y_min = ay.minimum; d->abs_y_max = ay.maximum;
        d->slot_min = as.minimum; d->slot_max = as.maximum;
        d->slot_current = as.value;
        return 1;
    }

    int single = !pointer && test_bit(ABS_X, abs) && test_bit(ABS_Y, abs) &&
                 d->has_ev_key && test_bit(BTN_TOUCH, key);
    if (single) {
        struct input_absinfo ax, ay;
        if (ioctl(fd, EVIOCGABS(ABS_X), &ax) < 0 ||
            ioctl(fd, EVIOCGABS(ABS_Y), &ay) < 0 ||
            ax.maximum <= ax.minimum || ay.maximum <= ay.minimum) {
            *reason = IGRAB_REASON_STARTUP_IO_ERROR;
            return -1;
        }
        if (protected_power) {
            *reason = IGRAB_REASON_GRAB_FAILED;
            return -1;
        }
        d->kind = IGRAB_KIND_SINGLE_TOUCH;
        d->forward = 1;
        d->abs_x_min = ax.minimum; d->abs_x_max = ax.maximum;
        d->abs_y_min = ay.minimum; d->abs_y_max = ay.maximum;
        d->slot_min = d->slot_max = 0;
        return 1;
    }

    /* A direct device that is neither supported Type-B touch nor BTN_TOUCH single
     * touch would otherwise keep reaching Android while we claimed full immersion. */
    if (direct) {
        *reason = IGRAB_REASON_STARTUP_IO_ERROR;
        return -1;
    }

    if (pointer) {
        if (d->has_exit) {
            d->kind = IGRAB_KIND_KEY;
            d->forward = 0;
            return 1;
        }
        return 0;
    }

    if (d->has_ev_key && any_key_except_reserved(key)) {
        if (protected_power) {
            /* EVIOCGRAB is per node. Leave a built-in Power-bearing node with
             * Android; retain it only when it carries the configured exit key. */
            if (d->has_exit) {
                d->kind = IGRAB_KIND_KEY;
                d->forward = 0;
                return 1;
            }
            return 0;
        }
        d->kind = IGRAB_KIND_KEY;
        d->forward = 1;
        return 1;
    }

    if (d->has_exit) {
        d->kind = IGRAB_KIND_KEY;
        d->forward = 0;
        return 1;
    }
    return 0;
}

static int inotify_changed(int fd)
{
    char buf[4096];
    int changed = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            changed = 1;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return changed;
        return n == 0 ? changed : -1;
    }
}

static int collect_devices(struct dev *devs, int max, int exit_code,
                           int *count, int *forward_count, int *reason)
{
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        *reason = IGRAB_REASON_INPUT_DIR_OPEN_FAILED;
        return -1;
    }

    int n = 0, forwards = 0, exit_visible = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;

        char path[128];
        int plen = snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        if (plen < 0 || plen >= (int)sizeof path) {
            *reason = IGRAB_REASON_STARTUP_IO_ERROR;
            goto fail;
        }
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            *reason = IGRAB_REASON_STARTUP_IO_ERROR;
            goto fail;
        }

        struct dev d;
        memset(&d, 0, sizeof d);
        d.fd = fd;
        d.wire_index = -1;
        int keep = classify_device(fd, exit_code, &d, reason);
        if (keep < 0) {
            close(fd);
            goto fail;
        }
        if (keep == 0) {
            close(fd);
            continue;
        }
        if (n >= max) {
            close(fd);
            *reason = IGRAB_REASON_DEVICE_LIMIT;
            goto fail;
        }
        snprintf(d.path, sizeof d.path, "%s", path);
        if (d.forward)
            d.wire_index = forwards++;
        if (d.has_exit)
            exit_visible = 1;
        devs[n++] = d;
    }
    closedir(dir);

    if (!exit_visible) {
        *reason = IGRAB_REASON_EXIT_KEY_UNAVAILABLE;
        goto fail_closed;
    }
    if (forwards == 0) {
        *reason = IGRAB_REASON_NO_GRABBED_DEVICE;
        goto fail_closed;
    }
    *count = n;
    *forward_count = forwards;
    return 0;

fail:
    closedir(dir);
fail_closed:
    for (int i = 0; i < n; i++)
        close(devs[i].fd);
    return -1;
}

static int check_idle(struct dev *devs, int n, int exit_code, int *reason)
{
    for (int i = 0; i < n; i++) {
        struct dev *d = &devs[i];
        unsigned long key[NBITS(KEY_CNT)];
        memset(key, 0, sizeof key);
        if (d->has_ev_key) {
            if (ioctl(d->fd, EVIOCGKEY(sizeof key), key) < 0) {
                *reason = IGRAB_REASON_STARTUP_IO_ERROR;
                return -1;
            }
            if (exit_code > 0 && exit_code < KEY_CNT && test_bit(exit_code, key)) {
                *reason = IGRAB_REASON_EXIT_KEY_HELD;
                return -1;
            }
            if (d->forward) {
                for (int code = 1; code < KEY_CNT; code++) {
                    if (test_bit(code, key)) {
                        *reason = IGRAB_REASON_INPUT_BUSY;
                        return -1;
                    }
                }
            }
        }

        if (d->forward && d->kind == IGRAB_KIND_TOUCH) {
            int slots = d->slot_max - d->slot_min + 1;
            int32_t values[IGRAB_MAX_SLOTS + 1];
            memset(values, 0, sizeof values);
            values[0] = ABS_MT_TRACKING_ID;
            size_t bytes = (size_t)(slots + 1) * sizeof(values[0]);
            if (ioctl(d->fd, EVIOCGMTSLOTS(bytes), values) < 0) {
                *reason = IGRAB_REASON_STARTUP_IO_ERROR;
                return -1;
            }
            for (int s = 1; s <= slots; s++) {
                if (values[s] >= 0) {
                    *reason = IGRAB_REASON_INPUT_BUSY;
                    return -1;
                }
            }
        }
    }
    return 0;
}

/* Opening an evdev fd starts a private event queue. EVIOCGKEY/EVIOCGMTSLOTS only
 * describe the current state, so a quick press-and-release could otherwise occur
 * entirely between the two idle snapshots and remain queued for the new session.
 * Treat any such startup-window record as busy and retry from a clean open. */
static int reject_pending_startup_events(struct dev *devs, int n, int exit_code,
                                         int *reason)
{
    struct input_event events[IGRAB_READ_BATCH];
    for (int i = 0; i < n; i++) {
        for (;;) {
            ssize_t got = read(devs[i].fd, events, sizeof events);
            if (got < 0 && errno == EINTR)
                continue;
            if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (got <= 0 || got % (ssize_t)sizeof(events[0]) != 0) {
                *reason = IGRAB_REASON_DEVICE_LOST;
                return -1;
            }
            int count = (int)(got / (ssize_t)sizeof(events[0]));
            for (int k = 0; k < count; k++) {
                if (events[k].type == EV_KEY && events[k].code == exit_code &&
                    events[k].value != 0) {
                    *reason = IGRAB_REASON_EXIT_KEY_HELD;
                    return -1;
                }
            }
            *reason = IGRAB_REASON_INPUT_BUSY;
            return -1;
        }
    }
    return 0;
}

static void ungrab_and_close(struct dev *devs, int n)
{
    for (int i = 0; i < n; i++) {
        if (devs[i].fd < 0)
            continue;
        if (devs[i].grabbed)
            (void)ioctl(devs[i].fd, EVIOCGRAB, 0);
        close(devs[i].fd);
        devs[i].fd = -1;
        devs[i].grabbed = 0;
    }
}

static int grab_all(struct dev *devs, int n, int *reason)
{
    int grabbed = 0;
    for (int i = 0; i < n; i++) {
        if (!devs[i].forward)
            continue;
        if (ioctl(devs[i].fd, EVIOCGRAB, 1) < 0) {
            LOGE("EVIOCGRAB %s failed: %s", devs[i].path, strerror(errno));
            *reason = IGRAB_REASON_GRAB_FAILED;
            return -1;
        }
        devs[i].grabbed = 1;
        grabbed++;
    }
    if (grabbed == 0) {
        *reason = IGRAB_REASON_NO_GRABBED_DEVICE;
        return -1;
    }
    return 0;
}

static int forward_record(int sock, const struct igrab_rec *rec)
{
    return send_rec_nb(sock, rec);
}

static int run_stream(struct dev *devs, int n, int inotify_fd, int sock, int exit_code)
{
    struct pollfd pfd[IGRAB_MAX_DEVICES + 2];
    struct pending_batch batches[IGRAB_MAX_DEVICES];
    for (int i = 0; i < n; i++) {
        pfd[i].fd = devs[i].fd;
        pfd[i].events = POLLIN;
    }
    int ino_slot = n;
    pfd[ino_slot].fd = inotify_fd;
    pfd[ino_slot].events = POLLIN;
    int sock_slot = -1;
    int nfds = n + 1;
    if (sock >= 0) {
        sock_slot = nfds++;
        pfd[sock_slot].fd = sock;
        pfd[sock_slot].events = POLLIN;
    }

    for (;;) {
        int r = poll(pfd, (nfds_t)nfds, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return IGRAB_REASON_STREAM_IO_ERROR;
        }

        memset(batches, 0, sizeof batches);
        int pending_reason = IGRAB_REASON_NONE;
        int changed = 0;
        if (pfd[ino_slot].revents & POLLIN) {
            int cr = inotify_changed(inotify_fd);
            changed = cr != 0;
            if (cr < 0)
                pending_reason = IGRAB_REASON_DEVICE_CHANGED;
        }
        if (pfd[ino_slot].revents & (POLLERR | POLLHUP | POLLNVAL))
            pending_reason = IGRAB_REASON_DEVICE_CHANGED;
        if (changed && pending_reason == IGRAB_REASON_NONE)
            pending_reason = IGRAB_REASON_DEVICE_CHANGED;

        if (sock_slot >= 0 &&
            (pfd[sock_slot].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)))
            pending_reason = IGRAB_REASON_PEER_CLOSED;

        for (int i = 0; i < n; i++) {
            if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL))
                pending_reason = IGRAB_REASON_DEVICE_LOST;
            if (!(pfd[i].revents & POLLIN))
                continue;
            ssize_t got = read(devs[i].fd, batches[i].events, sizeof batches[i].events);
            if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                continue;
            if (got <= 0 || got % (ssize_t)sizeof(struct input_event) != 0) {
                pending_reason = IGRAB_REASON_DEVICE_LOST;
                continue;
            }
            batches[i].count = (int)(got / (ssize_t)sizeof(struct input_event));
        }

        /* Scan every ready node for the escape before forwarding any record from
         * this poll batch. This preserves the root-side escape even under heavy touch. */
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < batches[i].count; k++) {
                struct input_event *ev = &batches[i].events[k];
                if (devs[i].has_exit && ev->type == EV_KEY &&
                    ev->code == exit_code && ev->value == 1)
                    return IGRAB_REASON_EXIT_KEY;
                /* A kernel-side evdev overrun means the current key/slot state is
                 * unknowable without querying and reseeding every device. Hand input
                 * back to Android instead of risking a stuck or misidentified touch. */
                if (ev->type == EV_SYN && ev->code == SYN_DROPPED)
                    return IGRAB_REASON_APP_STALLED;
            }
        }

        if (pending_reason != IGRAB_REASON_NONE)
            return pending_reason;

        for (int i = 0; i < n; i++) {
            struct dev *d = &devs[i];
            for (int k = 0; k < batches[i].count; k++) {
                struct input_event *ev = &batches[i].events[k];
                if (d->has_exit && ev->type == EV_KEY && ev->code == exit_code)
                    continue;
                if (!d->forward)
                    continue;

                struct igrab_rec rec = {
                    .index = (uint32_t)d->wire_index,
                    .type = ev->type,
                    .code = ev->code,
                    .value = ev->value,
                };
                if (sock >= 0) {
                    int fr = forward_record(sock, &rec);
                    if (fr < 0)
                        return IGRAB_REASON_PEER_CLOSED;
                    /* A full bridge means at least one framed state transition would
                     * be lost. Release immediately; root-side exit detection remains
                     * responsive and the producer can never retain a phantom press. */
                    if (fr == 0)
                        return IGRAB_REASON_APP_STALLED;
                } else if (ev->type != EV_SYN || ev->code != SYN_REPORT) {
                    printf("dev%d t=%u code=%u val=%d\n", d->wire_index,
                           ev->type, ev->code, ev->value);
                    fflush(stdout);
                }
            }
        }
    }
}

static void send_release_control(int sock, int reason)
{
    if (sock < 0)
        return;
    struct igrab_rec rec = {
        .index = 0,
        .type = IGRAB_REC_TYPE_CONTROL,
        .code = IGRAB_CONTROL_RELEASE,
        .value = reason,
    };
    (void)send_rec_nb(sock, &rec);
}

static int parse_exit_code(const char *s)
{
    if (!s || !*s)
        return -1;
    char *end = NULL;
    errno = 0;
    long value = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value <= 0 || value >= KEY_CNT)
        return -1;
    return (int)value;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        LOGE("usage: %s <bridge_socket|selftest> <exit_keycode>", argv[0]);
        return 1;
    }

    int selftest = strcmp(argv[1], "selftest") == 0;
    int sock = -1;
    if (!selftest) {
        sock = connect_bridge(argv[1]);
        if (sock < 0) {
            LOGE("connect bridge %s failed: %s", argv[1], strerror(errno));
            return 3;
        }
    }

    int exit_code = parse_exit_code(argv[2]);
    if (exit_code <= 0) {
        send_failed_hello(sock, IGRAB_REASON_INVALID_EXIT_KEY);
        if (sock >= 0)
            close(sock);
        return 1;
    }

    int inotify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (inotify_fd < 0 ||
        inotify_add_watch(inotify_fd, "/dev/input",
                          IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                          IN_DELETE_SELF | IN_MOVE_SELF) < 0) {
        LOGE("inotify /dev/input failed: %s", strerror(errno));
        send_failed_hello(sock, IGRAB_REASON_HOTPLUG_WATCH_FAILED);
        if (inotify_fd >= 0)
            close(inotify_fd);
        if (sock >= 0)
            close(sock);
        return 2;
    }

    struct dev devs[IGRAB_MAX_DEVICES];
    memset(devs, 0, sizeof devs);
    for (int i = 0; i < IGRAB_MAX_DEVICES; i++)
        devs[i].fd = -1;
    int n = 0, forward_count = 0;
    int reason = IGRAB_REASON_NONE;
    int success_hello_started = 0;

    if (collect_devices(devs, IGRAB_MAX_DEVICES, exit_code,
                        &n, &forward_count, &reason) < 0)
        goto startup_fail;
    int changed = inotify_changed(inotify_fd);
    if (changed != 0) {
        reason = changed < 0 ? IGRAB_REASON_HOTPLUG_WATCH_FAILED
                             : IGRAB_REASON_DEVICE_CHANGED;
        goto startup_fail;
    }
    if (check_idle(devs, n, exit_code, &reason) < 0)
        goto startup_fail;
    if (grab_all(devs, n, &reason) < 0)
        goto startup_fail;
    if (check_idle(devs, n, exit_code, &reason) < 0)
        goto startup_fail;
    changed = inotify_changed(inotify_fd);
    if (changed != 0) {
        reason = changed < 0 ? IGRAB_REASON_HOTPLUG_WATCH_FAILED
                             : IGRAB_REASON_DEVICE_CHANGED;
        goto startup_fail;
    }
    if (reject_pending_startup_events(devs, n, exit_code, &reason) < 0)
        goto startup_fail;

    if (sock >= 0) {
        success_hello_started = 1;
        if (send_success_hello(sock, devs, n, forward_count) < 0) {
            reason = IGRAB_REASON_STARTUP_IO_ERROR;
            goto startup_fail;
        }
    }

    LOGI("immersive grab active: %d forwarded devices, exit key %d",
         forward_count, exit_code);
    if (selftest) {
        printf("SELFTEST %d forwarded devices (exit evdev key %d)\n",
               forward_count, exit_code);
        for (int i = 0; i < n; i++) {
            printf("  %s %s kind=%u%s\n", devs[i].forward ? "grabbed" : "watching",
                   devs[i].path, devs[i].kind,
                   devs[i].has_exit ? " [exit]" : "");
        }
        fflush(stdout);
    }

    reason = run_stream(devs, n, inotify_fd, sock, exit_code);
    /* Input must return to Android before any best-effort notification. */
    ungrab_and_close(devs, n);
    send_release_control(sock, reason);
    LOGI("released input devices, reason=%d", reason);
    close(inotify_fd);
    if (sock >= 0)
        close(sock);
    return 0;

startup_fail:
    ungrab_and_close(devs, n);
    LOGE("immersive grab startup failed, reason=%d", reason);
    if (!success_hello_started)
        send_failed_hello(sock, reason);
    close(inotify_fd);
    if (sock >= 0)
        close(sock);
    return 2;
}
