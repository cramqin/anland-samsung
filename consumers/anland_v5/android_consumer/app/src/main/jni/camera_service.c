#define _GNU_SOURCE
#include "camera_service.h"

#include <android/log.h>
#include <android/sharedmem.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

#define TAG "AnlandCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Fallback slot size when a camera's max resolution is unknown (1080p I420). */
#define CAM_FALLBACK_BYTES (1920 * 1080 * 3 / 2)

/*
 * Single global instance. Created once in camera_service_init() and persistent for
 * the process. Per camera we own: a SEQPACKET stream socketpair (small control/pacing
 * messages, the producer gets the remote end via SCM_RIGHTS) and an ashmem region of
 * CAMERA_SLOTS * slot_bytes holding the I420 double buffer. The Java analyzer writes
 * pixels straight into a slot (via the direct ByteBuffers) and the io thread does the
 * READY/DONE handshake with the producer.
 */
static struct camera_state {
    int ctrl_local;
    int ctrl_remote;
    int stream_local[MAX_CAMERAS];
    int stream_remote[MAX_CAMERAS];
    int num_cameras;

    /* { ctrl_remote, stream_remote[0..N-1] } -- handed to the producer verbatim. */
    int alloc_fds[1 + MAX_CAMERAS];

    /* Per-camera shared-memory double buffer. */
    int      shm_fd[MAX_CAMERAS];
    uint8_t *shm_ptr[MAX_CAMERAS];           /* mmap, CAMERA_SLOTS * slot_bytes */
    size_t   slot_bytes[MAX_CAMERAS];
    jobject  slot_buf[MAX_CAMERAS][CAMERA_SLOTS];  /* global refs: direct ByteBuffers */

    /* Slot ownership handshake: slot_free[c][s] is true once the producer has DONE'd
     * that slot (or it has never been handed out), meaning the analyzer may write it. */
    pthread_mutex_t slot_lock[MAX_CAMERAS];
    pthread_cond_t  slot_cond[MAX_CAMERAS];
    bool            slot_free[MAX_CAMERAS][CAMERA_SLOTS];

    JavaVM   *jvm;
    jobject   svc_obj;       /* global ref to the Java CameraServices instance */
    jmethodID m_getCount;    /* int  getCameraCount()              */
    jmethodID m_maxW;        /* int  getCameraMaxWidth(int)        */
    jmethodID m_maxH;        /* int  getCameraMaxHeight(int)       */
    jmethodID m_start;       /* void startRecording(int,int,int)   */
    jmethodID m_stop;        /* void stopRecording(int)            */
    jmethodID m_stopAll;     /* void stopAllRecording()            */
    jmethodID m_release;     /* void release()                     */

    pthread_t      io_thread;
    volatile bool  running;
    /* Read by do_connect() on the render thread; written on the main thread in
     * init/destroy. volatile so the render thread sees a fresh value when it
     * checks camera_service_is_ready() right after nativeStart. */
    volatile bool  ready;
} g_camera;

/* Attach the calling thread to the JVM if needed; *did_attach is set so the
 * caller knows whether to detach afterwards. Returns NULL on failure. */
static JNIEnv *cam_get_env(bool *did_attach)
{
    *did_attach = false;
    if (!g_camera.jvm)
        return NULL;
    JNIEnv *env = NULL;
    int st = (*g_camera.jvm)->GetEnv(g_camera.jvm, (void **)&env, JNI_VERSION_1_6);
    if (st == JNI_EDETACHED) {
        if ((*g_camera.jvm)->AttachCurrentThread(g_camera.jvm, &env, NULL) != 0)
            return NULL;
        *did_attach = true;
    } else if (st != JNI_OK) {
        return NULL;
    }
    return env;
}

static void cam_detach(bool did_attach)
{
    if (did_attach && g_camera.jvm)
        (*g_camera.jvm)->DetachCurrentThread(g_camera.jvm);
}

