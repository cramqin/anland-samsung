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
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
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
    /* Serialises publication/detach of ctx with hot JNI/grab sends. Java's RW lock
     * covers public lifecycle calls, but the render thread performs do_connect()
     * independently, so C needs its own pointer-lifetime fence. */
    pthread_mutex_t ctx_call_lock;
    ANativeWindow *window;
    display_ctx *ctx;
    pthread_t render_thread;
    atomic_bool running;

    //Note: it is Deamon's Reconnect, not Fallback Flag
    //Fallback is maintained by display lib, and the consumer should not care about it.
    atomic_bool need_reconnect;

    int buf_count;
    int dmabuf_fds[MAX_COLLECT_BUFS];
    struct buf_info dmabuf_infos[MAX_COLLECT_BUFS];
    ANativeWindowBuffer *buf_anb[MAX_COLLECT_BUFS];

    int screen_w;
    int screen_h;

    // Latest display refresh rate (milli-Hz) reported from Java. Read on
    // (re)connect to seed the producer; updated live by nativeSetRefreshRate.
    atomic_uint refresh_mhz;

    // Event (output) thread
    pthread_t event_thread;
    atomic_bool event_running;
    atomic_bool event_thread_alive;
    /* True while event_thread holds a started-but-not-yet-joined thread. stop only
     * signals (never joins -- on_fallback can run ON the event thread); the join is
     * deferred to the next start_event_thread() (create time), which runs on the
     * render thread and so cannot self-join. */
    bool event_thread_joinable;

    /* Immersive full-input grab: libinputgrab.so (root, via su) EVIOCGRABs the
     * touchscreen + key devices and streams raw evdev here over a bridge socket;
     * we translate and push_input_event(). See input_grab.h and grab_thread_func. */
    pthread_t grab_thread;
    /* Serialises public grab start/stop with transport ctx/ref teardown. The finer
     * grab_lock below protects the pthread handle/join state itself. */
    pthread_mutex_t grab_lifecycle_lock;
    pthread_mutex_t grab_lock;
    pthread_cond_t grab_cond;
    bool grab_thread_joinable;
    bool grab_join_in_progress;
    atomic_bool grab_stop_requested;
    atomic_int grab_forced_reason;
    atomic_uint_fast64_t grab_live_session;
    uint64_t grab_session_id;
    bool grab_touchpad_mode;
    int grab_cancel_fd;
    char grab_helper_path[512];
    char grab_bridge_path[512];
    /* Display rotation for the grab touch transform: Surface.ROTATION_* (0..3).
     * Set from Java at start and updated live on rotation. */
    atomic_int grab_rotation;
    /* evdev key that toggles immersion. Passed through to the helper, which
     * detects it root-side so the escape works even if we are wedged. */
    int grab_exit_code;

    /* The display transport is not usable merely because nativeStart returned: the
     * render thread still has to finish the producer handshake and leave fallback.
     * A grab thread waits on this condition before it is allowed to launch the root
     * helper, otherwise EVIOCGRAB could take input while every event is discarded into
     * a NULL/fallback ctx. The ctx itself is never destroyed until the grab thread has
     * been stopped and joined. */
    pthread_mutex_t transport_lock;
    pthread_cond_t transport_cond;
    atomic_bool transport_ready;

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

    /* buf_count is an ownership count, never a requested count. Clearing it before
     * enumeration prevents a partial failure/retry from making cleanup_dmabufs()
     * close calloc-initialised slot 0 (stdin) or other unowned descriptors. */
    s->buf_count = 0;
    for (int i = 0; i < target; i++) {
        s->dmabuf_fds[i] = -1;
        s->buf_anb[i] = NULL;
    }

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
            s->buf_anb[i] = NULL;
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

static display_ctx *detach_display_ctx(struct consumer_state *s)
{
    pthread_mutex_lock(&s->ctx_call_lock);
    display_ctx *ctx = s->ctx;
    s->ctx = NULL;
    pthread_mutex_unlock(&s->ctx_call_lock);
    return ctx;
}

static void publish_display_ctx(struct consumer_state *s, display_ctx *ctx)
{
    pthread_mutex_lock(&s->ctx_call_lock);
    s->ctx = ctx;
    pthread_mutex_unlock(&s->ctx_call_lock);
}

/* Report the current display refresh rate to the producer over the data
 * channel, reusing the InputEvent framing (see INPUT_TYPE_DISPLAY_REFRESH).
 * No-op when disconnected or rate unknown. */
static void send_refresh_rate(struct consumer_state *s)
{
    if (s->refresh_mhz == 0)
        return;
    pthread_mutex_lock(&s->ctx_call_lock);
    display_ctx *ctx = s->ctx;
    if (!ctx) {
        pthread_mutex_unlock(&s->ctx_call_lock);
        return;
    }
    struct InputEvent ev = {
        .type = INPUT_TYPE_DISPLAY_REFRESH,
        .display = { .refresh_mhz = s->refresh_mhz },
    };
    push_input_event(ctx, &ev);
    pthread_mutex_unlock(&s->ctx_call_lock);
}

static int try_push_hot_event(struct consumer_state *s, const struct InputEvent *event)
{
    if (pthread_mutex_trylock(&s->ctx_call_lock) != 0)
        return 0;
    display_ctx *ctx = s->ctx;
    int result = ctx ? push_input_event(ctx, event) : 0;
    pthread_mutex_unlock(&s->ctx_call_lock);
    return result;
}

static int try_push_hot_event_with_length(struct consumer_state *s,
                                          const struct InputEvent *event,
                                          void *payload, size_t size)
{
    if (pthread_mutex_trylock(&s->ctx_call_lock) != 0)
        return 0;
    display_ctx *ctx = s->ctx;
    int result = ctx ? push_input_event_with_length(ctx, event, payload, size) : 0;
    pthread_mutex_unlock(&s->ctx_call_lock);
    return result;
}

/*
 * Event thread: listens for output events (clipboard, etc.) from the producer
 * on the data_fd. Runs while s->event_running is true.
 */
