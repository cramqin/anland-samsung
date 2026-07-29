#define _GNU_SOURCE
#include "display_consumer.h"
#include "../common/socket_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum connection_state {
    CONNECTION_FALLBACK = 0,
    CONNECTION_STARTING,
    CONNECTION_ACTIVE,
};

struct display_ctx {
    int      ctrl_fd;
    int      data_fd;
    int      buf_ready_efd;
    int      fence_fd;        /* read end of the dedicated render-done fence channel */
    int      shm_fd;
    int      audio_fd;        /* local end of the bidirectional audio socketpair (hello slot 4) */
    volatile uint32_t *shm_ptr;
    uint32_t screen_w, screen_h;
    uint32_t pixel_format;
    enum connection_state connection_state;
    uint64_t connection_generation;
    bool     buffer_pending;
    uint64_t buffer_pending_generation;
    uint64_t pending_output_generation;

    /* The ACTIVE callback may itself send data and discover a dead peer. In that
     * case enter_fallback() must not notify FALLBACK before the ACTIVE callback has
     * returned, otherwise native can observe the callbacks in reverse order and
     * publish a false transport-ready state. */
    bool activation_callback_pending;
    bool fallback_callback_pending;
    bool fallback_callback_deferred;
    bool control_dead;
    bool control_error_notified;

    /* The display lib is called concurrently: the render thread (select_dmabuf /
     * refresh_done / push_dmabufs), the event thread (poll_output_event /
     * handle_resource_request) and JNI input threads (push_input_event*). Locks have
     * a fixed order (state_lock -> resource_lock -> data_lock):
     *   - state_lock guards connection_state/generation, every fd field, shm_ptr,
     *     pending frame/output generations and the callback ordering flags.
     *   - data_lock serialises concurrent WRITES to data_fd.
     *   - resource_lock guards services/resources and keeps a service fd bundle alive
     *     across its fd-carrying data write.
     * Invariant: never acquire state_lock while holding data_lock, and never call a
     * user callback while holding either lock (callbacks can re-enter this library). */
    pthread_mutex_t state_lock;
    pthread_mutex_t resource_lock;
    pthread_mutex_t data_lock;

    /* Track which service fd bundles were sent in this connection. Every data_fd
     * write stays serialized by data_lock: immersive raw input adds a native writer
     * alongside Java pointer/key/clipboard writers, and stream writes must never
     * interleave even after service negotiation has finished. */
    uint32_t services_sent_mask;

    int              stored_fds[MAX_BUFS];
    struct buf_info  stored_infos[MAX_BUFS];
    int              stored_count;

    void (*fallback_cb)(void *);
    void (*exit_fallback_cb)(void *);
    void  *fallback_userdata;
    void  *exit_fallback_userdata;
    struct service_info *services;
    int             num_services;
    struct resources *resources;
};

/* Reset service-fd bookkeeping for a fresh connected session. Callers either hold
 * data_lock (enter_fallback) or run single-threaded at (re)connect. */
static void reset_service_send_state(struct display_ctx *ctx)
{
    ctx->services_sent_mask = 0;
}

static bool connection_is_active_locked(const struct display_ctx *ctx)
{
    return ctx->connection_state == CONNECTION_ACTIVE;
}

static bool connection_matches_locked(const struct display_ctx *ctx,
                                      enum connection_state state,
                                      uint64_t generation)
{
    return ctx->connection_state == state &&
           ctx->connection_generation == generation;
}

static int dup_cloexec(int fd)
{
    if (fd < 0)
        return -1;
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);
}

static void set_socket_timeout(int fd, int option, int seconds)
{
    struct timeval timeout = { .tv_sec = seconds, .tv_usec = 0 };
    if (fd >= 0)
        setsockopt(fd, SOL_SOCKET, option, &timeout, sizeof(timeout));
}

static bool enter_fallback_for_generation(display_ctx *ctx,
                                          uint64_t expected_generation);

void free_resources(struct display_ctx *ctx)
{
    if (!ctx)
        return;

    /* Extract one resource at a time and invoke user code outside every library lock.
     * type == -2 denotes an allocation callback currently running; invalidating that
     * sentinel makes its eventual commit discard/free the newly allocated bundle. */
    for (;;) {
        struct resources resource = { .type = -1 };
        void (*free_resource)(struct resources, void *) = NULL;
        void *userdata = NULL;

        pthread_mutex_lock(&ctx->resource_lock);
        int found = -1;
        for (int i = 0; i < ctx->num_services; i++) {
            if (ctx->resources[i].type == -2) {
                ctx->resources[i].type = -1;
                ctx->resources[i].num = 0;
                ctx->resources[i].fds = NULL;
            } else if (ctx->resources[i].type != -1) {
                found = i;
                resource = ctx->resources[i];
                free_resource = ctx->services[i].free_resource;
                userdata = ctx->services[i].userdata;
                ctx->resources[i].type = -1;
                ctx->resources[i].num = 0;
                ctx->resources[i].fds = NULL;
                break;
            }
        }
        pthread_mutex_unlock(&ctx->resource_lock);

        if (found < 0)
            break;
        if (free_resource)
            free_resource(resource, userdata);
    }
}

void allocate_services(struct display_ctx *ctx, struct service_info *services,
                       int num_services)
{
    if (!ctx || num_services < 0 || (num_services > 0 && !services))
        return;

    free_resources(ctx);
    struct resources *new_resources = num_services > 0
            ? calloc((size_t)num_services, sizeof(*new_resources)) : NULL;
    if (num_services > 0 && !new_resources)
        num_services = 0;
    for (int i = 0; i < num_services; i++) {
        new_resources[i].service_type = services[i].type;
        new_resources[i].type = -1;
    }

    pthread_mutex_lock(&ctx->resource_lock);
    free(ctx->resources);
    ctx->resources = new_resources;
    ctx->services = num_services > 0 ? services : NULL;
    ctx->num_services = num_services;
    pthread_mutex_unlock(&ctx->resource_lock);

    pthread_mutex_lock(&ctx->data_lock);
    reset_service_send_state(ctx);
    pthread_mutex_unlock(&ctx->data_lock);
}

