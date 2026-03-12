/* SPDX-License-Identifier: BSD-2-Clause */

#include "m2sdr_sigmf.h"
#include "m2sdr_json.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has_suffix(const char *path, const char *suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);

    if (path_len < suffix_len)
        return 0;
    return strcmp(path + path_len - suffix_len, suffix) == 0;
}

static int is_absolute_path(const char *path)
{
    return path && path[0] == '/';
}

static void dirname_of(const char *path, char *out, size_t out_len)
{
    const char *slash;

    if (!path || !out || out_len == 0)
        return;
    slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, out_len, ".");
        return;
    }
    if (slash == path) {
        snprintf(out, out_len, "/");
        return;
    }
    snprintf(out, out_len, "%.*s", (int)(slash - path), path);
}

const char *m2sdr_sigmf_datatype_from_format(enum m2sdr_format format)
{
    switch (format) {
    case M2SDR_FORMAT_SC16_Q11:
        return "ci16_le";
    case M2SDR_FORMAT_SC8_Q7:
        return "ci8";
    default:
        return NULL;
    }
}

enum m2sdr_format m2sdr_sigmf_format_from_datatype(const char *datatype)
{
    if (!datatype)
        return (enum m2sdr_format)-1;
    if (strcmp(datatype, "ci16_le") == 0)
        return M2SDR_FORMAT_SC16_Q11;
    if (strcmp(datatype, "ci8") == 0)
        return M2SDR_FORMAT_SC8_Q7;
    return (enum m2sdr_format)-1;
}

int m2sdr_sigmf_is_meta_path(const char *path)
{
    return path && has_suffix(path, ".sigmf-meta");
}

int m2sdr_sigmf_is_data_path(const char *path)
{
    return path && has_suffix(path, ".sigmf-data");
}

int m2sdr_sigmf_derive_paths(const char *input_path,
                             char *data_path,
                             size_t data_path_len,
                             char *meta_path,
                             size_t meta_path_len)
{
    size_t len;

    if (!input_path || !data_path || !meta_path)
        return -1;

    if (m2sdr_sigmf_is_meta_path(input_path)) {
        len = strlen(input_path) - strlen(".sigmf-meta");
        if (snprintf(data_path, data_path_len, "%.*s.sigmf-data", (int)len, input_path) >= (int)data_path_len)
            return -1;
        if (snprintf(meta_path, meta_path_len, "%s", input_path) >= (int)meta_path_len)
            return -1;
        return 0;
    }

    if (m2sdr_sigmf_is_data_path(input_path)) {
        len = strlen(input_path) - strlen(".sigmf-data");
        if (snprintf(data_path, data_path_len, "%s", input_path) >= (int)data_path_len)
            return -1;
        if (snprintf(meta_path, meta_path_len, "%.*s.sigmf-meta", (int)len, input_path) >= (int)meta_path_len)
            return -1;
        return 0;
    }

    if (snprintf(data_path, data_path_len, "%s.sigmf-data", input_path) >= (int)data_path_len)
        return -1;
    if (snprintf(meta_path, meta_path_len, "%s.sigmf-meta", input_path) >= (int)meta_path_len)
        return -1;
    return 0;
}

static void json_write_escaped(FILE *f, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    fputc('"', f);
    while (*p) {
        switch (*p) {
        case '\\':
        case '"':
            fputc('\\', f);
            fputc(*p, f);
            break;
        case '\n':
            fputs("\\n", f);
            break;
        case '\r':
            fputs("\\r", f);
            break;
        case '\t':
            fputs("\\t", f);
            break;
        default:
            if (*p < 0x20)
                fprintf(f, "\\u%04x", *p);
            else
                fputc(*p, f);
            break;
        }
        p++;
    }
    fputc('"', f);
}