static void *event_thread_func(void *arg)
{
    struct consumer_state *s = arg;
    atomic_store_explicit(&s->event_thread_alive, true, memory_order_release);
    LOGI("event thread started");

    JNIEnv *env = NULL;
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != 0) {
        LOGE("event thread: AttachCurrentThread failed");
        atomic_store_explicit(&s->event_running, false, memory_order_release);
        atomic_store_explicit(&s->event_thread_alive, false, memory_order_release);
        return NULL;
    }

    /* Find classes/methods once */
    jclass ctxClass = (*env)->GetObjectClass(env, s->clipboard_obj);
    jmethodID setClipMethod = (*env)->GetMethodID(env, ctxClass, "nativeSetClipboardText", "(Ljava/lang/String;)V");
    if (!setClipMethod) {
        LOGE("event thread: nativeSetClipboardText not found");
        atomic_store_explicit(&s->event_running, false, memory_order_release);
        atomic_store_explicit(&s->event_thread_alive, false, memory_order_release);
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

    atomic_store_explicit(&s->event_running, false, memory_order_release);
    atomic_store_explicit(&s->event_thread_alive, false, memory_order_release);
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

/* Teardown callers are non-event threads. Wake a reader that made it past poll() and
 * is blocked in the display library's fixed/extended recv_all before joining; freeing
 * ctx first would be a UAF, while signal-only could leave nativeDestroy stuck forever
 * on a peer that sent a partial clipboard payload. */
static void stop_and_join_event_thread(struct consumer_state *s)
{
    stop_event_thread(s);
    /* The ordinary poll loop has a 500 ms timeout. Give it that clean path first so a
     * normal lifecycle stop does not manufacture a fallback callback; only force the
     * socket awake if it is still inside an extended/partial read afterwards. */
    for (int waited = 0; waited < 600 &&
         atomic_load_explicit(&s->event_thread_alive, memory_order_acquire); waited += 10)
        usleep(10000);
    if (atomic_load_explicit(&s->event_thread_alive, memory_order_acquire) && s->ctx) {
        int fd = get_data_fd(s->ctx);
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
    }
    join_event_thread(s);
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
#define GABS_X              0x00
#define GABS_Y              0x01
#define GABS_MT_SLOT        0x2f
#define GABS_MT_POSITION_X  0x35
#define GABS_MT_POSITION_Y  0x36
#define GABS_MT_TRACKING_ID 0x39
#define GBTN_TOUCH          0x14a
#define GRAB_KEY_CNT        768   /* KEY_CNT: track held keys for synth-release */
#define GRAB_TRANSPORT_WAIT_MS 10000
#define GRAB_HELPER_WAIT_MS    30000
#define GRAB_HELLO_WAIT_MS      5000
#define GRAB_FRAME_WAIT_MS      2000
#define GRAB_TOUCH_ACTION_CANCEL 3

struct grab_slot { int active, down, up, dirty; int32_t x, y; };
struct grab_devstate {
    int present;
    uint32_t kind;
    int is_touch;
    int desynced;
    int index;
    int32_t x_min, x_max, y_min, y_max;
    int32_t slot_min, slot_max;
    int slot_count;
    int cur_slot;
    struct grab_slot slots[IGRAB_MAX_SLOTS];
};

struct grab_keystate {
    unsigned char down[IGRAB_MAX_DEVICES][GRAB_KEY_CNT];
    uint16_t refs[GRAB_KEY_CNT];
};

enum grab_callback_kind {
    GRAB_CALLBACK_STARTED,
    GRAB_CALLBACK_FAILED,
    GRAB_CALLBACK_RELEASED,
};

enum grab_recv_result {
    GRAB_RECV_OK = 0,
    GRAB_RECV_IO = -1,
    GRAB_RECV_CANCELLED = -2,
    GRAB_RECV_TIMEOUT = -3,
};

struct grab_touch_sink {
    JNIEnv *env;
    bool attached;
    jobject target;
    jclass target_class;
    jmethodID on_frame;
};

static bool grab_reason_valid(int reason)
{
    return reason >= 0 && (uint32_t)reason <= IGRAB_REASON_INPUT_BUSY;
}

/* Take a local JNI ref while holding the instance lock. nativeStart replaces the
 * activity global ref only after the old grab thread has joined, but the local ref
 * also makes this helper robust against future lifecycle changes. */
static void grab_notify_java(struct consumer_state *s, enum grab_callback_kind kind,
                             uint64_t session_id, int reason)
{
    if (!g_jvm)
        return;

    JNIEnv *env = NULL;
    bool attached = false;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED)
        attached = ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == 0);
    if (!env)
        return;

    jobject target = NULL;
    pthread_mutex_lock(&s->lock);
    if (s->activity_obj)
        target = (*env)->NewLocalRef(env, s->activity_obj);
    pthread_mutex_unlock(&s->lock);

    if (target) {
        const char *name;
        const char *sig;
        switch (kind) {
        case GRAB_CALLBACK_STARTED:
            name = "onImmersionStarted";
            sig = "(J)V";
            break;
        case GRAB_CALLBACK_FAILED:
            name = "onImmersionFailed";
            sig = "(JI)V";
            break;
        default:
            name = "onImmersionReleased";
            sig = "(JI)V";
            break;
        }

        jclass cls = (*env)->GetObjectClass(env, target);
        jmethodID method = cls ? (*env)->GetMethodID(env, cls, name, sig) : NULL;
        if (method) {
            if (kind == GRAB_CALLBACK_STARTED)
                (*env)->CallVoidMethod(env, target, method, (jlong)session_id);
            else
                (*env)->CallVoidMethod(env, target, method, (jlong)session_id,
                                       (jint)reason);
        } else {
            LOGE("grab: Java callback %s%s not found", name, sig);
        }
        if ((*env)->ExceptionCheck(env)) {
            LOGE("grab: Java callback %s threw", name);
            (*env)->ExceptionClear(env);
        }
        if (cls)
            (*env)->DeleteLocalRef(env, cls);
        (*env)->DeleteLocalRef(env, target);
    }

    if (attached)
        (*g_jvm)->DetachCurrentThread(g_jvm);
}

/* Touch frames can arrive at display refresh rate, so keep the grab pthread attached
 * for the session instead of Attach/DetachCurrentThread on every SYN_REPORT. The
 * activity global reference cannot be replaced until this thread is joined (the
 * public lifecycle is fenced by grab_lifecycle_lock); a local reference nevertheless
 * makes that lifetime explicit and keeps all JNI use on this thread. */
static bool grab_touch_sink_init(struct consumer_state *s,
                                 struct grab_touch_sink *sink)
{
    memset(sink, 0, sizeof *sink);
    if (!g_jvm)
        return false;

    jint state = (*g_jvm)->GetEnv(g_jvm, (void **)&sink->env, JNI_VERSION_1_6);
    if (state == JNI_EDETACHED) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &sink->env, NULL) != 0)
            return false;
        sink->attached = true;
    } else if (state != JNI_OK) {
        sink->env = NULL;
        return false;
    }

    pthread_mutex_lock(&s->lock);
    if (s->activity_obj)
        sink->target = (*sink->env)->NewLocalRef(sink->env, s->activity_obj);
    pthread_mutex_unlock(&s->lock);
    if (!sink->target)
        goto fail;

    sink->target_class = (*sink->env)->GetObjectClass(sink->env, sink->target);
    if (!sink->target_class)
        goto fail;
    sink->on_frame = (*sink->env)->GetMethodID(
        sink->env, sink->target_class, "onImmersiveTouchFrame", "(J[I[I[F[FJ)V");
    if (!sink->on_frame) {
        LOGE("grab: Java callback onImmersiveTouchFrame(J[I[I[F[FJ)V not found");
        goto fail;
    }
    return true;

fail:
    if ((*sink->env)->ExceptionCheck(sink->env))
        (*sink->env)->ExceptionClear(sink->env);
    if (sink->target_class)
        (*sink->env)->DeleteLocalRef(sink->env, sink->target_class);
    if (sink->target)
        (*sink->env)->DeleteLocalRef(sink->env, sink->target);
    if (sink->attached)
        (*g_jvm)->DetachCurrentThread(g_jvm);
    memset(sink, 0, sizeof *sink);
    return false;
}

static void grab_touch_sink_destroy(struct grab_touch_sink *sink)
{
    if (!sink->env)
        return;
    if (sink->target_class)
        (*sink->env)->DeleteLocalRef(sink->env, sink->target_class);
    if (sink->target)
        (*sink->env)->DeleteLocalRef(sink->env, sink->target);
    if (sink->attached && g_jvm)
        (*g_jvm)->DetachCurrentThread(g_jvm);
    memset(sink, 0, sizeof *sink);
}

static void set_transport_ready(struct consumer_state *s, bool ready)
{
    pthread_mutex_lock(&s->transport_lock);
    atomic_store_explicit(&s->transport_ready, ready, memory_order_release);
    pthread_cond_broadcast(&s->transport_cond);
    pthread_mutex_unlock(&s->transport_lock);
}

static void grab_signal_cancel(struct consumer_state *s)
{
    if (s->grab_cancel_fd >= 0) {
        eventfd_t one = 1;
        while (eventfd_write(s->grab_cancel_fd, one) < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN)
                LOGE("grab: eventfd_write(cancel): %s", strerror(errno));
            break;
        }
    }
    pthread_mutex_lock(&s->transport_lock);
    pthread_cond_broadcast(&s->transport_cond);
    pthread_mutex_unlock(&s->transport_lock);
}

static void request_grab_stop(struct consumer_state *s, int reason)
{
    if (reason != IGRAB_REASON_NONE && grab_reason_valid(reason)) {
        int expected = IGRAB_REASON_NONE;
        atomic_compare_exchange_strong(&s->grab_forced_reason, &expected, reason);
    }
    atomic_store_explicit(&s->grab_stop_requested, true, memory_order_release);
    grab_signal_cancel(s);
}

static void stop_grab_thread(struct consumer_state *s)
{
    request_grab_stop(s, IGRAB_REASON_NONE);
}

static void grab_drain_cancel(struct consumer_state *s)
{
    if (s->grab_cancel_fd < 0)
        return;
    eventfd_t value;
    for (;;) {
        if (eventfd_read(s->grab_cancel_fd, &value) == 0)
            continue;
        if (errno == EINTR)
            continue;
        break;
    }
}