/* Read exactly len bytes (blocking) from fd. Returns 0 on success, -1 on error. */
static int read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n == 0)
            return -1;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

/* Send a fixed stream-control message, optionally with an fd attached (SCM_RIGHTS). */
static void send_stream(int sock, uint8_t type, uint8_t slot, uint16_t fmt,
                        uint32_t a, uint32_t b, int fd)
{
    struct cam_stream_msg m = { .type = type, .slot = slot, .fmt = fmt,
                                .a = a, .b = b };
    struct iovec iov = { .iov_base = &m, .iov_len = sizeof(m) };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg;
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (fd >= 0) {
        msg.msg_control = cmsg.buf;
        msg.msg_controllen = sizeof(cmsg.buf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    if (sendmsg(sock, &msg, MSG_NOSIGNAL) < 0)
        LOGE("stream: send type=%u failed: %s", type, strerror(errno));
}

/* --- control channel (ctrl_fd) message handling --- */

static void ctrl_handle_msg(JNIEnv *env, const struct camera_ctrl_msg *hdr,
                            const uint8_t *payload)
{
    switch (hdr->type) {
    case CAMERA_CTRL_GET_INFO: {
        uint8_t reply[sizeof(struct camera_ctrl_msg) + 1 + MAX_CAMERAS * 4];
        struct camera_ctrl_msg *rh = (struct camera_ctrl_msg *)reply;
        rh->type = CAMERA_CTRL_INFO_REPLY;
        rh->reserved = 0;
        uint8_t *pl = reply + sizeof(struct camera_ctrl_msg);
        int n = g_camera.num_cameras;
        pl[0] = (uint8_t)n;
        size_t off = 1;
        for (int i = 0; i < n; i++) {
            int w = (*env)->CallIntMethod(env, g_camera.svc_obj, g_camera.m_maxW, (jint)i);
            int h = (*env)->CallIntMethod(env, g_camera.svc_obj, g_camera.m_maxH, (jint)i);
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
                w = h = 0;
            }
            uint16_t w16 = (uint16_t)w, h16 = (uint16_t)h;
            memcpy(pl + off, &w16, sizeof(w16)); off += sizeof(w16);
            memcpy(pl + off, &h16, sizeof(h16)); off += sizeof(h16);
        }
        rh->len = (uint16_t)off;
        if (send(g_camera.ctrl_local, reply, sizeof(struct camera_ctrl_msg) + off,
                 MSG_NOSIGNAL) < 0)
            LOGE("ctrl: INFO_REPLY send failed: %s", strerror(errno));
        break;
    }
    case CAMERA_CTRL_START_RECORD: {
        if (hdr->len < 5) {
            LOGE("ctrl: START_RECORD short payload (%u)", hdr->len);
            break;
        }
        uint8_t id = payload[0];
        uint16_t w, h;
        memcpy(&w, payload + 1, sizeof(w));
        memcpy(&h, payload + 3, sizeof(h));
        if (id >= g_camera.num_cameras) {
            LOGE("ctrl: START_RECORD bad id %u", id);
            break;
        }
        LOGI("ctrl: START_RECORD cam=%u %ux%u", id, w, h);
        (*env)->CallVoidMethod(env, g_camera.svc_obj, g_camera.m_start,
                               (jint)id, (jint)w, (jint)h);
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        break;
    }
    case CAMERA_CTRL_STOP_RECORD: {
        if (hdr->len < 1) {
            LOGE("ctrl: STOP_RECORD short payload");
            break;
        }
        uint8_t id = payload[0];
        if (id >= g_camera.num_cameras)
            break;
        LOGI("ctrl: STOP_RECORD cam=%u", id);
        (*env)->CallVoidMethod(env, g_camera.svc_obj, g_camera.m_stop, (jint)id);
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        break;
    }
    default:
        LOGE("ctrl: unknown msg type 0x%02x", hdr->type);
        break;
    }
}

/* --- stream channel (stream_fd[i]) message handling --- */

static void stream_handle_msg(int cam, const struct cam_stream_msg *m)
{
    switch (m->type) {
    case CAM_STREAM_GET_SHM:
        /* Producer wants the shm fd: offer the pre-created region. */
        send_stream(g_camera.stream_local[cam], CAM_STREAM_SHM_OFFER, 0, 0,
                    (uint32_t)g_camera.slot_bytes[cam], 0, g_camera.shm_fd[cam]);
        LOGI("stream cam=%d: offered shm (%zu B/slot)", cam, g_camera.slot_bytes[cam]);
        break;
    case CAM_STREAM_DONE: {
        uint8_t s = m->slot;
        if (s >= CAMERA_SLOTS)
            break;
        pthread_mutex_lock(&g_camera.slot_lock[cam]);
        g_camera.slot_free[cam][s] = true;
        pthread_cond_broadcast(&g_camera.slot_cond[cam]);
        pthread_mutex_unlock(&g_camera.slot_lock[cam]);
        break;
    }
    default:
        LOGE("stream cam=%d: unknown msg type %u", cam, m->type);
        break;
    }
}

/* --- I/O thread: polls ctrl_local + every stream_local for incoming messages --- */

static void *io_thread_func(void *arg)
{
    (void)arg;
    LOGI("io thread started");

    JNIEnv *env = NULL;
    if ((*g_camera.jvm)->AttachCurrentThread(g_camera.jvm, &env, NULL) != 0) {
        LOGE("io thread: AttachCurrentThread failed");
        return NULL;
    }

    const int n = g_camera.num_cameras;
    struct pollfd pfds[1 + MAX_CAMERAS];

    while (g_camera.running) {
        pfds[0].fd = g_camera.ctrl_local;
        pfds[0].events = POLLIN;
        for (int i = 0; i < n; i++) {
            pfds[1 + i].fd = g_camera.stream_local[i];
            pfds[1 + i].events = POLLIN;
        }

        int r = poll(pfds, 1 + n, 500);
        if (r <= 0)
            continue;

        if (pfds[0].revents & POLLIN) {
            struct camera_ctrl_msg hdr;
            if (read_full(g_camera.ctrl_local, &hdr, sizeof(hdr)) == 0) {
                uint8_t payload[256];
                uint16_t len = hdr.len;
                if (len > sizeof(payload))
                    len = sizeof(payload);
                if (len == 0 || read_full(g_camera.ctrl_local, payload, len) == 0)
                    ctrl_handle_msg(env, &hdr, payload);
            } else {
                usleep(50000);   /* real read error: back off rather than spin */
            }
        }

        for (int i = 0; i < n; i++) {
            if (!(pfds[1 + i].revents & POLLIN))
                continue;
            struct cam_stream_msg m;
            ssize_t got = recv(g_camera.stream_local[i], &m, sizeof(m), 0);
            if (got == (ssize_t)sizeof(m))
                stream_handle_msg(i, &m);
        }
    }

    (*g_camera.jvm)->DetachCurrentThread(g_camera.jvm);
    LOGI("io thread stopped");
    return NULL;
}

/* --- JNI: Java capture loop <-> shared memory slots --- */

JNIEXPORT jobject JNICALL
Java_com_anland_consumer_CameraServices_nativeGetSlotBuffer(
    JNIEnv *env, jobject thiz, jint cam, jint slot)
{
    (void)env;
    (void)thiz;
    if (cam < 0 || cam >= g_camera.num_cameras || slot < 0 || slot >= CAMERA_SLOTS)
        return NULL;
    return g_camera.slot_buf[cam][slot];
}

/* Block until the producer has released `slot` (DONE) or timeoutMs elapses, then claim
 * it for writing. Returns 0 if the slot was free, -1 on timeout (claimed anyway, so the
 * analyzer drops/overwrites rather than stalling forever when nothing is consuming). */
JNIEXPORT jint JNICALL
Java_com_anland_consumer_CameraServices_nativeAwaitSlotFree(
    JNIEnv *env, jobject thiz, jint cam, jint slot, jint timeoutMs)
{
    (void)env;
    (void)thiz;
    if (cam < 0 || cam >= g_camera.num_cameras || slot < 0 || slot >= CAMERA_SLOTS)
        return -1;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeoutMs / 1000;
    ts.tv_nsec += (long)(timeoutMs % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    int rc = 0;
    pthread_mutex_lock(&g_camera.slot_lock[cam]);
    while (!g_camera.slot_free[cam][slot]) {
        if (pthread_cond_timedwait(&g_camera.slot_cond[cam],
                                   &g_camera.slot_lock[cam], &ts) == ETIMEDOUT) {
            rc = -1;
            break;
        }
    }
    g_camera.slot_free[cam][slot] = false;   /* claim it for writing */
    pthread_mutex_unlock(&g_camera.slot_lock[cam]);
    return rc;
}

/* The analyzer finished writing `slot`: notify the producer it can copy it out. */
JNIEXPORT void JNICALL
Java_com_anland_consumer_CameraServices_nativeFrameReady(
    JNIEnv *env, jobject thiz, jint cam, jint slot, jint w, jint h, jint fmt)
{
    (void)env;
    (void)thiz;
    if (cam < 0 || cam >= g_camera.num_cameras || slot < 0 || slot >= CAMERA_SLOTS)
        return;
    send_stream(g_camera.stream_local[cam], CAM_STREAM_READY, (uint8_t)slot,
                (uint16_t)fmt, (uint32_t)w, (uint32_t)h, -1);
}

/*
 * Pack a YUV_420_888 frame from CameraX's plane buffers into the shm slot as NV21
 * (Y plane + interleaved V,U), which is the producer's fixed node format. NV21 is the
 * overwhelming Android default, so the common path is a verbatim chroma memcpy with no
 * de-interleave; the rare NV12 / planar sources are converted with a light per-pair
 * loop. Returns CAM_FMT_NV21. Bounds are guaranteed: w*h*3/2 <= slot_bytes (sized for
 * the camera max).
 */
JNIEXPORT jint JNICALL
Java_com_anland_consumer_CameraServices_nativePackFrame(
    JNIEnv *env, jobject thiz, jint cam, jint slot,
    jobject yBuf, jint yRow, jint yPix,
    jobject uBuf, jint uRow, jint uPix,
    jobject vBuf, jint vRow, jint vPix,
    jint w, jint h)
{
    (void)thiz;
    (void)yPix;
    if (cam < 0 || cam >= g_camera.num_cameras || slot < 0 || slot >= CAMERA_SLOTS)
        return CAM_FMT_I420;

    uint8_t *dst = g_camera.shm_ptr[cam] + (size_t)slot * g_camera.slot_bytes[cam];
    const uint8_t *y = (*env)->GetDirectBufferAddress(env, yBuf);
    const uint8_t *u = (*env)->GetDirectBufferAddress(env, uBuf);
    const uint8_t *v = (*env)->GetDirectBufferAddress(env, vBuf);
    if (!y || !u || !v)
        return CAM_FMT_I420;

    size_t ySize = (size_t)w * h;
    if (ySize + ySize / 2 > g_camera.slot_bytes[cam])
        return CAM_FMT_I420;   /* shouldn't happen (slot sized for max) */

    uint8_t *p = dst;
    if (yRow == w) {
        memcpy(p, y, ySize);
        p += ySize;
    } else {
        for (int r = 0; r < h; r++) {
            memcpy(p, y + (size_t)r * yRow, (size_t)w);
            p += w;
        }
    }

    int chh = h / 2;
    int cw = w / 2;
    if (uPix == 2 && vPix == 2 && v < u) {
        /* Source is NV21 (V before U): the interleaved V,U block is already NV21. */
        for (int r = 0; r < chh; r++) {
            memcpy(p, v + (size_t)r * vRow, (size_t)w);
            p += w;
        }
    } else if (uPix == 2 && vPix == 2) {
        /* Source is NV12 (U,V interleaved): swap each pair to V,U. */
        for (int r = 0; r < chh; r++) {
            const uint8_t *su = u + (size_t)r * uRow;  /* U at [0], V at [1] */
            for (int k = 0; k < cw; k++) {
                p[2 * k]     = su[2 * k + 1];  /* V */
                p[2 * k + 1] = su[2 * k];      /* U */
            }
            p += w;
        }
    } else {
        /* Planar source (pixelStride 1): interleave V,U. */
        for (int r = 0; r < chh; r++) {
            const uint8_t *su = u + (size_t)r * uRow;
            const uint8_t *sv = v + (size_t)r * vRow;
            for (int k = 0; k < cw; k++) {
                p[2 * k]     = sv[k];  /* V */
                p[2 * k + 1] = su[k];  /* U */
            }
            p += w;
        }
    }
    return CAM_FMT_NV21;
}

/* --- service_info callbacks --- */

struct resources camera_allocate_resource(uint32_t *args)
{
    (void)args;
    struct resources res = {
        .service_type = SERVICE_TYPE_CAMERA,
        .type = -1,
        .num = 0,
        .fds = NULL,
    };
    if (!g_camera.ready)
        return res;

    res.type = 0;
    res.num = (uint32_t)(1 + g_camera.num_cameras);
    res.fds = g_camera.alloc_fds;
    LOGI("allocate_resource: %u fds (1 ctrl + %d stream)",
         res.num, g_camera.num_cameras);
    return res;
}

void camera_free_resource(struct resources res)
{
    (void)res;
    if (!g_camera.ready || !g_camera.svc_obj)
        return;
    LOGI("free_resource: stopping all recording");
    bool attached = false;
    JNIEnv *env = cam_get_env(&attached);
    if (env) {
        (*env)->CallVoidMethod(env, g_camera.svc_obj, g_camera.m_stopAll);
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
    }
    cam_detach(attached);

    /* Producer is gone: its DONEs will never come. Release every slot so a fresh
     * producer/analyzer cycle isn't wedged waiting on a stale claim. */
    for (int i = 0; i < g_camera.num_cameras; i++) {
        pthread_mutex_lock(&g_camera.slot_lock[i]);
        for (int s = 0; s < CAMERA_SLOTS; s++)
            g_camera.slot_free[i][s] = true;
        pthread_cond_broadcast(&g_camera.slot_cond[i]);
        pthread_mutex_unlock(&g_camera.slot_lock[i]);
    }
}

/* --- lifecycle --- */

bool camera_service_is_ready(void)
{
    return g_camera.ready;
}

/* Create the ashmem region + direct ByteBuffers for one camera. Returns 0/-1. */
static int create_camera_shm(JNIEnv *env, int i)
{
    int w = (*env)->CallIntMethod(env, g_camera.svc_obj, g_camera.m_maxW, (jint)i);
    int h = (*env)->CallIntMethod(env, g_camera.svc_obj, g_camera.m_maxH, (jint)i);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        w = h = 0;
    }
    size_t slot_bytes = (w > 0 && h > 0) ? (size_t)w * h * 3 / 2 : CAM_FALLBACK_BYTES;
    g_camera.slot_bytes[i] = slot_bytes;

    int fd = ASharedMemory_create("anland-camera", CAMERA_SLOTS * slot_bytes);
    if (fd < 0) {
        LOGE("init: ASharedMemory_create[%d] failed", i);
        return -1;
    }
    g_camera.shm_fd[i] = fd;

    void *p = mmap(NULL, CAMERA_SLOTS * slot_bytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        LOGE("init: mmap shm[%d] failed: %s", i, strerror(errno));
        return -1;
    }
    g_camera.shm_ptr[i] = p;

    for (int s = 0; s < CAMERA_SLOTS; s++) {
        jobject bb = (*env)->NewDirectByteBuffer(env,
                        g_camera.shm_ptr[i] + (size_t)s * slot_bytes, slot_bytes);
        if (!bb) {
            LOGE("init: NewDirectByteBuffer[%d][%d] failed", i, s);
            return -1;
        }
        g_camera.slot_buf[i][s] = (*env)->NewGlobalRef(env, bb);
        (*env)->DeleteLocalRef(env, bb);
        g_camera.slot_free[i][s] = true;
    }
    pthread_mutex_init(&g_camera.slot_lock[i], NULL);
    pthread_cond_init(&g_camera.slot_cond[i], NULL);
    return 0;
}

int camera_service_init(JNIEnv *env, jobject activity_obj)
{
    if (g_camera.ready)
        return 0;                    /* idempotent */

    memset(&g_camera, 0, sizeof(g_camera));
    g_camera.ctrl_local = g_camera.ctrl_remote = -1;
    for (int i = 0; i < MAX_CAMERAS; i++) {
        g_camera.stream_local[i] = g_camera.stream_remote[i] = -1;
        g_camera.shm_fd[i] = -1;
    }

    (*env)->GetJavaVM(env, &g_camera.jvm);

    jclass cls = (*env)->FindClass(env, "com/anland/consumer/CameraServices");
    if (!cls) {
        LOGE("init: CameraServices class not found");
        (*env)->ExceptionClear(env);
        return -1;
    }
    jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "()V");
    jmethodID m_init = (*env)->GetMethodID(env, cls, "init", "(Landroid/content/Context;)V");
    g_camera.m_getCount = (*env)->GetMethodID(env, cls, "getCameraCount", "()I");
    g_camera.m_maxW     = (*env)->GetMethodID(env, cls, "getCameraMaxWidth", "(I)I");
    g_camera.m_maxH     = (*env)->GetMethodID(env, cls, "getCameraMaxHeight", "(I)I");
    g_camera.m_start    = (*env)->GetMethodID(env, cls, "startRecording", "(III)V");
    g_camera.m_stop     = (*env)->GetMethodID(env, cls, "stopRecording", "(I)V");
    g_camera.m_stopAll  = (*env)->GetMethodID(env, cls, "stopAllRecording", "()V");
    g_camera.m_release  = (*env)->GetMethodID(env, cls, "release", "()V");
    if (!ctor || !m_init || !g_camera.m_getCount || !g_camera.m_maxW ||
        !g_camera.m_maxH || !g_camera.m_start ||
        !g_camera.m_stop || !g_camera.m_stopAll || !g_camera.m_release) {
        LOGE("init: CameraServices method lookup failed");
        (*env)->ExceptionClear(env);
        return -1;
    }

    jobject obj = (*env)->NewObject(env, cls, ctor);
    if (!obj || (*env)->ExceptionCheck(env)) {
        LOGE("init: CameraServices construction failed");
        (*env)->ExceptionClear(env);
        return -1;
    }
    g_camera.svc_obj = (*env)->NewGlobalRef(env, obj);
    (*env)->CallVoidMethod(env, g_camera.svc_obj, m_init, activity_obj);
    if ((*env)->ExceptionCheck(env)) {
        LOGE("init: CameraServices.init() threw");
        (*env)->ExceptionClear(env);
    }

    int num = (*env)->CallIntMethod(env, g_camera.svc_obj, g_camera.m_getCount);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        num = 0;
    }
    if (num < 0)
        num = 0;
    if (num > MAX_CAMERAS)
        num = MAX_CAMERAS;
    g_camera.num_cameras = num;
    LOGI("init: %d camera(s)", num);

    /* Shared control pair (SOCK_STREAM: variable-length ctrl msgs). */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        LOGE("init: ctrl socketpair failed: %s", strerror(errno));
        goto fail;
    }
    g_camera.ctrl_local  = sv[0];
    g_camera.ctrl_remote = sv[1];
    g_camera.alloc_fds[0] = g_camera.ctrl_remote;

    /* Per camera: a SEQPACKET stream control pair + an ashmem double buffer. */
    for (int i = 0; i < num; i++) {
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) {
            LOGE("init: stream socketpair[%d] failed: %s", i, strerror(errno));
            goto fail;
        }
        g_camera.stream_local[i]  = sv[0];
        g_camera.stream_remote[i] = sv[1];
        g_camera.alloc_fds[1 + i] = g_camera.stream_remote[i];

        if (create_camera_shm(env, i) < 0)
            goto fail;
    }

    g_camera.running = true;
    if (pthread_create(&g_camera.io_thread, NULL, io_thread_func, NULL) != 0) {
        LOGE("init: io thread create failed");
        g_camera.running = false;
        goto fail;
    }

    g_camera.ready = true;
    LOGI("camera service initialised");
    return 0;

