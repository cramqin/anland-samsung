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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/input.h>

#include "input_grab.h"
#include "socket_utils.h"

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AnlandGrab", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AnlandGrab", __VA_ARGS__)
#else
#define LOGI(...) fprintf(stderr, __VA_ARGS__)
#define LOGE(...) fprintf(stderr, __VA_ARGS__)
#endif

#define BITS_PER_LONG (int)(sizeof(long) * 8)
#define NBITS(x) ((((x) - 1) / BITS_PER_LONG) + 1)
static int test_bit(int bit, const unsigned long *arr) {
    return (arr[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL;
}

struct dev {
    int fd;
    char path[64];
    uint32_t kind;
    /* 0 = watched but NOT EVIOCGRABbed (a built-in node that also carries Power:
     * the grab is per-node, so grabbing it would swallow Power and destroy the
     * hardware escape hatch). Such nodes are read for exit-key detection only
     * and their events are NOT forwarded, so Android keeps handling them. */
    int grab;
    int32_t abs_x_min, abs_x_max, abs_y_min, abs_y_max;
};

/* Consecutive dropped records before we assume the app will never drain again and
 * hand input back to Android. Only reached while events are actually flowing, so an
 * idle immersive session is never torn down. */
#define IGRAB_MAX_DROPS 2000

/*
 * Decide whether to grab a device and how to classify it.
 *   touchscreen  = INPUT_PROP_DIRECT + ABS_MT_POSITION_X  -> KIND_TOUCH
 *   key device   = has volume keys OR is a keyboard        -> KIND_KEY
 * Deliberately SKIP: pointer devices (mouse/touchpad -> handled by the existing
 * pointer-capture feature), power-only nodes (so Power stays a hardware escape
 * hatch and the device can't be locked out), pens, jacks, fingerprint, etc.
 * Returns kind, or -1 to skip; fills abs ranges for touch.
 */
static int classify(int fd, struct dev *d) {
    unsigned long prop[NBITS(INPUT_PROP_CNT)];
    unsigned long key[NBITS(KEY_CNT)];
    unsigned long abs[NBITS(ABS_CNT)];
    memset(prop, 0, sizeof prop);
    memset(key, 0, sizeof key);
    memset(abs, 0, sizeof abs);
    ioctl(fd, EVIOCGPROP(sizeof prop), prop);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);

    int direct  = test_bit(INPUT_PROP_DIRECT, prop);
    int pointer = test_bit(INPUT_PROP_POINTER, prop);
    int mt      = test_bit(ABS_MT_POSITION_X, abs);
    int vol     = test_bit(KEY_VOLUMEUP, key) || test_bit(KEY_VOLUMEDOWN, key);
    int alpha   = test_bit(KEY_Q, key) && test_bit(KEY_P, key); /* keyboard */
    int power   = test_bit(KEY_POWER, key);

    if (pointer)
        return -1;                       /* mouse / touchpad -> pointer-capture */
    if (direct && mt) {
        struct input_absinfo ax, ay;
        memset(&ax, 0, sizeof ax); memset(&ay, 0, sizeof ay);
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax);
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
        d->abs_x_min = ax.minimum; d->abs_x_max = ax.maximum;
        d->abs_y_min = ay.minimum; d->abs_y_max = ay.maximum;
        d->grab = 1;
        return IGRAB_KIND_TOUCH;
    }
    if (vol || alpha) {
        /* EVIOCGRAB is exclusive per NODE, not per key. Many SoCs (MTK mtk-kpd,
         * some gpio-keys) expose Power on the SAME node as the volume keys; on
         * those, grabbing it would take Power too and leave the user with no
         * hardware escape at all. Watch such a node un-grabbed instead: the exit
         * key is still detected, Power keeps working, and Android keeps the
         * volume keys (a UX regression only on those devices). An external
         * keyboard may also report KEY_POWER but is not the device's escape
         * hatch, so keep grabbing those. */
        d->grab = !(power && !alpha);
        return IGRAB_KIND_KEY;           /* volume node / keyboard / consumer ctrl */
    }
    return -1;                           /* power-only, pen, jack, fingerprint... */
}

/* Enumerate /dev/input, classify, and EVIOCGRAB every wanted device. Returns
 * the number of devices grabbed; fills devs[]. */
