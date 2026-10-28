/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteUSB lib: Bulk streams (buffer ring over asynchronous libusb transfers).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libusb.h>

#include "liteusb.h"
#include "liteusb_internal.h"

#define LITEUSB_STOP_TIMEOUT_MS 1000

/* Slot states. */
enum {
    SLOT_IDLE      = 0, /* RX: to submit, TX: free/being filled. */
    SLOT_SUBMITTED = 1, /* Transfer in flight. */
    SLOT_DONE      = 2, /* RX: received buffers to read. */
};

struct liteusb_slot {
    struct libusb_transfer *xfer;
    struct liteusb_stream *s;
    int state;
    size_t bufs; /* RX: received buffers, TX: filled buffers. */
};

struct liteusb_stream {
    struct liteusb_connection *conn;
    unsigned char ep;
    int is_rx;
    int started;

    size_t buf_size;
    size_t buf_count;
    size_t bufs_per_xfer;
    size_t slot_count;
    uint8_t *ring;
    int ring_dev_mem; /* Ring from libusb_dev_mem_alloc (usbfs DMA memory). */
    struct liteusb_slot *slots;

    /* Cursor (RX: read, TX: write), in slot/buffer. */
    size_t cur_slot;
    size_t cur_buf;
    int    cur_held; /* RX: a buffer was returned (consumed on the next call), TX: a buffer is being filled. */

    struct liteusb_stream_stats stats;
};

/* Helpers */
/*---------*/

static int64_t liteusb_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

/* Process libusb events while the slot transfer is in flight or until timeout (< 0: wait
 * forever), returns 0 once completed. */
static int liteusb_wait_slot(struct liteusb_stream *s, const struct liteusb_slot *slot, int timeout_ms)
{
    int64_t deadline = liteusb_now_ms() + timeout_ms;

    while (slot->state == SLOT_SUBMITTED) {
        struct timeval tv = {0, 100000};
        if (timeout_ms >= 0) {
            int64_t remaining = deadline - liteusb_now_ms();
            if (remaining <= 0) {
                /* Last non-blocking pass. */
                tv.tv_usec = 0;
                libusb_handle_events_timeout_completed(s->conn->ctx, &tv, NULL);
                break;
            }
            if (remaining < 100)
                tv.tv_usec = remaining*1000;
        }
        if (libusb_handle_events_timeout_completed(s->conn->ctx, &tv, NULL) < 0)
            break;
    }
    return slot->state == SLOT_SUBMITTED ? LIBUSB_ERROR_TIMEOUT : 0;
}

static void LIBUSB_CALL liteusb_xfer_cb(struct libusb_transfer *xfer)
{
    struct liteusb_slot *slot = xfer->user_data;
    struct liteusb_stream *s  = slot->s;

    if (xfer->status != LIBUSB_TRANSFER_COMPLETED && xfer->status != LIBUSB_TRANSFER_CANCELLED) {
        s->stats.errors++;
        s->stats.last_status = xfer->status;
    }
    if (xfer->status == LIBUSB_TRANSFER_COMPLETED)
        s->stats.transfers++;

    if (s->is_rx) {
        size_t bufs = 0;
        if (xfer->status == LIBUSB_TRANSFER_COMPLETED) {
            bufs = (size_t)xfer->actual_length / s->buf_size;
            if ((size_t)xfer->actual_length % s->buf_size)
                s->stats.partial++;
        }
        s->stats.buffers += bufs;
        s->stats.bytes   += bufs*s->buf_size;
        slot->bufs  = bufs;
        slot->state = (xfer->status == LIBUSB_TRANSFER_CANCELLED) ? SLOT_IDLE : SLOT_DONE;
    } else {
        if (xfer->status == LIBUSB_TRANSFER_COMPLETED) {
            s->stats.buffers += slot->bufs;
            s->stats.bytes   += (size_t)xfer->actual_length;
        }
        slot->bufs  = 0;
        slot->state = SLOT_IDLE;
    }
}

static int liteusb_slot_submit(struct liteusb_stream *s, size_t index, size_t bufs)
{
    struct liteusb_slot *slot = &s->slots[index];
    int rc;

    libusb_fill_bulk_transfer(slot->xfer, s->conn->handle, s->ep,
                              s->ring + index*s->bufs_per_xfer*s->buf_size,
                              (int)(bufs*s->buf_size), liteusb_xfer_cb, slot, 0);
    slot->bufs  = bufs;
    slot->state = SLOT_SUBMITTED;
    rc = libusb_submit_transfer(slot->xfer);
    if (rc != 0) {
        slot->state = s->is_rx ? SLOT_DONE : SLOT_IDLE;
        slot->bufs  = 0;
        s->stats.errors++;
        s->stats.last_status = rc;
    }
    return rc;
}