static int send_resource_response(display_ctx *ctx, int index,
                                  uint64_t generation,
                                  const struct resources *expected)
{
    struct InputEvent event = {
        .type = INPUT_TYPE_RESOURCE,
        .resource = {
            .type = expected->service_type,
            .fdnum = expected->num,
        },
    };
    struct data_msg hdr = {
        .type = DATA_MSG_INPUT_EVENT,
        .size = sizeof(struct InputEvent),
    };
    uint8_t msg[sizeof(struct data_msg) + sizeof(struct InputEvent)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), &event, sizeof(event));

    pthread_mutex_lock(&ctx->state_lock);
    if (!connection_matches_locked(ctx, CONNECTION_ACTIVE, generation)) {
        pthread_mutex_unlock(&ctx->state_lock);
        return 0;
    }
    pthread_mutex_lock(&ctx->resource_lock);
    bool allocated = expected->type != -1;
    if (index < 0 || index >= ctx->num_services ||
        (allocated &&
         (ctx->resources[index].type != expected->type ||
          ctx->resources[index].fds != expected->fds ||
          ctx->resources[index].num != expected->num))) {
        pthread_mutex_unlock(&ctx->resource_lock);
        pthread_mutex_unlock(&ctx->state_lock);
        return 0;
    }
    pthread_mutex_lock(&ctx->data_lock);
    int fd = ctx->data_fd;
    pthread_mutex_unlock(&ctx->state_lock);

    bool ok = fd >= 0 && send_all(fd, msg, sizeof(msg)) == 0;
    if (ok && expected->num > 0) {
        struct data_msg fhdr = { .type = DATA_MSG_INPUT_EXTEND_FDS, .size = 0 };
        ok = send_fds(fd, &fhdr, sizeof(fhdr), expected->fds,
                      (int)expected->num) == 0;
    }
    if (ok && ctx->num_services > 0 && ctx->num_services < 32)
        ctx->services_sent_mask |= (1u << index);
    pthread_mutex_unlock(&ctx->data_lock);
    pthread_mutex_unlock(&ctx->resource_lock);

    if (!ok)
        enter_fallback_for_generation(ctx, generation);
    return ok ? 0 : -1;
}

void handle_resource_request(struct display_ctx *ctx, struct OutputEvent *event)
{
    if (!ctx || !event)
        return;

    uint32_t service_type = event->resources_request.type;
    struct service_info service = {0};
    struct resources previous = { .type = -1 };
    int index = -1;
    uint64_t generation;

    pthread_mutex_lock(&ctx->state_lock);
    if (!connection_is_active_locked(ctx)) {
        pthread_mutex_unlock(&ctx->state_lock);
        return;
    }
    generation = ctx->connection_generation;
    pthread_mutex_lock(&ctx->resource_lock);
    for (int i = 0; i < ctx->num_services; i++) {
        if (ctx->services[i].type == service_type &&
            ctx->resources[i].type != -2) {
            index = i;
            service = ctx->services[i];
            previous = ctx->resources[i];
            ctx->resources[i].service_type = service_type;
            ctx->resources[i].type = -2;
            ctx->resources[i].num = 0;
            ctx->resources[i].fds = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->resource_lock);
    pthread_mutex_unlock(&ctx->state_lock);
    if (index < 0)
        return;

    if (previous.type != -1 && service.free_resource)
        service.free_resource(previous, service.userdata);

    struct resources allocated = {
        .service_type = service_type,
        .type = -1,
        .num = 0,
        .fds = NULL,
    };
    if (service.allocate_resource) {
        uint32_t args[3];
        memcpy(args, event->resources_request.args, sizeof(args));
        allocated = service.allocate_resource(args, service.userdata);
    }
    allocated.service_type = service_type;
    if (allocated.type == -1 || allocated.num > 253 ||
        (allocated.num > 0 && !allocated.fds)) {
        if (allocated.type != -1 && service.free_resource)
            service.free_resource(allocated, service.userdata);
        allocated.type = -1;
        allocated.num = 0;
        allocated.fds = NULL;
    }

    pthread_mutex_lock(&ctx->state_lock);
    bool current = connection_matches_locked(ctx, CONNECTION_ACTIVE, generation);
    pthread_mutex_lock(&ctx->resource_lock);
    bool owns_slot = index < ctx->num_services &&
                     ctx->resources[index].type == -2;
    if (owns_slot && current)
        ctx->resources[index] = allocated;
    else if (owns_slot) {
        ctx->resources[index].type = -1;
        ctx->resources[index].num = 0;
        ctx->resources[index].fds = NULL;
    }
    pthread_mutex_unlock(&ctx->resource_lock);
    pthread_mutex_unlock(&ctx->state_lock);

    if (!owns_slot || !current) {
        if (allocated.type != -1 && service.free_resource)
            service.free_resource(allocated, service.userdata);
        return;
    }

    send_resource_response(ctx, index, generation, &allocated);
}
static int create_shm(display_ctx *ctx)
{
    ctx->shm_fd = memfd_create("buf_select", MFD_CLOEXEC);
    if (ctx->shm_fd < 0)
        return -1;
    if (ftruncate(ctx->shm_fd, sizeof(uint32_t)) < 0) {
        close(ctx->shm_fd);
        ctx->shm_fd = -1;
        return -1;
    }
    ctx->shm_ptr = mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE,
                        MAP_SHARED, ctx->shm_fd, 0);
    if (ctx->shm_ptr == MAP_FAILED) {
        ctx->shm_ptr = NULL;
        close(ctx->shm_fd);
        ctx->shm_fd = -1;
        return -1;
    }
    *ctx->shm_ptr = 0;
    return 0;
}

