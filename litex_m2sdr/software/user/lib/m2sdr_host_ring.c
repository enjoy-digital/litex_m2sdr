#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "m2sdr_host_ring.h"

struct m2sdr_host_ring_sync {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
};

static struct m2sdr_host_ring_sync *m2sdr_host_ring_sync(struct m2sdr_host_ring *ring)
{
    return (struct m2sdr_host_ring_sync *)ring->sync;
}

int m2sdr_host_ring_init(struct m2sdr_host_ring *ring, unsigned capacity, size_t slot_size)
{
    struct m2sdr_host_ring_sync *sync;

    if (!ring || capacity == 0 || slot_size == 0)
        return -1;

    memset(ring, 0, sizeof(*ring));
    ring->storage = calloc(capacity, slot_size);
    ring->lengths = calloc(capacity, sizeof(*ring->lengths));
    ring->metadata = calloc(capacity, sizeof(*ring->metadata));
    ring->has_metadata = calloc(capacity, sizeof(*ring->has_metadata));
    sync = calloc(1, sizeof(*sync));
    if (!ring->storage || !ring->lengths || !ring->metadata || !ring->has_metadata || !sync) {
        free(ring->storage);
        free(ring->lengths);
        free(ring->metadata);
        free(ring->has_metadata);
        free(sync);
        memset(ring, 0, sizeof(*ring));
        return -1;
    }

    pthread_mutex_init(&sync->mutex, NULL);
    pthread_cond_init(&sync->not_empty, NULL);
    pthread_cond_init(&sync->not_full, NULL);

    ring->slot_size = slot_size;
    ring->capacity = capacity;
    ring->sync = sync;
    return 0;
}

void m2sdr_host_ring_destroy(struct m2sdr_host_ring *ring)
{
    struct m2sdr_host_ring_sync *sync;

    if (!ring || !ring->sync)
        return;

    sync = m2sdr_host_ring_sync(ring);
    pthread_mutex_destroy(&sync->mutex);
    pthread_cond_destroy(&sync->not_empty);
    pthread_cond_destroy(&sync->not_full);
    free(sync);
    free(ring->storage);
    free(ring->lengths);
    free(ring->metadata);
    free(ring->has_metadata);
    memset(ring, 0, sizeof(*ring));
}

void m2sdr_host_ring_close_writer(struct m2sdr_host_ring *ring)
{
    struct m2sdr_host_ring_sync *sync;

    if (!ring || !ring->sync)
        return;
    sync = m2sdr_host_ring_sync(ring);
    pthread_mutex_lock(&sync->mutex);
    ring->writer_closed = true;
    pthread_cond_broadcast(&sync->not_empty);
    pthread_mutex_unlock(&sync->mutex);
}

void m2sdr_host_ring_stop(struct m2sdr_host_ring *ring)
{
    struct m2sdr_host_ring_sync *sync;

    if (!ring || !ring->sync)
        return;
    sync = m2sdr_host_ring_sync(ring);
    pthread_mutex_lock(&sync->mutex);
    ring->stop = true;
    pthread_cond_broadcast(&sync->not_empty);
    pthread_cond_broadcast(&sync->not_full);
    pthread_mutex_unlock(&sync->mutex);
}

int m2sdr_host_ring_push(struct m2sdr_host_ring *ring,
                         const void *data,
                         size_t len,
                         const struct m2sdr_metadata *meta,
                         bool has_meta,
                         enum m2sdr_host_ring_policy policy)
{
    struct m2sdr_host_ring_sync *sync;
    unsigned idx;

    if (!ring || !ring->sync || !data || len > ring->slot_size)
        return -1;
    sync = m2sdr_host_ring_sync(ring);
    pthread_mutex_lock(&sync->mutex);

    while (!ring->stop && ring->count == ring->capacity && policy == M2SDR_HOST_RING_BLOCK)
        pthread_cond_wait(&sync->not_full, &sync->mutex);
    if (ring->stop) {
        pthread_mutex_unlock(&sync->mutex);
        return -1;
    }

