/* SPDX-License-Identifier: BSD-2-Clause */

#include "m2sdr_sigmf.h"

#include <ctype.h>
#include <inttypes.h>
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

static const char *json_find_key(const char *buf, const char *key)
{
    char pattern[128];

    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern))
        return NULL;
    return strstr(buf, pattern);
}

static int json_extract_string(const char *buf, const char *key, char *out, size_t out_len)
{
    const char *p = json_find_key(buf, key);
    size_t i = 0;

    if (!p || !out || out_len == 0)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '"')
        return -1;
    p++;
    while (*p && *p != '"' && i + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[i++] = '\n'; break;
            case 'r': out[i++] = '\r'; break;
            case 't': out[i++] = '\t'; break;
            default:  out[i++] = *p; break;
            }
            p++;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (*p == '"') ? 0 : -1;
}

static int json_extract_double(const char *buf, const char *key, double *value)
{
    const char *p = json_find_key(buf, key);
    char *end = NULL;

    if (!p || !value)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    *value = strtod(p, &end);
    return (end != p) ? 0 : -1;
}

static int json_extract_uint(const char *buf, const char *key, unsigned *value)
{
    const char *p = json_find_key(buf, key);
    char *end = NULL;
    unsigned long v;

    if (!p || !value)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    v = strtoul(p, &end, 10);
    if (end == p)
        return -1;
    *value = (unsigned)v;
    return 0;
}

static int json_extract_u64(const char *buf, const char *key, uint64_t *value)
{
    const char *p = json_find_key(buf, key);
    char *end = NULL;
    unsigned long long v;

    if (!p || !value)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    v = strtoull(p, &end, 10);
    if (end == p)
        return -1;
    *value = (uint64_t)v;
    return 0;
}

static const char *json_find_array_start(const char *buf, const char *key)
{
    const char *p = json_find_key(buf, key);

    if (!p)
        return NULL;
    p = strchr(p, ':');
    if (!p)
        return NULL;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    return (*p == '[') ? p : NULL;
}

static const char *json_find_object_end(const char *start)
{
    int depth = 0;
    int in_string = 0;
    int escaped = 0;
    const char *p = start;

    for (; *p; p++) {
        char c = *p;

        if (in_string) {
            if (escaped) {
                escaped = 0;
            } else if (c == '\\') {
                escaped = 1;
            } else if (c == '"') {
                in_string = 0;
            }
            continue;
        }

        if (c == '"') {
            in_string = 1;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0)
                return p;
        }
    }
    return NULL;
}

static void json_parse_annotations(const char *buf, struct m2sdr_sigmf_meta *meta)
{
    const char *p = json_find_array_start(buf, "annotations");

    if (!p || !meta)
        return;
    p++;
    while (*p && meta->annotation_count < M2SDR_SIGMF_MAX_ANNOTATIONS) {
        const char *obj_end;
        struct m2sdr_sigmf_annotation *ann;
        char obj[1024];
        size_t obj_len;

        while (*p && (isspace((unsigned char)*p) || *p == ','))
            p++;
        if (*p == ']' || *p == '\0')
            break;
        if (*p != '{')
            break;

        obj_end = json_find_object_end(p);
        if (!obj_end)
            break;
        obj_len = (size_t)(obj_end - p + 1);
        if (obj_len >= sizeof(obj))
            obj_len = sizeof(obj) - 1;
        memcpy(obj, p, obj_len);
        obj[obj_len] = '\0';

        ann = &meta->annotations[meta->annotation_count];
        memset(ann, 0, sizeof(*ann));
        if (json_extract_u64(obj, "core:sample_start", &ann->sample_start) != 0) {
            p = obj_end + 1;
            continue;
        }
        if (json_extract_u64(obj, "core:sample_count", &ann->sample_count) == 0)
            ann->has_sample_count = true;
        if (json_extract_double(obj, "core:freq_lower_edge", &ann->freq_lower_edge) == 0)
            ann->has_freq_lower_edge = true;
        if (json_extract_double(obj, "core:freq_upper_edge", &ann->freq_upper_edge) == 0)
            ann->has_freq_upper_edge = true;
        json_extract_string(obj, "core:label", ann->label, sizeof(ann->label));
        json_extract_string(obj, "core:comment", ann->comment, sizeof(ann->comment));
        meta->annotation_count++;
        p = obj_end + 1;
    }
}

static void json_parse_captures(const char *buf, struct m2sdr_sigmf_meta *meta)
{
    const char *p = json_find_array_start(buf, "captures");

    if (!p || !meta)
        return;
    p++;
    while (*p && meta->capture_count < M2SDR_SIGMF_MAX_CAPTURES) {
        const char *obj_end;
        struct m2sdr_sigmf_capture *cap;
        char obj[1024];
        size_t obj_len;

        while (*p && (isspace((unsigned char)*p) || *p == ','))
            p++;
        if (*p == ']' || *p == '\0')
            break;
        if (*p != '{')
            break;

        obj_end = json_find_object_end(p);
        if (!obj_end)
            break;
        obj_len = (size_t)(obj_end - p + 1);
        if (obj_len >= sizeof(obj))
            obj_len = sizeof(obj) - 1;
        memcpy(obj, p, obj_len);
        obj[obj_len] = '\0';

        cap = &meta->captures[meta->capture_count];
        memset(cap, 0, sizeof(*cap));
        if (json_extract_u64(obj, "core:sample_start", &cap->sample_start) != 0) {
            p = obj_end + 1;
            continue;
        }
        if (json_extract_double(obj, "core:frequency", &cap->center_freq) == 0)
            cap->has_center_freq = true;
        if (json_extract_string(obj, "core:datetime", cap->datetime, sizeof(cap->datetime)) == 0)
            cap->has_datetime = true;
        if (json_extract_uint(obj, "core:header_bytes", &cap->header_bytes) == 0)
            cap->has_header_bytes = true;
        meta->capture_count++;
        p = obj_end + 1;
    }
}

int m2sdr_sigmf_read(const char *input_path, struct m2sdr_sigmf_meta *meta)
{
    FILE *f;
    long len;
    char *buf = NULL;
    char dataset_path[1024] = {0};
    char meta_dir[1024] = {0};

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

    json_extract_string(buf, "core:datatype", meta->datatype, sizeof(meta->datatype));
    json_extract_string(buf, "core:dataset", dataset_path, sizeof(dataset_path));
    json_extract_string(buf, "core:description", meta->description, sizeof(meta->description));
    json_extract_string(buf, "core:author", meta->author, sizeof(meta->author));
    json_extract_string(buf, "core:hw", meta->hw, sizeof(meta->hw));
    json_extract_string(buf, "core:recorder", meta->recorder, sizeof(meta->recorder));
    if (json_extract_double(buf, "core:sample_rate", &meta->sample_rate) == 0)
        meta->has_sample_rate = true;
    if (json_extract_uint(buf, "core:num_channels", &meta->num_channels) == 0)
        meta->has_num_channels = true;
    json_parse_captures(buf, meta);
    json_parse_annotations(buf, meta);

    if (meta->datatype[0] == '\0') {
        free(buf);
        return -1;
    }
    if (meta->has_num_channels && meta->num_channels == 0) {
        free(buf);
        return -1;
    }
    if (meta->has_sample_rate && meta->sample_rate <= 0.0) {
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
    free(buf);
    return 0;
}