static int send_hello_fds(display_ctx *ctx)
{
    /* Three dedicated socketpairs:
     *   - data:  consumer->producer input/bufs (reverse direction reserved for future)
     *   - fence: producer->consumer render-done messages; the message itself is the
     *            "frame rendered" signal (no separate eventfd, no cross-channel ordering).
     *   - audio: full-duplex PCM -- producer writes playback, consumer writes mic.
     * We keep one end of each and hand the other to the producer. The fd slot order
     * must match the producer's pickup_fds(): { buf_ready, fence, data, shm, audio }. */
    int sv[2], fv[2], av[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fv) < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    /* SEQPACKET: each PCM/format message is one atomic datagram, so neither end can
     * desync mid-frame the way a byte stream could on a partial send/recv. */
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, av) < 0) {
        close(sv[0]);
        close(sv[1]);
        close(fv[0]);
        close(fv[1]);
        return -1;
    }
    ctx->data_fd  = sv[0];
    ctx->fence_fd = fv[0];
    ctx->audio_fd = av[0];

    /* Every stream operation must be bounded. In particular, fallback teardown and
     * native lifecycle joins must not wait forever on a peer that accepted the fds
     * but stopped draining a partial frame. */
    set_socket_timeout(ctx->data_fd, SO_SNDTIMEO, 2);
    set_socket_timeout(ctx->data_fd, SO_RCVTIMEO, 2);
    set_socket_timeout(ctx->fence_fd, SO_RCVTIMEO, 2);

    struct ctrl_msg hdr = { .type = CTRL_MSG_CONSUMER_HELLO, .size = 0 };
    int fds[5] = { ctx->buf_ready_efd, fv[1], sv[1], ctx->shm_fd, av[1] };
    int ret = send_fds(ctx->ctrl_fd, &hdr, sizeof(hdr), fds, 5);
    close(sv[1]);
    close(fv[1]);
    close(av[1]);
    if (ret == 0) {
        if (++ctx->connection_generation == 0)
            ++ctx->connection_generation;
    } else {
        /* A failed/partial HELLO corrupts the SOCK_STREAM control transaction; it is
         * not safe to keep retrying new HELLO frames on this ctrl connection. */
        ctx->control_dead = true;
    }
    return ret;
}

/* state_lock and data_lock are held, in that order. */
static void close_session_resources_locked(display_ctx *ctx)
{
    /* Owned duplicates may still be blocked in event/audio/fence code. shutdown()
     * affects the shared socket description and wakes them before the context drops
     * its original descriptor; close alone would leave those duplicates alive. */
    if (ctx->data_fd >= 0)       { shutdown(ctx->data_fd, SHUT_RDWR);  close(ctx->data_fd);  ctx->data_fd = -1; }
    if (ctx->buf_ready_efd >= 0) { close(ctx->buf_ready_efd); ctx->buf_ready_efd = -1; }
    if (ctx->fence_fd >= 0)      { shutdown(ctx->fence_fd, SHUT_RDWR); close(ctx->fence_fd); ctx->fence_fd = -1; }
    if (ctx->audio_fd >= 0)      { shutdown(ctx->audio_fd, SHUT_RDWR); close(ctx->audio_fd); ctx->audio_fd = -1; }
    if (ctx->shm_ptr) {
        volatile uint32_t *p = ctx->shm_ptr;
        ctx->shm_ptr = NULL;
        munmap((void *)p, sizeof(uint32_t));
    }
    if (ctx->shm_fd >= 0)        { close(ctx->shm_fd);        ctx->shm_fd = -1; }
}

/* Re-deposit a complete fresh fd set while remaining in FALLBACK. The caller holds
 * state_lock -> data_lock, so no writer can retain the old data fd and render code
 * cannot touch the old shm/eventfd while they are replaced. */
static bool rebuild_session_resources_locked(display_ctx *ctx)
{
    close_session_resources_locked(ctx);
    ctx->buffer_pending = false;
    ctx->buffer_pending_generation = 0;
    ctx->pending_output_generation = 0;
    reset_service_send_state(ctx);

    ctx->buf_ready_efd = eventfd(0, EFD_CLOEXEC);
    if (ctx->buf_ready_efd < 0 || create_shm(ctx) < 0 || send_hello_fds(ctx) < 0) {
        close_session_resources_locked(ctx);
        return false;
    }
    return true;
}

/* Return 0 when the complete BUFS_READY transaction was written, 1 when the
 * requested connection generation/state is stale, and -1 on transport I/O error. */
static int push_dmabufs_internal(display_ctx *ctx,
                                 enum connection_state expected_state,
                                 uint64_t expected_generation)
{
    pthread_mutex_lock(&ctx->state_lock);
    if (!connection_matches_locked(ctx, expected_state, expected_generation)) {
        pthread_mutex_unlock(&ctx->state_lock);
        return 1;
    }
    if (ctx->stored_count <= 0) {
        pthread_mutex_unlock(&ctx->state_lock);
        return 0;
    }

    /* BUFS_READY is a two-write transaction on a SOCK_STREAM channel. Keep both
     * state and data locks until the header/fds and descriptor payload are complete,
     * so input cannot split the frame and fallback cannot swap the fd underneath it. */
    pthread_mutex_lock(&ctx->data_lock);
    int fd = ctx->data_fd;
    struct data_msg dhdr = {
        .type = DATA_MSG_BUFS_READY,
        .size = ctx->stored_count * sizeof(struct buf_info),
    };
    bool ok = fd >= 0 &&
              send_fds(fd, &dhdr, sizeof(dhdr),
                       ctx->stored_fds, ctx->stored_count) == 0 &&
              send_all(fd, ctx->stored_infos,
                       ctx->stored_count * sizeof(struct buf_info)) == 0;
    pthread_mutex_unlock(&ctx->data_lock);
    pthread_mutex_unlock(&ctx->state_lock);
    return ok ? 0 : -1;
}

