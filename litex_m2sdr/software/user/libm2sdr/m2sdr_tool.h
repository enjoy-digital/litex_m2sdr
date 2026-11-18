/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef M2SDR_TOOL_H
#define M2SDR_TOOL_H

#include <stdint.h>

#define M2SDR_TOOL_DMA_HEADER_SYNC_WORD UINT64_C(0x5aa55aa55aa55aa5)

int m2sdr_tool_parse_dma_header(const uint8_t *buf, uint64_t *timestamp);
void m2sdr_tool_write_dma_header(uint8_t *buf, uint64_t timestamp);

#endif /* M2SDR_TOOL_H */
