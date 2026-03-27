/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR SigMF Utility.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <getopt.h>

#include "m2sdr_sigmf.h"

static void help(void)
{
    printf("M2SDR SigMF Info Utility.\n"
           "usage: m2sdr_sigmf [--validate] [--strict] [--ci] <sigmf-meta|sigmf-data|basename>\n");
}

static void validation_message(const char *level, const char *msg)
{
    printf("%s: %s\n", level, msg);
}

static bool annotation_is_within_capture(const struct m2sdr_sigmf_meta *meta,
                                         const struct m2sdr_sigmf_annotation *ann)
{
    unsigned i;
    uint64_t ann_end = ann->has_sample_count ? (ann->sample_start + ann->sample_count) : (ann->sample_start + 1);

    if (!meta || !ann || meta->capture_count == 0)
        return false;

    for (i = 0; i < meta->capture_count; i++) {
        uint64_t cap_start = meta->captures[i].sample_start;
        uint64_t cap_end = (i + 1u < meta->capture_count) ? meta->captures[i + 1u].sample_start : UINT64_MAX;

        if (ann->sample_start >= cap_start && ann_end <= cap_end)
            return true;
    }
    return false;
}

static int validate_sigmf(const struct m2sdr_sigmf_meta *meta, bool strict, bool ci_mode)
{
    unsigned i;
    int errors = 0;
    int warnings = 0;

    if (!meta->datatype[0]) {
        if (!ci_mode) validation_message("ERROR", "missing core:datatype");
        errors++;
    } else if (m2sdr_sigmf_format_from_datatype(meta->datatype) == (enum m2sdr_format)-1) {
        if (!ci_mode) validation_message("ERROR", "unsupported core:datatype for current m2sdr tools");
        errors++;
    }

    if (!meta->data_path[0]) {
        if (!ci_mode) validation_message("ERROR", "missing or unresolved core:dataset");
        errors++;
    } else if (access(meta->data_path, R_OK) != 0) {
        if (!ci_mode) validation_message("WARNING", "dataset file is not readable from current path");
        warnings++;
    }

    if (meta->has_sample_rate && meta->sample_rate <= 0.0) {
        if (!ci_mode) validation_message("ERROR", "core:sample_rate must be > 0");
        errors++;
    }
    if (meta->has_num_channels && meta->num_channels == 0) {
        if (!ci_mode) validation_message("ERROR", "core:num_channels must be > 0");
        errors++;
    }
    if (meta->capture_count == 0) {
        if (!ci_mode) validation_message("WARNING", "no captures[] entries found");
        warnings++;
    }

    for (i = 0; i < meta->capture_count; i++) {
        const struct m2sdr_sigmf_capture *cap = &meta->captures[i];

        if (i > 0 && cap->sample_start < meta->captures[i - 1].sample_start) {
            if (!ci_mode) validation_message("WARNING", "captures[] are not ordered by core:sample_start");
            warnings++;
            break;
        }
        if (i > 0 && cap->sample_start == meta->captures[i - 1].sample_start) {
            if (!ci_mode) validation_message("WARNING", "captures[] contain duplicate core:sample_start values");
            warnings++;
        }
        if (cap->has_header_bytes && cap->header_bytes != 0 && cap->header_bytes != 16) {
            if (!ci_mode) validation_message("ERROR", "capture core:header_bytes is unsupported by m2sdr tools");
            errors++;
        }
    }

    for (i = 0; i < meta->annotation_count; i++) {
        const struct m2sdr_sigmf_annotation *ann = &meta->annotations[i];

        if (ann->has_freq_lower_edge && ann->has_freq_upper_edge &&
            ann->freq_lower_edge > ann->freq_upper_edge) {
            if (!ci_mode) validation_message("ERROR", "annotation has freq_lower_edge > freq_upper_edge");
            errors++;
        }
        if (ann->has_sample_count && ann->sample_count == 0) {
            if (!ci_mode) validation_message("WARNING", "annotation has zero sample_count");
            warnings++;
        }
        if (meta->capture_count > 0 && ann->sample_start < meta->captures[0].sample_start) {
            if (!ci_mode) validation_message("WARNING", "annotation starts before first capture");
            warnings++;
        }
        if (meta->capture_count > 0 && !annotation_is_within_capture(meta, ann)) {
            if (!ci_mode) validation_message("WARNING", "annotation is not fully contained in any capture interval");
            warnings++;
        }
    }

    if (meta->capture_count > 1) {
        for (i = 0; i + 1u < meta->capture_count; i++) {
            uint64_t a = meta->captures[i].sample_start;
            uint64_t b = meta->captures[i + 1u].sample_start;

            if (b < a) {
                if (!ci_mode) validation_message("ERROR", "captures[] overlap or are reversed");
                errors++;
                break;
            }
            if (b > a && b - a > 1u) {
                if (!ci_mode) validation_message("WARNING", "captures[] contain uncovered sample gaps");
                warnings++;
                break;
            }
        }
    }

    if (ci_mode)
        printf("VALIDATION status=%s errors=%d warnings=%d strict=%d\n",
               (errors == 0 && (!strict || warnings == 0)) ? "ok" : "fail",
               errors, warnings, strict ? 1 : 0);
    else
        printf("Validation summary: %d error(s), %d warning(s)\n", errors, warnings);
    return (errors == 0 && (!strict || warnings == 0)) ? 0 : 1;
}

int main(int argc, char **argv)
{
    struct m2sdr_sigmf_meta meta;
    bool validate_only = false;
    bool strict = false;
    bool ci_mode = false;
    unsigned i;
    int c;
    int option_index = 0;
    static struct option options[] = {
        {"help", no_argument, NULL, 'h'},
        {"validate", no_argument, NULL, 'v'},
        {"strict", no_argument, NULL, 's'},
        {"ci", no_argument, NULL, 'c'},
        {NULL, 0, NULL, 0}
    };

    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        help();
        return 0;
    }
    opterr = 0;

    for (;;) {
        c = getopt_long(argc, argv, "hvsc", options, &option_index);
        if (c == -1)
            break;
        switch (c) {
        case 'h':
            help();
            return 0;
        case 'v':
            validate_only = true;
            break;
        case 's':
            strict = true;
            break;
        case 'c':
            ci_mode = true;
            break;
        default:
            help();
            return 1;
        }
    }

    if (optind + 1 != argc) {
        help();
        return 1;
    }

    if (m2sdr_sigmf_read(argv[optind], &meta) != 0) {
        fprintf(stderr, "Could not read SigMF metadata from %s\n", argv[optind]);
        return 1;
    }

    if (validate_only)
        return validate_sigmf(&meta, strict, ci_mode);

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