/* Open/Close */
/*------------*/

struct liteusb_stream *liteusb_stream_open(struct liteusb_connection *conn, unsigned char ep,
                                           size_t buf_size, size_t buf_count, size_t bufs_per_xfer)
{
    struct liteusb_stream *s;

    if (!conn || !buf_size || !buf_count || !bufs_per_xfer || (buf_count % bufs_per_xfer))
        return NULL;

    s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->conn          = conn;
    s->ep            = ep;
    s->is_rx         = (ep & LIBUSB_ENDPOINT_IN) ? 1 : 0;
    s->buf_size      = buf_size;
    s->buf_count     = buf_count;
    s->bufs_per_xfer = bufs_per_xfer;
    s->slot_count    = buf_count/bufs_per_xfer;

    /* DMA-able buffers from usbfs when available (zero-copy), heap otherwise. */
    s->ring = libusb_dev_mem_alloc(conn->handle, buf_size*buf_count);
    s->ring_dev_mem = s->ring != NULL;
    if (!s->ring)
        s->ring = malloc(buf_size*buf_count);
    s->slots = calloc(s->slot_count, sizeof(*s->slots));
    if (!s->ring || !s->slots)
        goto fail;
    for (size_t i = 0; i < s->slot_count; i++) {
        s->slots[i].s    = s;
        s->slots[i].xfer = libusb_alloc_transfer(0);
        if (!s->slots[i].xfer)
            goto fail;
    }
    return s;

fail:
    liteusb_stream_close(s);
    return NULL;
}

void liteusb_stream_close(struct liteusb_stream *s)
{
    if (!s)
        return;
    liteusb_stream_stop(s);
    if (s->slots) {
        for (size_t i = 0; i < s->slot_count; i++)
            if (s->slots[i].xfer)
                libusb_free_transfer(s->slots[i].xfer);
        free(s->slots);
    }
    if (s->ring_dev_mem)
        libusb_dev_mem_free(s->conn->handle, s->ring, s->buf_size*s->buf_count);
    else
        free(s->ring);
    free(s);
}

/* Start/Stop */
/*------------*/

int liteusb_stream_start(struct liteusb_stream *s)
{
    int rc;

    if (!s)
        return LIBUSB_ERROR_INVALID_PARAM;
    liteusb_stream_stop(s);

    /* CLEAR_FEATURE(ENDPOINT_HALT): host/device Sequence Numbers reset, device packets queued
     * before the stream (re)start dropped. */
    rc = libusb_clear_halt(s->conn->handle, s->ep);
    if (rc != 0)
        return rc;

    s->cur_slot = 0;
    s->cur_buf  = 0;
    s->cur_held = 0;
    for (size_t i = 0; i < s->slot_count; i++) {
        s->slots[i].state = SLOT_IDLE;
        s->slots[i].bufs  = 0;
    }
    if (s->is_rx) {
        for (size_t i = 0; i < s->slot_count; i++) {
            rc = liteusb_slot_submit(s, i, s->bufs_per_xfer);
            if (rc != 0) {
                liteusb_stream_stop(s);
                return rc;
            }
        }
    }
    s->started = 1;
    return 0;
}

int liteusb_stream_stop(struct liteusb_stream *s)
{
    int64_t deadline;
    int pending;

    if (!s || !s->slots)
        return LIBUSB_ERROR_INVALID_PARAM;

    for (size_t i = 0; i < s->slot_count; i++)
        if (s->slots[i].state == SLOT_SUBMITTED)
            libusb_cancel_transfer(s->slots[i].xfer);

    deadline = liteusb_now_ms() + LITEUSB_STOP_TIMEOUT_MS;
    do {
        struct timeval tv = {0, 10000};
        pending = 0;
        for (size_t i = 0; i < s->slot_count; i++)
            pending |= (s->slots[i].state == SLOT_SUBMITTED);
        if (!pending)
            break;
        libusb_handle_events_timeout_completed(s->conn->ctx, &tv, NULL);
    } while (liteusb_now_ms() < deadline);

    s->started  = 0;
    s->cur_held = 0;
    return pending ? LIBUSB_ERROR_TIMEOUT : 0;
}

/* RX */
/*----*/

