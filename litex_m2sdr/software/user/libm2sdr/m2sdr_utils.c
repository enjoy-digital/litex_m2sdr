/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR utility helpers
 */

#include "m2sdr.h"

#include <stdlib.h>

size_t m2sdr_format_size(enum m2sdr_format format)
{
    switch (format) {
    case M2SDR_FORMAT_SC16_Q11:
        return 4;
    case M2SDR_FORMAT_SC8_Q7:
        return 2;
    default:
        return 0;
    }
}

void *m2sdr_alloc_buffer(enum m2sdr_format format, unsigned num_samples)
{
    size_t sz = m2sdr_format_size(format);
    if (!sz || num_samples == 0)
        return NULL;
    size_t bytes = sz * num_samples;
    void *buf = NULL;
    if (posix_memalign(&buf, 64, bytes) != 0)
        return NULL;
    return buf;
}

void m2sdr_free_buffer(void *buf)
{
    free(buf);
}
