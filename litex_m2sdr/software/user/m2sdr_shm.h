/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Shared Memory Ring Buffer.
 *
 * Unified SHM ring buffer used by both RX and TX streaming executables.
 * RX and TX use separate SHM files, so shared fields can have
 * direction-dependent meaning (e.g. error_count = overflow for RX,
 * underflow for TX).
 *
 * Shared Memory Header Layout (64 bytes, cache-line aligned):
 *   Bytes  0-7:   write_index (uint64_t)        — next slot to write (producer)
 *   Bytes  8-15:  read_index (uint64_t)          — next slot to read (consumer)
 *   Bytes 16-23:  error_count (uint64_t)         — RX: DMA overflows; TX: DMA underflows
 *   Bytes 24-27:  chunk_size (uint32_t)          — samples per chunk per channel
 *   Bytes 28-31:  num_slots (uint32_t)           — number of slots in ring buffer
 *   Bytes 32-33:  num_channels (uint16_t)        — 1 or 2
 *   Bytes 34-35:  flags (uint16_t)               — bit 0 = writer_done
 *   Bytes 36-39:  sample_size (uint32_t)         — bytes per sample (4 for Complex{Int16})
 *   Bytes 40-47:  buffer_stall_count (uint64_t)  — RX: buffer-full waits; TX: buffer-empty events
 *   Bytes 48-63:  reserved (16 bytes)
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 */

#ifndef M2SDR_SHM_H
#define M2SDR_SHM_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

/*
 * Header Layout
 */
#define SHM_HEADER_SIZE              64
#define SHM_OFFSET_WRITE_INDEX       0
#define SHM_OFFSET_READ_INDEX        8
#define SHM_OFFSET_ERROR_COUNT       16
#define SHM_OFFSET_CHUNK_SIZE        24
#define SHM_OFFSET_NUM_SLOTS         28
#define SHM_OFFSET_NUM_CHANNELS      32
#define SHM_OFFSET_FLAGS             34
#define SHM_OFFSET_SAMPLE_SIZE       36
#define SHM_OFFSET_BUFFER_STALL      40

#define SHM_FLAG_WRITER_DONE         (1 << 0)

/* Sample size for Complex{Int16} (I + Q, each int16) */
#define SHM_BYTES_PER_COMPLEX        4

/*
 * Ring Buffer Struct
 */
typedef struct {
    uint8_t  *base;          /* mmap base pointer */
    size_t    total_size;    /* total mmap size */
    uint32_t  num_slots;     /* number of slots */
    uint32_t  chunk_size;    /* samples per chunk per channel */
    uint32_t  chunk_bytes;   /* bytes per chunk (all channels) */
    uint16_t  num_channels;  /* 1 or 2 */
} shm_buffer_t;

/*
 * Atomic Accessors
 */
static inline uint64_t shm_load_write_index(shm_buffer_t *shm) {
    return __atomic_load_n((uint64_t *)(shm->base + SHM_OFFSET_WRITE_INDEX), __ATOMIC_ACQUIRE);
}

static inline void shm_store_write_index(shm_buffer_t *shm, uint64_t val) {
    __atomic_store_n((uint64_t *)(shm->base + SHM_OFFSET_WRITE_INDEX), val, __ATOMIC_RELEASE);
}

static inline uint64_t shm_load_read_index(shm_buffer_t *shm) {
    return __atomic_load_n((uint64_t *)(shm->base + SHM_OFFSET_READ_INDEX), __ATOMIC_ACQUIRE);
}

static inline void shm_store_read_index(shm_buffer_t *shm, uint64_t val) {
    __atomic_store_n((uint64_t *)(shm->base + SHM_OFFSET_READ_INDEX), val, __ATOMIC_RELEASE);
}

static inline uint64_t shm_load_error_count(shm_buffer_t *shm) {
    return __atomic_load_n((uint64_t *)(shm->base + SHM_OFFSET_ERROR_COUNT), __ATOMIC_RELAXED);
}

static inline void shm_store_error_count(shm_buffer_t *shm, uint64_t val) {
    __atomic_store_n((uint64_t *)(shm->base + SHM_OFFSET_ERROR_COUNT), val, __ATOMIC_RELAXED);
}

static inline uint64_t shm_load_buffer_stall(shm_buffer_t *shm) {
    return __atomic_load_n((uint64_t *)(shm->base + SHM_OFFSET_BUFFER_STALL), __ATOMIC_RELAXED);
}

static inline void shm_store_buffer_stall(shm_buffer_t *shm, uint64_t val) {
    __atomic_store_n((uint64_t *)(shm->base + SHM_OFFSET_BUFFER_STALL), val, __ATOMIC_RELAXED);
}

static inline void shm_set_flags(shm_buffer_t *shm, uint16_t flags) {
    __atomic_store_n((uint16_t *)(shm->base + SHM_OFFSET_FLAGS), flags, __ATOMIC_RELEASE);
}

static inline uint16_t shm_get_flags(shm_buffer_t *shm) {
    return __atomic_load_n((uint16_t *)(shm->base + SHM_OFFSET_FLAGS), __ATOMIC_ACQUIRE);
}

/*
 * Ring Buffer Helpers
 */

/* Get pointer to slot data */
static inline uint8_t *shm_slot_ptr(shm_buffer_t *shm, uint64_t index) {
    size_t slot_offset = SHM_HEADER_SIZE + (index % shm->num_slots) * shm->chunk_bytes;
    return shm->base + slot_offset;
}

/* Check if producer can write (ring buffer not full) */
static inline bool shm_can_write(shm_buffer_t *shm, uint64_t write_idx) {
    uint64_t read_idx = shm_load_read_index(shm);
    return (write_idx - read_idx) < shm->num_slots;
}