static int grab_wait_transport(struct consumer_state *s)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += GRAB_TRANSPORT_WAIT_MS / 1000;
    deadline.tv_nsec += (long)(GRAB_TRANSPORT_WAIT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&s->transport_lock);
    int result = GRAB_RECV_OK;
    while (!atomic_load_explicit(&s->transport_ready, memory_order_acquire) &&
           !atomic_load_explicit(&s->grab_stop_requested, memory_order_acquire)) {
        int r = pthread_cond_timedwait(&s->transport_cond, &s->transport_lock,
                                       &deadline);
        if (r == ETIMEDOUT) {
            result = GRAB_RECV_TIMEOUT;
            break;
        }
    }
    if (atomic_load_explicit(&s->grab_stop_requested, memory_order_acquire))
        result = GRAB_RECV_CANCELLED;
    pthread_mutex_unlock(&s->transport_lock);
    return result;
}

static int64_t grab_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Read one fixed-size protocol item without ever becoming an uninterruptible
 * recv_all(). Waiting for the first byte may be infinite for the event stream, but
 * the cancel eventfd always wakes it; once a frame has begun, the peer has a strict
 * deadline to finish it so stop/destroy can always join. */
static int grab_recv_exact(struct consumer_state *s, int fd, void *buf, size_t len,
                           int first_timeout_ms, int partial_timeout_ms)
{
    size_t got = 0;
    int64_t deadline = first_timeout_ms >= 0 ? grab_now_ms() + first_timeout_ms : -1;
    while (got < len) {
        if (atomic_load_explicit(&s->grab_stop_requested, memory_order_acquire))
            return GRAB_RECV_CANCELLED;

        int timeout = -1;
        if (deadline >= 0) {
            int64_t left = deadline - grab_now_ms();
            if (left <= 0)
                return GRAB_RECV_TIMEOUT;
            timeout = left > INT32_MAX ? INT32_MAX : (int)left;
        }

        struct pollfd pfd[2] = {
            { .fd = fd, .events = POLLIN },
            { .fd = s->grab_cancel_fd, .events = POLLIN },
        };
        int r = poll(pfd, s->grab_cancel_fd >= 0 ? 2 : 1, timeout);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return GRAB_RECV_IO;
        }
        if (r == 0)
            return GRAB_RECV_TIMEOUT;
        if (s->grab_cancel_fd >= 0 && (pfd[1].revents & POLLIN))
            return GRAB_RECV_CANCELLED;
        if (!(pfd[0].revents & POLLIN)) {
            if (pfd[0].revents & (POLLHUP | POLLERR | POLLNVAL))
                return GRAB_RECV_IO;
            continue;
        }

        ssize_t n = recv(fd, (char *)buf + got, len - got, MSG_DONTWAIT);
        if (n > 0) {
            got += (size_t)n;
            if (got < len)
                deadline = grab_now_ms() + partial_timeout_ms;
            continue;
        }
        if (n == 0)
            return GRAB_RECV_IO;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        return GRAB_RECV_IO;
    }
    return GRAB_RECV_OK;
}

/* Deliver exactly one Java callback for one evdev SYN_REPORT. Arrays contain only
 * slots changed by that report; Java owns the session's active-pointer table and can
 * therefore turn simultaneous multi-finger changes into correctly ordered
 * MotionEvents on its main thread. Coordinates are rotated normalized values. */
static int grab_notify_touch_frame(struct consumer_state *s,
                                   struct grab_touch_sink *sink,
                                   uint64_t session_id, const jint *pointer_ids,
                                   const jint *actions, const jfloat *xs,
                                   const jfloat *ys, int count,
                                   int64_t event_time_ms)
{
    if (!sink || !sink->env || !sink->target || !sink->on_frame || count <= 0)
        return -1;
    if (atomic_load_explicit(&s->grab_live_session, memory_order_acquire) != session_id)
        return -1;

    JNIEnv *env = sink->env;
    jintArray jids = (*env)->NewIntArray(env, count);
    jintArray jactions = (*env)->NewIntArray(env, count);
    jfloatArray jxs = (*env)->NewFloatArray(env, count);
    jfloatArray jys = (*env)->NewFloatArray(env, count);
    bool failed = !jids || !jactions || !jxs || !jys;
    if (!failed) {
        (*env)->SetIntArrayRegion(env, jids, 0, count, pointer_ids);
        (*env)->SetIntArrayRegion(env, jactions, 0, count, actions);
        (*env)->SetFloatArrayRegion(env, jxs, 0, count, xs);
        (*env)->SetFloatArrayRegion(env, jys, 0, count, ys);
        failed = (*env)->ExceptionCheck(env);
    }
    if (!failed) {
        (*env)->CallVoidMethod(env, sink->target, sink->on_frame,
                               (jlong)session_id, jids, jactions, jxs, jys,
                               (jlong)event_time_ms);
        failed = (*env)->ExceptionCheck(env);
    }
    if (failed) {
        LOGE("grab: Java touch-frame callback failed");
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
    }
    if (jys)
        (*env)->DeleteLocalRef(env, jys);
    if (jxs)
        (*env)->DeleteLocalRef(env, jxs);
    if (jactions)
        (*env)->DeleteLocalRef(env, jactions);
    if (jids)
        (*env)->DeleteLocalRef(env, jids);

    if (failed) {
        request_grab_stop(s, IGRAB_REASON_STREAM_IO_ERROR);
        return -1;
    }
    return 0;
}

static int grab_push_event(struct consumer_state *s, const struct InputEvent *ev)
{
    if (!atomic_load_explicit(&s->transport_ready, memory_order_acquire))
        return -1;
    pthread_mutex_lock(&s->ctx_call_lock);
    display_ctx *ctx = s->ctx;
    int r = ctx ? push_input_event(ctx, ev) : -1;
    pthread_mutex_unlock(&s->ctx_call_lock);
    if (r < 0)
        request_grab_stop(s, IGRAB_REASON_STREAM_IO_ERROR);
    return r;
}

static int grab_push_key(struct consumer_state *s, int code, int down)
{
    struct InputEvent ev = { .type = INPUT_TYPE_KEY,
        .key = { .action = down ? INPUT_ACTION_DOWN : INPUT_ACTION_UP, .keycode = code } };
    return grab_push_event(s, &ev);
}

/* Raw axis value -> [0,1] over its reported ABS range. */
static float grab_norm(int32_t v, int32_t lo, int32_t hi)
{
    if (hi <= lo)
        return 0.f;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return ((float)v - (float)lo) / ((float)hi - (float)lo);
}

/* Raw panel coordinates are in NATURAL (unrotated) orientation because EVIOCGRAB
 * bypasses Android. Convert to rotated surface-space normalized coordinates once;
 * absolute immersion scales them to output pixels while touchpad immersion sends
 * the normalized values to Java's existing relative-gesture implementation. */
static void grab_transform_touch(struct consumer_state *s,
                                 const struct grab_devstate *d,
                                 int32_t rx, int32_t ry, jfloat *nx, jfloat *ny)
{
    float u = grab_norm(rx, d->x_min, d->x_max);
    float v = grab_norm(ry, d->y_min, d->y_max);
    switch (atomic_load_explicit(&s->grab_rotation, memory_order_relaxed)) {
    case 1:  *nx = v;         *ny = 1.f - u;   break;  /* ROTATION_90  */
    case 2:  *nx = 1.f - u;   *ny = 1.f - v;   break;  /* ROTATION_180 */
    case 3:  *nx = 1.f - v;   *ny = u;         break;  /* ROTATION_270 */
    default: *nx = u;         *ny = v;         break;  /* ROTATION_0   */
    }
}

static int grab_touch_pointer_id(const struct grab_devstate *d, int slot)
{
    return d->index * IGRAB_MAX_SLOTS + slot;
}

static int grab_push_touch(struct consumer_state *s, int action,
                           int32_t rx, int32_t ry, struct grab_devstate *d, int slot)
{
    jfloat nx, ny;
    grab_transform_touch(s, d, rx, ry, &nx, &ny);
    int pointer_id = d->index * IGRAB_MAX_SLOTS + slot;
    struct InputEvent ev = { .type = INPUT_TYPE_TOUCH,
        .touch = { .action = action,
                   .x = (float)nx * (float)s->screen_w,
                   .y = (float)ny * (float)s->screen_h,
                   .pointer_id = pointer_id } };
    return grab_push_event(s, &ev);
}

static int grab_push_touch_frame(struct consumer_state *s)
{
    struct InputEvent ev = { .type = INPUT_TYPE_TOUCH_FRAME };
    return grab_push_event(s, &ev);
}

/* On SYN_REPORT, absolute mode preserves the producer protocol exactly. In a
 * touchpad-mode session only Type-B finger touch is diverted: one Java callback
 * receives every changed slot in the frame. SINGLE_TOUCH (normally a pen/stylus)
 * intentionally remains absolute. */
