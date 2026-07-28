#define _GNU_SOURCE
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <errno.h>
#include <poll.h>
#include <jni.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <dirent.h>
#include <unistd.h>

#include "anw_hidden.h"
#include "camera_service.h"
#include "display_consumer.h"
#include "native_audio.h"
#include "protocol.h"
#include "socket_utils.h"
#include "input_grab.h"

#define TAG "Anland"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define PIXEL_FORMAT_RGBA_8888 1
#define MAX_COLLECT_BUFS 8

/* Saved JVM reference for event-thread JNI callbacks. Process-global (the JVM is);
 * the per-thread env is attached as needed. The activity callback target is
 * per-instance -> consumer_state.clipboard_obj. */
static JavaVM *g_jvm = NULL;

/* ANativeWindow hidden-API function pointers: loaded once, read-only afterwards, so
 * safe to share across instances. */
static struct anw_api api;
static bool api_loaded = false;
static void on_fallback(void *userdata);

static void on_exit_fallback(void *userdata);
struct consumer_state {
    pthread_mutex_t lock;
    ANativeWindow *window;
    display_ctx *ctx;
    pthread_t render_thread;
    volatile bool running;

    //Note: it is Deamon's Reconnect, not Fallback Flag
    //Fallback is maintained by display lib, and the consumer should not care about it.
    volatile bool need_reconnect;

    int buf_count;
    int dmabuf_fds[MAX_COLLECT_BUFS];
    struct buf_info dmabuf_infos[MAX_COLLECT_BUFS];
    ANativeWindowBuffer *buf_anb[MAX_COLLECT_BUFS];

    int screen_w;
    int screen_h;

    // Latest display refresh rate (milli-Hz) reported from Java. Read on
    // (re)connect to seed the producer; updated live by nativeSetRefreshRate.
    volatile uint32_t refresh_mhz;

    // Event (output) thread
    pthread_t event_thread;
    volatile bool event_running;
    /* True while event_thread holds a started-but-not-yet-joined thread. stop only
     * signals (never joins -- on_fallback can run ON the event thread); the join is
     * deferred to the next start_event_thread() (create time), which runs on the
     * render thread and so cannot self-join. */
    bool event_thread_joinable;

    /* Immersive full-input grab: libinputgrab.so (root, via su) EVIOCGRABs the
     * touchscreen + key devices and streams raw evdev here over a bridge socket;
     * we translate and push_input_event(). See input_grab.h and grab_thread_func. */
    pthread_t grab_thread;
    volatile bool grab_running;
    bool grab_thread_joinable;
    char grab_helper_path[512];
    char grab_bridge_path[512];
    /* Display rotation for the grab touch transform: Surface.ROTATION_* (0..3).
     * Set from Java at start and updated live on rotation. */
    volatile int grab_rotation;
    /* evdev key that toggles immersion. Passed through to the helper, which
     * detects it root-side so the escape works even if we are wedged. */
    int grab_exit_code;

    /* Connection config, set from Java via nativeConfigure() and read on each
     * (re)connect in do_connect(). Guarded by cfg_lock. Per-instance. */
    pthread_mutex_t cfg_lock;
    char cfg_socket_path[256];
    bool cfg_use_root;
    char cfg_helper_path[512];
    char cfg_bridge_path[512];
    int  cfg_custom_width;
    int  cfg_custom_height;

    /* Pointer-motion delta tracking (per-instance). */
    bool  motion_has_last;
    float motion_last_x, motion_last_y;

    /* Clipboard callback target: the Java object whose nativeSetClipboardText /
     * nativeClipListening / nativeClipboardSync the event thread calls (per-instance). */
    jobject clipboard_obj;

    /* Owning MainActivity: on_fallback() calls its onFallback() when the display lib
     * drops the connection, so Java can probe the daemon socket and close the window
     * if the daemon is gone (per-instance global ref). */
    jobject activity_obj;

    /* Per-instance audio bridge (own AAudio streams, own producer). */
    audio_bridge *audio;

    /* Per-instance camera service registration; userdata points back at this state
     * so the camera layer can tell instances apart (see camera_service.c). */
    struct service_info camera_svc;
};

static int collect_dmabufs(struct consumer_state *s)
{
    ANativeWindow *win = s->window;
    int target = s->buf_count;
    int found = 0;

    LOGI("collecting %d dma-bufs via dequeue/queue", target);

    for (int attempt = 0; attempt < target * 4 && found < target; attempt++) {
        ANativeWindowBuffer *anb = NULL;
        int fence = -1;
        if (api.dequeueBuffer(win, &anb, &fence) != 0 || !anb) {
            LOGE("dequeueBuffer failed on attempt %d", attempt);
            if (fence >= 0)
                close(fence);
            break;
        }
        if (fence >= 0)
            close(fence);   /* enumeration only: no need to wait the fence */

        if (!anb->handle || anb->handle->numFds < 1) {
            LOGE("dequeued buffer has no dma-buf handle on attempt %d", attempt);
            api.cancelBuffer(win, anb, -1);
            continue;
        }

        int fd = anb->handle->data[0];   /* first fd backs the dma-buf */
        int stride = anb->stride, width = anb->width, height = anb->height;

        /* deduplicate by ANativeWindowBuffer pointer (stable per queue slot) */
        bool dup_found = false;
        for (int i = 0; i < found; i++) {
            if (s->buf_anb[i] == anb) {
                dup_found = true;
                break;
            }
        }

        /* post it back so the next dequeue rotates to another slot */
        api.queueBuffer(win, anb, -1);

        if (dup_found)
            continue;

        int dup_fd = dup(fd);
        if (dup_fd < 0)
            continue;

        s->buf_anb[found] = anb;
        s->dmabuf_fds[found] = dup_fd;
        s->dmabuf_infos[found].stride = stride * 4;
        s->dmabuf_infos[found].width  = width;
        s->dmabuf_infos[found].height = height;
        s->dmabuf_infos[found].format = PIXEL_FORMAT_RGBA_8888;
        s->dmabuf_infos[found].modifier = 0;
        s->dmabuf_infos[found].offset = 0;
        LOGI("  buf[%d]: anb=%p fd=%d dup=%d %dx%d stride=%d",
             found, (void *)anb, fd, dup_fd, width, height, stride);
        found++;
    }

    if (found < target) {
        LOGE("only collected %d/%d", found, target);
        for (int i = 0; i < found; i++) {
            close(s->dmabuf_fds[i]);
            s->dmabuf_fds[i] = -1;
        }
        return -1;
    }

    s->buf_count = found;
    LOGI("collected %d dma-bufs", found);
    return 0;
}