int m2sdr_sigmf_write(const struct m2sdr_sigmf_meta *meta)
{
    FILE *f;
    const char *dataset_name;
    const char *slash;

    if (!meta || !meta->meta_path[0] || !meta->data_path[0] || !meta->datatype[0])
        return -1;

    f = fopen(meta->meta_path, "wb");
    if (!f)
        return -1;

    slash = strrchr(meta->data_path, '/');
    dataset_name = slash ? slash + 1 : meta->data_path;

    fprintf(f, "{\n");
    fprintf(f, "  \"global\": {\n");
    fprintf(f, "    \"core:version\": \"1.2.5\",\n");
    fprintf(f, "    \"core:datatype\": ");
    json_write_escaped(f, meta->datatype);
    fprintf(f, ",\n");
    fprintf(f, "    \"core:dataset\": ");
    json_write_escaped(f, dataset_name);
    if (meta->has_sample_rate) {
        fprintf(f, ",\n    \"core:sample_rate\": %.17g", meta->sample_rate);
    }
    if (meta->has_num_channels) {
        fprintf(f, ",\n    \"core:num_channels\": %u", meta->num_channels);
    }
    if (meta->description[0]) {
        fprintf(f, ",\n    \"core:description\": ");
        json_write_escaped(f, meta->description);
    }
    if (meta->author[0]) {
        fprintf(f, ",\n    \"core:author\": ");
        json_write_escaped(f, meta->author);
    }
    if (meta->hw[0]) {
        fprintf(f, ",\n    \"core:hw\": ");
        json_write_escaped(f, meta->hw);
    }
    if (meta->recorder[0]) {
        fprintf(f, ",\n    \"core:recorder\": ");
        json_write_escaped(f, meta->recorder);
    }
    fprintf(f, "\n  },\n");
    fprintf(f, "  \"captures\": [\n");
    fprintf(f, "    {\n");
    fprintf(f, "      \"core:sample_start\": 0");
    if (meta->has_center_freq)
        fprintf(f, ",\n      \"core:frequency\": %.17g", meta->center_freq);
    if (meta->has_datetime) {
        fprintf(f, ",\n      \"core:datetime\": ");
        json_write_escaped(f, meta->datetime);
    }
    if (meta->has_header_bytes)
        fprintf(f, ",\n      \"core:header_bytes\": %u", meta->header_bytes);
    fprintf(f, "\n    }\n");
    fprintf(f, "  ],\n");
    fprintf(f, "  \"annotations\": [\n");
    for (unsigned i = 0; i < meta->annotation_count; i++) {
        const struct m2sdr_sigmf_annotation *ann = &meta->annotations[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"core:sample_start\": %" PRIu64, ann->sample_start);
        if (ann->has_sample_count)
            fprintf(f, ",\n      \"core:sample_count\": %" PRIu64, ann->sample_count);
        if (ann->has_freq_lower_edge)
            fprintf(f, ",\n      \"core:freq_lower_edge\": %.17g", ann->freq_lower_edge);
        if (ann->has_freq_upper_edge)
            fprintf(f, ",\n      \"core:freq_upper_edge\": %.17g", ann->freq_upper_edge);
        if (ann->label[0]) {
            fprintf(f, ",\n      \"core:label\": ");
            json_write_escaped(f, ann->label);
        }
        if (ann->comment[0]) {
            fprintf(f, ",\n      \"core:comment\": ");
            json_write_escaped(f, ann->comment);
        }
        fprintf(f, "\n    }%s\n", (i + 1u < meta->annotation_count) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    fclose(f);
    return 0;
}

int m2sdr_sigmf_capture_sample_range(const struct m2sdr_sigmf_meta *meta,
                                     unsigned capture_index,
                                     uint64_t *start_sample,
                                     uint64_t *end_sample)
{
    if (!meta || capture_index >= meta->capture_count || !start_sample || !end_sample)
        return -1;

    *start_sample = meta->captures[capture_index].sample_start;
    *end_sample = (capture_index + 1u < meta->capture_count) ?
        meta->captures[capture_index + 1u].sample_start : 0;
    return 0;
}

bool m2sdr_sigmf_timestamp_jump_is_anomalous(uint64_t nominal_dt_ns,
                                             uint64_t dt_ns,
                                             double threshold_pct)
{
    double diff_ratio;

    if (nominal_dt_ns == 0 || threshold_pct < 0.0)
        return false;
    diff_ratio = fabs((double)dt_ns - (double)nominal_dt_ns) / (double)nominal_dt_ns;
    return diff_ratio > (threshold_pct / 100.0);
}

static void sigmf_read_annotation(const char *buf,
                                  const struct m2sdr_json_token *tokens,
                                  int count,
                                  int object_index,
                                  struct m2sdr_sigmf_annotation *ann)
{
    int idx;

    if (!buf || !tokens || !ann || object_index < 0 || object_index >= count)
        return;

    memset(ann, 0, sizeof(*ann));
    idx = m2sdr_json_object_get(buf, tokens, count, object_index, "core:sample_start");
    if (idx >= 0)
        m2sdr_json_token_tou64(buf, &tokens[idx], &ann->sample_start);
    idx = m2sdr_json_object_get(buf, tokens, count, object_index, "core:sample_count");
    if (idx >= 0 && m2sdr_json_token_tou64(buf, &tokens[idx], &ann->sample_count) == 0)
        ann->has_sample_count = true;
    idx = m2sdr_json_object_get(buf, tokens, count, object_index, "core:freq_lower_edge");
    if (idx >= 0 && m2sdr_json_token_todouble(buf, &tokens[idx], &ann->freq_lower_edge) == 0)
        ann->has_freq_lower_edge = true;
    idx = m2sdr_json_object_get(buf, tokens, count, object_index, "core:freq_upper_edge");
    if (idx >= 0 && m2sdr_json_token_todouble(buf, &tokens[idx], &ann->freq_upper_edge) == 0)
        ann->has_freq_upper_edge = true;
    idx = m2sdr_json_object_get(buf, tokens, count, object_index, "core:label");
    if (idx >= 0)
        m2sdr_json_token_tostr(buf, &tokens[idx], ann->label, sizeof(ann->label));
    idx = m2sdr_json_object_get(buf, tokens, count, object_index, "core:comment");
    if (idx >= 0)
        m2sdr_json_token_tostr(buf, &tokens[idx], ann->comment, sizeof(ann->comment));
}

static void sigmf_read_capture(const char *buf,
                               const struct m2sdr_json_token *tokens,
                               int count,
                               int object_index,
                               struct m2sdr_sigmf_capture *cap)
{
    int idx;

    if (!buf || !tokens || !cap || object_index < 0 || object_index >= count)
        return;

    memset(cap, 0, sizeof(*cap));
    idx = m2sdr_json_object_get(buf, tokens, count, object_index, "core:sample_start");
    if (idx >= 0)
        m2sdr_json_token_tou64(buf, &tokens[idx], &cap->sample_start);
    idx = m2sdr_json_object_get(buf, tokens, count, object_index, "core:frequency");
    if (idx >= 0 && m2sdr_json_token_todouble(buf, &tokens[idx], &cap->center_freq) == 0)
        cap->has_center_freq = true;
    idx = m2sdr_json_object_get(buf, tokens, count, object_index, "core:datetime");
    if (idx >= 0 && m2sdr_json_token_tostr(buf, &tokens[idx], cap->datetime, sizeof(cap->datetime)) == 0)
        cap->has_datetime = true;
    idx = m2sdr_json_object_get(buf, tokens, count, object_index, "core:header_bytes");
    if (idx >= 0 && m2sdr_json_token_touint(buf, &tokens[idx], &cap->header_bytes) == 0)
        cap->has_header_bytes = true;
}

static void sigmf_read_capture_array(const char *buf,
                                     const struct m2sdr_json_token *tokens,
                                     int count,
                                     int array_index,
                                     struct m2sdr_sigmf_meta *meta)
{
    int i = array_index + 1;

    while (i < count && tokens[i].start < tokens[array_index].end &&
           meta->capture_count < M2SDR_SIGMF_MAX_CAPTURES) {
        if (tokens[i].parent == array_index && tokens[i].type == M2SDR_JSON_OBJECT) {
            sigmf_read_capture(buf, tokens, count, i, &meta->captures[meta->capture_count]);
            meta->capture_count++;
        }
        i = m2sdr_json_skip(tokens, count, i);
    }
}

static void sigmf_read_annotation_array(const char *buf,
                                        const struct m2sdr_json_token *tokens,
                                        int count,
                                        int array_index,
                                        struct m2sdr_sigmf_meta *meta)
{
    int i = array_index + 1;

    while (i < count && tokens[i].start < tokens[array_index].end &&
           meta->annotation_count < M2SDR_SIGMF_MAX_ANNOTATIONS) {
        if (tokens[i].parent == array_index && tokens[i].type == M2SDR_JSON_OBJECT) {
            sigmf_read_annotation(buf, tokens, count, i, &meta->annotations[meta->annotation_count]);
            meta->annotation_count++;
        }
        i = m2sdr_json_skip(tokens, count, i);
    }
}

int m2sdr_sigmf_read(const char *input_path, struct m2sdr_sigmf_meta *meta)
{
    FILE *f;
    long len;
    char *buf = NULL;
    struct m2sdr_json_parser parser;
    struct m2sdr_json_token *tokens = NULL;
    int token_count;
    char dataset_path[1024] = {0};
    char meta_dir[1024] = {0};
    int root_index = 0;
    int global_index;
    int captures_index;
    int annotations_index;
    int idx;

    if (!input_path || !meta)
        return -1;
    memset(meta, 0, sizeof(*meta));
    if (m2sdr_sigmf_derive_paths(input_path, meta->data_path, sizeof(meta->data_path),
                                 meta->meta_path, sizeof(meta->meta_path)) != 0)
        return -1;

    f = fopen(meta->meta_path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    len = ftell(f);
    if (len < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);
    buf = calloc((size_t)len + 1u, 1u);
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    tokens = calloc(512u, sizeof(*tokens));
    if (!tokens) {
        free(buf);
        return -1;
    }
    m2sdr_json_parser_init(&parser);
    token_count = m2sdr_json_parse(&parser, buf, (size_t)len, tokens, 512u);
    if (token_count <= 0 || tokens[root_index].type != M2SDR_JSON_OBJECT) {
        free(tokens);
        free(buf);
        return -1;
    }

    global_index = m2sdr_json_object_get(buf, tokens, token_count, root_index, "global");
    captures_index = m2sdr_json_object_get(buf, tokens, token_count, root_index, "captures");
    annotations_index = m2sdr_json_object_get(buf, tokens, token_count, root_index, "annotations");
    if (global_index < 0 || tokens[global_index].type != M2SDR_JSON_OBJECT) {
        free(tokens);
        free(buf);
        return -1;
    }

    idx = m2sdr_json_object_get(buf, tokens, token_count, global_index, "core:datatype");
    if (idx >= 0)
        m2sdr_json_token_tostr(buf, &tokens[idx], meta->datatype, sizeof(meta->datatype));
    idx = m2sdr_json_object_get(buf, tokens, token_count, global_index, "core:dataset");
    if (idx >= 0)
        m2sdr_json_token_tostr(buf, &tokens[idx], dataset_path, sizeof(dataset_path));
    idx = m2sdr_json_object_get(buf, tokens, token_count, global_index, "core:description");
    if (idx >= 0)
        m2sdr_json_token_tostr(buf, &tokens[idx], meta->description, sizeof(meta->description));
    idx = m2sdr_json_object_get(buf, tokens, token_count, global_index, "core:author");
    if (idx >= 0)
        m2sdr_json_token_tostr(buf, &tokens[idx], meta->author, sizeof(meta->author));
    idx = m2sdr_json_object_get(buf, tokens, token_count, global_index, "core:hw");
    if (idx >= 0)
        m2sdr_json_token_tostr(buf, &tokens[idx], meta->hw, sizeof(meta->hw));
    idx = m2sdr_json_object_get(buf, tokens, token_count, global_index, "core:recorder");
    if (idx >= 0)
        m2sdr_json_token_tostr(buf, &tokens[idx], meta->recorder, sizeof(meta->recorder));
    idx = m2sdr_json_object_get(buf, tokens, token_count, global_index, "core:sample_rate");
    if (idx >= 0 && m2sdr_json_token_todouble(buf, &tokens[idx], &meta->sample_rate) == 0)
        meta->has_sample_rate = true;
    idx = m2sdr_json_object_get(buf, tokens, token_count, global_index, "core:num_channels");
    if (idx >= 0 && m2sdr_json_token_touint(buf, &tokens[idx], &meta->num_channels) == 0)
        meta->has_num_channels = true;

    if (captures_index >= 0 && tokens[captures_index].type == M2SDR_JSON_ARRAY)
        sigmf_read_capture_array(buf, tokens, token_count, captures_index, meta);
    if (annotations_index >= 0 && tokens[annotations_index].type == M2SDR_JSON_ARRAY)
        sigmf_read_annotation_array(buf, tokens, token_count, annotations_index, meta);

    if (meta->datatype[0] == '\0') {
        free(tokens);
        free(buf);
        return -1;
    }
    if (meta->has_num_channels && meta->num_channels == 0) {
        free(tokens);
        free(buf);
        return -1;
    }
    if (meta->has_sample_rate && meta->sample_rate <= 0.0) {
        free(tokens);
        free(buf);
        return -1;
    }
    if (meta->capture_count > 0) {
        const struct m2sdr_sigmf_capture *cap0 = &meta->captures[0];
        if (cap0->has_center_freq) {
            meta->center_freq = cap0->center_freq;
            meta->has_center_freq = true;
        }
        if (cap0->has_datetime) {
            snprintf(meta->datetime, sizeof(meta->datetime), "%s", cap0->datetime);
            meta->has_datetime = true;
        }
        if (cap0->has_header_bytes) {
            meta->header_bytes = cap0->header_bytes;
            meta->has_header_bytes = true;
        }
    }
    if (dataset_path[0]) {
        if (is_absolute_path(dataset_path)) {
            snprintf(meta->data_path, sizeof(meta->data_path), "%s", dataset_path);
        } else {
            dirname_of(meta->meta_path, meta_dir, sizeof(meta_dir));
            if (strcmp(meta_dir, ".") == 0)
                snprintf(meta->data_path, sizeof(meta->data_path), "%s", dataset_path);
            else
                snprintf(meta->data_path, sizeof(meta->data_path), "%s/%s", meta_dir, dataset_path);
        }
    }
    free(tokens);
    free(buf);
    return 0;
}