static void grab_flush_touch(struct consumer_state *s, struct grab_devstate *d,
                             bool touchpad_mode, uint64_t session_id,
                             struct grab_touch_sink *sink)
{
    const bool to_java = touchpad_mode && d->kind == IGRAB_KIND_TOUCH;
    jint pointer_ids[IGRAB_MAX_SLOTS];
    jint actions[IGRAB_MAX_SLOTS];
    jfloat xs[IGRAB_MAX_SLOTS];
    jfloat ys[IGRAB_MAX_SLOTS];
    int emitted = 0;
    for (int i = 0; i < d->slot_count; i++) {
        struct grab_slot *sl = &d->slots[i];
        if (!sl->dirty)
            continue;
        int action = sl->up ? INPUT_ACTION_UP
                            : (sl->down ? INPUT_ACTION_DOWN : INPUT_ACTION_MOVE);
        if (to_java) {
            pointer_ids[emitted] = grab_touch_pointer_id(d, i);
            actions[emitted] = action;
            grab_transform_touch(s, d, sl->x, sl->y,
                                 &xs[emitted], &ys[emitted]);
        } else {
            grab_push_touch(s, action, sl->x, sl->y, d, i);
        }
        emitted++;
        if (sl->up)
            sl->active = 0;
        sl->down = sl->up = sl->dirty = 0;
    }
    if (emitted) {
        if (to_java) {
            if (grab_notify_touch_frame(s, sink, session_id, pointer_ids, actions,
                                        xs, ys, emitted, grab_now_ms()) < 0 &&
                atomic_load_explicit(&s->grab_live_session, memory_order_acquire) ==
                    session_id) {
                request_grab_stop(s, IGRAB_REASON_STREAM_IO_ERROR);
            }
        } else {
            grab_push_touch_frame(s);
        }
    }
}

static void grab_set_key(struct consumer_state *s, struct grab_keystate *keys,
                         int device_index, int code, bool down)
{
    if (device_index < 0 || device_index >= IGRAB_MAX_DEVICES ||
        code < 0 || code >= GRAB_KEY_CNT)
        return;
    unsigned char *held = &keys->down[device_index][code];
    if (down) {
        if (*held)
            return;
        *held = 1;
        if (keys->refs[code]++ == 0)
            grab_push_key(s, code, 1);
    } else {
        if (!*held)
            return;
        *held = 0;
        if (keys->refs[code] > 0 && --keys->refs[code] == 0)
            grab_push_key(s, code, 0);
    }
}

static void grab_release_device(struct consumer_state *s, struct grab_devstate *devs,
                                struct grab_keystate *keys, int device_index,
                                bool touchpad_mode, uint64_t session_id,
                                struct grab_touch_sink *sink)
{
    if (device_index < 0 || device_index >= IGRAB_MAX_DEVICES)
        return;
    struct grab_devstate *d = &devs[device_index];
    for (int c = 0; c < GRAB_KEY_CNT; c++) {
        if (keys->down[device_index][c])
            grab_set_key(s, keys, device_index, c, false);
    }
    if (!d->present || !d->is_touch)
        return;

    const bool cancel_java = touchpad_mode && d->kind == IGRAB_KIND_TOUCH;
    if (cancel_java) {
        jint pointer_ids[IGRAB_MAX_SLOTS];
        jint actions[IGRAB_MAX_SLOTS];
        jfloat xs[IGRAB_MAX_SLOTS];
        jfloat ys[IGRAB_MAX_SLOTS];
        int count = 0;
        for (int k = 0; k < d->slot_count; k++) {
            struct grab_slot *sl = &d->slots[k];
            if (sl->active) {
                pointer_ids[count] = grab_touch_pointer_id(d, k);
                actions[count] = GRAB_TOUCH_ACTION_CANCEL;
                grab_transform_touch(s, d, sl->x, sl->y, &xs[count], &ys[count]);
                count++;
            }
            memset(sl, 0, sizeof *sl);
        }
        if (count > 0 &&
            grab_notify_touch_frame(s, sink, session_id, pointer_ids, actions,
                                    xs, ys, count, grab_now_ms()) < 0 &&
            atomic_load_explicit(&s->grab_live_session, memory_order_acquire) ==
                session_id) {
            request_grab_stop(s, IGRAB_REASON_STREAM_IO_ERROR);
        }
    } else {
        int any = 0;
        for (int k = 0; k < d->slot_count; k++) {
            struct grab_slot *sl = &d->slots[k];
            if (sl->active) {
                grab_push_touch(s, INPUT_ACTION_UP, sl->x, sl->y, d, k);
                any = 1;
            }
            memset(sl, 0, sizeof *sl);
        }
        if (any)
            grab_push_touch_frame(s);
    }
    d->cur_slot = d->slot_count > 0 ? 0 : -1;
}

/* Lift every held key and finger. Used on teardown and when framing is lost so the
 * producer can never retain a press for which the matching release was dropped. */
static void grab_release_all(struct consumer_state *s, struct grab_devstate *devs,
                             struct grab_keystate *keys, bool touchpad_mode,
                             uint64_t session_id, struct grab_touch_sink *sink)
{
    for (int i = 0; i < IGRAB_MAX_DEVICES; i++) {
        if (devs[i].present)
            grab_release_device(s, devs, keys, i, touchpad_mode, session_id, sink);
    }
}

/* Reap an `su` child without ever blocking indefinitely. The root manager's grant
 * prompt can sit unanswered for as long as the user ignores it, and anything joining
 * this thread would block for exactly that long. Killing the su *client* is safe: if
 * the grant arrives later the helper cannot connect (the bridge is already unlinked)
 * so it ungrabs and exits, and the next session's pkill sweeps any leftover. */
static void reap_su(pid_t pid, int timeout_ms)
{
    if (pid <= 0)
        return;
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        pid_t r = waitpid(pid, NULL, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD))
            return;
        if (r < 0 && errno != EINTR) {
            LOGE("grab: waitpid(%d): %s", pid, strerror(errno));
            return;
        }
        usleep(10000);
    }
    /* The su client and helper inherit this private process group where supported.
     * Signal the group first so a child helper cannot outlive a killed su wrapper. */
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