static void cleanup_dmabufs(struct consumer_state *s)
{
    for (int i = 0; i < s->buf_count; i++) {
        if (s->dmabuf_fds[i] >= 0) {
            close(s->dmabuf_fds[i]);
            s->dmabuf_fds[i] = -1;
        }
    }
    s->buf_count = 0;
}

/* Report the current display refresh rate to the producer over the data
 * channel, reusing the InputEvent framing (see INPUT_TYPE_DISPLAY_REFRESH).
 * No-op when disconnected or rate unknown. */
static void send_refresh_rate(struct consumer_state *s)
{
    if (!s->ctx || s->refresh_mhz == 0)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_DISPLAY_REFRESH,
        .display = { .refresh_mhz = s->refresh_mhz },
    };
    push_input_event(s->ctx, &ev);
}

/*
 * Event thread: listens for output events (clipboard, etc.) from the producer
 * on the data_fd. Runs while s->event_running is true.
 */
static void *event_thread_func(void *arg)
{
    struct consumer_state *s = arg;
    LOGI("event thread started");

    JNIEnv *env = NULL;
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != 0) {
        LOGE("event thread: AttachCurrentThread failed");
        return NULL;
    }

    /* Find classes/methods once */
    jclass ctxClass = (*env)->GetObjectClass(env, s->clipboard_obj);
    jmethodID setClipMethod = (*env)->GetMethodID(env, ctxClass, "nativeSetClipboardText", "(Ljava/lang/String;)V");
    if (!setClipMethod) {
        LOGE("event thread: nativeSetClipboardText not found");
        (*g_jvm)->DetachCurrentThread(g_jvm);
        return NULL;
    }

    while (s->event_running) {
        if (!s->ctx) {
            usleep(50000);
            continue;
        }

        struct OutputEvent ev;
        int ret = poll_output_event(s->ctx, &ev, 500);
        if (ret <= 0)
            continue;

        if (ev.type == OUTPUT_TYPE_RESOURCES_REQUEST) {
            /* Producer is asking for a service's fds (e.g. camera). The display lib
             * matches the type against the registered services and sends the
             * pre-created fds back over SCM_RIGHTS. */
            handle_resource_request(s->ctx, &ev);
        } else if (ev.type == OUTPUT_TYPE_CLIPBOARD && ev.clipboard.size > 0) {
            char *buf = malloc(ev.clipboard.size + 1);
            if (!buf)
                continue;

            if (poll_output_event_extend_data(s->ctx, buf, ev.clipboard.size, 5000) == 1) {
                buf[ev.clipboard.size] = '\0';
                jstring jstr = (*env)->NewStringUTF(env, buf);
                if (jstr) {
                    (*env)->CallVoidMethod(env, s->clipboard_obj, setClipMethod, jstr);
                    (*env)->DeleteLocalRef(env, jstr);
                }
            }
            free(buf);
        } else {
            /* Unknown or zero-length event: drain any trailing data if size > 0 */
            LOGI("event thread: unknown output event type=%u size=%u", ev.type, ev.clipboard.size);
        }
    }

    (*g_jvm)->DetachCurrentThread(g_jvm);
    LOGI("event thread stopped");
    return NULL;
}

static void join_event_thread(struct consumer_state *s)
{
    /* Idempotent. MUST be called only from a non-event thread (render / JNI teardown);
     * never from on_fallback (which may run on the event thread). */
    if (s->event_thread_joinable) {
        pthread_join(s->event_thread, NULL);
        s->event_thread_joinable = false;
    }
}

static void start_event_thread(struct consumer_state *s)
{
    if (s->event_running)
        return;
    /* Reap the previous stopped-but-unjoined thread before spawning a new one. Runs on
     * the render thread (on_exit_fallback), so this join can't self-deadlock. */
    join_event_thread(s);
    s->event_running = true;
    if (pthread_create(&s->event_thread, NULL, event_thread_func, s) == 0)
        s->event_thread_joinable = true;
    else
        s->event_running = false;
}

static void stop_event_thread(struct consumer_state *s)
{
    /* Signal only -- do NOT join here. enter_fallback()->on_fallback() can execute on
     * the event thread itself, so joining would self-deadlock. The handle stays in
     * event_thread (event_thread_joinable) and is reaped at create time by the next
     * start_event_thread() (or do_connect's reconnect path, both on the render
     * thread). */
    s->event_running = false;
}

/*
 * "Connect with root" handshake. The app cannot connect() to a root-owned
 * daemon socket directly, so it listens on a bridge socket, launches the bundled
 * helper through `su -c`, and the helper (as root) connects to the daemon and
 * passes the connected fd back over the bridge. Returns the received fd (caller
 * owns it) or -1 on failure.
 */
static int recv_fd_via_root_helper(const char *daemon_sock,
                                   const char *helper_path,
                                   const char *bridge_path)
{
    if (helper_path[0] == '\0' || bridge_path[0] == '\0') {
        LOGE("root helper: helper/bridge path not configured");
        return -1;
    }

    unlink(bridge_path);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) {
        LOGE("root helper: socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, bridge_path, sizeof(addr.sun_path) - 1);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("root helper: bind(%s) failed: %s", bridge_path, strerror(errno));
        close(lfd);
        return -1;
    }
    /* Root helper runs in a different SELinux/uid context; make the socket file
     * reachable. (Root bypasses DAC, but be permissive anyway.) */
    chmod(bridge_path, 0777);

    if (listen(lfd, 1) < 0) {
        LOGE("root helper: listen() failed: %s", strerror(errno));
        close(lfd);
        unlink(bridge_path);
        return -1;
    }

    /* Build the command su runs: "<helper> <daemon_sock> <bridge_path>". */
    char inner[1100];
    snprintf(inner, sizeof(inner), "%s %s %s",
             helper_path, daemon_sock, bridge_path);

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("root helper: fork() failed: %s", strerror(errno));
        close(lfd);
        unlink(bridge_path);
        return -1;
    }
    if (pid == 0) {
        execlp("su", "su", "-c", inner, (char *)NULL);
        _exit(127);   /* su not found / exec failed */
    }

    /* Wait for the helper to connect (root prompt may take a while). */
    int fd = -1;
    struct pollfd pfd = { .fd = lfd, .events = POLLIN };
    if (poll(&pfd, 1, 30000) > 0 && (pfd.revents & POLLIN)) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd >= 0) {
            char b;
            int got = 0;
            if (recv_fds(cfd, &b, 1, &fd, 1, &got) < 0 || got < 1)
                fd = -1;
            close(cfd);
        }
    } else {
        LOGE("root helper: timed out waiting for helper connection");
    }

    int status = 0;
    waitpid(pid, &status, 0);
    close(lfd);
    unlink(bridge_path);

    if (fd < 0)
        LOGE("root helper: did not receive daemon fd (su status=%d)", status);
    return fd;
}

