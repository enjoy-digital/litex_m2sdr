/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

/* Includes */
/*----------*/

#include <stdlib.h>

#include "m2sdr.h"

/* Utility helpers */
/*-----------------*/

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

size_t m2sdr_samples_to_bytes(enum m2sdr_format format, unsigned num_samples)
{
    size_t sample_size = m2sdr_format_size(format);
    if (!sample_size || num_samples == 0)
        return 0;
    return sample_size * num_samples;
}

unsigned m2sdr_bytes_to_samples(enum m2sdr_format format, size_t num_bytes)
{
    size_t sample_size = m2sdr_format_size(format);
    if (!sample_size || (num_bytes % sample_size) != 0)
        return 0;
    return (unsigned)(num_bytes / sample_size);
}

void *m2sdr_alloc_buffer(enum m2sdr_format format, unsigned num_samples)
{
    size_t bytes = m2sdr_samples_to_bytes(format, num_samples);
    if (!bytes)
        return NULL;
    void *buf = NULL;
    if (posix_memalign(&buf, 64, bytes) != 0)
        return NULL;
    return buf;
}

void m2sdr_free_buffer(void *buf)
{
    free(buf);
}