static void *grab_thread_func(void *arg)
{
    struct consumer_state *s = arg;
    uint64_t session_id;
    char helper[sizeof(s->grab_helper_path)];
    char bridge[sizeof(s->grab_bridge_path)];
    int exit_code;
    bool touchpad_mode;

    pthread_mutex_lock(&s->grab_lock);
    session_id = s->grab_session_id;
    memcpy(helper, s->grab_helper_path, sizeof helper);
    memcpy(bridge, s->grab_bridge_path, sizeof bridge);
    exit_code = s->grab_exit_code;
    touchpad_mode = s->grab_touchpad_mode;
    pthread_mutex_unlock(&s->grab_lock);

    LOGI("grab thread started session=%llu touchpad=%d",
         (unsigned long long)session_id, touchpad_mode);

    int failure_reason = IGRAB_REASON_STARTUP_IO_ERROR;
    int release_reason = IGRAB_REASON_NONE;
    bool started = false;
    bool has_touchpad_device = false;
    int lfd = -1, cfd = -1;
    pid_t pid = -1;
    struct grab_touch_sink touch_sink;
    struct grab_devstate devs[IGRAB_MAX_DEVICES];
    struct grab_keystate keys;
    unsigned char mt_seed_expected[IGRAB_MAX_DEVICES];
    unsigned char mt_seed_seen[IGRAB_MAX_DEVICES];
    memset(devs, 0, sizeof devs);
    memset(&keys, 0, sizeof keys);
    memset(mt_seed_expected, 0, sizeof mt_seed_expected);
    memset(mt_seed_seen, 0, sizeof mt_seed_seen);
    memset(&touch_sink, 0, sizeof touch_sink);

    if (helper[0] == '\0' || bridge[0] == '\0') {
        LOGE("grab: helper/bridge path unset");
        goto done;
    }
    if (exit_code <= 0) {
        failure_reason = IGRAB_REASON_INVALID_EXIT_KEY;
        LOGE("grab: invalid exit key %d", exit_code);
        goto done;
    }

    int wr = grab_wait_transport(s);
    if (wr == GRAB_RECV_CANCELLED)
        goto done;
    if (wr != GRAB_RECV_OK) {
        failure_reason = IGRAB_REASON_PEER_CLOSED;
        LOGE("grab: display transport did not become ready");
        goto done;
    }
    if (!s->ctx || s->screen_w <= 0 || s->screen_h <= 0) {
        failure_reason = IGRAB_REASON_PEER_CLOSED;
        LOGE("grab: display transport has no usable output geometry");
        goto done;
    }

    /* Kill any leaked helper from a previous session before launching a new one.
     * A stale helper still holding the grab with no reader is exactly the lockout
     * we are preventing -- only ever let one exist. Runs as root via su so it can
     * signal the root helper; pkill never signals itself. */
    pid_t kpid = fork();
    if (kpid == 0) {
        setpgid(0, 0);
        execlp("su", "su", "-c", "pkill -f libinputgrab.so", (char *)NULL);
        _exit(127);
    }
    if (kpid > 0) {
        setpgid(kpid, kpid);
        reap_su(kpid, 3000);
    }

    /* A stop requested while the cleanup su prompt was pending must not launch a
     * brand-new helper after the user has already left immersion. */
    if (atomic_load_explicit(&s->grab_stop_requested, memory_order_acquire))
        goto done;

    unlink(bridge);
    lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) { LOGE("grab: socket: %s", strerror(errno)); goto done; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(bridge) >= sizeof addr.sun_path) {
        LOGE("grab: bridge path is too long");
        goto done;
    }
    strncpy(addr.sun_path, bridge, sizeof addr.sun_path - 1);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        LOGE("grab: bind(%s): %s", bridge, strerror(errno));
        goto done;
    }
    chmod(bridge, 0777);
    if (listen(lfd, 1) < 0) {
        LOGE("grab: listen: %s", strerror(errno));
        goto done;
    }

    char inner[1200];
    int cmd_len = snprintf(inner, sizeof inner, "%s %s %d", helper, bridge, exit_code);
    if (cmd_len < 0 || cmd_len >= (int)sizeof inner) {
        LOGE("grab: helper command is too long");
        goto done;
    }
    if (atomic_load_explicit(&s->grab_stop_requested, memory_order_acquire))
        goto done;

    pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        execlp("su", "su", "-c", inner, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        LOGE("grab: fork: %s", strerror(errno));
        goto done;
    }
    setpgid(pid, pid);

    int64_t connect_deadline = grab_now_ms() + GRAB_HELPER_WAIT_MS;
    while (!atomic_load_explicit(&s->grab_stop_requested, memory_order_acquire)) {
        int64_t left = connect_deadline - grab_now_ms();
        if (left <= 0)
            break;
        struct pollfd pfd[2] = {
            { .fd = lfd, .events = POLLIN },
            { .fd = s->grab_cancel_fd, .events = POLLIN },
        };
        int r = poll(pfd, s->grab_cancel_fd >= 0 ? 2 : 1,
                     left > 250 ? 250 : (int)left);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (s->grab_cancel_fd >= 0 && (pfd[1].revents & POLLIN))
            break;
        if (pfd[0].revents & POLLIN) {
            cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            if (cfd < 0 && errno == EINTR)
                continue;
            break;
        }
        if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
    }
    close(lfd);
    lfd = -1;
    unlink(bridge);
    if (cfd < 0) {
        if (!atomic_load_explicit(&s->grab_stop_requested, memory_order_acquire))
            LOGE("grab: helper did not connect");
        goto done;
    }

    struct igrab_hello hello;
    int rr = grab_recv_exact(s, cfd, &hello, sizeof hello, GRAB_HELLO_WAIT_MS,
                             GRAB_FRAME_WAIT_MS);
    if (rr != GRAB_RECV_OK) {
        if (rr != GRAB_RECV_CANCELLED)
            LOGE("grab: hello receive failed (%d)", rr);
        goto done;
    }
    if (hello.magic != IGRAB_MAGIC || hello.version != IGRAB_PROTOCOL_VERSION) {
        LOGE("grab: bad hello");
        goto done;
    }
    if (hello.status == IGRAB_STATUS_FAILED) {
        failure_reason = hello.reason != IGRAB_REASON_NONE &&
                         grab_reason_valid((int)hello.reason)
                       ? (int)hello.reason : IGRAB_REASON_STARTUP_IO_ERROR;
        LOGE("grab: helper rejected startup reason=%d", failure_reason);
        goto done;
    }
    if (hello.status != IGRAB_STATUS_OK || hello.reason != IGRAB_REASON_NONE ||
        hello.dev_count == 0 || hello.dev_count > IGRAB_MAX_DEVICES) {
        LOGE("grab: invalid hello status=%u reason=%u devices=%u",
             hello.status, hello.reason, hello.dev_count);
        goto done;
    }

    for (uint32_t i = 0; i < hello.dev_count; i++) {
        struct igrab_dev gd;
        rr = grab_recv_exact(s, cfd, &gd, sizeof gd, GRAB_HELLO_WAIT_MS,
                             GRAB_FRAME_WAIT_MS);
        if (rr != GRAB_RECV_OK)
            goto done;
        if (gd.index >= IGRAB_MAX_DEVICES || devs[gd.index].present ||
            (gd.kind != IGRAB_KIND_KEY && gd.kind != IGRAB_KIND_TOUCH &&
             gd.kind != IGRAB_KIND_SINGLE_TOUCH)) {
            LOGE("grab: invalid/duplicate descriptor index=%u kind=%u", gd.index, gd.kind);
            goto done;
        }
        struct grab_devstate *d = &devs[gd.index];
        d->present = 1;
        d->index = (int)gd.index;
        d->kind = gd.kind;
        if (gd.kind == IGRAB_KIND_TOUCH)
            has_touchpad_device = true;
        d->is_touch = (gd.kind != IGRAB_KIND_KEY);
        d->cur_slot = -1;
        if (d->is_touch) {
            int64_t slot_count = d->kind == IGRAB_KIND_SINGLE_TOUCH ? 1
                               : (int64_t)gd.abs_slot_max - gd.abs_slot_min + 1;
            if (gd.abs_x_max <= gd.abs_x_min || gd.abs_y_max <= gd.abs_y_min ||
                slot_count <= 0 || slot_count > IGRAB_MAX_SLOTS) {
                LOGE("grab: invalid touch descriptor index=%u slots=%lld", gd.index,
                     (long long)slot_count);
                goto done;
            }
            d->x_min = gd.abs_x_min; d->x_max = gd.abs_x_max;
            d->y_min = gd.abs_y_min; d->y_max = gd.abs_y_max;
            d->slot_min = d->kind == IGRAB_KIND_SINGLE_TOUCH ? 0 : gd.abs_slot_min;
            d->slot_max = d->kind == IGRAB_KIND_SINGLE_TOUCH ? 0 : gd.abs_slot_max;
            d->slot_count = (int)slot_count;
            if (d->kind == IGRAB_KIND_SINGLE_TOUCH) {
                d->cur_slot = 0;
            } else {
                mt_seed_expected[gd.index] = 1;
            }
        }
    }

    /* A Type-B evdev client's current slot is device-global state and is not
     * guaranteed to be repeated before the next contact. The helper therefore sends
     * exactly one ABS_MT_SLOT seed for every Type-B descriptor, ordered by wire index.
     * Treat those records as part of the startup handshake: entering ACTIVE before
     * validating them could route the first contact through the wrong slot. */
    for (uint32_t expected = 0; expected < IGRAB_MAX_DEVICES; expected++) {
        if (!mt_seed_expected[expected])
            continue;

        struct igrab_rec seed;
        rr = grab_recv_exact(s, cfd, &seed, sizeof seed, GRAB_HELLO_WAIT_MS,
                             GRAB_FRAME_WAIT_MS);
        if (rr != GRAB_RECV_OK) {
            if (rr != GRAB_RECV_CANCELLED)
                LOGE("grab: Type-B slot seed receive failed index=%u (%d)",
                     expected, rr);
            goto done;
        }
        if (seed.index >= IGRAB_MAX_DEVICES || seed.index != expected ||
            mt_seed_seen[seed.index] || seed.type != GEV_ABS ||
            seed.code != GABS_MT_SLOT) {
            LOGE("grab: invalid/duplicate Type-B slot seed expected=%u "
                 "index=%u type=%u code=%u", expected, seed.index,
                 seed.type, seed.code);
            goto done;
        }

        struct grab_devstate *d = &devs[seed.index];
        if (!d->present || d->kind != IGRAB_KIND_TOUCH ||
            seed.value < d->slot_min || seed.value > d->slot_max) {
            LOGE("grab: out-of-range Type-B slot seed index=%u value=%d "
                 "range=%d..%d", seed.index, seed.value,
                 d->slot_min, d->slot_max);
            goto done;
        }

        d->cur_slot = seed.value - d->slot_min;
        mt_seed_seen[seed.index] = 1;
    }

    if (atomic_load_explicit(&s->grab_stop_requested, memory_order_acquire))
        goto done;
    if (touchpad_mode && has_touchpad_device &&
        !grab_touch_sink_init(s, &touch_sink)) {
        LOGE("grab: could not initialize Java touch-frame callback");
        goto done;
    }
    if (atomic_load_explicit(&s->grab_stop_requested, memory_order_acquire))
        goto done;

    started = true;
    LOGI("grab: streaming %u devices session=%llu", hello.dev_count,
         (unsigned long long)session_id);
    grab_notify_java(s, GRAB_CALLBACK_STARTED, session_id, IGRAB_REASON_NONE);

    while (!atomic_load_explicit(&s->grab_stop_requested, memory_order_acquire)) {
        struct igrab_rec rec;
        rr = grab_recv_exact(s, cfd, &rec, sizeof rec, -1, GRAB_FRAME_WAIT_MS);
        if (rr == GRAB_RECV_CANCELLED)
            break;
        if (rr != GRAB_RECV_OK) {
            release_reason = IGRAB_REASON_STREAM_IO_ERROR;
            break;
        }

        if (rec.type == IGRAB_REC_TYPE_CONTROL) {
            if (rec.code == IGRAB_CONTROL_RELEASE)
                release_reason = grab_reason_valid(rec.value)
                               ? rec.value : IGRAB_REASON_STREAM_IO_ERROR;
            else
                release_reason = IGRAB_REASON_STREAM_IO_ERROR;
            break;
        }

        struct grab_devstate *d = (rec.index < (uint32_t)IGRAB_MAX_DEVICES)
                                  ? &devs[rec.index] : NULL;
        if (!d || !d->present)
            continue;

        /* Discard the remainder of the damaged device frame. Resuming immediately
         * after SYN_DROPPED can combine a partial frame with stale slot selection and
         * create a phantom touch; the next SYN_REPORT is the clean boundary. */
        if (rec.type == GEV_SYN && rec.code == GSYN_DROPPED) {
            LOGI("grab: device %u dropped records -> resync", rec.index);
            grab_release_device(s, devs, &keys, (int)rec.index, touchpad_mode,
                                session_id, &touch_sink);
            d->desynced = 1;
            continue;
        }
        if (d->desynced) {
            if (rec.type == GEV_SYN && rec.code == GSYN_REPORT)
                d->desynced = 0;
            continue;
        }

        /* Touchscreens commonly emit BTN_TOUCH/BTN_TOOL_FINGER EV_KEY records. They
         * are touch protocol helpers, not keyboard keys, and must not be injected as
         * raw Linux key codes into the compositor. */
        if (!d->is_touch && rec.type == GEV_KEY) {
            int code = rec.code, val = rec.value;
            if (val == 2)
                continue;               /* autorepeat: the compositor repeats */
            if (val == 0 || val == 1)
                grab_set_key(s, &keys, (int)rec.index, code, val == 1);
        } else if (d->kind == IGRAB_KIND_SINGLE_TOUCH && rec.type == GEV_KEY) {
            if (rec.code != GBTN_TOUCH)
                continue;
            struct grab_slot *sl = &d->slots[0];
            if (rec.value == 1) {
                sl->active = 1;
                sl->down = 1;
                sl->up = 0;
                sl->dirty = 1;
            } else if (rec.value == 0 && sl->active) {
                sl->up = 1;
                sl->dirty = 1;
            }
        } else if (d->kind == IGRAB_KIND_SINGLE_TOUCH && rec.type == GEV_ABS) {
            struct grab_slot *sl = &d->slots[0];
            if (rec.code == GABS_X) {
                sl->x = rec.value;
                if (sl->active && !sl->down)
                    sl->dirty = 1;
            } else if (rec.code == GABS_Y) {
                sl->y = rec.value;
                if (sl->active && !sl->down)
                    sl->dirty = 1;
            }
        } else if (d->kind == IGRAB_KIND_TOUCH && rec.type == GEV_ABS) {
            if (rec.code == GABS_MT_SLOT) {
                int64_t local = (int64_t)rec.value - d->slot_min;
                /* Never coerce an invalid slot to zero: doing so writes the bad
                 * device's events into a previously valid finger. Ignore until a
                 * subsequent valid ABS_MT_SLOT selects a real slot. */
                d->cur_slot = (local >= 0 && local < d->slot_count) ? (int)local : -1;
                continue;
            }
            if (d->cur_slot < 0 || d->cur_slot >= d->slot_count)
                continue;
            struct grab_slot *sl = &d->slots[d->cur_slot];
            switch (rec.code) {
            case GABS_MT_TRACKING_ID:
                if (rec.value == -1) {
                    if (sl->active) {
                        sl->up = 1;
                        sl->dirty = 1;
                    }
                } else {
                    sl->active = 1;
                    sl->down = 1;
                    sl->up = 0;
                    sl->dirty = 1;
                }
                break;
            case GABS_MT_POSITION_X:
                sl->x = rec.value;
                if (sl->active && !sl->down)
                    sl->dirty = 1;
                break;
            case GABS_MT_POSITION_Y:
                sl->y = rec.value;
                if (sl->active && !sl->down)
                    sl->dirty = 1;
                break;
            }
        } else if (d->is_touch && rec.type == GEV_SYN && rec.code == GSYN_REPORT) {
            grab_flush_touch(s, d, touchpad_mode, session_id, &touch_sink);
        }
    }