/* ============================================================
 * Immersive full-input grab. libinputgrab.so (root, launched via su) takes an
 * exclusive EVIOCGRAB on the touchscreen + key devices, so Android sees NO input
 * at all -- home/back/recents gestures, the notification shade, and every key are
 * dead -- and streams the raw evdev triples to us over a bridge socket. We rebuild
 * multitouch (MT-slot) and keys and push_input_event() them to the producer, the
 * same path the normal touch/key JNI uses. The user's bound toggle key releases it,
 * detected by the helper itself. The grab also auto-releases when we close the socket
 * or the app dies (the grab lives on the fds the helper holds -- see input_grab.h).
 * ============================================================ */
#define GEV_SYN             0
#define GEV_KEY             1
#define GEV_ABS             3
#define GSYN_REPORT         0
#define GSYN_DROPPED        3     /* helper dropped records: resync, don't trust state */
#define GABS_MT_SLOT        0x2f
#define GABS_MT_POSITION_X  0x35
#define GABS_MT_POSITION_Y  0x36
#define GABS_MT_TRACKING_ID 0x39
#define GRAB_MAX_SLOTS      10
#define GRAB_KEY_CNT        768   /* KEY_CNT: track held keys for synth-release */

struct grab_slot { int active, down, up, dirty; int32_t x, y; };
struct grab_devstate {
    int is_touch;
    int32_t x_min, x_max, y_min, y_max;
    int cur_slot;
    struct grab_slot slots[GRAB_MAX_SLOTS];
};

/* Post to Java that the helper released the grab of its own accord. The Java side
 * MUST handle it asynchronously (runOnUiThread) -- it calls back into
 * nativeStopInputGrab, and doing that synchronously here (on the grab thread) would
 * self-join-deadlock. */
static void grab_notify_released(struct consumer_state *s)
{
    if (!g_jvm || !s->activity_obj)
        return;
    JNIEnv *env = NULL;
    bool att = false;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED)
        att = ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == 0);
    if (env) {
        jclass c = (*env)->GetObjectClass(env, s->activity_obj);
        jmethodID m = (*env)->GetMethodID(env, c, "onImmersionReleased", "()V");
        if (m)
            (*env)->CallVoidMethod(env, s->activity_obj, m);
    }
    if (att)
        (*g_jvm)->DetachCurrentThread(g_jvm);
}

static void grab_push_key(struct consumer_state *s, int code, int down)
{
    display_ctx *ctx = s->ctx;
    if (!ctx)
        return;
    struct InputEvent ev = { .type = INPUT_TYPE_KEY,
        .key = { .action = down ? INPUT_ACTION_DOWN : INPUT_ACTION_UP, .keycode = code } };
    push_input_event(ctx, &ev);
}

/* Raw axis value -> [0,1] over its reported ABS range. */
static float grab_norm(int32_t v, int32_t lo, int32_t hi)
{
    if (hi <= lo)
        return 0.f;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (float)(v - lo) / (float)(hi - lo);
}

/*
 * Map a raw panel touch to the producer's output-pixel space, applying the
 * display rotation. We bypass Android, so raw coords are in the panel's NATURAL
 * (unrotated) orientation; the producer expects surface-space coords, which are
 * rotated by grab_rotation (Surface.ROTATION_0/90/180/270 -> 0/1/2/3). Standard
 * rotation of normalized (u,v):
 *   0:(u,v)  90:(v,1-u)  180:(1-u,1-v)  270:(1-v,u)
 */
static void grab_push_touch(struct consumer_state *s, int action,
                            int32_t rx, int32_t ry, struct grab_devstate *d, int id)
{
    display_ctx *ctx = s->ctx;
    if (!ctx)
        return;
    float u = grab_norm(rx, d->x_min, d->x_max);
    float v = grab_norm(ry, d->y_min, d->y_max);
    float nx, ny;
    switch (s->grab_rotation) {
    case 1:  nx = v;         ny = 1.f - u;   break;   /* ROTATION_90  */
    case 2:  nx = 1.f - u;   ny = 1.f - v;   break;   /* ROTATION_180 */
    case 3:  nx = 1.f - v;   ny = u;         break;   /* ROTATION_270 */
    default: nx = u;         ny = v;         break;   /* ROTATION_0   */
    }
    struct InputEvent ev = { .type = INPUT_TYPE_TOUCH,
        .touch = { .action = action,
                   .x = nx * (float)s->screen_w,
                   .y = ny * (float)s->screen_h,
                   .pointer_id = id } };
    push_input_event(ctx, &ev);
}

/* On SYN_REPORT: emit one touch event per changed slot, then one frame. */
static void grab_flush_touch(struct consumer_state *s, struct grab_devstate *d)
{
    int emitted = 0;
    for (int i = 0; i < GRAB_MAX_SLOTS; i++) {
        struct grab_slot *sl = &d->slots[i];
        if (!sl->dirty)
            continue;
        int action = sl->up ? INPUT_ACTION_UP
                            : (sl->down ? INPUT_ACTION_DOWN : INPUT_ACTION_MOVE);
        grab_push_touch(s, action, sl->x, sl->y, d, i);
        emitted = 1;
        if (sl->up)
            sl->active = 0;
        sl->down = sl->up = sl->dirty = 0;
    }
    if (emitted && s->ctx) {
        struct InputEvent ev = { .type = INPUT_TYPE_TOUCH_FRAME };
        push_input_event(s->ctx, &ev);
    }
}

