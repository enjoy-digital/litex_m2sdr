/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef M2SDR_HOST_QUEUE_H
#define M2SDR_HOST_QUEUE_H

#include <stddef.h>
#include <stdint.h>

enum m2sdr_host_queue_result {
    M2SDR_HOST_QUEUE_OK = 0,
    M2SDR_HOST_QUEUE_CLOSED = 1,
    M2SDR_HOST_QUEUE_STOPPED = -1,
    M2SDR_HOST_QUEUE_ERROR = -2,
};

struct m2sdr_host_queue {
    uint8_t *storage;
    size_t *lengths;
    uint64_t *tags;
    size_t slot_size;
    unsigned capacity;
    unsigned head;
    unsigned tail;
    unsigned count;
    int writer_closed;
    int stopped;
    void *sync;
};

int m2sdr_host_queue_init(struct m2sdr_host_queue *queue,
                          unsigned capacity,
                          size_t slot_size);
void m2sdr_host_queue_destroy(struct m2sdr_host_queue *queue);
void m2sdr_host_queue_close_writer(struct m2sdr_host_queue *queue);
void m2sdr_host_queue_stop(struct m2sdr_host_queue *queue);

int m2sdr_host_queue_push(struct m2sdr_host_queue *queue,
                          const void *data,
                          size_t len,
                          uint64_t tag);
int m2sdr_host_queue_pop(struct m2sdr_host_queue *queue,
                         void *dst,
                         size_t dst_len,
                         size_t *len,
                         uint64_t *tag);
int m2sdr_host_queue_wait_count(struct m2sdr_host_queue *queue,
                                unsigned min_count);

#endif /* M2SDR_HOST_QUEUE_H */