done:
    if (started)
        grab_release_all(s, devs, &keys, touchpad_mode, session_id, &touch_sink);
    if (cfd >= 0)
        close(cfd);
    if (lfd >= 0)
        close(lfd);
    unlink(bridge);

    int forced_reason = atomic_load_explicit(&s->grab_forced_reason, memory_order_acquire);
    bool cancelled = atomic_load_explicit(&s->grab_stop_requested, memory_order_acquire);
    uint_fast64_t expected_session = session_id;
    atomic_compare_exchange_strong(&s->grab_live_session, &expected_session, 0);
    atomic_store_explicit(&s->grab_stop_requested, true, memory_order_release);

    /* Publish STOPPED before the asynchronous Java callback. A new generation can
     * never mistake this thread for its own session, and Java independently filters
     * callbacks by session id. User-requested stop is already known to Java, so it is
     * intentionally silent; forced transport release must still be reported. */
    if (!started) {
        if (!cancelled || forced_reason != IGRAB_REASON_NONE) {
            int reason = forced_reason != IGRAB_REASON_NONE ? forced_reason : failure_reason;
            grab_notify_java(s, GRAB_CALLBACK_FAILED, session_id, reason);
        }
    } else {
        int reason = release_reason;
        if (reason == IGRAB_REASON_NONE && forced_reason != IGRAB_REASON_NONE)
            reason = forced_reason;
        if (reason == IGRAB_REASON_NONE && !cancelled)
            reason = IGRAB_REASON_STREAM_IO_ERROR;
        if (reason != IGRAB_REASON_NONE)
            grab_notify_java(s, GRAB_CALLBACK_RELEASED, session_id, reason);
    }

    grab_touch_sink_destroy(&touch_sink);
    reap_su(pid, 3000);
    LOGI("grab thread stopped session=%llu", (unsigned long long)session_id);
    return NULL;
}

static void join_grab_thread(struct consumer_state *s)
{
    pthread_t thread;
    pthread_mutex_lock(&s->grab_lock);
    while (s->grab_join_in_progress)
        pthread_cond_wait(&s->grab_cond, &s->grab_lock);
    if (!s->grab_thread_joinable) {
        pthread_mutex_unlock(&s->grab_lock);
        return;
    }
    thread = s->grab_thread;
    if (pthread_equal(thread, pthread_self())) {
        LOGE("grab: refusing to join current thread");
        pthread_mutex_unlock(&s->grab_lock);
        return;
    }
    s->grab_join_in_progress = true;
    pthread_mutex_unlock(&s->grab_lock);

    pthread_join(thread, NULL);

    pthread_mutex_lock(&s->grab_lock);
    s->grab_thread_joinable = false;
    s->grab_join_in_progress = false;
    pthread_cond_broadcast(&s->grab_cond);
    pthread_mutex_unlock(&s->grab_lock);
}