/* Lift every held key and finger. Used on teardown and on SYN_DROPPED (the helper
 * had to drop records, so our slot/key state may describe presses that already
 * ended) -- either way the producer must never be left with something stuck down. */
static void grab_release_all(struct consumer_state *s, struct grab_devstate *devs,
                             unsigned char *keydown)
{
    for (int c = 0; c < GRAB_KEY_CNT; c++) {
        if (keydown[c]) {
            grab_push_key(s, c, 0);
            keydown[c] = 0;
        }
    }
    for (int i = 0; i < IGRAB_MAX_DEVICES; i++) {
        if (!devs[i].is_touch)
            continue;
        int any = 0;
        for (int k = 0; k < GRAB_MAX_SLOTS; k++) {
            struct grab_slot *sl = &devs[i].slots[k];
            if (sl->active) {
                grab_push_touch(s, INPUT_ACTION_UP, sl->x, sl->y, &devs[i], k);
                any = 1;
            }
            memset(sl, 0, sizeof *sl);
        }
        devs[i].cur_slot = 0;
        if (any && s->ctx) {
            struct InputEvent ev = { .type = INPUT_TYPE_TOUCH_FRAME };
            push_input_event(s->ctx, &ev);
        }
    }
}

/* Reap an `su` child without ever blocking indefinitely. The root manager's grant
 * prompt can sit unanswered for as long as the user ignores it, and anything joining
 * this thread would block for exactly that long. Killing the su *client* is safe: if
 * the grant arrives later the helper cannot connect (the bridge is already unlinked)
 * so it ungrabs and exits, and the next session's pkill sweeps any leftover. */
static void reap_su(pid_t pid, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (waitpid(pid, NULL, WNOHANG) != 0)
            return;
        usleep(10000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static void *grab_thread_func(void *arg)
{
    struct consumer_state *s = arg;
    LOGI("grab thread started");

    pthread_mutex_lock(&s->cfg_lock);
    char helper[sizeof(s->grab_helper_path)];
    char bridge[sizeof(s->grab_bridge_path)];
    memcpy(helper, s->grab_helper_path, sizeof helper);
    memcpy(bridge, s->grab_bridge_path, sizeof bridge);
    pthread_mutex_unlock(&s->cfg_lock);
    if (helper[0] == '\0' || bridge[0] == '\0') {
        LOGE("grab: helper/bridge path unset");
        return NULL;
    }

    /* Kill any leaked helper from a previous session before launching a new one.
     * A stale helper still holding the grab with no reader is exactly the lockout
     * we are preventing -- only ever let one exist. Runs as root via su so it can
     * signal the root helper; pkill never signals itself. */
    pid_t kpid = fork();
    if (kpid == 0) {
        execlp("su", "su", "-c", "pkill -f libinputgrab.so", (char *)NULL);
        _exit(127);
    }
    if (kpid > 0)
        reap_su(kpid, 3000);

    unlink(bridge);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) { LOGE("grab: socket: %s", strerror(errno)); return NULL; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, bridge, sizeof addr.sun_path - 1);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        LOGE("grab: bind(%s): %s", bridge, strerror(errno));
        close(lfd);
        return NULL;
    }
    chmod(bridge, 0777);
    if (listen(lfd, 1) < 0) {
        LOGE("grab: listen: %s", strerror(errno));
        close(lfd); unlink(bridge);
        return NULL;
    }

    char inner[1200];
    snprintf(inner, sizeof inner, "%s %s %d", helper, bridge, s->grab_exit_code);
    pid_t pid = fork();
    if (pid == 0) {
        execlp("su", "su", "-c", inner, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        LOGE("grab: fork: %s", strerror(errno));
        close(lfd); unlink(bridge);
        return NULL;
    }

    int cfd = -1;
    while (s->grab_running) {
        struct pollfd p = { .fd = lfd, .events = POLLIN };
        int r = poll(&p, 1, 200);
        if (r > 0 && (p.revents & POLLIN)) {
            cfd = accept(lfd, NULL, NULL);
            break;
        }
    }
    close(lfd);
    unlink(bridge);
    if (cfd < 0) {
        LOGI("grab: stopped before helper connected");
        reap_su(pid, 3000);
        return NULL;
    }

    struct igrab_hello hello;
    struct grab_devstate devs[IGRAB_MAX_DEVICES];
    memset(devs, 0, sizeof devs);
    if (recv_all(cfd, &hello, sizeof hello) < 0 || hello.magic != IGRAB_MAGIC) {
        LOGE("grab: bad hello");
        close(cfd);
        reap_su(pid, 3000);
        return NULL;
    }
    for (uint32_t i = 0; i < hello.dev_count; i++) {
        struct igrab_dev gd;
        if (recv_all(cfd, &gd, sizeof gd) < 0) { close(cfd); goto reap; }
        if (gd.index < (uint32_t)IGRAB_MAX_DEVICES) {
            struct grab_devstate *d = &devs[gd.index];
            d->is_touch = (gd.kind == IGRAB_KIND_TOUCH);
            d->x_min = gd.abs_x_min; d->x_max = gd.abs_x_max;
            d->y_min = gd.abs_y_min; d->y_max = gd.abs_y_max;
        }
    }
    LOGI("grab: streaming %u devices", hello.dev_count);

    unsigned char keydown[GRAB_KEY_CNT];
    memset(keydown, 0, sizeof keydown);

    while (s->grab_running) {
        struct pollfd p = { .fd = cfd, .events = POLLIN };
        int r = poll(&p, 1, 200);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) continue;
        if (p.revents & (POLLHUP | POLLERR)) break;
        if (!(p.revents & POLLIN)) continue;

        struct igrab_rec rec;
        if (recv_all(cfd, &rec, sizeof rec) < 0)
            break;
        struct grab_devstate *d = (rec.index < (uint32_t)IGRAB_MAX_DEVICES)
                                  ? &devs[rec.index] : NULL;

        /* The helper dropped records because we were not draining fast enough, so
         * a press/lift may be missing. Lift everything before trusting new input. */
        if (rec.type == GEV_SYN && rec.code == GSYN_DROPPED) {
            LOGI("grab: helper dropped records -> resync (releasing keys/fingers)");
            grab_release_all(s, devs, keydown);
            continue;
        }

        if (rec.type == GEV_KEY) {
            int code = rec.code, val = rec.value;
            if (val == 2)
                continue;               /* autorepeat: the compositor repeats */
            grab_push_key(s, code, val != 0);
            if (code >= 0 && code < GRAB_KEY_CNT)
                keydown[code] = (val != 0);
        } else if (d && d->is_touch && rec.type == GEV_ABS) {
            int slot = d->cur_slot;
            if (slot < 0 || slot >= GRAB_MAX_SLOTS) slot = 0;
            struct grab_slot *sl = &d->slots[slot];
            switch (rec.code) {
            case GABS_MT_SLOT:
                if (rec.value >= 0 && rec.value < GRAB_MAX_SLOTS)
                    d->cur_slot = rec.value;
                break;
            case GABS_MT_TRACKING_ID:
                if (rec.value == -1) { sl->up = 1; sl->dirty = 1; }
                else { sl->active = 1; sl->down = 1; sl->dirty = 1; }
                break;
            case GABS_MT_POSITION_X:
                sl->x = rec.value;
                if (!sl->down) sl->dirty = 1;
                break;
            case GABS_MT_POSITION_Y:
                sl->y = rec.value;
                if (!sl->down) sl->dirty = 1;
                break;
            }
        } else if (d && d->is_touch && rec.type == GEV_SYN && rec.code == GSYN_REPORT) {
            grab_flush_touch(s, d);
        }
    }

    /* Synthesize releases so the producer never sees a stuck key/finger. */
    grab_release_all(s, devs, keydown);
    close(cfd);
    /* Still "running" means the stream ended on the HELPER's initiative, not ours:
     * the user hit the exit key, the helper released after a sustained stall, or
     * it died. Either way the grab is gone, so tell Java to leave the immersed state --
     * otherwise its view of the world would be stuck on. (When we asked it to stop,
     * grab_running is already false and Java knows.) */
    if (s->grab_running)
        grab_notify_released(s);
reap:
    reap_su(pid, 3000);
    LOGI("grab thread stopped");
    return NULL;
}