    if (ring->count == ring->capacity) {
        if (policy == M2SDR_HOST_RING_DROP_NEWEST) {
            ring->dropped++;
            pthread_mutex_unlock(&sync->mutex);
            return 1;
        }
        if (policy == M2SDR_HOST_RING_DROP_OLDEST) {
            ring->tail = (ring->tail + 1) % ring->capacity;
            ring->count--;
            ring->dropped++;
        }
    }

    idx = ring->head;
    memcpy(ring->storage + ((size_t)idx * ring->slot_size), data, len);
    ring->lengths[idx] = len;
    if (has_meta && meta) {
        ring->metadata[idx] = *meta;
        ring->has_metadata[idx] = 1;
    } else {
        memset(&ring->metadata[idx], 0, sizeof(ring->metadata[idx]));
        ring->has_metadata[idx] = 0;
    }
    ring->head = (ring->head + 1) % ring->capacity;
    ring->count++;
    if (ring->count > ring->high_watermark)
        ring->high_watermark = ring->count;

    pthread_cond_signal(&sync->not_empty);
    pthread_mutex_unlock(&sync->mutex);
    return 0;
}

int m2sdr_host_ring_pop(struct m2sdr_host_ring *ring,
                        void *dst,
                        size_t *len,
                        struct m2sdr_metadata *meta,
                        bool *has_meta)
{
    struct m2sdr_host_ring_sync *sync;
    unsigned idx;
    size_t slot_len;

    if (!ring || !ring->sync || !dst || !len)
        return -1;
    sync = m2sdr_host_ring_sync(ring);
    pthread_mutex_lock(&sync->mutex);

    while (!ring->stop && ring->count == 0 && !ring->writer_closed)
        pthread_cond_wait(&sync->not_empty, &sync->mutex);
    if (ring->stop) {
        pthread_mutex_unlock(&sync->mutex);
        return -1;
    }
    if (ring->count == 0 && ring->writer_closed) {
        pthread_mutex_unlock(&sync->mutex);
        return 1;
    }

    idx = ring->tail;
    slot_len = ring->lengths[idx];
    memcpy(dst, ring->storage + ((size_t)idx * ring->slot_size), slot_len);
    *len = slot_len;
    if (has_meta)
        *has_meta = ring->has_metadata[idx] ? true : false;
    if (meta && ring->has_metadata[idx])
        *meta = ring->metadata[idx];
    ring->tail = (ring->tail + 1) % ring->capacity;
    ring->count--;

    pthread_cond_signal(&sync->not_full);
    pthread_mutex_unlock(&sync->mutex);
    return 0;
}

unsigned m2sdr_host_ring_count(struct m2sdr_host_ring *ring)
{
    struct m2sdr_host_ring_sync *sync;
    unsigned count = 0;

    if (!ring || !ring->sync)
        return 0;
    sync = m2sdr_host_ring_sync(ring);
    pthread_mutex_lock(&sync->mutex);
    count = ring->count;
    pthread_mutex_unlock(&sync->mutex);
    return count;
}

unsigned m2sdr_host_ring_high_watermark(struct m2sdr_host_ring *ring)
{
    struct m2sdr_host_ring_sync *sync;
    unsigned high = 0;

    if (!ring || !ring->sync)
        return 0;
    sync = m2sdr_host_ring_sync(ring);
    pthread_mutex_lock(&sync->mutex);
    high = ring->high_watermark;
    pthread_mutex_unlock(&sync->mutex);
    return high;
}

uint64_t m2sdr_host_ring_dropped(struct m2sdr_host_ring *ring)
{
    struct m2sdr_host_ring_sync *sync;
    uint64_t dropped = 0;

    if (!ring || !ring->sync)
        return 0;
    sync = m2sdr_host_ring_sync(ring);
    pthread_mutex_lock(&sync->mutex);
    dropped = ring->dropped;
    pthread_mutex_unlock(&sync->mutex);
    return dropped;
}

bool m2sdr_host_ring_writer_closed(struct m2sdr_host_ring *ring)
{
    struct m2sdr_host_ring_sync *sync;
    bool closed = false;

    if (!ring || !ring->sync)
        return true;
    sync = m2sdr_host_ring_sync(ring);
    pthread_mutex_lock(&sync->mutex);
    closed = ring->writer_closed;
    pthread_mutex_unlock(&sync->mutex);
    return closed;
}
