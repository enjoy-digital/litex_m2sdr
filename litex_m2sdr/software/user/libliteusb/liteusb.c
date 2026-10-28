/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteUSB lib (USB3-PIPE device: EP0 Wishbone bridge + Bulk streams, libusb-1.0).
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb.h>

#include "liteusb.h"
#include "liteusb_internal.h"

#define LITEUSB_CONTROL_TIMEOUT_MS 1000

/* Helpers */
/*---------*/

static void liteusb_device_path(libusb_device *udev, char *path, size_t size)
{
    uint8_t ports[8];
    int n = libusb_get_port_numbers(udev, ports, sizeof(ports));
    int len = snprintf(path, size, "%u", libusb_get_bus_number(udev));

    for (int i = 0; i < n && len > 0 && (size_t)len < size; i++)
        len += snprintf(path + len, size - len, "%c%u", i == 0 ? '-' : '.', ports[i]);
}

static int liteusb_device_matches(libusb_device *udev, uint16_t vid, uint16_t pid,
                                  struct libusb_device_descriptor *desc)
{
    if (libusb_get_device_descriptor(udev, desc) != 0)
        return 0;
    return desc->idVendor == vid && desc->idProduct == pid;
}

static void liteusb_device_serial(libusb_device *udev, const struct libusb_device_descriptor *desc,
                                  char *serial, size_t size)
{
    libusb_device_handle *handle = NULL;

    serial[0] = '\0';
    if (!desc->iSerialNumber || libusb_open(udev, &handle) != 0)
        return;
    if (libusb_get_string_descriptor_ascii(handle, desc->iSerialNumber,
                                           (unsigned char *)serial, (int)size) < 0)
        serial[0] = '\0';
    libusb_close(handle);
}

static void liteusb_fill_info(libusb_device *udev, const struct libusb_device_descriptor *desc,
                              struct liteusb_device_info *info)
{
    memset(info, 0, sizeof(*info));
    info->vid   = desc->idVendor;
    info->pid   = desc->idProduct;
    info->speed = libusb_get_device_speed(udev);
    liteusb_device_path(udev, info->path, sizeof(info->path));
    liteusb_device_serial(udev, desc, info->serial, sizeof(info->serial));
}

/* Discovery */
/*-----------*/

int liteusb_list(uint16_t vid, uint16_t pid, struct liteusb_device_info *infos, int max_infos)
{
    libusb_context *ctx = NULL;
    libusb_device **list = NULL;
    ssize_t n;
    int found = 0;

    if (!vid)
        vid = LITEUSB_DEFAULT_VID;
    if (!pid)
        pid = LITEUSB_DEFAULT_PID;
    if (libusb_init(&ctx) != 0)
        return LIBUSB_ERROR_OTHER;

    n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n && found < max_infos; i++) {
        struct libusb_device_descriptor desc;
        if (!liteusb_device_matches(list[i], vid, pid, &desc))
            continue;
        if (infos)
            liteusb_fill_info(list[i], &desc, &infos[found]);
        found++;
    }
    if (list)
        libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return n < 0 ? (int)n : found;
}

/* Connection */
/*------------*/

struct liteusb_connection *liteusb_connect(uint16_t vid, uint16_t pid,
                                           const char *serial, const char *path)
{
    struct liteusb_connection *conn;
    libusb_device **list = NULL;
    ssize_t n;

    if (!vid)
        vid = LITEUSB_DEFAULT_VID;
    if (!pid)
        pid = LITEUSB_DEFAULT_PID;

    conn = calloc(1, sizeof(*conn));
    if (!conn)
        return NULL;
    conn->magic = LITEUSB_CONNECTION_MAGIC;
    if (libusb_init(&conn->ctx) != 0) {
        free(conn);
        return NULL;
    }

    n = libusb_get_device_list(conn->ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor desc;
        struct liteusb_device_info info;

        if (!liteusb_device_matches(list[i], vid, pid, &desc))
            continue;
        liteusb_fill_info(list[i], &desc, &info);
        if (path && path[0] && strcmp(path, info.path) != 0)
            continue;
        if (serial && serial[0] && strcmp(serial, info.serial) != 0)
            continue;
        if (libusb_open(list[i], &conn->handle) != 0) {
            conn->handle = NULL;
            continue;
        }
        conn->info = info;
        break;
    }
    if (list)
        libusb_free_device_list(list, 1);

    if (!conn->handle)
        goto fail;
    (void)libusb_set_auto_detach_kernel_driver(conn->handle, 1);
    if (libusb_claim_interface(conn->handle, LITEUSB_INTERFACE) != 0)
        goto fail;
    conn->claimed = 1;
    return conn;

fail:
    liteusb_disconnect(conn);
    return NULL;
}

