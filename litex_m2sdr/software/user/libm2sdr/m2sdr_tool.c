/* SPDX-License-Identifier: BSD-2-Clause */

#include <string.h>

#include "m2sdr_tool.h"

int m2sdr_tool_parse_dma_header(const uint8_t *buf, uint64_t *timestamp)
{
    uint64_t sync_word = 0;
    uint64_t ts = 0;

    if (!buf || !timestamp)
        return 0;

    memcpy(&sync_word, buf, sizeof(sync_word));
    if (sync_word != M2SDR_TOOL_DMA_HEADER_SYNC_WORD)
        return 0;

    memcpy(&ts, buf + 8, sizeof(ts));
    *timestamp = ts;
    return 1;
}

void m2sdr_tool_write_dma_header(uint8_t *buf, uint64_t timestamp)
{
    const uint64_t sync_word = M2SDR_TOOL_DMA_HEADER_SYNC_WORD;

    if (!buf)
        return;

    memcpy(buf, &sync_word, sizeof(sync_word));
    memcpy(buf + 8, &timestamp, sizeof(timestamp));
}