/* A failed STARTING transaction has already consumed FDS_READY and may have emitted
 * a partial BUFS_READY frame. Throw that session away and re-deposit a clean one,
 * but do not fire fallback_cb: the host was never told that this session was active. */
static void restart_failed_start(display_ctx *ctx, uint64_t generation)
{
    pthread_mutex_lock(&ctx->state_lock);
    if (!connection_matches_locked(ctx, CONNECTION_STARTING, generation)) {
        pthread_mutex_unlock(&ctx->state_lock);
        return;
    }
    ctx->connection_state = CONNECTION_FALLBACK;
    pthread_mutex_lock(&ctx->data_lock);
    rebuild_session_resources_locked(ctx);
    pthread_mutex_unlock(&ctx->data_lock);
    pthread_mutex_unlock(&ctx->state_lock);
}

static void notify_control_failure(display_ctx *ctx)
{
    void (*fallback_cb)(void *) = NULL;
    void *fallback_userdata = NULL;

    pthread_mutex_lock(&ctx->state_lock);
    if (!ctx->control_dead || ctx->control_error_notified ||
        ctx->activation_callback_pending || ctx->fallback_callback_pending ||
        ctx->fallback_callback_deferred) {
        pthread_mutex_unlock(&ctx->state_lock);
        return;
    }
    ctx->control_error_notified = true;
    ctx->fallback_callback_pending = true;
    ctx->connection_state = CONNECTION_FALLBACK;
    ctx->buffer_pending = false;
    ctx->buffer_pending_generation = 0;
    ctx->pending_output_generation = 0;
    fallback_cb = ctx->fallback_cb;
    fallback_userdata = ctx->fallback_userdata;

    pthread_mutex_lock(&ctx->data_lock);
    close_session_resources_locked(ctx);
    pthread_mutex_unlock(&ctx->data_lock);
    if (ctx->ctrl_fd >= 0) {
        shutdown(ctx->ctrl_fd, SHUT_RDWR);
        close(ctx->ctrl_fd);
        ctx->ctrl_fd = -1;
    }
    pthread_mutex_unlock(&ctx->state_lock);

    free_resources(ctx);
    if (fallback_cb)
        fallback_cb(fallback_userdata);

    pthread_mutex_lock(&ctx->state_lock);
    ctx->fallback_callback_pending = false;
    pthread_mutex_unlock(&ctx->state_lock);
}

static void fail_control_channel(display_ctx *ctx)
{
    pthread_mutex_lock(&ctx->state_lock);
    ctx->control_dead = true;
    pthread_mutex_unlock(&ctx->state_lock);
    notify_control_failure(ctx);
}

/* Consumer-side fallback -> active is atomic from the producer's point of view:
 * FDS_READY alone is insufficient. Keep input gated in STARTING until the complete
 * dma-buf seed transaction succeeds, then publish ACTIVE and notify the host. */