uint8_t *liteusb_stream_next_read_buffer(struct liteusb_stream *s, int timeout_ms)
{
    struct liteusb_slot *slot;

    if (!s || !s->is_rx || !s->started)
        return NULL;

    /* Consume the previously returned buffer, resubmit its slot once fully read. */
    if (s->cur_held) {
        s->cur_held = 0;
        if (++s->cur_buf >= s->slots[s->cur_slot].bufs) {
            liteusb_slot_submit(s, s->cur_slot, s->bufs_per_xfer);
            s->cur_buf  = 0;
            s->cur_slot = (s->cur_slot + 1) % s->slot_count;
        }
    }

    for (;;) {
        slot = &s->slots[s->cur_slot];
        if (liteusb_wait_slot(s, slot, timeout_ms) != 0)
            return NULL;
        if (slot->state == SLOT_IDLE) {
            /* Cancelled/never submitted: resubmit and wait for it. */
            if (liteusb_slot_submit(s, s->cur_slot, s->bufs_per_xfer) != 0)
                return NULL;
            continue;
        }
        /* SLOT_DONE: skip empty (errored) transfers. */
        if (slot->bufs == 0) {
            if (liteusb_slot_submit(s, s->cur_slot, s->bufs_per_xfer) != 0)
                return NULL;
            s->cur_buf  = 0;
            s->cur_slot = (s->cur_slot + 1) % s->slot_count;
            if (timeout_ms == 0)
                return NULL;
            continue;
        }
        break;
    }

    s->cur_held = 1;
    return s->ring + (s->cur_slot*s->bufs_per_xfer + s->cur_buf)*s->buf_size;
}

/* TX */
/*----*/

uint8_t *liteusb_stream_next_write_buffer(struct liteusb_stream *s, int timeout_ms)
{
    struct liteusb_slot *slot;

    if (!s || s->is_rx || !s->started)
        return NULL;

    slot = &s->slots[s->cur_slot];
    if (liteusb_wait_slot(s, slot, timeout_ms) != 0)
        return NULL;

    s->cur_held = 1;
    return s->ring + (s->cur_slot*s->bufs_per_xfer + s->cur_buf)*s->buf_size;
}

int liteusb_stream_flush(struct liteusb_stream *s)
{
    int rc;

    if (!s || s->is_rx || !s->started)
        return LIBUSB_ERROR_INVALID_PARAM;
    if (s->cur_buf == 0)
        return 0;

    rc = liteusb_slot_submit(s, s->cur_slot, s->cur_buf);
    s->cur_buf  = 0;
    s->cur_slot = (s->cur_slot + 1) % s->slot_count;
    return rc;
}

int liteusb_stream_write_submit(struct liteusb_stream *s)
{
    if (!s || s->is_rx || !s->started || !s->cur_held)
        return LIBUSB_ERROR_INVALID_PARAM;

    s->cur_held = 0;
    if (++s->cur_buf < s->bufs_per_xfer) {
        /* Keep the transfers going while the slot fills. */
        struct timeval tv = {0, 0};
        libusb_handle_events_timeout_completed(s->conn->ctx, &tv, NULL);
        return 0;
    }
    return liteusb_stream_flush(s);
}

int liteusb_stream_drain(struct liteusb_stream *s, int timeout_ms)
{
    int64_t deadline;

    if (!s || s->is_rx)
        return LIBUSB_ERROR_INVALID_PARAM;
    if (s->started) {
        int rc = liteusb_stream_flush(s);
        if (rc != 0)
            return rc;
    }

    deadline = liteusb_now_ms() + timeout_ms;
    for (;;) {
        struct timeval tv = {0, 10000};
        int pending = 0;
        for (size_t i = 0; i < s->slot_count; i++)
            pending |= (s->slots[i].state == SLOT_SUBMITTED);
        if (!pending)
            return 0;
        if (timeout_ms >= 0 && liteusb_now_ms() >= deadline)
            return LIBUSB_ERROR_TIMEOUT;
        libusb_handle_events_timeout_completed(s->conn->ctx, &tv, NULL);
    }
}

/* Info */
/*------*/

size_t liteusb_stream_buffer_size(struct liteusb_stream *s)
{
    return s ? s->buf_size : 0;
}

size_t liteusb_stream_buffer_count(struct liteusb_stream *s)
{
    return s ? s->buf_count : 0;
}

uint8_t *liteusb_stream_buffer_base(struct liteusb_stream *s)
{
    return s ? s->ring : NULL;
}

void liteusb_stream_get_stats(struct liteusb_stream *s, struct liteusb_stream_stats *stats)
{
    if (!s || !stats)
        return;
    *stats = s->stats;
}
