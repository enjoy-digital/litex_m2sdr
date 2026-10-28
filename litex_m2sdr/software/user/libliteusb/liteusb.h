/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteUSB lib (USB3-PIPE device: EP0 Wishbone bridge + Bulk streams, libusb-1.0).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef LITEUSB_H
#define LITEUSB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Defaults (gateware/usb3.py) */
/*-----------------------------*/

#define LITEUSB_DEFAULT_VID      0x1d50
#define LITEUSB_DEFAULT_PID      0x605a
#define LITEUSB_INTERFACE        0
#define LITEUSB_RX_EP            0x81 /* Bulk IN  (EP1): RFIC RX samples. */
#define LITEUSB_TX_EP            0x02 /* Bulk OUT (EP2): RFIC TX samples. */

/* EP0 Wishbone bridge (LiteX CommUSB protocol, burst extension). */
#define LITEUSB_WB_READ          0xc3 /* bmRequestType: Device-to-Host, Vendor, Other. */
#define LITEUSB_WB_WRITE         0x43 /* bmRequestType: Host-to-Device, Vendor, Other. */
#define LITEUSB_WB_REQUEST       0x00
#define LITEUSB_WB_MAX_WORDS     128  /* Words per Control transfer. */

#define LITEUSB_PATH_MAX         32
#define LITEUSB_SERIAL_MAX       64

/* Connection handles start with this (negative) magic: legacy raw handles can then be told apart
 * from Etherbone connections (starting with their socket fd, >= 0), see libm2sdr.h. */
#define LITEUSB_CONNECTION_MAGIC ((int)0xc5b30001)

/* Connection */
/*------------*/

struct liteusb_connection;

struct liteusb_device_info {
    uint16_t vid;
    uint16_t pid;
    int      speed;                       /* libusb speed (LIBUSB_SPEED_SUPER: 5Gbps). */
    char     path[LITEUSB_PATH_MAX];      /* Bus-port path, as sysfs: "2-1.3". */
    char     serial[LITEUSB_SERIAL_MAX];  /* iSerial string (empty when unavailable). */
};

static inline int liteusb_is_connection(const void *handle)
{
    return handle && *(const int *)handle == LITEUSB_CONNECTION_MAGIC;
}

/* List the matching devices (vid/pid: 0 = default), returns the number found (<= max_infos) or < 0. */
int liteusb_list(uint16_t vid, uint16_t pid, struct liteusb_device_info *infos, int max_infos);

/* Open a device (vid/pid: 0 = default, serial/path: NULL = any) and claim its interface. */
struct liteusb_connection *liteusb_connect(uint16_t vid, uint16_t pid,
                                           const char *serial, const char *path);
void liteusb_disconnect(struct liteusb_connection *conn);

int liteusb_get_info(struct liteusb_connection *conn, struct liteusb_device_info *info);
int liteusb_last_error(struct liteusb_connection *conn);

/* SoC bus/CSR accesses (byte addresses, 32-bit words), return 0 or a negative libusb error. */
int liteusb_read32(struct liteusb_connection *conn, uint32_t addr, uint32_t *val);
int liteusb_write32(struct liteusb_connection *conn, uint32_t addr, uint32_t val);
int liteusb_read_burst(struct liteusb_connection *conn, uint32_t addr, uint32_t *vals, size_t count);
int liteusb_write_burst(struct liteusb_connection *conn, uint32_t addr, const uint32_t *vals, size_t count);

/* Bulk streams */
/*--------------*/

/* Buffer ring split in slots of bufs_per_xfer buffers, each slot one libusb Bulk transfer:
 * - RX: all slots submitted; a slot is resubmitted once all its buffers have been read.
 * - TX: buffers written in order; a slot is submitted once full (or flushed).
 * Events are processed from the read/write calls (no thread). */
struct liteusb_stream;

struct liteusb_stream_stats {
    uint64_t transfers;      /* Completed transfers. */
    uint64_t buffers;        /* Buffers received (RX) or sent (TX). */
    uint64_t bytes;
    uint64_t errors;         /* Transfers completed with an error. */
    uint64_t partial;        /* RX: transfers with a partial last buffer (bytes dropped). */
    int      last_status;    /* Last libusb transfer error status. */
};

/* ep: LITEUSB_RX_EP/LITEUSB_TX_EP (direction from the endpoint address). */
struct liteusb_stream *liteusb_stream_open(struct liteusb_connection *conn, unsigned char ep,
                                           size_t buf_size, size_t buf_count, size_t bufs_per_xfer);
void liteusb_stream_close(struct liteusb_stream *s);

/* Start: endpoint halt cleared (Sequence Numbers reset, stale device packets dropped), RX slots
 * submitted. Stop: transfers cancelled. */
int liteusb_stream_start(struct liteusb_stream *s);
int liteusb_stream_stop(struct liteusb_stream *s);

/* RX: next received buffer (valid until the next call), NULL on timeout/error. */
uint8_t *liteusb_stream_next_read_buffer(struct liteusb_stream *s, int timeout_ms);

/* TX: next free buffer, NULL on timeout/error; submit it (sent once its slot is full), flush
 * (send a partially filled slot), drain (wait for all the submitted transfers). */
uint8_t *liteusb_stream_next_write_buffer(struct liteusb_stream *s, int timeout_ms);
int liteusb_stream_write_submit(struct liteusb_stream *s);
int liteusb_stream_flush(struct liteusb_stream *s);
int liteusb_stream_drain(struct liteusb_stream *s, int timeout_ms);

size_t liteusb_stream_buffer_size(struct liteusb_stream *s);
size_t liteusb_stream_buffer_count(struct liteusb_stream *s);
uint8_t *liteusb_stream_buffer_base(struct liteusb_stream *s);
void liteusb_stream_get_stats(struct liteusb_stream *s, struct liteusb_stream_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* LITEUSB_H */