static bool try_exit_fallback(display_ctx *ctx)
{
    pthread_mutex_lock(&ctx->state_lock);
    if (ctx->control_dead) {
        pthread_mutex_unlock(&ctx->state_lock);
        notify_control_failure(ctx);
        return false;
    }
    if (ctx->connection_state == CONNECTION_FALLBACK &&
        !ctx->activation_callback_pending && !ctx->fallback_callback_pending &&
        !ctx->fallback_callback_deferred && ctx->ctrl_fd >= 0 &&
        (ctx->data_fd < 0 || ctx->fence_fd < 0 || ctx->audio_fd < 0 ||
         ctx->buf_ready_efd < 0 || ctx->shm_fd < 0 || !ctx->shm_ptr)) {
        /* A previous re-deposit can fail transiently (fd/memfd pressure). Do not
         * strand the state machine forever in FALLBACK with no session to hand to
         * the producer; the render tick is the bounded retry driver. */
        pthread_mutex_lock(&ctx->data_lock);
        rebuild_session_resources_locked(ctx);
        pthread_mutex_unlock(&ctx->data_lock);
    }
    bool can_start = ctx->connection_state == CONNECTION_FALLBACK &&
                     !ctx->control_dead &&
                     !ctx->activation_callback_pending &&
                     !ctx->fallback_callback_pending &&
                     !ctx->fallback_callback_deferred &&
                     ctx->ctrl_fd >= 0 && ctx->data_fd >= 0;
    int ctrl_fd = ctx->ctrl_fd;
    pthread_mutex_unlock(&ctx->state_lock);
    if (!can_start)
        return false;

    struct pollfd pfd = { .fd = ctrl_fd, .events = POLLIN };
    int poll_result;
    do {
        poll_result = poll(&pfd, 1, 0);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result < 0 ||
        (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
        fail_control_channel(ctx);
        return false;
    }
    if (poll_result == 0 || !(pfd.revents & POLLIN))
        return false;

    struct ctrl_msg hdr;
    if (recv_all(ctrl_fd, &hdr, sizeof(hdr)) != 0 ||
        hdr.type != CTRL_MSG_FDS_READY || hdr.size != 0) {
        fail_control_channel(ctx);
        return false;
    }

    pthread_mutex_lock(&ctx->state_lock);
    if (ctx->connection_state != CONNECTION_FALLBACK) {
        pthread_mutex_unlock(&ctx->state_lock);
        return false;
    }
    ctx->connection_state = CONNECTION_STARTING;
    uint64_t generation = ctx->connection_generation;
    reset_service_send_state(ctx);
    pthread_mutex_unlock(&ctx->state_lock);

    int seed_result = push_dmabufs_internal(ctx, CONNECTION_STARTING, generation);
    if (seed_result != 0) {
        if (seed_result < 0)
            restart_failed_start(ctx, generation);
        return false;
    }

    void (*exit_cb)(void *) = NULL;
    void *exit_userdata = NULL;
    pthread_mutex_lock(&ctx->state_lock);
    if (!connection_matches_locked(ctx, CONNECTION_STARTING, generation)) {
        pthread_mutex_unlock(&ctx->state_lock);
        return false;
    }
    ctx->connection_state = CONNECTION_ACTIVE;
    ctx->activation_callback_pending = true;
    exit_cb = ctx->exit_fallback_cb;
    exit_userdata = ctx->exit_fallback_userdata;
    pthread_mutex_unlock(&ctx->state_lock);

    if (exit_cb)
        exit_cb(exit_userdata);

    void (*deferred_cb)(void *) = NULL;
    void *deferred_userdata = NULL;
    bool run_deferred_callback = false;
    pthread_mutex_lock(&ctx->state_lock);
    ctx->activation_callback_pending = false;
    if (ctx->fallback_callback_deferred) {
        ctx->fallback_callback_deferred = false;
        ctx->fallback_callback_pending = true;
        run_deferred_callback = true;
        deferred_cb = ctx->fallback_cb;
        deferred_userdata = ctx->fallback_userdata;
    }
    bool active = connection_matches_locked(ctx, CONNECTION_ACTIVE, generation);
    pthread_mutex_unlock(&ctx->state_lock);

    if (deferred_cb)
        deferred_cb(deferred_userdata);
    if (run_deferred_callback) {
        pthread_mutex_lock(&ctx->state_lock);
        ctx->fallback_callback_pending = false;
        pthread_mutex_unlock(&ctx->state_lock);
    }
    return active;
}

static bool enter_fallback_for_generation(display_ctx *ctx,
                                          uint64_t expected_generation)
{
    void (*fallback_cb)(void *) = NULL;
    void *fallback_userdata = NULL;

    pthread_mutex_lock(&ctx->state_lock);
    if (!connection_is_active_locked(ctx) ||
        (expected_generation != 0 &&
         ctx->connection_generation != expected_generation)) {
        pthread_mutex_unlock(&ctx->state_lock);
        return false;
    }

    ctx->connection_state = CONNECTION_FALLBACK;
    ctx->buffer_pending = false;
    ctx->buffer_pending_generation = 0;
    ctx->pending_output_generation = 0;

    bool defer_callback = ctx->activation_callback_pending;
    if (defer_callback)
        ctx->fallback_callback_deferred = true;
    else {
        ctx->fallback_callback_pending = true;
        fallback_cb = ctx->fallback_cb;
        fallback_userdata = ctx->fallback_userdata;
    }

    pthread_mutex_lock(&ctx->data_lock);
    rebuild_session_resources_locked(ctx);
    pthread_mutex_unlock(&ctx->data_lock);
    if (ctx->control_dead && ctx->fallback_cb)
        ctx->control_error_notified = true;
    pthread_mutex_unlock(&ctx->state_lock);

    free_resources(ctx);
    if (fallback_cb)
        fallback_cb(fallback_userdata);
    if (!defer_callback) {
        pthread_mutex_lock(&ctx->state_lock);
        ctx->fallback_callback_pending = false;
        pthread_mutex_unlock(&ctx->state_lock);
    }
    return true;
}

int connect_to_deamon(display_ctx **out, const char *socket_path){
    return connect_to_deamon_with_fd(out, connect_unix(socket_path));
}
int connect_to_deamon_with_fd(display_ctx **out, int ctrl_fd)
{
    display_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;

    pthread_mutex_init(&ctx->state_lock, NULL);
    pthread_mutex_init(&ctx->resource_lock, NULL);
    pthread_mutex_init(&ctx->data_lock, NULL);

    ctx->ctrl_fd = -1;
    ctx->data_fd = -1;
    ctx->buf_ready_efd = -1;
    ctx->fence_fd = -1;
    ctx->shm_fd = -1;
    ctx->audio_fd = -1;
    ctx->shm_ptr = NULL;
    ctx->connection_state = CONNECTION_FALLBACK;
    ctx->connection_generation = 0;

    ctx->ctrl_fd = ctrl_fd;
    if (ctx->ctrl_fd < 0)
        goto fail;
    set_socket_timeout(ctx->ctrl_fd, SO_SNDTIMEO, 2);
    set_socket_timeout(ctx->ctrl_fd, SO_RCVTIMEO, 2);

    /* buf_ready_efd is the consumer->producer pacing eventfd; fence_fd is created as a
     * socketpair inside send_hello_fds(). */
    ctx->buf_ready_efd = eventfd(0, EFD_CLOEXEC);
    if (ctx->buf_ready_efd < 0)
        goto fail;

    if (create_shm(ctx) < 0)
        goto fail;

    if (send_hello_fds(ctx) < 0)
        goto fail;

    *out = ctx;
    return 0;

fail:
    if (ctx->shm_ptr) munmap((void *)ctx->shm_ptr, sizeof(uint32_t));
    if (ctx->shm_fd >= 0)         close(ctx->shm_fd);
    if (ctx->ctrl_fd >= 0)         close(ctx->ctrl_fd);
    if (ctx->data_fd >= 0)         close(ctx->data_fd);
    if (ctx->buf_ready_efd >= 0)   close(ctx->buf_ready_efd);
    if (ctx->fence_fd >= 0)        close(ctx->fence_fd);
    if (ctx->audio_fd >= 0)        close(ctx->audio_fd);
    pthread_mutex_destroy(&ctx->state_lock);
    pthread_mutex_destroy(&ctx->resource_lock);
    pthread_mutex_destroy(&ctx->data_lock);
    free(ctx);
    return -1;
}

void disconnect(display_ctx *ctx)
{
    if (!ctx)
        return;

    /* API contract: the owner first stops and joins render/event/input users. Take
     * the lifecycle locks as a final fence against an in-flight bounded writer; no
     * new call may begin after disconnect() itself starts. */
    pthread_mutex_lock(&ctx->state_lock);
    ctx->connection_state = CONNECTION_FALLBACK;
    pthread_mutex_lock(&ctx->data_lock);
    close_session_resources_locked(ctx);
    pthread_mutex_unlock(&ctx->data_lock);
    if (ctx->ctrl_fd >= 0) {
        close(ctx->ctrl_fd);
        ctx->ctrl_fd = -1;
    }
    pthread_mutex_unlock(&ctx->state_lock);

    free_resources(ctx);
    pthread_mutex_lock(&ctx->resource_lock);
    free(ctx->resources);
    ctx->resources = NULL;
    ctx->services = NULL;
    ctx->num_services = 0;
    pthread_mutex_unlock(&ctx->resource_lock);
    pthread_mutex_destroy(&ctx->state_lock);
    pthread_mutex_destroy(&ctx->resource_lock);
    pthread_mutex_destroy(&ctx->data_lock);
    free(ctx);
}

int set_screen_info(display_ctx *ctx, uint32_t width, uint32_t height, uint32_t format, uint32_t refresh)
{
    ctx->screen_w = width;
    ctx->screen_h = height;
    ctx->pixel_format = format;

    struct ctrl_msg hdr = { .type = CTRL_MSG_SCREEN_INFO, .size = sizeof(struct screen_info) };
    struct screen_info si = { .width = width, .height = height, .format = format, .refresh = refresh };
    uint8_t msg[sizeof(struct ctrl_msg) + sizeof(struct screen_info)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), &si, sizeof(si));
    int result = send_all(ctx->ctrl_fd, msg, sizeof(msg));
    if (result < 0) {
        pthread_mutex_lock(&ctx->state_lock);
        ctx->control_dead = true;
        pthread_mutex_unlock(&ctx->state_lock);
    }
    return result;
}