static int start_grab_thread(struct consumer_state *s)
{
    join_grab_thread(s);
    pthread_mutex_lock(&s->grab_lock);
    grab_drain_cancel(s);
    atomic_store_explicit(&s->grab_stop_requested, false, memory_order_release);
    atomic_store_explicit(&s->grab_forced_reason, IGRAB_REASON_NONE,
                          memory_order_release);
    atomic_store_explicit(&s->grab_live_session, s->grab_session_id,
                          memory_order_release);
    int r = pthread_create(&s->grab_thread, NULL, grab_thread_func, s);
    if (r == 0) {
        s->grab_thread_joinable = true;
    } else {
        atomic_store_explicit(&s->grab_stop_requested, true, memory_order_release);
        atomic_store_explicit(&s->grab_live_session, 0, memory_order_release);
    }
    pthread_mutex_unlock(&s->grab_lock);
    return r == 0 ? 0 : -1;
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

    pthread_mutex_lock(&s->ctx_call_lock);
    bool has_old_ctx = s->ctx != NULL;
    pthread_mutex_unlock(&s->ctx_call_lock);
    if (has_old_ctx) {
        /* No thread may retain/use a display_ctx across disconnect(). If a transport
         * rebuild ever happens while immersed, first make the helper release input and
         * wait for the app-side reader to leave push_input_event(). */
        set_transport_ready(s, false);
        request_grab_stop(s, IGRAB_REASON_STREAM_IO_ERROR);
        join_grab_thread(s);
        audio_set_ctx(s->audio, NULL);   /* detach audio before the old ctx (and its fd) dies */
        stop_and_join_event_thread(s);
        display_ctx *old_ctx = detach_display_ctx(s);
        if (old_ctx)
            disconnect(old_ctx);
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

    display_ctx *new_ctx = NULL;
    if (use_root) {
        int ctrl_fd = recv_fd_via_root_helper(sock, helper_path, bridge_path);
        if (ctrl_fd < 0) {
            LOGE("root helper connect failed");
            return -1;
        }
        if (connect_to_deamon_with_fd(&new_ctx, ctrl_fd) < 0) {
            LOGE("connect_to_deamon_with_fd failed");
            return -1;
        }
    } else if (connect_to_deamon(&new_ctx, sock) < 0) {
        LOGE("connect_to_deamon failed");
        return -1;
    }

    if (set_screen_info(new_ctx, s->screen_w, s->screen_h,
                        PIXEL_FORMAT_RGBA_8888, s->refresh_mhz) < 0) {
        LOGE("failed to send screen info");
        disconnect(new_ctx);
        return -1;
    }
    push_dmabufs(new_ctx, s->dmabuf_fds, s->dmabuf_infos, s->buf_count);

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
        allocate_services(new_ctx, &s->camera_svc, 1);
    }

    set_fallback_callback(new_ctx, on_fallback, s);
    set_exit_fallback_callback(new_ctx, on_exit_fallback, s);

    publish_display_ctx(s, new_ctx);
    audio_set_ctx(s->audio, new_ctx);   /* fd becomes readable after ACTIVE */

    s->need_reconnect = false;
    LOGI("connected");
    return 0;
}

static void on_fallback(void *userdata)
{
    struct consumer_state *s = userdata;
    LOGI("fallback triggered");

    /* A fallback ctx discards input. Keeping EVIOCGRAB active would make the device
     * appear dead while events vanish, so request an asynchronous helper release. This
     * callback may itself be running on the grab thread; signal only, never self-join. */
    set_transport_ready(s, false);
    request_grab_stop(s, IGRAB_REASON_STREAM_IO_ERROR);
    if (s->ctx && display_control_dead(s->ctx)) {
        /* The daemon control stream is terminal (HUP/timeout/protocol corruption),
         * unlike an ordinary producer fallback that the display library can recover
         * in-place. Let the render loop build a fresh ctrl connection. */
        atomic_store_explicit(&s->need_reconnect, true, memory_order_release);
    }

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

    /* on_fallback detached the bridge before the library replaced audio_fd. Reattach
     * only after the new display session is ACTIVE; audio_set_ctx also forces a fresh
     * format handshake even if an audio loop never observed the brief NULL state. */
    if (s->ctx)
        audio_set_ctx(s->audio, s->ctx);

    /* Bound every producer write, including raw-grab forwarding. Without a send
     * timeout a connected-but-frozen producer can park the grab reader inside
     * push_input_event forever, defeating cancel/join. The fd changes after each
     * fallback recovery, so reapply here. */
    if (s->ctx) {
        int fd = get_data_fd(s->ctx);
        if (fd >= 0) {
            struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
            close(fd);
        }
    }

    send_refresh_rate(s);
    if (!s->ctx || !display_is_active(s->ctx))
        return;

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

    /* Clipboard sync is itself a transport write and may have discovered a dead
     * producer. Only release the immersive helper wait after every reconnect seed and
     * host callback completed on the same still-active connection. */
    if (!s->ctx || !display_is_active(s->ctx)) {
        stop_event_thread(s);
        return;
    }
    set_transport_ready(s, true);
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
    (void)clazz;
    struct consumer_state *s = calloc(1, sizeof(*s));
    if (!s)
        return 0;
    pthread_mutex_init(&s->lock, NULL);
    pthread_mutex_init(&s->ctx_call_lock, NULL);
    pthread_mutex_init(&s->cfg_lock, NULL);
    pthread_mutex_init(&s->grab_lifecycle_lock, NULL);
    pthread_mutex_init(&s->grab_lock, NULL);
    pthread_cond_init(&s->grab_cond, NULL);
    pthread_mutex_init(&s->transport_lock, NULL);
    pthread_cond_init(&s->transport_cond, NULL);
    atomic_init(&s->running, false);
    atomic_init(&s->need_reconnect, false);
    atomic_init(&s->refresh_mhz, 0);
    atomic_init(&s->event_running, false);
    atomic_init(&s->event_thread_alive, false);
    atomic_init(&s->grab_stop_requested, true);
    atomic_init(&s->grab_forced_reason, IGRAB_REASON_NONE);
    atomic_init(&s->grab_live_session, 0);
    atomic_init(&s->grab_rotation, 0);
    atomic_init(&s->transport_ready, false);
    s->grab_cancel_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (s->grab_cancel_fd < 0) {
        LOGE("grab: eventfd create failed: %s", strerror(errno));
        pthread_cond_destroy(&s->transport_cond);
        pthread_mutex_destroy(&s->transport_lock);
        pthread_cond_destroy(&s->grab_cond);
        pthread_mutex_destroy(&s->grab_lock);
        pthread_mutex_destroy(&s->grab_lifecycle_lock);
        pthread_mutex_destroy(&s->cfg_lock);
        pthread_mutex_destroy(&s->ctx_call_lock);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return 0;
    }
    if (!g_jvm)
        (*env)->GetJavaVM(env, &g_jvm);
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
    pthread_mutex_lock(&s->grab_lifecycle_lock);
    set_transport_ready(s, false);
    stop_grab_thread(s);
    join_grab_thread(s);

    pthread_mutex_lock(&s->lock);
    if (s->running) {
        s->running = false;
        pthread_mutex_unlock(&s->lock);
        pthread_join(s->render_thread, NULL);
        pthread_mutex_lock(&s->lock);
    }
    set_transport_ready(s, false);
    audio_set_ctx(s->audio, NULL);
    audio_stop(s->audio);
    stop_and_join_event_thread(s);
    display_ctx *old_ctx = detach_display_ctx(s);
    if (old_ctx)
        disconnect(old_ctx);
    cleanup_dmabufs(s);
    if (s->window) {
        ANativeWindow_release(s->window);
        s->window = NULL;
    }
    pthread_mutex_unlock(&s->lock);

    audio_destroy(s->audio);
    s->audio = NULL;
    camera_release_client(s);   /* window gone: tear down its camera channels */

    if ((s->clipboard_obj || s->activity_obj) && g_jvm) {
        JNIEnv *e = NULL;
        bool attached = false;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&e, JNI_VERSION_1_6) == JNI_EDETACHED)
            attached = ((*g_jvm)->AttachCurrentThread(g_jvm, &e, NULL) == 0);
        if (e) {
            if (s->clipboard_obj)
                (*e)->DeleteGlobalRef(e, s->clipboard_obj);
            if (s->activity_obj)
                (*e)->DeleteGlobalRef(e, s->activity_obj);
        }
        if (attached)
            (*g_jvm)->DetachCurrentThread(g_jvm);
    }
    s->clipboard_obj = NULL;
    s->activity_obj = NULL;
    pthread_mutex_unlock(&s->grab_lifecycle_lock);
    if (s->grab_cancel_fd >= 0) {
        close(s->grab_cancel_fd);
        s->grab_cancel_fd = -1;
    }
    pthread_cond_destroy(&s->transport_cond);
    pthread_mutex_destroy(&s->transport_lock);
    pthread_cond_destroy(&s->grab_cond);
    pthread_mutex_destroy(&s->grab_lock);
    pthread_mutex_destroy(&s->grab_lifecycle_lock);
    pthread_mutex_destroy(&s->cfg_lock);
    pthread_mutex_destroy(&s->ctx_call_lock);
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

