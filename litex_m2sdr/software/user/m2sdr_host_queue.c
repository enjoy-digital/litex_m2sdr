/* SPDX-License-Identifier: BSD-2-Clause */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_host_queue.h"

struct m2sdr_host_queue_sync {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
};

static struct m2sdr_host_queue_sync *m2sdr_host_queue_sync(struct m2sdr_host_queue *queue)
{
    return (struct m2sdr_host_queue_sync *)queue->sync;
}

int m2sdr_host_queue_init(struct m2sdr_host_queue *queue,
                          unsigned capacity,
                          size_t slot_size)
{
    struct m2sdr_host_queue_sync *sync;
    int mutex_ready = 0;
    int not_empty_ready = 0;
    int not_full_ready = 0;

    if (!queue || capacity == 0 || slot_size == 0)
        return M2SDR_HOST_QUEUE_ERROR;

    memset(queue, 0, sizeof(*queue));
    queue->storage = calloc(capacity, slot_size);
    queue->lengths = calloc(capacity, sizeof(*queue->lengths));
    queue->tags = calloc(capacity, sizeof(*queue->tags));
    sync = calloc(1, sizeof(*sync));
    if (!queue->storage || !queue->lengths || !queue->tags || !sync) {
        free(queue->storage);
        free(queue->lengths);
        free(queue->tags);
        free(sync);
        memset(queue, 0, sizeof(*queue));
        return M2SDR_HOST_QUEUE_ERROR;
    }

    if (pthread_mutex_init(&sync->mutex, NULL) != 0)
        goto fail;
    mutex_ready = 1;
    if (pthread_cond_init(&sync->not_empty, NULL) != 0)
        goto fail;
    not_empty_ready = 1;
    if (pthread_cond_init(&sync->not_full, NULL) != 0)
        goto fail;
    not_full_ready = 1;

    queue->slot_size = slot_size;
    queue->capacity = capacity;
    queue->sync = sync;
    return M2SDR_HOST_QUEUE_OK;

fail:
    if (not_full_ready)
        pthread_cond_destroy(&sync->not_full);
    if (not_empty_ready)
        pthread_cond_destroy(&sync->not_empty);
    if (mutex_ready)
        pthread_mutex_destroy(&sync->mutex);
    free(queue->storage);
    free(queue->lengths);
    free(queue->tags);
    free(sync);
    memset(queue, 0, sizeof(*queue));
    return M2SDR_HOST_QUEUE_ERROR;
}

void m2sdr_host_queue_destroy(struct m2sdr_host_queue *queue)
{
    struct m2sdr_host_queue_sync *sync;

    if (!queue || !queue->sync)
        return;

    sync = m2sdr_host_queue_sync(queue);
    pthread_mutex_destroy(&sync->mutex);
    pthread_cond_destroy(&sync->not_empty);
    pthread_cond_destroy(&sync->not_full);
    free(sync);
    free(queue->storage);
    free(queue->lengths);
    free(queue->tags);
    memset(queue, 0, sizeof(*queue));
}

void m2sdr_host_queue_close_writer(struct m2sdr_host_queue *queue)
{
    struct m2sdr_host_queue_sync *sync;

    if (!queue || !queue->sync)
        return;
    sync = m2sdr_host_queue_sync(queue);
    pthread_mutex_lock(&sync->mutex);
    queue->writer_closed = 1;
    pthread_cond_broadcast(&sync->not_empty);
    pthread_mutex_unlock(&sync->mutex);
}

void m2sdr_host_queue_stop(struct m2sdr_host_queue *queue)
{
    struct m2sdr_host_queue_sync *sync;

    if (!queue || !queue->sync)
        return;
    sync = m2sdr_host_queue_sync(queue);
    pthread_mutex_lock(&sync->mutex);
    queue->stopped = 1;
    pthread_cond_broadcast(&sync->not_empty);
    pthread_cond_broadcast(&sync->not_full);
    pthread_mutex_unlock(&sync->mutex);
}

int m2sdr_host_queue_push(struct m2sdr_host_queue *queue,
                          const void *data,
                          size_t len,
                          uint64_t tag)
{
    struct m2sdr_host_queue_sync *sync;
    unsigned idx;

    if (!queue || !queue->sync || len > queue->slot_size || (!data && len != 0))
        return M2SDR_HOST_QUEUE_ERROR;

    sync = m2sdr_host_queue_sync(queue);
    pthread_mutex_lock(&sync->mutex);

    while (!queue->stopped && queue->count == queue->capacity)
        pthread_cond_wait(&sync->not_full, &sync->mutex);

    if (queue->stopped) {
        pthread_mutex_unlock(&sync->mutex);
        return M2SDR_HOST_QUEUE_STOPPED;
    }

    idx = queue->head;
    if (len != 0)
        memcpy(queue->storage + ((size_t)idx * queue->slot_size), data, len);
    queue->lengths[idx] = len;
    queue->tags[idx] = tag;
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count++;

    pthread_cond_signal(&sync->not_empty);
    pthread_mutex_unlock(&sync->mutex);
    return M2SDR_HOST_QUEUE_OK;
}

int m2sdr_host_queue_pop(struct m2sdr_host_queue *queue,
                         void *dst,
                         size_t dst_len,
                         size_t *len,
                         uint64_t *tag)
{
    struct m2sdr_host_queue_sync *sync;
    unsigned idx;
    size_t slot_len;

    if (!queue || !queue->sync || !dst || !len)
        return M2SDR_HOST_QUEUE_ERROR;

    sync = m2sdr_host_queue_sync(queue);
    pthread_mutex_lock(&sync->mutex);

    while (!queue->stopped && queue->count == 0 && !queue->writer_closed)
        pthread_cond_wait(&sync->not_empty, &sync->mutex);

    if (queue->stopped) {
        pthread_mutex_unlock(&sync->mutex);
        return M2SDR_HOST_QUEUE_STOPPED;
    }
    if (queue->count == 0 && queue->writer_closed) {
        pthread_mutex_unlock(&sync->mutex);
        return M2SDR_HOST_QUEUE_CLOSED;
    }

    idx = queue->tail;
    slot_len = queue->lengths[idx];
    if (slot_len > dst_len) {
        pthread_mutex_unlock(&sync->mutex);
        return M2SDR_HOST_QUEUE_ERROR;
    }
    if (slot_len != 0)
        memcpy(dst, queue->storage + ((size_t)idx * queue->slot_size), slot_len);
    *len = slot_len;
    if (tag)
        *tag = queue->tags[idx];
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count--;

    pthread_cond_signal(&sync->not_full);
    pthread_mutex_unlock(&sync->mutex);
    return M2SDR_HOST_QUEUE_OK;
}

int m2sdr_host_queue_wait_count(struct m2sdr_host_queue *queue,
                                unsigned min_count)
{
    struct m2sdr_host_queue_sync *sync;

    if (!queue || !queue->sync)
        return M2SDR_HOST_QUEUE_ERROR;

    sync = m2sdr_host_queue_sync(queue);
    pthread_mutex_lock(&sync->mutex);
    while (!queue->stopped && queue->count < min_count && !queue->writer_closed)
        pthread_cond_wait(&sync->not_empty, &sync->mutex);

    if (queue->stopped) {
        pthread_mutex_unlock(&sync->mutex);
        return M2SDR_HOST_QUEUE_STOPPED;
    }
    pthread_mutex_unlock(&sync->mutex);
    return M2SDR_HOST_QUEUE_OK;
}
