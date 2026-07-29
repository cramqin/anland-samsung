#define _GNU_SOURCE
#include "native_audio.h"
#include "protocol.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <poll.h>
#include <stdatomic.h>
#include <unistd.h>

#define TAG "AnlandAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Channel-count preferences; the device may override (we read back the actuals).
 * Sample rate is never pinned -- AAudio picks the device-optimal rate and we honour
 * it, telling the producer so PipeWire matches. See protocol.h. */
#define WANT_PLAY_CHANNELS 2
#define WANT_CAP_CHANNELS  1
#define MAX_DGRAM          (64 * 1024)
#define MIC_MAX_FRAMES     1024   /* upper bound on frames per mic read */

struct audio_bridge {
    atomic_bool running;
    atomic_bool mic_enabled;

    /* The live connection. current_fd() duplicates the channel while holding this
     * mutex; audio_set_ctx(NULL) therefore waits until no thread can dereference the
     * old display_ctx, and the owned duplicate remains valid across fallback close. */
    pthread_mutex_t ctx_lock;
    display_ctx *ctx;
    uint64_t ctx_epoch;

    pthread_t play_thread;
    pthread_t cap_thread;
    bool play_thread_joinable;
    bool cap_thread_joinable;

    AAudioStream *play;   /* output: desktop -> speaker */
    AAudioStream *rec;    /* input:  mic -> producer    */

    /* Actual device-chosen formats, read back after the streams open. */
    int play_rate, play_channels;
    int cap_rate, cap_channels;

    /* Latency presets in ms (0 = engine default), set from the settings UI. */
    atomic_int play_latency_ms;
    atomic_int cap_latency_ms;
    atomic_bool resend_formats;   /* a preset changed -> re-announce on the live fd */

    uint8_t rx[MAX_DGRAM];
};

static int current_fd(struct audio_bridge *b, uint64_t *epoch)
{
    pthread_mutex_lock(&b->ctx_lock);
    display_ctx *ctx = b->ctx;
    int fd = ctx ? get_audio_fd(ctx) : -1;
    if (epoch)
        *epoch = b->ctx_epoch;
    pthread_mutex_unlock(&b->ctx_lock);
    return fd;
}

/* ---- AAudio stream helpers ---- */

