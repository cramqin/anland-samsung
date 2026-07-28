/*
 * input_grab — wire format shared by the root grab helper (input_grab.c,
 * shipped as libinputgrab.so) and the app-side reader in native_consumer.c.
 *
 * Immersive full-input capture: the helper runs as root (`su -c`), which is the
 * only SELinux domain already allowed to read input_device chr_files. It takes
 * an exclusive EVIOCGRAB on the touchscreen + key devices (volume, keyboard,
 * consumer-control), so Android's SystemUI never sees those events -- home/back/
 * recents gestures, the notification-shade swipe, and all keys stop reaching
 * Android. It then streams the raw evdev (type,code,value) triples to the app
 * over a unix bridge socket. The app (untrusted_app) only reads that socket
 * (already allowed by sepolicy: untrusted_app -> su unix_stream_socket), so this
 * needs NO new SELinux rule.
 *
 * Lifecycle / auto-release: the grab lives on the open file descriptions the
 * helper holds. When the app closes the bridge socket (immersive off, or app
 * crash) the helper's poll() sees POLLHUP / its send fails, and it exits ->
 * fds close -> grab released. If the helper itself dies, same thing.
 *
 * An app that HANGS rather than dies closes nothing, so that path alone is not
 * enough -- and a blocking forward would then park the helper and kill its own
 * escape. Hence two further guarantees, both in input_grab.c: the user's bound
 * exit key is evaluated before any forwarding on a path that can never block,
 * and forwarding drops records (releasing the grab entirely after a sustained
 * stall) rather than waiting on the app. A node that also carries KEY_POWER is
 * never grabbed, so Power always remains a hardware escape.
 *
 *   usage: libinputgrab.so <bridge_socket_path>   (real mode: stream to app)
 *          libinputgrab.so selftest               (grab + print to stdout)
 */
#ifndef ANLAND_INPUT_GRAB_H
#define ANLAND_INPUT_GRAB_H

#include <stdint.h>

#define IGRAB_MAGIC       0x49475242u  /* "IGRB" */
#define IGRAB_KIND_KEY    0u
#define IGRAB_KIND_TOUCH  1u
#define IGRAB_MAX_DEVICES 16

/* First message the helper sends after connecting: a hello then dev_count
 * device descriptors, then an open-ended stream of igrab_rec. */
struct igrab_hello {
    uint32_t magic;
    uint32_t dev_count;
} __attribute__((packed));

struct igrab_dev {
    uint32_t index;      /* matches igrab_rec.index */
    uint32_t kind;       /* IGRAB_KIND_KEY | IGRAB_KIND_TOUCH */
    int32_t  abs_x_min;  /* touch only (else 0) */
    int32_t  abs_x_max;
    int32_t  abs_y_min;
    int32_t  abs_y_max;
} __attribute__((packed));

/* One raw evdev event, tagged with the device it came from. Arch-independent
 * (no timeval), so the wire layout is fixed regardless of 32/64-bit. */
struct igrab_rec {
    uint32_t index;
    uint16_t type;       /* EV_KEY / EV_ABS / EV_SYN ... */
    uint16_t code;       /* KEY_* / ABS_* / SYN_* */
    int32_t  value;
} __attribute__((packed));

#endif