static void join_grab_thread(struct consumer_state *s)
{
    if (s->grab_thread_joinable) {
        pthread_join(s->grab_thread, NULL);
        s->grab_thread_joinable = false;
    }
}

static void start_grab_thread(struct consumer_state *s)
{
    if (s->grab_running)
        return;
    join_grab_thread(s);              /* reap a previous stopped-but-unjoined one */
    s->grab_running = true;
    if (pthread_create(&s->grab_thread, NULL, grab_thread_func, s) == 0)
        s->grab_thread_joinable = true;
    else
        s->grab_running = false;
}

/* Signal only. The thread notices within one poll timeout (200 ms), synthesizes
 * releases, closes the socket (-> helper releases the grab), and exits. The join
 * happens from the JNI caller (a non-grab thread), never from the notify path. */
static void stop_grab_thread(struct consumer_state *s)
{
    s->grab_running = false;
}

static int do_connect(struct consumer_state *s)
{
    /* Snapshot the connection config for this attempt. */
    pthread_mutex_lock(&s->cfg_lock);
    bool use_root = s->cfg_use_root;
    char sock_path[sizeof(s->cfg_socket_path)];
    char helper_path[sizeof(s->cfg_helper_path)];
    char bridge_path[sizeof(s->cfg_bridge_path)];
    memcpy(sock_path, s->cfg_socket_path, sizeof(sock_path));
    memcpy(helper_path, s->cfg_helper_path, sizeof(helper_path));
    memcpy(bridge_path, s->cfg_bridge_path, sizeof(bridge_path));
    pthread_mutex_unlock(&s->cfg_lock);

    const char *sock = sock_path;

    if (s->ctx) {
        audio_set_ctx(s->audio, NULL);   /* detach audio before the old ctx (and its fd) dies */
        stop_event_thread(s);
        join_event_thread(s);
        disconnect(s->ctx);
        s->ctx = NULL;
    }
    cleanup_dmabufs(s);

    ANativeWindow *win = s->window;
    pthread_mutex_lock(&s->cfg_lock);
    int cw = s->cfg_custom_width;
    int ch = s->cfg_custom_height;
    pthread_mutex_unlock(&s->cfg_lock);

    if (cw > 0 && ch > 0) {
        s->screen_w = cw;
        s->screen_h = ch;
    } else {
       s->screen_w = ANativeWindow_getWidth(win);
       s->screen_h = ANativeWindow_getHeight(win);
    }

    /* dequeueBuffer needs the window connected to an API first (ANativeWindow_lock
     * did this internally). Disconnect first so reconnect is idempotent. */
    anw_api_disconnect(win, ANW_API_CPU);
    if (anw_api_connect(win, ANW_API_CPU) != 0) {
        LOGE("api_connect(CPU) failed");
        return -1;
    }

    ANativeWindow_setBuffersGeometry(win, s->screen_w, s->screen_h,
                                     AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);

    int min_undequeued = 0;
    api.query(win, ANATIVEWINDOW_QUERY_MIN_UNDEQUEUED_BUFFERS, &min_undequeued);
    int total = min_undequeued + 2;
    if (total > MAX_COLLECT_BUFS)
        total = MAX_COLLECT_BUFS;

    api.setBufferCount(win, total);

    s->buf_count = total;
    if (collect_dmabufs(s) < 0)
        return -1;

    LOGI("connecting to %s (%dx%d, %d bufs, root=%d)", sock,
         s->screen_w, s->screen_h, s->buf_count, use_root);

    if (use_root) {
        int ctrl_fd = recv_fd_via_root_helper(sock, helper_path, bridge_path);
        if (ctrl_fd < 0) {
            LOGE("root helper connect failed");
            return -1;
        }
        if (connect_to_deamon_with_fd(&s->ctx, ctrl_fd) < 0) {
            LOGE("connect_to_deamon_with_fd failed");
            return -1;
        }
    } else if (connect_to_deamon(&s->ctx, sock) < 0) {
        LOGE("connect_to_deamon failed");
        return -1;
    }

    set_screen_info(s->ctx, s->screen_w, s->screen_h,
                    PIXEL_FORMAT_RGBA_8888, s->refresh_mhz);
    push_dmabufs(s->ctx, s->dmabuf_fds, s->dmabuf_infos, s->buf_count);

    /* Register the camera service only when it was initialised (i.e. the user
     * enabled it in settings and granted CAMERA). The service_info lives in this
     * per-instance state (outlives the ctx) and carries userdata=s so the camera
     * layer knows which instance's client to serve. The producer drives it via
     * RESOURCES_REQUEST (handled on the event thread). */
    if (camera_service_is_ready()) {
        s->camera_svc.type = SERVICE_TYPE_CAMERA;
        s->camera_svc.allocate_resource = camera_allocate_resource;
        s->camera_svc.free_resource = camera_free_resource;
        s->camera_svc.userdata = s;
        allocate_services(s->ctx, &s->camera_svc, 1);
    }

    set_fallback_callback(s->ctx, on_fallback, s);
    set_exit_fallback_callback(s->ctx, on_exit_fallback, s);

    audio_set_ctx(s->audio, s->ctx);   /* audio fd is now live; threads pick it up via get_audio_fd */

    s->need_reconnect = false;
    LOGI("connected");
    return 0;
}