/* Check if consumer can read (data available) */
static inline bool shm_can_read(shm_buffer_t *shm) {
    uint64_t write_idx = shm_load_write_index(shm);
    uint64_t read_idx = shm_load_read_index(shm);
    return read_idx < write_idx;
}

/* Check if writer has signaled done */
static inline bool shm_is_writer_done(shm_buffer_t *shm) {
    return (shm_get_flags(shm) & SHM_FLAG_WRITER_DONE) != 0;
}

/* Signal writer done */
static inline void shm_set_writer_done(shm_buffer_t *shm) {
    shm_set_flags(shm, shm_get_flags(shm) | SHM_FLAG_WRITER_DONE);
}

/*
 * Create shared memory ring buffer (producer role)
 */
static inline shm_buffer_t *shm_create(const char *path, uint32_t chunk_bytes,
                                         uint16_t num_channels, double buffer_seconds,
                                         uint32_t sample_rate)
{
    shm_buffer_t *shm = calloc(1, sizeof(shm_buffer_t));
    if (!shm) {
        perror("calloc");
        return NULL;
    }

    shm->num_channels = num_channels;
    shm->chunk_bytes = chunk_bytes;
    shm->chunk_size = chunk_bytes / (SHM_BYTES_PER_COMPLEX * num_channels);

    /* Calculate number of slots for requested buffer duration */
    uint64_t bytes_per_second = (uint64_t)sample_rate * SHM_BYTES_PER_COMPLEX * num_channels;
    uint64_t total_buffer_bytes = (uint64_t)(bytes_per_second * buffer_seconds);
    shm->num_slots = (total_buffer_bytes + chunk_bytes - 1) / chunk_bytes;
    if (shm->num_slots < 16)
        shm->num_slots = 16;

    shm->total_size = SHM_HEADER_SIZE + (size_t)shm->num_slots * shm->chunk_bytes;

    /* Create/open shared memory file */
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror(path);
        free(shm);
        return NULL;
    }

    if (ftruncate(fd, shm->total_size) < 0) {
        perror("ftruncate");
        close(fd);
        free(shm);
        return NULL;
    }

    shm->base = mmap(NULL, shm->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (shm->base == MAP_FAILED) {
        perror("mmap");
        free(shm);
        return NULL;
    }

    /* Initialize header */
    memset(shm->base, 0, SHM_HEADER_SIZE);
    *(uint32_t *)(shm->base + SHM_OFFSET_CHUNK_SIZE)   = shm->chunk_size;
    *(uint32_t *)(shm->base + SHM_OFFSET_NUM_SLOTS)    = shm->num_slots;
    *(uint16_t *)(shm->base + SHM_OFFSET_NUM_CHANNELS) = num_channels;
    *(uint32_t *)(shm->base + SHM_OFFSET_SAMPLE_SIZE)  = SHM_BYTES_PER_COMPLEX;

    printf("Created shared memory ring buffer:\n");
    printf("  Path: %s\n", path);
    printf("  Chunk size: %u bytes (%u samples/channel)\n", chunk_bytes, shm->chunk_size);
    printf("  Num channels: %u\n", num_channels);
    printf("  Num slots: %u\n", shm->num_slots);
    printf("  Buffer size: %.1f MB\n", shm->total_size / 1024.0 / 1024.0);
    printf("  Buffer time: %.2f seconds\n", (double)shm->num_slots * shm->chunk_size / sample_rate);

    return shm;
}

/*
 * Open existing shared memory ring buffer (consumer role)
 */
static inline shm_buffer_t *shm_open_existing(const char *path)
{
    shm_buffer_t *shm = calloc(1, sizeof(shm_buffer_t));
    if (!shm) {
        perror("calloc");
        return NULL;
    }

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        perror(path);
        free(shm);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        free(shm);
        return NULL;
    }
    shm->total_size = st.st_size;

    shm->base = mmap(NULL, shm->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (shm->base == MAP_FAILED) {
        perror("mmap");
        free(shm);
        return NULL;
    }

    /* Read header metadata */
    shm->chunk_size   = *(uint32_t *)(shm->base + SHM_OFFSET_CHUNK_SIZE);
    shm->num_slots    = *(uint32_t *)(shm->base + SHM_OFFSET_NUM_SLOTS);
    shm->num_channels = *(uint16_t *)(shm->base + SHM_OFFSET_NUM_CHANNELS);
    uint32_t sample_size = *(uint32_t *)(shm->base + SHM_OFFSET_SAMPLE_SIZE);
    if (sample_size == 0)
        sample_size = SHM_BYTES_PER_COMPLEX; /* fallback for legacy SHM files */
    shm->chunk_bytes  = shm->chunk_size * sample_size * shm->num_channels;

    printf("Opened shared memory ring buffer:\n");
    printf("  Path: %s\n", path);
    printf("  Chunk size: %u samples/channel (%u bytes)\n", shm->chunk_size, shm->chunk_bytes);
    printf("  Num channels: %u\n", shm->num_channels);
    printf("  Num slots: %u\n", shm->num_slots);
    printf("  Sample size: %u bytes\n", sample_size);
    printf("  Total size: %.1f MB\n", shm->total_size / 1024.0 / 1024.0);

    return shm;
}

/*
 * Destroy shared memory ring buffer
 */
static inline void shm_destroy(shm_buffer_t *shm) {
    if (shm) {
        if (shm->base && shm->base != MAP_FAILED) {
            shm_set_writer_done(shm);
            munmap(shm->base, shm->total_size);
        }
        free(shm);
    }
}

#endif /* M2SDR_SHM_H */
