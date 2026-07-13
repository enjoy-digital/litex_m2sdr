/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR SATA helpers
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "m2sdr.h"

#if defined(CSR_SATA_PHY_BASE) && defined(CSR_SATA_IDENTIFY_BASE)

/* Full 512-byte ATA IDENTIFY block. Draining all of it keeps no residue in
 * the identify FIFO for the next identify. */
#define SATA_IDENT_WORDS 256
#define SATA_DEFAULT_IDENT_TIMEOUT_MS 1000u

static int sata_read32(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val)
{
    return (m2sdr_reg_read(dev, addr, val) == 0) ? M2SDR_ERR_OK : M2SDR_ERR_IO;
}

static int sata_write32(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    return (m2sdr_reg_write(dev, addr, val) == 0) ? M2SDR_ERR_OK : M2SDR_ERR_IO;
}

static void sata_msleep(unsigned ms)
{
    usleep(ms * 1000u);
}

/* Extract a CSR sub-field given its bit offset and size. */
static bool sata_status_field(uint32_t status, unsigned offset, unsigned size)
{
    return ((status >> offset) & ((1u << size) - 1u)) != 0;
}

static void sata_decode_string(char *dst, size_t dst_len,
                               const uint16_t *words,
                               unsigned first_word,
                               unsigned word_count)
{
    size_t out = 0;

    if (!dst || dst_len == 0)
        return;

    /* ATA IDENTIFY string fields are byte-swapped: the high byte of each
     * 16-bit word holds the first character. */
    for (unsigned i = 0; i < word_count && out + 1 < dst_len; i++) {
        uint16_t word = words[first_word + i];
        dst[out++] = (char)((word >> 8) & 0xff);
        if (out + 1 < dst_len)
            dst[out++] = (char)(word & 0xff);
    }

    while (out > 0 && dst[out - 1] == ' ')
        out--;
    dst[out] = '\0';
}

/* ATA IDENTIFY integers span consecutive little-endian 16-bit words. */
static uint64_t sata_ident_u64(const uint16_t *words, unsigned first_word)
{
    return ((uint64_t)words[first_word + 0] <<  0) |
           ((uint64_t)words[first_word + 1] << 16) |
           ((uint64_t)words[first_word + 2] << 32) |
           ((uint64_t)words[first_word + 3] << 48);
}

static uint32_t sata_ident_u32(const uint16_t *words, unsigned first_word)
{
    return ((uint32_t)words[first_word + 0] <<  0) |
           ((uint32_t)words[first_word + 1] << 16);
}

static void sata_decode_identify(struct m2sdr_sata_info *info, const uint16_t *words)
{
    uint64_t lba48_sectors = sata_ident_u64(words, 100);
    uint64_t lba28_sectors = sata_ident_u32(words, 60);
    uint32_t logical_sector_size = 512;

    sata_decode_string(info->serial, sizeof(info->serial), words, 10, 10);
    sata_decode_string(info->firmware, sizeof(info->firmware), words, 23, 4);
    sata_decode_string(info->model, sizeof(info->model), words, 27, 20);

    info->sector_count = lba48_sectors ? lba48_sectors : lba28_sectors;
    if (((words[106] & (1u << 14)) != 0) &&
        ((words[106] & (1u << 15)) == 0) &&
        ((words[106] & (1u << 12)) != 0)) {
        uint64_t words_per_sector = sata_ident_u32(words, 117);
        if (words_per_sector > 0 && words_per_sector <= UINT32_MAX / 2u)
            logical_sector_size = (uint32_t)words_per_sector * 2u;
    }
    info->logical_sector_size = logical_sector_size;
}

/* Poll the identify engine's done flag; *done reflects the last read. */
static int sata_identify_wait_done(struct m2sdr_dev *dev, unsigned timeout_ms,
                                   unsigned poll_ms, bool *done)
{
    unsigned elapsed_ms = 0;
    uint32_t value = 0;

    for (;;) {
        if (sata_read32(dev, CSR_SATA_IDENTIFY_DONE_ADDR, &value) != 0)
            return M2SDR_ERR_IO;
        *done = (value & 0x1u) != 0;
        if (*done || elapsed_ms >= timeout_ms)
            return M2SDR_ERR_OK;
        sata_msleep(poll_ms);
        elapsed_ms += poll_ms;
    }
}

#endif