static void on_fallback(void *userdata)
{
    struct consumer_state *s = userdata;
    LOGI("fallback triggered");

    audio_set_ctx(s->audio, NULL);   /* the lib has closed the audio fd; stop touching it */

    /* Let the owning MainActivity probe the daemon socket and close the window if the
     * daemon is gone. onFallback() marshals itself to the UI thread on the Java side. */
    if (g_jvm && s->activity_obj) {
        JNIEnv *env = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == 0)
                attached = true;
        }
        if (env) {
            jclass cls = (*env)->GetObjectClass(env, s->activity_obj);
            jmethodID mid = (*env)->GetMethodID(env, cls, "onFallback", "()V");
            if (mid)
                (*env)->CallVoidMethod(env, s->activity_obj, mid);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }

    // Disable clip listener on Java side before stopping event thread
    if (g_jvm && s->clipboard_obj) {
        JNIEnv *env = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == 0)
                attached = true;
        }
        if (env) {
            jclass cls = (*env)->GetObjectClass(env, s->clipboard_obj);
            jmethodID mid = (*env)->GetMethodID(env, cls, "nativeClipListening", "(Z)V");
            if (mid)
                (*env)->CallVoidMethod(env, s->clipboard_obj, mid, JNI_FALSE);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }

    stop_event_thread(s);
}

static void on_exit_fallback(void *userdata)
{
    struct consumer_state *s = userdata;
    LOGI("exit fallback triggered");

    send_refresh_rate(s);

    JNIEnv *env = NULL;
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != 0) {
        LOGE("on_exit_fallback: AttachCurrentThread failed");
        return;
    }

    // Enable clip listener on Java side
    jclass cls = (*env)->GetObjectClass(env, s->clipboard_obj);
    jmethodID listenMid = (*env)->GetMethodID(env, cls, "nativeClipListening", "(Z)V");
    if (listenMid)
        (*env)->CallVoidMethod(env, s->clipboard_obj, listenMid, JNI_TRUE);

    start_event_thread(s);

    // Initial clipboard sync: read current system clipboard and send to producer
    jmethodID syncMethod = (*env)->GetMethodID(env, cls, "nativeClipboardSync", "()V");
    if (syncMethod)
        (*env)->CallVoidMethod(env, s->clipboard_obj, syncMethod);

    (*g_jvm)->DetachCurrentThread(g_jvm);
}

static void *render_thread_func(void *arg)
{
    struct consumer_state *s = arg;
    LOGI("render thread started");

    while (s->running) {
        if (s->need_reconnect) {
            LOGI("reconnecting...");
            if (do_connect(s) < 0) {
                usleep(500000);
                continue;
            }
        }

        ANativeWindowBuffer *anb = NULL;
        int acqfence = -1;
        if (api.dequeueBuffer(s->window, &anb, &acqfence) != 0 || !anb) {
            usleep(16000);
            continue;
        }
        /* Emulate ANativeWindow_lock: CPU-wait the acquire fence so the buffer is
         * already safe to write (SurfaceFlinger done reading the previous frame)
         * before we hand it to the producer. A sync_file fd signals POLLIN. */
        if (acqfence >= 0) {
            struct pollfd fpfd = { .fd = acqfence, .events = POLLIN };
            poll(&fpfd, 1, 1000);
            close(acqfence);
        }

        int idx = -1;
        for (int i = 0; i < s->buf_count; i++) {
            if (s->buf_anb[i] == anb) {
                idx = i;
                break;
            }
        }

        if (idx < 0) {
            api.queueBuffer(s->window, anb, -1);
            usleep(16000);
            continue;
        }

        if (select_dmabuf(s->ctx, idx) < 0) {
            api.queueBuffer(s->window, anb, -1);
            usleep(16000);
            continue;
        }

        /* The producer renders into the buffer and hands back a render-done fence
         * over data_fd (reverse). Queue with it so SurfaceFlinger waits GPU-side
         * before scanout -- this lets the producer submit before its GPU render
         * completes (no glFinish stall). rfence == -1 falls back to "ready now". */
        int rfence = refresh_done(s->ctx);
        api.queueBuffer(s->window, anb, rfence);
    }

    LOGI("render thread stopped");
    return NULL;
}

/* ---------- JNI ---------- */

static void copy_jstring(JNIEnv *env, jstring js, char *dst, size_t dstsz)
{
    if (!js) {
        dst[0] = '\0';
        return;
    }
    const char *s = (*env)->GetStringUTFChars(env, js, NULL);
    if (s) {
        strncpy(dst, s, dstsz - 1);
        dst[dstsz - 1] = '\0';
        (*env)->ReleaseStringUTFChars(env, js, s);
    } else {
        dst[0] = '\0';
    }
}

/* Every JNI entry point below takes a jlong handle -- the consumer_state* returned
 * by nativeCreate -- so multiple instances (windows) coexist in one process. */
#define STATE(h) ((struct consumer_state *)(uintptr_t)(h))