static int collect_and_grab(struct dev *devs, int max) {
    DIR *dir = opendir("/dev/input");
    if (!dir) { LOGE("opendir /dev/input: %s\n", strerror(errno)); return 0; }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && n < max) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        char path[64];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct dev d;
        memset(&d, 0, sizeof d);
        int kind = classify(fd, &d);
        if (kind < 0) { close(fd); continue; }
        if (d.grab && ioctl(fd, EVIOCGRAB, 1) < 0) {
            LOGE("EVIOCGRAB %s failed: %s\n", path, strerror(errno));
            close(fd);
            continue;
        }
        d.fd = fd;
        d.kind = (uint32_t)kind;
        snprintf(d.path, sizeof d.path, "%s", path);
        devs[n++] = d;
        LOGI("%s %s kind=%s%s\n", d.grab ? "grabbed" : "watching (has Power)", path,
             kind == IGRAB_KIND_TOUCH ? "TOUCH" : "KEY",
             kind == IGRAB_KIND_TOUCH ? " (touchscreen)" : "");
    }
    closedir(dir);
    return n;
}

static void ungrab_all(struct dev *devs, int n) {
    for (int i = 0; i < n; i++) {
        if (devs[i].fd >= 0) {
            ioctl(devs[i].fd, EVIOCGRAB, 0);
            close(devs[i].fd);
            devs[i].fd = -1;
        }
    }
}

/*
 * Bounded, record-atomic forward of one event to the app.
 *
 * CRITICAL: the exit key is detected on this same thread, so a slow or wedged app
 * must never be able to park us inside send(). A blocking send here was a real
 * lockout: the app can stall (its own push_input_event() to the compositor is a
 * blocking write, or the process gets frozen) WITHOUT closing the socket, the bridge
 * buffer fills in well under a second of dragging, and we would then never return to
 * poll()/read() -- so the exit key is never even read, and the touchscreen stays
 * grabbed with no way out but adb or a forced reboot.
 *
 * So: never wait. A full buffer just DROPS the record (never a partial one -- that
 * would desync the app's fixed-size framing) and we go straight back to reading input.
 * Returns 1 = sent, 0 = dropped, -1 = fatal (caller releases the grab and exits).
 */
static int send_rec_nb(int sock, const struct igrab_rec *rec) {
    ssize_t n = send(sock, rec, sizeof *rec, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n == (ssize_t)sizeof *rec)
        return 1;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;                        /* app not draining -> drop, keep reading */
    /* Peer gone (EPIPE/ECONNRESET), or a short write, which would mean the framing is
     * already lost. Either way tear down so the grab is released. */
    return -1;
}

/* Real mode: stream tagged raw events to the app over the bridge socket until
 * it closes (POLLHUP) or a send fails. sock < 0 means selftest (print).
 * exit_code is the evdev key that releases the grab; it is always set. */