int push_dmabufs(display_ctx *ctx, const int *fds, const struct buf_info *infos, int count)
{
    if (!ctx || count < 0 || (count > 0 && (!fds || !infos)))
        return -1;
    if (count > MAX_BUFS)
        count = MAX_BUFS;

    pthread_mutex_lock(&ctx->state_lock);
    if (count > 0) {
        memcpy(ctx->stored_fds, fds, count * sizeof(int));
        memcpy(ctx->stored_infos, infos, count * sizeof(struct buf_info));
    }
    ctx->stored_count = count;
    bool active = connection_is_active_locked(ctx);
    uint64_t generation = ctx->connection_generation;
    pthread_mutex_unlock(&ctx->state_lock);

    if (!active)
        return 0;

    int ret = push_dmabufs_internal(ctx, CONNECTION_ACTIVE, generation);
    if (ret < 0)
        enter_fallback_for_generation(ctx, generation);
    return ret < 0 ? -1 : 0;
}

int select_dmabuf(display_ctx *ctx, int idx)
{
    if (!ctx)
        return -1;

    pthread_mutex_lock(&ctx->state_lock);
    enum connection_state state = ctx->connection_state;
    pthread_mutex_unlock(&ctx->state_lock);
    if (state != CONNECTION_ACTIVE) {
        if (state != CONNECTION_FALLBACK || !try_exit_fallback(ctx))
            return 0;
    }

    pthread_mutex_lock(&ctx->state_lock);
    if (!connection_is_active_locked(ctx)) {
        pthread_mutex_unlock(&ctx->state_lock);
        return 0;
    }
    if (idx < 0 || idx >= ctx->stored_count || !ctx->shm_ptr ||
        ctx->buf_ready_efd < 0) {
        pthread_mutex_unlock(&ctx->state_lock);
        return -1;
    }

    uint64_t generation = ctx->connection_generation;
    *ctx->shm_ptr = (uint32_t)idx;
    eventfd_t val = 1;
    int write_result = eventfd_write(ctx->buf_ready_efd, val);
    if (write_result == 0) {
        ctx->buffer_pending = true;
        ctx->buffer_pending_generation = generation;
    }
    pthread_mutex_unlock(&ctx->state_lock);

    if (write_result < 0) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }
    return 0;
}

/* Wait for the producer to finish the frame, then return its render-done fence so
 * the caller can hand it to ANativeWindow_queueBuffer (SurfaceFlinger waits on it
 * GPU-side before scanout). The producer sends exactly one message per frame on the
 * dedicated fence channel; the message itself is the "frame rendered" signal (no
 * separate eventfd, no cross-channel ordering) and the optional fence rides as
 * SCM_RIGHTS ancillary data. Returns the fence fd (caller owns it), or -1 if none /
 * on error. */
