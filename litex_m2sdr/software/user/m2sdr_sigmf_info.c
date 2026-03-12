/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR SigMF Info Utility.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "m2sdr_sigmf.h"

static void help(void)
{
    printf("M2SDR SigMF Info Utility.\n"
           "usage: m2sdr_sigmf_info <sigmf-meta|sigmf-data|basename>\n");
}

int main(int argc, char **argv)
{
    struct m2sdr_sigmf_meta meta;
    unsigned i;

    if (argc != 2) {
        help();
        return 1;
    }

    if (m2sdr_sigmf_read(argv[1], &meta) != 0) {
        fprintf(stderr, "Could not read SigMF metadata from %s\n", argv[1]);
        return 1;
    }

    printf("SigMF\n");
    printf("  Meta Path         %s\n", meta.meta_path);
    printf("  Data Path         %s\n", meta.data_path);
    printf("  Datatype          %s\n", meta.datatype);
    if (meta.has_sample_rate)
        printf("  Sample Rate       %.17g\n", meta.sample_rate);
    if (meta.has_num_channels)
        printf("  Num Channels      %u\n", meta.num_channels);
    if (meta.description[0])
        printf("  Description       %s\n", meta.description);
    if (meta.author[0])
        printf("  Author            %s\n", meta.author);
    if (meta.hw[0])
        printf("  Hardware          %s\n", meta.hw);
    if (meta.recorder[0])
        printf("  Recorder          %s\n", meta.recorder);

    printf("\nCaptures (%u)\n", meta.capture_count);
    for (i = 0; i < meta.capture_count; i++) {
        const struct m2sdr_sigmf_capture *cap = &meta.captures[i];
        printf("  [%u] sample_start=%" PRIu64, i, cap->sample_start);
        if (cap->has_center_freq)
            printf(" frequency=%.17g", cap->center_freq);
        if (cap->has_header_bytes)
            printf(" header_bytes=%u", cap->header_bytes);
        printf("\n");
        if (cap->has_datetime)
            printf("      datetime=%s\n", cap->datetime);
    }

    printf("\nAnnotations (%u)\n", meta.annotation_count);
    for (i = 0; i < meta.annotation_count; i++) {
        const struct m2sdr_sigmf_annotation *ann = &meta.annotations[i];
        printf("  [%u] sample_start=%" PRIu64, i, ann->sample_start);
        if (ann->has_sample_count)
            printf(" sample_count=%" PRIu64, ann->sample_count);
        printf("\n");
        if (ann->label[0])
            printf("      label=%s\n", ann->label);
        if (ann->has_freq_lower_edge || ann->has_freq_upper_edge)
            printf("      freq=%.17g..%.17g\n", ann->freq_lower_edge, ann->freq_upper_edge);
        if (ann->comment[0])
            printf("      comment=%s\n", ann->comment);
    }

    return 0;
}