fail:
    if (g_camera.ctrl_local >= 0)  close(g_camera.ctrl_local);
    if (g_camera.ctrl_remote >= 0) close(g_camera.ctrl_remote);
    for (int i = 0; i < MAX_CAMERAS; i++) {
        if (g_camera.stream_local[i] >= 0)  close(g_camera.stream_local[i]);
        if (g_camera.stream_remote[i] >= 0) close(g_camera.stream_remote[i]);
        if (g_camera.shm_ptr[i])
            munmap(g_camera.shm_ptr[i], CAMERA_SLOTS * g_camera.slot_bytes[i]);
        if (g_camera.shm_fd[i] >= 0) close(g_camera.shm_fd[i]);
        for (int s = 0; s < CAMERA_SLOTS; s++)
            if (g_camera.slot_buf[i][s])
                (*env)->DeleteGlobalRef(env, g_camera.slot_buf[i][s]);
    }
    if (g_camera.svc_obj) {
        (*env)->DeleteGlobalRef(env, g_camera.svc_obj);
        g_camera.svc_obj = NULL;
    }
    memset(&g_camera, 0, sizeof(g_camera));
    g_camera.ctrl_local = g_camera.ctrl_remote = -1;
    return -1;
}

void camera_service_destroy(JNIEnv *env)
{
    if (!g_camera.ready)
        return;
    LOGI("camera service destroying");

    g_camera.ready = false;
    g_camera.running = false;
    pthread_join(g_camera.io_thread, NULL);

    if (g_camera.svc_obj) {
        (*env)->CallVoidMethod(env, g_camera.svc_obj, g_camera.m_release);
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        (*env)->DeleteGlobalRef(env, g_camera.svc_obj);
        g_camera.svc_obj = NULL;
    }

    if (g_camera.ctrl_local >= 0)  close(g_camera.ctrl_local);
    if (g_camera.ctrl_remote >= 0) close(g_camera.ctrl_remote);
    for (int i = 0; i < MAX_CAMERAS; i++) {
        if (g_camera.stream_local[i] >= 0)  close(g_camera.stream_local[i]);
        if (g_camera.stream_remote[i] >= 0) close(g_camera.stream_remote[i]);
        if (g_camera.shm_ptr[i])
            munmap(g_camera.shm_ptr[i], CAMERA_SLOTS * g_camera.slot_bytes[i]);
        if (g_camera.shm_fd[i] >= 0) close(g_camera.shm_fd[i]);
        for (int s = 0; s < CAMERA_SLOTS; s++)
            if (g_camera.slot_buf[i][s])
                (*env)->DeleteGlobalRef(env, g_camera.slot_buf[i][s]);
        if (g_camera.num_cameras > i) {
            pthread_mutex_destroy(&g_camera.slot_lock[i]);
            pthread_cond_destroy(&g_camera.slot_cond[i]);
        }
    }

    memset(&g_camera, 0, sizeof(g_camera));
    g_camera.ctrl_local = g_camera.ctrl_remote = -1;
}
