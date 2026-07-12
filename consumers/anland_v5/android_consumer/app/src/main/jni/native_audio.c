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
    volatile bool running;
    volatile bool mic_enabled;

    /* The live connection. Set by audio_set_ctx() from the render thread, read by
     * the audio threads -- same lightweight convention as the event thread's
     * s->ctx. get_audio_fd() returns -1 in fallback, so a stale-but-valid ctx just
     * yields no fd rather than misbehaving. */
    display_ctx *volatile ctx;

    pthread_t play_thread;
    pthread_t cap_thread;

    AAudioStream *play;   /* output: desktop -> speaker */
    AAudioStream *rec;    /* input:  mic -> producer    */

    /* Actual device-chosen formats, read back after the streams open. */
    int play_rate, play_channels;
    int cap_rate, cap_channels;

    /* Latency presets in ms (0 = engine default), set from the settings UI. */
    volatile int play_latency_ms;
    volatile int cap_latency_ms;
    volatile bool resend_formats;   /* a preset changed -> re-announce on the live fd */

    uint8_t rx[MAX_DGRAM];
};

static int current_fd(struct audio_bridge *b)
{
    display_ctx *ctx = b->ctx;
    return ctx ? get_audio_fd(ctx) : -1;
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
    
    if (dir == AAUDIO_DIRECTION_OUTPUT) {
        // Fix crackling: Maximize the buffer size to absorb large PulseAudio packets without dropping frames!
        int32_t capacity = AAudioStream_getBufferCapacityInFrames(stream);
        AAudioStream_setBufferSizeInFrames(stream, capacity);
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
    bool stream_paused = false;

    while (b->running) {
        int fd = current_fd(b);
        if (fd < 0) {
            had_fd = false;
            usleep(20000);
            continue;
        }

        /* Hand the producer the real device formats + latency presets for both
         * directions: once when the socket comes up (just left fallback), and again
         * whenever a preset changes so it re-sizes its PipeWire nodes live. */
        if (!had_fd || b->resend_formats) {
            b->resend_formats = false;
            send_format(fd, AUDIO_ROLE_PLAYBACK, b->play_rate, b->play_channels,
                        ms_to_frames(b->play_latency_ms, b->play_rate));
            send_format(fd, AUDIO_ROLE_CAPTURE, b->cap_rate, b->cap_channels,
                        ms_to_frames(b->cap_latency_ms, b->cap_rate));
            had_fd = true;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (poll(&pfd, 1, 200) <= 0) {
            if (b->play && !stream_paused) {
                LOGI("No audio data, pausing AAudio stream to prevent underrun");
                AAudioStream_requestPause(b->play);
                stream_paused = true;
            }
            continue;
        }
        if (pfd.revents & (POLLHUP | POLLERR)) {
            usleep(20000);
            continue;
        }

        ssize_t n = recv(fd, b->rx, sizeof(b->rx), 0);
        if (n < (ssize_t)sizeof(struct audio_msg))
            continue;

        struct audio_msg h;
        memcpy(&h, b->rx, sizeof(h));
        if (h.type != AUDIO_MSG_PCM || !b->play)
            continue;   /* the producer only sends PCM back; formats flow upstream */

        size_t avail = (size_t)n - sizeof(struct audio_msg);
        size_t bytes = h.size < avail ? h.size : avail;
        int32_t frames = (int32_t)(bytes / (sizeof(int16_t) * b->play_channels));
        if (frames <= 0)
            continue;

        if (!b->play) continue;
        if (stream_paused) {
            LOGI("Audio data received, resuming AAudio stream");
            AAudioStream_requestStart(b->play);
            stream_paused = false;
        }
        
        /* Blocking write with a larger timeout to absorb large PulseAudio chunks */
        aaudio_result_t res = AAudioStream_write(b->play, b->rx + sizeof(struct audio_msg), frames,
                           200 * 1000 * 1000L);
        if (res < 0 && res != -896 /* AAUDIO_ERROR_TIMEOUT */) {
            LOGI("AAudio playback stream died (error %d), recovering...", (int)res);
            AAudioStream_close(b->play);
            b->play = open_stream(AAUDIO_DIRECTION_OUTPUT, b->play_channels);
            if (b->play) {
                AAudioStream_requestStart(b->play);
                stream_paused = false;
            }
        }
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

    while (b->running) {
        int fd = current_fd(b);
        if (!b->mic_enabled || fd < 0) {
            if (started && b->rec) {
                AAudioStream_requestStop(b->rec);
                started = false;
            }
            usleep(20000);
            continue;
        }
        if (!b->rec) {
            usleep(20000);
            continue;
        }
        if (!started) {
            if (AAudioStream_requestStart(b->rec) != AAUDIO_OK) {
                usleep(50000);
                continue;
            }
            started = true;
        }

        int32_t got = AAudioStream_read(b->rec, buf, mic_frames, 100 * 1000 * 1000L);
        if (got < 0) {
            if (got != -896 /* AAUDIO_ERROR_TIMEOUT */) {
                LOGI("AAudio capture stream died (error %d), recovering...", (int)got);
                AAudioStream_close(b->rec);
                b->rec = open_stream(AAUDIO_DIRECTION_INPUT, b->cap_channels);
                if (b->rec) {
                    AAudioStream_requestStart(b->rec);
                    started = true;
                }
            }
            continue;
        }
        if (got == 0)
            continue;

        uint32_t bytes = (uint32_t)got * sizeof(int16_t) * b->cap_channels;
        struct audio_msg h = { .type = AUDIO_MSG_PCM, .size = bytes };
        struct iovec iov[2] = {
            { .iov_base = &h, .iov_len = sizeof(h) },
            { .iov_base = buf, .iov_len = bytes },
        };
        struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
        sendmsg(fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL);   /* drop if the socket is full */
    }

    if (started && b->rec)
        AAudioStream_requestStop(b->rec);
    LOGI("capture thread stopped");
    return NULL;
}

/* ---- public API ---- */

audio_bridge *audio_create(void)
{
    return calloc(1, sizeof(struct audio_bridge));
}

void audio_destroy(audio_bridge *b)
{
    if (!b)
        return;
    audio_stop(b);
    free(b);
}

void audio_start(audio_bridge *b)
{
    if (!b || b->running)
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

    b->running = true;
    pthread_create(&b->play_thread, NULL, play_thread_func, b);
    pthread_create(&b->cap_thread, NULL, cap_thread_func, b);
    LOGI("audio bridge started (play=%p rec=%p)", (void *)b->play, (void *)b->rec);
}

void audio_stop(audio_bridge *b)
{
    if (!b || !b->running)
        return;
    b->running = false;
    pthread_join(b->play_thread, NULL);
    pthread_join(b->cap_thread, NULL);

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
    b->ctx = NULL;
    LOGI("audio bridge stopped");
}

void audio_set_ctx(audio_bridge *b, display_ctx *ctx)
{
    if (b)
        b->ctx = ctx;
}

void audio_set_mic_enabled(audio_bridge *b, int enabled)
{
    if (!b)
        return;
    b->mic_enabled = enabled != 0;
    LOGI("mic %s", b->mic_enabled ? "enabled" : "disabled");
}

void audio_set_latency(audio_bridge *b, int speaker_ms, int mic_ms)
{
    if (!b)
        return;
    b->play_latency_ms = speaker_ms;
    b->cap_latency_ms = mic_ms;
    b->resend_formats = true;   /* picked up by the playback thread on the live fd */
    LOGI("latency preset: speaker=%dms mic=%dms", speaker_ms, mic_ms);
}