int refresh_done(display_ctx *ctx)
{
    if (!ctx)
        return -1;

    pthread_mutex_lock(&ctx->state_lock);
    if (!connection_is_active_locked(ctx) || !ctx->buffer_pending ||
        ctx->buffer_pending_generation != ctx->connection_generation ||
        ctx->fence_fd < 0) {
        pthread_mutex_unlock(&ctx->state_lock);
        return -1;
    }
    uint64_t generation = ctx->connection_generation;
    int fence_fd = dup_cloexec(ctx->fence_fd);
    pthread_mutex_unlock(&ctx->state_lock);
    if (fence_fd < 0) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    /* Block (with a 5s safety timeout) on the fence channel: the arrival of the
     * producer's per-frame message is the render-done signal. Timeout / no POLLIN
     * (producer stalled or gone) -> fall back so the render thread never hangs. */
    struct pollfd pfd = { .fd = fence_fd, .events = POLLIN };
    int ret;
    do {
        ret = poll(&pfd, 1, 5000);
    } while (ret < 0 && errno == EINTR);
    if (ret <= 0 || !(pfd.revents & POLLIN)) {
        close(fence_fd);
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    int rfence = -1;
    char b;
    struct iovec iov = { .iov_base = &b, .iov_len = 1 };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg;
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg.buf,
        .msg_controllen = sizeof(cmsg.buf),
    };
    /* fence_fd is a duplicate owned by this call, so fallback may close/recreate the
     * context's live fd without making us read a reused descriptor from a new
     * connection. No fence in a valid message means "ready now" (-1). */
    ssize_t n;
    do {
        n = recvmsg(fence_fd, &msg, MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);
    close(fence_fd);
    if (n <= 0) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
        c->cmsg_len >= CMSG_LEN(sizeof(int))) {
        memcpy(&rfence, CMSG_DATA(c), sizeof(int));
    }

    pthread_mutex_lock(&ctx->state_lock);
    bool current = connection_matches_locked(ctx, CONNECTION_ACTIVE, generation) &&
                   ctx->buffer_pending &&
                   ctx->buffer_pending_generation == generation;
    if (current) {
        ctx->buffer_pending = false;
        ctx->buffer_pending_generation = 0;
    }
    pthread_mutex_unlock(&ctx->state_lock);
    if (!current) {
        if (rfence >= 0)
            close(rfence);
        return -1;
    }
    return rfence;
}

static bool snapshot_active_generation(display_ctx *ctx, uint64_t *generation)
{
    pthread_mutex_lock(&ctx->state_lock);
    bool active = connection_is_active_locked(ctx);
    if (active)
        *generation = ctx->connection_generation;
    pthread_mutex_unlock(&ctx->state_lock);
    return active;
}

/* Acquire the write locks in the global state->data order and leave data_lock held.
 * Capturing the generation before message construction prevents a call queued on a
 * busy writer from leaking an old input event into a newly rebuilt session. */
static int begin_data_write(display_ctx *ctx, uint64_t generation, int *fd)
{
    pthread_mutex_lock(&ctx->state_lock);
    if (!connection_matches_locked(ctx, CONNECTION_ACTIVE, generation)) {
        pthread_mutex_unlock(&ctx->state_lock);
        return 0;
    }
    pthread_mutex_lock(&ctx->data_lock);
    *fd = ctx->data_fd;
    pthread_mutex_unlock(&ctx->state_lock);
    if (*fd < 0) {
        pthread_mutex_unlock(&ctx->data_lock);
        return -1;
    }
    return 1;
}