JNIEXPORT jlong JNICALL
Java_com_anland_consumer_Native_nativeCreate(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    struct consumer_state *s = calloc(1, sizeof(*s));
    if (!s)
        return 0;
    pthread_mutex_init(&s->lock, NULL);
    pthread_mutex_init(&s->cfg_lock, NULL);
    strncpy(s->cfg_socket_path, "/data/local/tmp/display_daemon.sock",
            sizeof(s->cfg_socket_path) - 1);
    s->audio = audio_create();
    LOGI("instance %p created", (void *)s);
    return (jlong)(uintptr_t)s;
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeDestroy(JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    /* Stop the transport (mirrors nativeStop), release the camera client + audio
     * bridge, then free. */
    stop_grab_thread(s);
    join_grab_thread(s);

    pthread_mutex_lock(&s->lock);
    if (s->running) {
        s->running = false;
        pthread_mutex_unlock(&s->lock);
        pthread_join(s->render_thread, NULL);
        pthread_mutex_lock(&s->lock);
    }
    if (s->ctx) {
        stop_event_thread(s);
        disconnect(s->ctx);
        s->ctx = NULL;
    }
    cleanup_dmabufs(s);
    if (s->window) {
        ANativeWindow_release(s->window);
        s->window = NULL;
    }
    pthread_mutex_unlock(&s->lock);

    audio_destroy(s->audio);
    s->audio = NULL;
    camera_release_client(s);   /* window gone: tear down its camera channels */

    if (s->clipboard_obj && g_jvm) {
        JNIEnv *e = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&e, JNI_VERSION_1_6) == JNI_EDETACHED)
            attached = ((*g_jvm)->AttachCurrentThread(g_jvm, &e, NULL) == 0);
        if (e) {
            (*e)->DeleteGlobalRef(e, s->clipboard_obj);
            if (s->activity_obj)
                (*e)->DeleteGlobalRef(e, s->activity_obj);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }
    s->activity_obj = NULL;
    pthread_mutex_destroy(&s->cfg_lock);
    pthread_mutex_destroy(&s->lock);
    LOGI("instance %p destroyed", (void *)s);
    free(s);
}

/* Mark this instance focused (real camera frames go to the focused instance; others
 * get blank frames). Called from Java on window focus gain. */
JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetFocused(
    JNIEnv *env, jclass clazz, jlong handle, jboolean focused)
{
    (void)env; (void)clazz;
    struct consumer_state *s = STATE(handle);
    if (s && focused)
        camera_set_focus(s);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeConfigure(
    JNIEnv *env, jclass clazz, jlong handle, jstring socketPath, jboolean useRoot,
    jstring helperPath, jstring bridgePath)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    pthread_mutex_lock(&s->cfg_lock);
    char tmp[sizeof(s->cfg_socket_path)];
    copy_jstring(env, socketPath, tmp, sizeof(tmp));
    if (tmp[0] != '\0')
        memcpy(s->cfg_socket_path, tmp, sizeof(s->cfg_socket_path));
    s->cfg_use_root = (useRoot == JNI_TRUE);
    copy_jstring(env, helperPath, s->cfg_helper_path, sizeof(s->cfg_helper_path));
    copy_jstring(env, bridgePath, s->cfg_bridge_path, sizeof(s->cfg_bridge_path));
    pthread_mutex_unlock(&s->cfg_lock);

    LOGI("configured: socket=%s root=%d helper=%s bridge=%s",
         s->cfg_socket_path, s->cfg_use_root, s->cfg_helper_path, s->cfg_bridge_path);
}

/* Start immersive full-input grab: launch libinputgrab.so via su and stream. The
 * paths are the extracted helper (nativeLibraryDir/libinputgrab.so) and a bridge
 * socket in the app's cache dir. Idempotent while already running. */
JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeStartInputGrab(
    JNIEnv *env, jclass clazz, jlong handle, jstring helperPath, jstring bridgePath,
    jint rotation, jint exitKeyCode)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    s->grab_rotation = rotation;
    s->grab_exit_code = exitKeyCode;
    pthread_mutex_lock(&s->cfg_lock);
    copy_jstring(env, helperPath, s->grab_helper_path, sizeof(s->grab_helper_path));
    copy_jstring(env, bridgePath, s->grab_bridge_path, sizeof(s->grab_bridge_path));
    pthread_mutex_unlock(&s->cfg_lock);
    start_grab_thread(s);
}

