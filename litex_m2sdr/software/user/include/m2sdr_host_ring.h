#ifndef M2SDR_HOST_RING_H
#define M2SDR_HOST_RING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "m2sdr.h"

enum m2sdr_host_ring_policy {
    M2SDR_HOST_RING_BLOCK = 0,
    M2SDR_HOST_RING_DROP_OLDEST,
    M2SDR_HOST_RING_DROP_NEWEST,
};

struct m2sdr_host_ring {
    uint8_t *storage;
    size_t *lengths;
    struct m2sdr_metadata *metadata;
    uint8_t *has_metadata;
    size_t slot_size;
    unsigned capacity;
    unsigned head;
    unsigned tail;
    unsigned count;
    unsigned high_watermark;
    uint64_t dropped;
    bool writer_closed;
    bool stop;
    void *sync;
};

int m2sdr_host_ring_init(struct m2sdr_host_ring *ring, unsigned capacity, size_t slot_size);
void m2sdr_host_ring_destroy(struct m2sdr_host_ring *ring);
void m2sdr_host_ring_close_writer(struct m2sdr_host_ring *ring);
void m2sdr_host_ring_stop(struct m2sdr_host_ring *ring);

int m2sdr_host_ring_push(struct m2sdr_host_ring *ring,
                         const void *data,
                         size_t len,
                         const struct m2sdr_metadata *meta,
                         bool has_meta,
                         enum m2sdr_host_ring_policy policy);

int m2sdr_host_ring_pop(struct m2sdr_host_ring *ring,
                        void *dst,
                        size_t *len,
                        struct m2sdr_metadata *meta,
                        bool *has_meta);

unsigned m2sdr_host_ring_count(struct m2sdr_host_ring *ring);
unsigned m2sdr_host_ring_high_watermark(struct m2sdr_host_ring *ring);
uint64_t m2sdr_host_ring_dropped(struct m2sdr_host_ring *ring);
bool m2sdr_host_ring_writer_closed(struct m2sdr_host_ring *ring);

#endif