int push_input_event(display_ctx *ctx, const struct InputEvent *event)
{
    if (!ctx || !event)
        return -1;
    uint64_t generation;
    if (!snapshot_active_generation(ctx, &generation))
        return 0;

    struct data_msg hdr = { .type = DATA_MSG_INPUT_EVENT, .size = sizeof(struct InputEvent) };
    uint8_t msg[sizeof(struct data_msg) + sizeof(struct InputEvent)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));

    int fd;
    int begin = begin_data_write(ctx, generation, &fd);
    if (begin == 0)
        return 0;
    int r = begin > 0 ? send_all(fd, msg, sizeof(msg)) : -1;
    if (begin > 0)
        pthread_mutex_unlock(&ctx->data_lock);

    if (r < 0) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }
    return 0;
}
int push_input_event_with_length(display_ctx *ctx, const struct InputEvent *event, void* payload, size_t size)
{
    if (!ctx || !event || (size > 0 && !payload))
        return -1;
    uint64_t generation;
    if (!snapshot_active_generation(ctx, &generation))
        return 0;

    if (size > SIZE_MAX - sizeof(struct data_msg) - sizeof(struct InputEvent))
        return -1;

    struct data_msg hdr = { .type = DATA_MSG_INPUT_EVENT, .size = sizeof(struct InputEvent) };
    size_t total = sizeof(struct data_msg) + sizeof(struct InputEvent) + size;
    uint8_t *msg = (uint8_t *)malloc(total);
    if (!msg)
        return -1;
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));
    if (size > 0)
        memcpy(msg + sizeof(hdr) + sizeof(struct InputEvent), payload, size);

    int fd;
    int begin = begin_data_write(ctx, generation, &fd);
    int r = 0;
    if (begin > 0) {
        r = send_all(fd, msg, total);
        pthread_mutex_unlock(&ctx->data_lock);
    } else if (begin < 0) {
        r = -1;
    }

    free(msg);
    if (r < 0) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }
    return 0;
}
int poll_output_event(display_ctx *ctx, struct OutputEvent *event, int timeout_ms)
{
    if (!ctx || !event)
        return -1;

    pthread_mutex_lock(&ctx->state_lock);
    if (!connection_is_active_locked(ctx) || ctx->data_fd < 0) {
        pthread_mutex_unlock(&ctx->state_lock);
        return 0;
    }
    uint64_t generation = ctx->connection_generation;
    int data_fd = dup_cloexec(ctx->data_fd);
    pthread_mutex_unlock(&ctx->state_lock);
    if (data_fd < 0) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    struct pollfd pfd = { .fd = data_fd, .events = POLLIN };
    int ret;
    do {
        ret = poll(&pfd, 1, timeout_ms);
    } while (ret < 0 && errno == EINTR);
    if (ret <= 0) {
        close(data_fd);
        return 0;
    }

    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
        close(data_fd);
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    uint8_t msg_buf[sizeof(struct data_msg) + sizeof(struct OutputEvent)];
    ssize_t n;
    do {
        n = recv(data_fd, msg_buf, sizeof(msg_buf), MSG_PEEK);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        close(data_fd);
        return 0;
    }
    if (n == 0) {
        close(data_fd);
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }
    if (n < (ssize_t)sizeof(struct data_msg)) {
        close(data_fd);
        return 0;
    }

    struct data_msg hdr;
    memcpy(&hdr, msg_buf, sizeof(hdr));
    if (hdr.type != DATA_MSG_OUTPUT_EVENT || hdr.size != sizeof(struct OutputEvent)) {
        close(data_fd);
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    if (recv_all(data_fd, msg_buf,
                 sizeof(struct data_msg) + sizeof(struct OutputEvent)) != 0) {
        close(data_fd);
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }
    close(data_fd);

    pthread_mutex_lock(&ctx->state_lock);
    bool current = connection_matches_locked(ctx, CONNECTION_ACTIVE, generation);
    if (current)
        ctx->pending_output_generation = generation;
    pthread_mutex_unlock(&ctx->state_lock);
    if (!current)
        return -1;

    memcpy(event, msg_buf + sizeof(struct data_msg), sizeof(*event));
    return 1;
}
int poll_output_event_extend_data(display_ctx *ctx, void* payload, size_t size, int timeout_ms)
{
    if (!ctx || (size > 0 && !payload))
        return -1;

    pthread_mutex_lock(&ctx->state_lock);
    uint64_t generation = ctx->pending_output_generation;
    if (!connection_matches_locked(ctx, CONNECTION_ACTIVE, generation) ||
        generation == 0 || ctx->data_fd < 0) {
        pthread_mutex_unlock(&ctx->state_lock);
        return 0;
    }
    int data_fd = dup_cloexec(ctx->data_fd);
    pthread_mutex_unlock(&ctx->state_lock);
    if (data_fd < 0) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    if (size == 0) {
        close(data_fd);
        pthread_mutex_lock(&ctx->state_lock);
        if (ctx->pending_output_generation == generation)
            ctx->pending_output_generation = 0;
        pthread_mutex_unlock(&ctx->state_lock);
        return 1;
    }

    struct pollfd pfd = { .fd = data_fd, .events = POLLIN };
    int ret;
    do {
        ret = poll(&pfd, 1, timeout_ms);
    } while (ret < 0 && errno == EINTR);
    if (ret <= 0 || !(pfd.revents & POLLIN) ||
        (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
        close(data_fd);
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    if (recv_all(data_fd, payload, size) != 0) {
        close(data_fd);
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }
    close(data_fd);

    pthread_mutex_lock(&ctx->state_lock);
    bool current = connection_matches_locked(ctx, CONNECTION_ACTIVE, generation) &&
                   ctx->pending_output_generation == generation;
    if (ctx->pending_output_generation == generation)
        ctx->pending_output_generation = 0;
    pthread_mutex_unlock(&ctx->state_lock);
    if (!current)
        return -1;
    return 1;
}
int set_fallback_callback(display_ctx *ctx, void (*on_fallback)(void *), void *userdata)
{
    if (!ctx)
        return -1;
    pthread_mutex_lock(&ctx->state_lock);
    ctx->fallback_cb = on_fallback;
    ctx->fallback_userdata = userdata;
    pthread_mutex_unlock(&ctx->state_lock);
    return 0;
}

int set_exit_fallback_callback(display_ctx *ctx, void (*on_exit_fallback)(void *), void *userdata)
{
    if (!ctx)
        return -1;
    pthread_mutex_lock(&ctx->state_lock);
    ctx->exit_fallback_cb = on_exit_fallback;
    ctx->exit_fallback_userdata = userdata;
    pthread_mutex_unlock(&ctx->state_lock);
    return 0;
}

int display_is_active(display_ctx *ctx)
{
    if (!ctx)
        return 0;
    pthread_mutex_lock(&ctx->state_lock);
    bool active = connection_is_active_locked(ctx);
    pthread_mutex_unlock(&ctx->state_lock);
    return active ? 1 : 0;
}

int display_control_dead(display_ctx *ctx)
{
    if (!ctx)
        return 1;
    pthread_mutex_lock(&ctx->state_lock);
    bool dead = ctx->control_dead;
    pthread_mutex_unlock(&ctx->state_lock);
    return dead ? 1 : 0;
}

/* Return an owned duplicate so shutdown/setsockopt cannot race fallback closing and
 * reusing the context's descriptor number. The caller must close the result. */
int get_data_fd(display_ctx *ctx)
{
    if (!ctx)
        return -1;
    pthread_mutex_lock(&ctx->state_lock);
    int fd = connection_is_active_locked(ctx) ? dup_cloexec(ctx->data_fd) : -1;
    pthread_mutex_unlock(&ctx->state_lock);
    return fd;
}
/* Owned duplicate of the active audio socket, or -1 in fallback. */
int get_audio_fd(display_ctx *ctx)
{
    if (!ctx)
        return -1;
    pthread_mutex_lock(&ctx->state_lock);
    int fd = connection_is_active_locked(ctx) ? dup_cloexec(ctx->audio_fd) : -1;
    pthread_mutex_unlock(&ctx->state_lock);
    return fd;
}
//用于处理未处理的变长payload事件
void handle_unhandled_event(display_ctx *ctx, const struct OutputEvent *event)
{
    switch (event->type)
    {
    case OUTPUT_TYPE_CLIPBOARD:
        //客户端发送了一个剪贴板事件，后续会有变长数据跟随，但是库调用者没有处理这个事件，所以我们需要把后续的变长数据读掉，避免阻塞
        if (event->clipboard.size > 0) {
            void* payload = malloc(event->clipboard.size);
            if (payload) {
                poll_output_event_extend_data(ctx, payload, event->clipboard.size, 1000);
                free(payload);
            }
        }
        break;
    default:
        break;
    }
}