static int run_stream(struct dev *devs, int n, int sock, int exit_code) {
    /* Send hello + descriptors first (real mode only). The socket carries an
     * SO_SNDTIMEO (set in main), so these cannot hang while the grab is held. */
    if (sock >= 0) {
        struct igrab_hello hello = { .magic = IGRAB_MAGIC, .dev_count = (uint32_t)n };
        if (send_all(sock, &hello, sizeof hello) < 0) return -1;
        for (int i = 0; i < n; i++) {
            struct igrab_dev gd = {
                .index = (uint32_t)i, .kind = devs[i].kind,
                .abs_x_min = devs[i].abs_x_min, .abs_x_max = devs[i].abs_x_max,
                .abs_y_min = devs[i].abs_y_min, .abs_y_max = devs[i].abs_y_max,
            };
            if (send_all(sock, &gd, sizeof gd) < 0) return -1;
        }
    }

    struct pollfd pfd[IGRAB_MAX_DEVICES + 1];
    for (int i = 0; i < n; i++) { pfd[i].fd = devs[i].fd; pfd[i].events = POLLIN; }
    int sock_slot = -1;
    if (sock >= 0) { sock_slot = n; pfd[n].fd = sock; pfd[n].events = POLLIN; }
    int nfds = (sock >= 0) ? n + 1 : n;

    struct input_event evs[64];
    int exiting = 0;
    int drops = 0, need_resync = 0;
    while (!exiting) {
        int r = poll(pfd, nfds, -1);
        if (r < 0) { if (errno == EINTR) continue; break; }
        /* App closed the bridge -> release and exit. */
        if (sock_slot >= 0 && (pfd[sock_slot].revents & (POLLHUP | POLLERR | POLLIN)))
            break;
        for (int i = 0; i < n && !exiting; i++) {
            if (!(pfd[i].revents & POLLIN))
                continue;
            ssize_t got = read(devs[i].fd, evs, sizeof evs);
            if (got <= 0) continue;
            int cnt = got / (ssize_t)sizeof(struct input_event);
            for (int k = 0; k < cnt; k++) {
                /* Device-side exit key. This lives in the HELPER (root) and is
                 * evaluated BEFORE any forwarding, on a path that can never block, so
                 * the escape works even if the app is frozen, wedged or dead -- that
                 * decoupling is exactly the failure that locked the device before.
                 * Checked on every device, grabbed or not, so an un-grabbed
                 * Power-bearing node can still trigger the exit. */
                if (evs[k].type == EV_KEY && evs[k].code == exit_code) {
                    if (evs[k].value == 1) {
                        LOGI("exit key %d -> releasing grab", exit_code);
                        exiting = 1;
                        break;
                    }
                    /* Never forward the toggle key: it is consumed by the toggle,
                     * so Linux must not also act on it. */
                    continue;
                }
                /* Un-grabbed node (Power shares it): exit-key watch only. Android
                 * still handles those keys, so forwarding would double-act. */
                if (!devs[i].grab)
                    continue;
                struct igrab_rec rec = {
                    .index = (uint32_t)i,
                    .type = evs[k].type, .code = evs[k].code, .value = evs[k].value,
                };
                if (sock >= 0) {
                    /* After a gap, tell the app to reset its slot/key state before
                     * it sees fresh events, so no finger or key can stay stuck. */
                    if (need_resync) {
                        struct igrab_rec mark = { .index = (uint32_t)i,
                                                  .type = EV_SYN, .code = SYN_DROPPED };
                        int mr = send_rec_nb(sock, &mark);
                        if (mr < 0) return 0;
                        if (mr == 1) need_resync = 0;
                    }
                    int sr = send_rec_nb(sock, &rec);
                    if (sr < 0)
                        return 0;          /* app gone -> release and exit */
                    if (sr == 1) {
                        drops = 0;
                    } else {
                        need_resync = 1;
                        if (++drops > IGRAB_MAX_DROPS) {
                            LOGE("app stopped draining (%d dropped) -> releasing "
                                 "grab so input returns to Android\n", drops);
                            return 0;
                        }
                    }
                } else if (evs[k].type != EV_SYN || evs[k].code != SYN_REPORT) {
                    printf("dev%u t=%u code=%u val=%d\n",
                           rec.index, rec.type, rec.code, rec.value);
                    fflush(stdout);
                }
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        LOGE("usage: %s <bridge_socket|selftest> <exit_keycode>\n", argv[0]);
        return 1;
    }
    /* The evdev key that toggles immersion, bound by the user in Settings. It is
     * REQUIRED: refusing to grab without one guarantees there is always a root-side
     * way out, whatever the app is doing. */
    int exit_code = atoi(argv[2]);
    if (exit_code <= 0) {
        LOGE("refusing to grab: no exit key given\n");
        return 1;
    }
    struct dev devs[IGRAB_MAX_DEVICES];
    int n = collect_and_grab(devs, IGRAB_MAX_DEVICES);
    if (n == 0) { LOGE("no devices grabbed\n"); return 2; }

    int sock = -1;
    if (strcmp(argv[1], "selftest") != 0) {
        sock = connect_unix(argv[1]);
        if (sock < 0) { LOGE("connect bridge %s failed\n", argv[1]); ungrab_all(devs, n); return 3; }
        /* Belt and braces: no send on this socket may ever hang while we hold the
         * grab (the per-event path is non-blocking; this covers the hello/descriptor
         * send_all()s, which would otherwise strand the grab before the loop runs). */
        struct timeval sto = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof sto);
    } else {
        printf("SELFTEST %d devices; touch/press to see events "
               "(evdev key %d, or Ctrl-C, to release)\n", n, exit_code);
        for (int i = 0; i < n; i++)
            printf("  dev%d %s kind=%s%s\n", i, devs[i].path,
                   devs[i].kind == IGRAB_KIND_TOUCH ? "TOUCH" : "KEY",
                   devs[i].grab ? "" : " [WATCH-ONLY: shares node with Power]");
        fflush(stdout);
    }

    run_stream(devs, n, sock, exit_code);

    if (sock >= 0) close(sock);
    ungrab_all(devs, n);
    LOGI("released %d devices, exiting\n", n);
    return 0;
}