void liteusb_disconnect(struct liteusb_connection *conn)
{
    if (!conn)
        return;
    if (conn->claimed)
        libusb_release_interface(conn->handle, LITEUSB_INTERFACE);
    if (conn->handle)
        libusb_close(conn->handle);
    if (conn->ctx)
        libusb_exit(conn->ctx);
    conn->magic = 0;
    free(conn);
}

int liteusb_get_info(struct liteusb_connection *conn, struct liteusb_device_info *info)
{
    if (!conn || !info)
        return LIBUSB_ERROR_INVALID_PARAM;
    *info = conn->info;
    return 0;
}

int liteusb_last_error(struct liteusb_connection *conn)
{
    return conn ? conn->last_error : LIBUSB_ERROR_INVALID_PARAM;
}

/* SoC bus/CSR accesses */
/*----------------------*/

static int liteusb_fail(struct liteusb_connection *conn, int err)
{
    conn->last_error = err;
    return err;
}

int liteusb_read_burst(struct liteusb_connection *conn, uint32_t addr, uint32_t *vals, size_t count)
{
    uint8_t data[4*LITEUSB_WB_MAX_WORDS];

    if (!conn || (!vals && count))
        return LIBUSB_ERROR_INVALID_PARAM;

    while (count) {
        size_t words = count > LITEUSB_WB_MAX_WORDS ? LITEUSB_WB_MAX_WORDS : count;
        int rc = libusb_control_transfer(conn->handle, LITEUSB_WB_READ, LITEUSB_WB_REQUEST,
                                         addr & 0xffff, (addr >> 16) & 0xffff,
                                         data, (uint16_t)(4*words), LITEUSB_CONTROL_TIMEOUT_MS);
        if (rc < 0)
            return liteusb_fail(conn, rc);
        if ((size_t)rc != 4*words)
            return liteusb_fail(conn, LIBUSB_ERROR_IO);
        for (size_t i = 0; i < words; i++)
            vals[i] = (uint32_t)data[4*i + 0] <<  0 |
                      (uint32_t)data[4*i + 1] <<  8 |
                      (uint32_t)data[4*i + 2] << 16 |
                      (uint32_t)data[4*i + 3] << 24;
        vals  += words;
        addr  += 4*words;
        count -= words;
    }
    return 0;
}

int liteusb_write_burst(struct liteusb_connection *conn, uint32_t addr, const uint32_t *vals, size_t count)
{
    uint8_t data[4*LITEUSB_WB_MAX_WORDS];

    if (!conn || (!vals && count))
        return LIBUSB_ERROR_INVALID_PARAM;

    while (count) {
        size_t words = count > LITEUSB_WB_MAX_WORDS ? LITEUSB_WB_MAX_WORDS : count;
        for (size_t i = 0; i < words; i++) {
            data[4*i + 0] = (vals[i] >>  0) & 0xff;
            data[4*i + 1] = (vals[i] >>  8) & 0xff;
            data[4*i + 2] = (vals[i] >> 16) & 0xff;
            data[4*i + 3] = (vals[i] >> 24) & 0xff;
        }
        int rc = libusb_control_transfer(conn->handle, LITEUSB_WB_WRITE, LITEUSB_WB_REQUEST,
                                         addr & 0xffff, (addr >> 16) & 0xffff,
                                         data, (uint16_t)(4*words), LITEUSB_CONTROL_TIMEOUT_MS);
        if (rc < 0)
            return liteusb_fail(conn, rc);
        if ((size_t)rc != 4*words)
            return liteusb_fail(conn, LIBUSB_ERROR_IO);
        vals  += words;
        addr  += 4*words;
        count -= words;
    }
    return 0;
}

int liteusb_read32(struct liteusb_connection *conn, uint32_t addr, uint32_t *val)
{
    return liteusb_read_burst(conn, addr, val, 1);
}

int liteusb_write32(struct liteusb_connection *conn, uint32_t addr, uint32_t val)
{
    return liteusb_write_burst(conn, addr, &val, 1);
}