int m2sdr_get_sata_info(struct m2sdr_dev *dev, struct m2sdr_sata_info *info, unsigned timeout_ms)
{
#if defined(CSR_SATA_PHY_BASE) && defined(CSR_SATA_IDENTIFY_BASE)
    uint32_t value = 0;
    uint16_t words[SATA_IDENT_WORDS];
    unsigned elapsed_ms = 0;
    unsigned poll_ms = 10;
    /* Link training completes within milliseconds or not at all; only the
     * identify phases need the extended budget for drives stalled by
     * garbage collection. */
    unsigned phy_timeout_ms = timeout_ms < 1000 ? timeout_ms : 1000;
    bool idle = false;

    if (!dev || !info)
        return M2SDR_ERR_INVAL;

    memset(info, 0, sizeof(*info));
    memset(words, 0, sizeof(words));

    if (timeout_ms == 0)
        timeout_ms = SATA_DEFAULT_IDENT_TIMEOUT_MS;

    if (sata_read32(dev, CSR_SATA_PHY_ENABLE_ADDR, &value) != 0)
        return M2SDR_ERR_IO;
    info->phy_enabled = (value & 0x1u) != 0;
    if (!info->phy_enabled) {
        if (sata_write32(dev, CSR_SATA_PHY_ENABLE_ADDR, 1) != 0)
            return M2SDR_ERR_IO;
        info->phy_enabled = true;
    }

    for (;;) {
        if (sata_read32(dev, CSR_SATA_PHY_STATUS_ADDR, &info->phy_status) != 0)
            return M2SDR_ERR_IO;
        info->phy_ready  = sata_status_field(info->phy_status,
            CSR_SATA_PHY_STATUS_READY_OFFSET,      CSR_SATA_PHY_STATUS_READY_SIZE);
        info->tx_ready   = sata_status_field(info->phy_status,
            CSR_SATA_PHY_STATUS_TX_READY_OFFSET,   CSR_SATA_PHY_STATUS_TX_READY_SIZE);
        info->rx_ready   = sata_status_field(info->phy_status,
            CSR_SATA_PHY_STATUS_RX_READY_OFFSET,   CSR_SATA_PHY_STATUS_RX_READY_SIZE);
        info->ctrl_ready = sata_status_field(info->phy_status,
            CSR_SATA_PHY_STATUS_CTRL_READY_OFFSET, CSR_SATA_PHY_STATUS_CTRL_READY_SIZE);

        if (info->phy_ready)
            break;
        if (elapsed_ms >= phy_timeout_ms)
            return M2SDR_ERR_OK;
        sata_msleep(poll_ms);
        elapsed_ms += poll_ms;
    }

    /* A previous identify can still be in flight: a drive busy with garbage
     * collection can spread the PIO data over hundreds of milliseconds, and
     * the identify FSM silently ignores a start while it is receiving. Wait
     * for it to go idle instead of firing a start into a busy engine. */
    if (sata_identify_wait_done(dev, timeout_ms, poll_ms, &idle) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (!idle)
        return M2SDR_ERR_OK;

    if (sata_write32(dev, CSR_SATA_IDENTIFY_START_ADDR, 1) != 0)
        return M2SDR_ERR_IO;

    if (sata_identify_wait_done(dev, timeout_ms, poll_ms,
                                &info->identify_done) != M2SDR_ERR_OK)
        return M2SDR_ERR_IO;
    if (!info->identify_done)
        return M2SDR_ERR_OK;

    /* Drain the complete identify block. The FIFO can momentarily run empty
     * while a slow transfer is still arriving, so retry on empty until the
     * block is complete or the timeout budget is spent. Draining everything
     * also keeps no residue behind for the next identify. */
    unsigned drained  = 0;
    unsigned empty_ms = 0;
    while (drained < SATA_IDENT_WORDS && empty_ms < timeout_ms) {
        if (sata_read32(dev, CSR_SATA_IDENTIFY_SOURCE_VALID_ADDR, &value) != 0)
            return M2SDR_ERR_IO;
        if ((value & 0x1u) == 0) {
            sata_msleep(poll_ms);
            empty_ms += poll_ms;
            continue;
        }
        empty_ms = 0;
        if (sata_read32(dev, CSR_SATA_IDENTIFY_SOURCE_DATA_ADDR, &value) != 0)
            return M2SDR_ERR_IO;
        if (sata_write32(dev, CSR_SATA_IDENTIFY_SOURCE_READY_ADDR, 1) != 0)
            return M2SDR_ERR_IO;
        words[drained + 0] = (uint16_t)((value >>  0) & 0xffffu);
        words[drained + 1] = (uint16_t)((value >> 16) & 0xffffu);
        drained += 2;
    }

    /* A half-up link (e.g. right after drive hot-plug) can complete with a
     * partial identify block. Decoding the zero-filled tail silently reports
     * a bogus LBA28-sized drive, so require the full block. */
    if (drained < SATA_IDENT_WORDS) {
        info->identify_done = false;
        return M2SDR_ERR_OK;
    }

    sata_decode_identify(info, words);
    info->drive_present = info->identify_done && info->sector_count > 0;
    return M2SDR_ERR_OK;
#else
    (void)dev;
    (void)timeout_ms;
    if (info)
        memset(info, 0, sizeof(*info));
    return M2SDR_ERR_UNSUPPORTED;
#endif
}