static AAudioStream *open_stream(aaudio_direction_t dir, int channels)
{
    AAudioStreamBuilder *bld = NULL;
    if (AAudio_createStreamBuilder(&bld) != AAUDIO_OK || !bld)
        return NULL;

    AAudioStreamBuilder_setDirection(bld, dir);
    /* UNSPECIFIED rate: let the device pick its optimal/native rate; we read it back. */
    AAudioStreamBuilder_setSampleRate(bld, AAUDIO_UNSPECIFIED);
    AAudioStreamBuilder_setChannelCount(bld, channels);
    AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(bld, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(bld, AAUDIO_SHARING_MODE_SHARED);

    AAudioStream *stream = NULL;
    aaudio_result_t r = AAudioStreamBuilder_openStream(bld, &stream);
    AAudioStreamBuilder_delete(bld);
    if (r != AAUDIO_OK || !stream) {
        LOGE("open %s stream failed: %s",
             dir == AAUDIO_DIRECTION_OUTPUT ? "output" : "input",
             AAudio_convertResultToText(r));
        return NULL;
    }
    return stream;
}

/* Convert a latency preset (ms) to a frame count at the given rate. 0 ms -> 0 (let
 * the producer pick the default quantum). */
static uint32_t ms_to_frames(int ms, int rate)
{
    if (ms <= 0 || rate <= 0)
        return 0;
    return (uint32_t)(((long)ms * rate) / 1000);
}

/* Tell the producer the device-chosen format + latency preset for one direction. */
static void send_format(int fd, uint32_t role, uint32_t rate, uint32_t channels,
                        uint32_t quantum)
{
    struct audio_format f = {
        .rate = rate,
        .channels = channels,
        .format = AUDIO_FORMAT_S16LE,
        .role = role,
        .quantum = quantum,
    };
    struct audio_msg h = { .type = AUDIO_MSG_FORMAT, .size = sizeof(f) };
    struct iovec iov[2] = {
        { .iov_base = &h, .iov_len = sizeof(h) },
        { .iov_base = &f, .iov_len = sizeof(f) },
    };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
    sendmsg(fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL);
}

/* ---- playback: socket -> speaker ---- */

static void *play_thread_func(void *arg)
{
    struct audio_bridge *b = arg;
    LOGI("playback thread started");

    bool had_fd = false;   /* drives a one-shot format handshake per connection */
    uint64_t last_epoch = 0;

    while (atomic_load_explicit(&b->running, memory_order_acquire)) {
        uint64_t epoch = 0;
        int fd = current_fd(b, &epoch);
        if (fd < 0) {
            had_fd = false;
            usleep(20000);
            continue;
        }

        /* Hand the producer the real device formats + latency presets for both
         * directions: once when the socket comes up (just left fallback), and again
         * whenever a preset changes so it re-sizes its PipeWire nodes live. */
        bool resend = atomic_exchange_explicit(&b->resend_formats, false,
                                               memory_order_acq_rel);
        if (!had_fd || epoch != last_epoch || resend) {
            send_format(fd, AUDIO_ROLE_PLAYBACK, b->play_rate, b->play_channels,
                        ms_to_frames(atomic_load(&b->play_latency_ms), b->play_rate));
            send_format(fd, AUDIO_ROLE_CAPTURE, b->cap_rate, b->cap_channels,
                        ms_to_frames(atomic_load(&b->cap_latency_ms), b->cap_rate));
            had_fd = true;
            last_epoch = epoch;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (poll(&pfd, 1, 200) <= 0) {
            close(fd);
            continue;
        }
        if (pfd.revents & (POLLHUP | POLLERR)) {
            close(fd);
            usleep(20000);
            continue;
        }

        ssize_t n = recv(fd, b->rx, sizeof(b->rx), 0);
        if (n < (ssize_t)sizeof(struct audio_msg)) {
            close(fd);
            continue;
        }

        struct audio_msg h;
        memcpy(&h, b->rx, sizeof(h));
        if (h.type != AUDIO_MSG_PCM || !b->play) {
            close(fd);
            continue;   /* the producer only sends PCM back; formats flow upstream */
        }

        size_t avail = (size_t)n - sizeof(struct audio_msg);
        size_t bytes = h.size < avail ? h.size : avail;
        int32_t frames = (int32_t)(bytes / (sizeof(int16_t) * b->play_channels));
        if (frames <= 0) {
            close(fd);
            continue;
        }

        /* Blocking write with a short timeout: on underrun/overrun AAudio paces us;
         * we never stall the loop longer than the timeout. */
        AAudioStream_write(b->play, b->rx + sizeof(struct audio_msg), frames,
                           20 * 1000 * 1000L);
        close(fd);
    }

    LOGI("playback thread stopped");
    return NULL;
}

/* ---- capture: mic -> socket ---- */

static void *cap_thread_func(void *arg)
{
    struct audio_bridge *b = arg;
    LOGI("capture thread started");

    bool started = false;
    int16_t buf[MIC_MAX_FRAMES * WANT_CAP_CHANNELS];
    /* ~10 ms per read at the device rate, capped to the buffer. */
    int32_t mic_frames = b->cap_rate / 100;
    if (mic_frames <= 0)
        mic_frames = 1;
    if (mic_frames > MIC_MAX_FRAMES)
        mic_frames = MIC_MAX_FRAMES;

    while (atomic_load_explicit(&b->running, memory_order_acquire)) {
        int fd = current_fd(b, NULL);
        if (!atomic_load_explicit(&b->mic_enabled, memory_order_acquire) || fd < 0) {
            if (fd >= 0)
                close(fd);
            if (started && b->rec) {
                AAudioStream_requestStop(b->rec);
                started = false;
            }
            usleep(20000);
            continue;
        }
        if (!b->rec) {
            close(fd);
            usleep(20000);
            continue;
        }
        if (!started) {
            if (AAudioStream_requestStart(b->rec) != AAUDIO_OK) {
                close(fd);
                usleep(50000);
                continue;
            }
            started = true;
        }

        int32_t got = AAudioStream_read(b->rec, buf, mic_frames, 100 * 1000 * 1000L);
        if (got <= 0) {
            close(fd);
            continue;
        }

        uint32_t bytes = (uint32_t)got * sizeof(int16_t) * b->cap_channels;
        struct audio_msg h = { .type = AUDIO_MSG_PCM, .size = bytes };
        struct iovec iov[2] = {
            { .iov_base = &h, .iov_len = sizeof(h) },
            { .iov_base = buf, .iov_len = bytes },
        };
        struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
        sendmsg(fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL);   /* drop if the socket is full */
        close(fd);
    }

    if (started && b->rec)
        AAudioStream_requestStop(b->rec);
    LOGI("capture thread stopped");
    return NULL;
}

/* ---- public API ---- */

audio_bridge *audio_create(void)
{
    audio_bridge *b = calloc(1, sizeof(struct audio_bridge));
    if (b) {
        pthread_mutex_init(&b->ctx_lock, NULL);
        atomic_init(&b->running, false);
        atomic_init(&b->mic_enabled, false);
        atomic_init(&b->play_latency_ms, 0);
        atomic_init(&b->cap_latency_ms, 0);
        atomic_init(&b->resend_formats, false);
    }
    return b;
}

void audio_destroy(audio_bridge *b)
{
    if (!b)
        return;
    audio_stop(b);
    pthread_mutex_destroy(&b->ctx_lock);
    free(b);
}

void audio_start(audio_bridge *b)
{
    if (!b || atomic_load_explicit(&b->running, memory_order_acquire))
        return;

    /* Open the output stream and read back the rate/channels the device actually
     * chose -- this is the real playback capability we negotiate with the producer. */
    b->play_rate = 48000;
    b->play_channels = WANT_PLAY_CHANNELS;
    b->play = open_stream(AAUDIO_DIRECTION_OUTPUT, WANT_PLAY_CHANNELS);
    if (b->play) {
        b->play_rate = AAudioStream_getSampleRate(b->play);
        b->play_channels = AAudioStream_getChannelCount(b->play);
        AAudioStream_requestStart(b->play);
    }

    /* Open the input stream even before the mic is enabled; it is started/stopped
     * by the capture thread. May be NULL if RECORD_AUDIO is not granted. */
    b->cap_rate = 48000;
    b->cap_channels = WANT_CAP_CHANNELS;
    b->rec = open_stream(AAUDIO_DIRECTION_INPUT, WANT_CAP_CHANNELS);
    if (b->rec) {
        b->cap_rate = AAudioStream_getSampleRate(b->rec);
        b->cap_channels = AAudioStream_getChannelCount(b->rec);
    }
    LOGI("device formats: playback %d Hz x%d, capture %d Hz x%d",
         b->play_rate, b->play_channels, b->cap_rate, b->cap_channels);

    atomic_store_explicit(&b->running, true, memory_order_release);
    b->play_thread_joinable =
            pthread_create(&b->play_thread, NULL, play_thread_func, b) == 0;
    b->cap_thread_joinable =
            pthread_create(&b->cap_thread, NULL, cap_thread_func, b) == 0;
    if (!b->play_thread_joinable)
        LOGE("failed to create playback thread");
    if (!b->cap_thread_joinable)
        LOGE("failed to create capture thread");
    LOGI("audio bridge started (play=%p rec=%p)", (void *)b->play, (void *)b->rec);
}

void audio_stop(audio_bridge *b)
{
    if (!b)
        return;
    audio_set_ctx(b, NULL);
    if (!atomic_load_explicit(&b->running, memory_order_acquire))
        return;
    atomic_store_explicit(&b->running, false, memory_order_release);
    if (b->play_thread_joinable) {
        pthread_join(b->play_thread, NULL);
        b->play_thread_joinable = false;
    }
    if (b->cap_thread_joinable) {
        pthread_join(b->cap_thread, NULL);
        b->cap_thread_joinable = false;
    }

    if (b->play) {
        AAudioStream_requestStop(b->play);
        AAudioStream_close(b->play);
        b->play = NULL;
    }
    if (b->rec) {
        AAudioStream_requestStop(b->rec);
        AAudioStream_close(b->rec);
        b->rec = NULL;
    }
    LOGI("audio bridge stopped");
}

void audio_set_ctx(audio_bridge *b, display_ctx *ctx)
{
    if (!b)
        return;
    pthread_mutex_lock(&b->ctx_lock);
    b->ctx = ctx;
    if (++b->ctx_epoch == 0)
        ++b->ctx_epoch;
    if (ctx)
        atomic_store_explicit(&b->resend_formats, true, memory_order_release);
    pthread_mutex_unlock(&b->ctx_lock);
}

void audio_set_mic_enabled(audio_bridge *b, int enabled)
{
    if (!b)
        return;
    bool mic_enabled = enabled != 0;
    atomic_store_explicit(&b->mic_enabled, mic_enabled, memory_order_release);
    LOGI("mic %s", mic_enabled ? "enabled" : "disabled");
}

void audio_set_latency(audio_bridge *b, int speaker_ms, int mic_ms)
{
    if (!b)
        return;
    atomic_store(&b->play_latency_ms, speaker_ms);
    atomic_store(&b->cap_latency_ms, mic_ms);
    atomic_store_explicit(&b->resend_formats, true, memory_order_release);
    LOGI("latency preset: speaker=%dms mic=%dms", speaker_ms, mic_ms);
}