/* Update the touch rotation live (called on display rotation while grabbing). */
JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetGrabRotation(
    JNIEnv *env, jclass clazz, jlong handle, jint rotation)
{
    (void)env; (void)clazz;
    struct consumer_state *s = STATE(handle);
    if (s)
        s->grab_rotation = rotation;
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeStopInputGrab(
    JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz;
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    stop_grab_thread(s);
    join_grab_thread(s);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetCustomResolution(
    JNIEnv* env, jclass clazz, jlong handle, jint width, jint height)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    pthread_mutex_lock(&s->cfg_lock);
    s->cfg_custom_width = width;
    s->cfg_custom_height = height;
    pthread_mutex_unlock(&s->cfg_lock);
    LOGI("custom resolution: %dx%d", width, height);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeStart(
    JNIEnv *env, jclass clazz, jlong handle, jobject surface, jobject clipboardTarget,
    jobject activityTarget)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    if (!api_loaded) {
        if (anw_api_load(&api) < 0) {
            LOGE("failed to load ANativeWindow hidden API");
            return;
        }
        api_loaded = true;
    }

    pthread_mutex_lock(&s->lock);

    if (s->running) {
        s->running = false;
        pthread_mutex_unlock(&s->lock);
        pthread_join(s->render_thread, NULL);
        pthread_mutex_lock(&s->lock);
    }

    if (s->ctx) {
        disconnect(s->ctx);
        s->ctx = NULL;
    }
    s->motion_has_last = false;
    cleanup_dmabufs(s);

    if (s->window) {
        ANativeWindow_release(s->window);
        s->window = NULL;
    }

    s->window = ANativeWindow_fromSurface(env, surface);
    if (!s->window) {
        LOGE("ANativeWindow_fromSurface failed");
        pthread_mutex_unlock(&s->lock);
        return;
    }

    /* Save JVM (process-global) and this instance's clipboard callback target. */
    if (!g_jvm) {
        (*env)->GetJavaVM(env, &g_jvm);
    }
    if (s->clipboard_obj) {
        (*env)->DeleteGlobalRef(env, s->clipboard_obj);
    }
    /* Static natives have no `thiz`; the Java layer passes the object whose
     * nativeSetClipboardText / nativeClipListening / nativeClipboardSync the
     * event thread calls back into (the Clipboard instance). */
    s->clipboard_obj = (*env)->NewGlobalRef(env, clipboardTarget);

    /* Owning MainActivity for the fallback callback (see on_fallback). */
    if (s->activity_obj) {
        (*env)->DeleteGlobalRef(env, s->activity_obj);
    }
    s->activity_obj = activityTarget ? (*env)->NewGlobalRef(env, activityTarget) : NULL;

    s->running = true;
    s->need_reconnect = true;
    pthread_create(&s->render_thread, NULL, render_thread_func, s);

    /* Audio streams live independently of the connection; the render thread attaches
     * the fd via audio_set_ctx() once connected. */
    audio_start(s->audio);

    pthread_mutex_unlock(&s->lock);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeStop(
    JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    /* Signal only -- never join here. nativeStop is called from onPause /
     * surfaceChanged / surfaceDestroyed on the MAIN thread, and the grab thread can
     * sit for seconds in waitpid() on an un-granted `su` prompt; joining it here is
     * the ANR we already hit. The join is deferred to start_grab_thread() and
     * nativeStopInputGrab(), which run on the app's grab executor. */
    stop_grab_thread(s);

    pthread_mutex_lock(&s->lock);

    if (s->running) {
        s->running = false;
        pthread_mutex_unlock(&s->lock);
        pthread_join(s->render_thread, NULL);
        pthread_mutex_lock(&s->lock);
    }

    /* Stop audio before the ctx (and its fd) is torn down. */
    audio_set_ctx(s->audio, NULL);
    audio_stop(s->audio);

    if (s->ctx) {
        stop_event_thread(s);
        join_event_thread(s);
        disconnect(s->ctx);
        s->ctx = NULL;
    }

    // Disable clip listener on Java side
    if (g_jvm && s->clipboard_obj) {
        JNIEnv *env2 = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env2, JNI_VERSION_1_6) == JNI_EDETACHED) {
            if ((*g_jvm)->AttachCurrentThread(g_jvm, &env2, NULL) == 0)
                attached = true;
        }
        if (env2) {
            jclass cls = (*env2)->GetObjectClass(env2, s->clipboard_obj);
            jmethodID mid = (*env2)->GetMethodID(env2, cls, "nativeClipListening", "(Z)V");
            if (mid)
                (*env2)->CallVoidMethod(env2, s->clipboard_obj, mid, JNI_FALSE);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }

    cleanup_dmabufs(s);

    if (s->window) {
        ANativeWindow_release(s->window);
        s->window = NULL;
    }

    pthread_mutex_unlock(&s->lock);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetRefreshRate(
    JNIEnv *env, jclass clazz, jlong handle, jfloat hz)
{
    struct consumer_state *s = STATE(handle);
    if (!s || hz <= 0.0f)
        return;
    s->refresh_mhz = (uint32_t)(hz * 1000.0f + 0.5f);
    // Apply live if already connected; otherwise do_connect() seeds it.
    send_refresh_rate(s);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTouch(
    JNIEnv *env, jclass clazz, jlong handle, jint action, jfloat x, jfloat y, jint pointer_id)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_TOUCH,
        .touch = { .action = action, .x = x, .y = y, .pointer_id = pointer_id },
    };
    push_input_event(s->ctx, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTouchFrame(
    JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_TOUCH_FRAME,
    };
    push_input_event(s->ctx, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendKey(
    JNIEnv *env, jclass clazz, jlong handle, jint action, jint keycode)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_KEY,
        .key = { .action = action, .keycode = keycode },
    };
    push_input_event(s->ctx, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseMotion(
    JNIEnv *env, jclass clazz, jlong handle, jfloat x, jfloat y, jfloat dx, jfloat dy)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;

    if (dx == 0.0f && dy == 0.0f && s->motion_has_last) {
        dx = x - s->motion_last_x;
        dy = y - s->motion_last_y;
    }

    s->motion_last_x = x;
    s->motion_last_y = y;
    s->motion_has_last = true;

    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_MOTION,
        .pointer_motion = { .x = x, .y = y, .dx = dx, .dy = dy },
    };
    push_input_event(s->ctx, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseButton(
    JNIEnv *env, jclass clazz, jlong handle, jint button, jboolean pressed)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_BUTTON,
        .pointer_button = { .button = button, .pressed = pressed ? 1 : 0 },
    };
    push_input_event(s->ctx, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseScroll(
    JNIEnv *env, jclass clazz, jlong handle, jint axis, jfloat value)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_AXIS,
        .pointer_axis = { .axis = axis, .value = value, .discrete = 0 },
    };
    push_input_event(s->ctx, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendClipboard(
    JNIEnv *env, jclass clazz, jlong handle, jbyteArray data)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;

    jsize len = (*env)->GetArrayLength(env, data);
    if (len <= 0)
        return;

    char *buf = malloc(len);
    if (!buf)
        return;
    (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte *)buf);

    struct InputEvent ev = {
        .type = INPUT_TYPE_CLIPBOARD,
        .clipboard = { .size = (uint32_t)len },
    };
    push_input_event_with_length(s->ctx, &ev, buf, len);
    free(buf);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTextInput(
    JNIEnv *env, jclass clazz, jlong handle, jbyteArray data)
{
    struct consumer_state *s = STATE(handle);
    if (!s || !s->ctx)
        return;

    jsize len = (*env)->GetArrayLength(env, data);
    if (len <= 0)
        return;

    char *buf = malloc(len);
    if (!buf)
        return;
    (*env)->GetByteArrayRegion(env, data, 0, len, (jbyte *)buf);

    struct InputEvent ev = {
        .type = INPUT_TYPE_TEXT_INPUT,
        .text_input = { .size = (uint32_t)len },
    };
    push_input_event_with_length(s->ctx, &ev, buf, len);
    free(buf);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetMicEnabled(
    JNIEnv *env, jclass clazz, jlong handle, jboolean enabled)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    audio_set_mic_enabled(s->audio, enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetAudioLatency(
    JNIEnv *env, jclass clazz, jlong handle, jint speakerMs, jint micMs)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    audio_set_latency(s->audio, speakerMs, micMs);
}