/* Start immersive full-input grab. Each request carries a Java-owned generation id;
 * callbacks echo it so a delayed old helper can never mutate a newer UI session. */
JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeStartInputGrab(
    JNIEnv *env, jclass clazz, jlong handle, jstring helperPath, jstring bridgePath,
    jint rotation, jint exitKeyCode, jlong sessionId, jboolean touchpadMode)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    pthread_mutex_lock(&s->grab_lifecycle_lock);
    /* Replacement is explicit, not merely "idempotent": finish the previous
     * generation before publishing the next thread's configuration. */
    stop_grab_thread(s);
    join_grab_thread(s);

    uint64_t session_id = (uint64_t)sessionId;
    if (session_id == 0) {
        grab_notify_java(s, GRAB_CALLBACK_FAILED, session_id,
                         IGRAB_REASON_STARTUP_IO_ERROR);
        pthread_mutex_unlock(&s->grab_lifecycle_lock);
        return;
    }

    atomic_store_explicit(&s->grab_rotation, rotation, memory_order_relaxed);
    pthread_mutex_lock(&s->grab_lock);
    s->grab_session_id = session_id;
    s->grab_exit_code = exitKeyCode;
    s->grab_touchpad_mode = touchpadMode == JNI_TRUE;
    copy_jstring(env, helperPath, s->grab_helper_path, sizeof(s->grab_helper_path));
    copy_jstring(env, bridgePath, s->grab_bridge_path, sizeof(s->grab_bridge_path));
    pthread_mutex_unlock(&s->grab_lock);
    if (start_grab_thread(s) < 0)
        grab_notify_java(s, GRAB_CALLBACK_FAILED, session_id,
                         IGRAB_REASON_STARTUP_IO_ERROR);
    pthread_mutex_unlock(&s->grab_lifecycle_lock);
}

/* Update the touch rotation live (called on display rotation while grabbing). */
JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSetGrabRotation(
    JNIEnv *env, jclass clazz, jlong handle, jint rotation)
{
    (void)env; (void)clazz;
    struct consumer_state *s = STATE(handle);
    if (s)
        atomic_store_explicit(&s->grab_rotation, rotation, memory_order_relaxed);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeStopInputGrab(
    JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env; (void)clazz;
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    pthread_mutex_lock(&s->grab_lifecycle_lock);
    stop_grab_thread(s);
    join_grab_thread(s);
    pthread_mutex_unlock(&s->grab_lifecycle_lock);
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

    pthread_mutex_lock(&s->grab_lifecycle_lock);
    set_transport_ready(s, false);
    stop_grab_thread(s);
    join_grab_thread(s);

    if (!api_loaded) {
        if (anw_api_load(&api) < 0) {
            LOGE("failed to load ANativeWindow hidden API");
            pthread_mutex_unlock(&s->grab_lifecycle_lock);
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

    set_transport_ready(s, false);
    audio_set_ctx(s->audio, NULL);
    audio_stop(s->audio);
    stop_and_join_event_thread(s);
    display_ctx *old_ctx = detach_display_ctx(s);
    if (old_ctx)
        disconnect(old_ctx);
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
        pthread_mutex_unlock(&s->grab_lifecycle_lock);
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
    if (pthread_create(&s->render_thread, NULL, render_thread_func, s) != 0) {
        LOGE("failed to create render thread");
        s->running = false;
        pthread_mutex_unlock(&s->lock);
        pthread_mutex_unlock(&s->grab_lifecycle_lock);
        return;
    }

    /* Audio streams live independently of the connection; the render thread attaches
     * the fd via audio_set_ctx() once connected. */
    audio_start(s->audio);

    pthread_mutex_unlock(&s->lock);
    pthread_mutex_unlock(&s->grab_lifecycle_lock);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeStop(
    JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    pthread_mutex_lock(&s->grab_lifecycle_lock);
    /* Java serialises transport lifecycle on its grab executor. Preserve the native
     * invariant too: no display_ctx/global activity ref is torn down while the grab
     * reader can still use it. Cancellation wakes every socket/transport wait. */
    set_transport_ready(s, false);
    stop_grab_thread(s);
    join_grab_thread(s);

    pthread_mutex_lock(&s->lock);

    if (s->running) {
        s->running = false;
        pthread_mutex_unlock(&s->lock);
        pthread_join(s->render_thread, NULL);
        pthread_mutex_lock(&s->lock);
    }

    set_transport_ready(s, false);
    /* Stop audio before the ctx (and its fd) is torn down. */
    audio_set_ctx(s->audio, NULL);
    audio_stop(s->audio);

    stop_and_join_event_thread(s);
    display_ctx *old_ctx = detach_display_ctx(s);
    if (old_ctx)
        disconnect(old_ctx);

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
    pthread_mutex_unlock(&s->grab_lifecycle_lock);
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
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_TOUCH,
        .touch = { .action = action, .x = x, .y = y, .pointer_id = pointer_id },
    };
    try_push_hot_event(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTouchFrame(
    JNIEnv *env, jclass clazz, jlong handle)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_TOUCH_FRAME,
    };
    try_push_hot_event(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendKey(
    JNIEnv *env, jclass clazz, jlong handle, jint action, jint keycode)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_KEY,
        .key = { .action = action, .keycode = keycode },
    };
    try_push_hot_event(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseMotion(
    JNIEnv *env, jclass clazz, jlong handle, jfloat x, jfloat y, jfloat dx, jfloat dy)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;

    if (pthread_mutex_trylock(&s->ctx_call_lock) != 0)
        return;
    display_ctx *ctx = s->ctx;
    if (!ctx) {
        pthread_mutex_unlock(&s->ctx_call_lock);
        return;
    }

    // Independently calculate dx and dy fallback if not provided to prevent diagonal motion jitter
    if (s->motion_has_last) {
        if (dx == 0.0f && x != s->motion_last_x) {
            dx = x - s->motion_last_x;
        }
        if (dy == 0.0f && y != s->motion_last_y) {
            dy = y - s->motion_last_y;
        }
    }

    s->motion_last_x = x;
    s->motion_last_y = y;
    s->motion_has_last = true;

    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_MOTION,
        .pointer_motion = { .x = x, .y = y, .dx = dx, .dy = dy },
    };
    push_input_event(ctx, &ev);
    pthread_mutex_unlock(&s->ctx_call_lock);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseButton(
    JNIEnv *env, jclass clazz, jlong handle, jint button, jboolean pressed)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_BUTTON,
        .pointer_button = { .button = button, .pressed = pressed ? 1 : 0 },
    };
    try_push_hot_event(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendMouseScroll(
    JNIEnv *env, jclass clazz, jlong handle, jint axis, jfloat value)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_AXIS,
        .pointer_axis = { .axis = axis, .value = value, .discrete = 0 },
    };
    try_push_hot_event(s, &ev);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendClipboard(
    JNIEnv *env, jclass clazz, jlong handle, jbyteArray data)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
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
    try_push_hot_event_with_length(s, &ev, buf, (size_t)len);
    free(buf);
}

JNIEXPORT void JNICALL
Java_com_anland_consumer_Native_nativeSendTextInput(
    JNIEnv *env, jclass clazz, jlong handle, jbyteArray data)
{
    struct consumer_state *s = STATE(handle);
    if (!s)
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
    try_push_hot_event_with_length(s, &ev, buf, (size_t)len);
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
