/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR RF Lab Dashboard (Dear ImGui + SDL2/OpenGL3).
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 */

#include <dirent.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <GL/gl.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui/cimgui.h"
#include "scan_ui/imgui_sdl_gl3_bridge.h"
#include "m2sdr_json.h"

#define igGetIO igGetIO_Nil

#define LAB_MAX_RUNS        256
#define LAB_MAX_REPORTS     256
#define LAB_MAX_JSON_TOKENS 8192

struct lab_run_info {
    char id[96];
    char kind[32];
    char status[32];
    char notes[192];
    char datatype[32];
    double sample_rate;
    double center_freq;
    unsigned capture_count;
    unsigned annotation_count;
};

enum report_kind {
    REPORT_UNKNOWN = 0,
    REPORT_RUN,
    REPORT_COMPARE,
    REPORT_VERIFY,
    REPORT_MEASURE,
    REPORT_PHASE,
    REPORT_TIMECHECK,
    REPORT_CAL,
    REPORT_SWEEP
};

struct lab_report_info {
    enum report_kind kind;
    char id[128];
    char title[160];
    char path[1024];
    char run_a[96];
    char run_b[96];
    char summary_a[128];
    char summary_b[128];
    double metric_a;
    double metric_b;
    bool flag_a;
    bool flag_b;
};

struct lab_state {
    char root[1024];
    char title[128];
    char description[256];
    char author[96];
    char created_at[64];
    char default_device[128];
    double default_sample_rate;
    double default_center_freq;
    int runs_count;
    int reports_count;
    struct lab_run_info runs[LAB_MAX_RUNS];
    struct lab_report_info reports[LAB_MAX_REPORTS];
};

struct ui_state {
    int selected_run;
    int selected_report;
    int selected_tab;
    char status[256];
};

static void copy_string_trunc(char *dst, size_t dst_len, const char *src)
{
    size_t len;

    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    len = strlen(src);
    if (len >= dst_len) {
        len = dst_len - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void copy_token_string(const char *js, const struct m2sdr_json_token *tok, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!js || !tok) {
        return;
    }
    if (tok->type == M2SDR_JSON_STRING) {
        m2sdr_json_token_tostr(js, tok, out, out_len);
    } else if (tok->type == M2SDR_JSON_PRIMITIVE) {
        size_t len;

        if (tok->start < 0 || tok->end < tok->start) {
            return;
        }
        len = (size_t)(tok->end - tok->start);
        if (len >= out_len) {
            len = out_len - 1;
        }
        memcpy(out, js + tok->start, len);
        out[len] = '\0';
    }
}

static bool json_get_string(const char *js, const struct m2sdr_json_token *tokens, int count,
                            int object_index, const char *key, char *out, size_t out_len)
{
    int idx = m2sdr_json_object_get(js, tokens, count, object_index, key);

    if (idx < 0) {
        if (out && out_len) {
            out[0] = '\0';
        }
        return false;
    }
    copy_token_string(js, &tokens[idx], out, out_len);
    return true;
}

static bool json_get_double(const char *js, const struct m2sdr_json_token *tokens, int count,
                            int object_index, const char *key, double *value)
{
    int idx = m2sdr_json_object_get(js, tokens, count, object_index, key);

    if (idx < 0) {
        return false;
    }
    return m2sdr_json_token_todouble(js, &tokens[idx], value) == 0;
}

static bool json_get_u64(const char *js, const struct m2sdr_json_token *tokens, int count,
                         int object_index, const char *key, uint64_t *value)
{
    int idx = m2sdr_json_object_get(js, tokens, count, object_index, key);

    if (idx < 0) {
        return false;
    }
    return m2sdr_json_token_tou64(js, &tokens[idx], value) == 0;
}

static bool json_get_bool(const char *js, const struct m2sdr_json_token *tokens, int count,
                          int object_index, const char *key, bool *value)
{
    int idx = m2sdr_json_object_get(js, tokens, count, object_index, key);
    char buf[16];

    if (idx < 0) {
        return false;
    }
    copy_token_string(js, &tokens[idx], buf, sizeof(buf));
    if (strcmp(buf, "true") == 0) {
        *value = true;
        return true;
    }
    if (strcmp(buf, "false") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static char *load_text_file(const char *path, size_t *len_out)
{
    FILE *f;
    long size;
    char *buf;

    f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[size] = '\0';
    if (len_out) {
        *len_out = (size_t)size;
    }
    return buf;
}

static bool parse_run_object(const char *js, const struct m2sdr_json_token *tokens, int count,
                             int run_index, struct lab_run_info *run)
{
    int sigmf_idx;
    uint64_t value_u64 = 0;

    memset(run, 0, sizeof(*run));
    json_get_string(js, tokens, count, run_index, "id", run->id, sizeof(run->id));
    json_get_string(js, tokens, count, run_index, "kind", run->kind, sizeof(run->kind));
    json_get_string(js, tokens, count, run_index, "status", run->status, sizeof(run->status));
    json_get_string(js, tokens, count, run_index, "notes", run->notes, sizeof(run->notes));
    sigmf_idx = m2sdr_json_object_get(js, tokens, count, run_index, "sigmf");
    if (sigmf_idx >= 0 && tokens[sigmf_idx].type == M2SDR_JSON_OBJECT) {
        json_get_string(js, tokens, count, sigmf_idx, "datatype", run->datatype, sizeof(run->datatype));
        json_get_double(js, tokens, count, sigmf_idx, "sample_rate", &run->sample_rate);
        json_get_double(js, tokens, count, sigmf_idx, "center_freq", &run->center_freq);
        if (json_get_u64(js, tokens, count, sigmf_idx, "capture_count", &value_u64)) {
            run->capture_count = (unsigned)value_u64;
        }
        if (json_get_u64(js, tokens, count, sigmf_idx, "annotation_count", &value_u64)) {
            run->annotation_count = (unsigned)value_u64;
        }
    }
    return run->id[0] != '\0';
}

static void parse_run_report(struct lab_report_info *report, const char *js,
                             const struct m2sdr_json_token *tokens, int count, int root_index)
{
    int run_idx = m2sdr_json_object_get(js, tokens, count, root_index, "run");

    report->kind = REPORT_RUN;
    json_get_string(js, tokens, count, root_index, "id", report->id, sizeof(report->id));
    snprintf(report->title, sizeof(report->title), "Run Report");
    if (run_idx >= 0 && tokens[run_idx].type == M2SDR_JSON_OBJECT) {
        json_get_string(js, tokens, count, run_idx, "id", report->run_a, sizeof(report->run_a));
        json_get_string(js, tokens, count, run_idx, "status", report->summary_a, sizeof(report->summary_a));
        json_get_string(js, tokens, count, run_idx, "kind", report->summary_b, sizeof(report->summary_b));
    }
}

static void parse_compare_report(struct lab_report_info *report, const char *js,
                                 const struct m2sdr_json_token *tokens, int count, int root_index)
{
    int cmp_idx = m2sdr_json_object_get(js, tokens, count, root_index, "comparison");
    int run_a_idx;
    int run_b_idx;

    report->kind = REPORT_COMPARE;
    json_get_string(js, tokens, count, root_index, "id", report->id, sizeof(report->id));
    snprintf(report->title, sizeof(report->title), "Compare");
    if (cmp_idx < 0 || tokens[cmp_idx].type != M2SDR_JSON_OBJECT) {
        return;
    }
    json_get_bool(js, tokens, count, cmp_idx, "compatible_for_replay", &report->flag_a);
    json_get_bool(js, tokens, count, cmp_idx, "exact_match", &report->flag_b);
    snprintf(report->summary_a, sizeof(report->summary_a), "Replay compatible: %s", report->flag_a ? "yes" : "no");
    snprintf(report->summary_b, sizeof(report->summary_b), "Exact match: %s", report->flag_b ? "yes" : "no");
    run_a_idx = m2sdr_json_object_get(js, tokens, count, cmp_idx, "run_a");
    run_b_idx = m2sdr_json_object_get(js, tokens, count, cmp_idx, "run_b");
    if (run_a_idx >= 0) {
        json_get_string(js, tokens, count, run_a_idx, "id", report->run_a, sizeof(report->run_a));
    }
    if (run_b_idx >= 0) {
        json_get_string(js, tokens, count, run_b_idx, "id", report->run_b, sizeof(report->run_b));
    }
}

static void parse_verify_report(struct lab_report_info *report, const char *js,
                                const struct m2sdr_json_token *tokens, int count, int root_index)
{
    int verify_idx = m2sdr_json_object_get(js, tokens, count, root_index, "verification");

    report->kind = REPORT_VERIFY;
    json_get_string(js, tokens, count, root_index, "id", report->id, sizeof(report->id));
    snprintf(report->title, sizeof(report->title), "Verify");
    if (verify_idx < 0 || tokens[verify_idx].type != M2SDR_JSON_OBJECT) {
        return;
    }
    json_get_string(js, tokens, count, verify_idx, "reference_run_id", report->run_a, sizeof(report->run_a));
    json_get_string(js, tokens, count, verify_idx, "candidate_run_id", report->run_b, sizeof(report->run_b));
    json_get_bool(js, tokens, count, verify_idx, "passed", &report->flag_a);
    json_get_bool(js, tokens, count, verify_idx, "exact_required", &report->flag_b);
    snprintf(report->summary_a, sizeof(report->summary_a), "Passed: %s", report->flag_a ? "yes" : "no");
    snprintf(report->summary_b, sizeof(report->summary_b), "Exact required: %s", report->flag_b ? "yes" : "no");
}

static void parse_measure_report(struct lab_report_info *report, const char *js,
                                 const struct m2sdr_json_token *tokens, int count, int root_index)
{
    int capture_idx = m2sdr_json_object_get(js, tokens, count, root_index, "capture");
    int channels_idx;
    int ch0_idx;
    int fft_idx;

    report->kind = REPORT_MEASURE;
    snprintf(report->title, sizeof(report->title), "Measure");
    if (capture_idx < 0 || tokens[capture_idx].type != M2SDR_JSON_OBJECT) {
        return;
    }
    json_get_string(js, tokens, count, capture_idx, "data_path", report->summary_a, sizeof(report->summary_a));
    json_get_double(js, tokens, count, capture_idx, "sample_rate", &report->metric_b);
    channels_idx = m2sdr_json_object_get(js, tokens, count, capture_idx, "channels");
    if (channels_idx >= 0 && tokens[channels_idx].type == M2SDR_JSON_OBJECT) {
        ch0_idx = m2sdr_json_object_get(js, tokens, count, channels_idx, "ch0");
        if (ch0_idx >= 0 && tokens[ch0_idx].type == M2SDR_JSON_OBJECT) {
            json_get_double(js, tokens, count, ch0_idx, "power_dbfs", &report->metric_a);
            fft_idx = m2sdr_json_object_get(js, tokens, count, ch0_idx, "fft");
            if (fft_idx >= 0 && tokens[fft_idx].type == M2SDR_JSON_OBJECT) {
                json_get_double(js, tokens, count, fft_idx, "peak_freq_hz", &report->metric_b);
            }
            snprintf(report->summary_b, sizeof(report->summary_b), "Ch0 power %.1f dBFS", report->metric_a);
        }
    }
}

static void parse_phase_report(struct lab_report_info *report, const char *js,
                               const struct m2sdr_json_token *tokens, int count, int root_index)
{
    int result_idx = m2sdr_json_object_get(js, tokens, count, root_index, "result");

    report->kind = REPORT_PHASE;
    snprintf(report->title, sizeof(report->title), "Phase");
    if (result_idx < 0 || tokens[result_idx].type != M2SDR_JSON_OBJECT) {
        return;
    }
    json_get_double(js, tokens, count, result_idx, "phase_delta_deg", &report->metric_a);
    json_get_double(js, tokens, count, result_idx, "gain_delta_db", &report->metric_b);
    snprintf(report->summary_a, sizeof(report->summary_a), "Phase %.1f deg", report->metric_a);
    snprintf(report->summary_b, sizeof(report->summary_b), "Gain %.1f dB", report->metric_b);
}

static void parse_timecheck_report(struct lab_report_info *report, const char *js,
                                   const struct m2sdr_json_token *tokens, int count, int root_index)
{
    int result_idx = m2sdr_json_object_get(js, tokens, count, root_index, "result");
    uint64_t count_u64 = 0;

    report->kind = REPORT_TIMECHECK;
    snprintf(report->title, sizeof(report->title), "Timecheck");
    if (result_idx < 0 || tokens[result_idx].type != M2SDR_JSON_OBJECT) {
        return;
    }
    json_get_bool(js, tokens, count, result_idx, "continuous_capture_layout", &report->flag_a);
    json_get_u64(js, tokens, count, result_idx, "capture_count", &count_u64);
    report->metric_a = (double)count_u64;
    snprintf(report->summary_a, sizeof(report->summary_a), "Continuous: %s", report->flag_a ? "yes" : "no");
    snprintf(report->summary_b, sizeof(report->summary_b), "Captures %.0f", report->metric_a);
}

static void parse_cal_report(struct lab_report_info *report, const char *js,
                             const struct m2sdr_json_token *tokens, int count, int root_index)
{
    int match_idx = m2sdr_json_object_get(js, tokens, count, root_index, "channel_match");

    report->kind = REPORT_CAL;
    snprintf(report->title, sizeof(report->title), "Calibration");
    if (match_idx < 0 || tokens[match_idx].type != M2SDR_JSON_OBJECT) {
        return;
    }
    json_get_double(js, tokens, count, match_idx, "phase_deg", &report->metric_a);
    json_get_double(js, tokens, count, match_idx, "gain_db", &report->metric_b);
    snprintf(report->summary_a, sizeof(report->summary_a), "Phase corr %.1f deg", report->metric_a);
    snprintf(report->summary_b, sizeof(report->summary_b), "Gain corr %.1f dB", report->metric_b);
}

static void parse_sweep_report(struct lab_report_info *report, const char *js,
                               const struct m2sdr_json_token *tokens, int count, int root_index)
{
    int points_idx = m2sdr_json_object_get(js, tokens, count, root_index, "points");
    int idx;
    unsigned ok = 0;
    unsigned total = 0;

    report->kind = REPORT_SWEEP;
    snprintf(report->title, sizeof(report->title), "Sweep");
    if (points_idx < 0 || tokens[points_idx].type != M2SDR_JSON_ARRAY) {
        return;
    }
    idx = points_idx + 1;
    while (idx < count && tokens[idx].start < tokens[points_idx].end) {
        int rc_idx;
        uint64_t rc = 0;

        if (tokens[idx].parent == points_idx && tokens[idx].type == M2SDR_JSON_OBJECT) {
            total++;
            rc_idx = m2sdr_json_object_get(js, tokens, count, idx, "returncode");
            if (rc_idx >= 0 && m2sdr_json_token_tou64(js, &tokens[rc_idx], &rc) == 0 && rc == 0) {
                ok++;
            }
        }
        idx = m2sdr_json_skip(tokens, count, idx);
    }
    report->metric_a = (double)ok;
    report->metric_b = (double)total;
    snprintf(report->summary_a, sizeof(report->summary_a), "OK %.0f / %.0f", report->metric_a, report->metric_b);
}

static bool parse_report_file(struct lab_state *lab, const char *path)
{
    char *js;
    size_t len = 0;
    struct m2sdr_json_parser parser;
    struct m2sdr_json_token *tokens;
    int count;
    struct lab_report_info report;
    char kind_buf[64];
    char tool_buf[64];

    if (lab->reports_count >= LAB_MAX_REPORTS) {
        return false;
    }
    js = load_text_file(path, &len);
    if (!js) {
        return false;
    }
    tokens = (struct m2sdr_json_token *)calloc(LAB_MAX_JSON_TOKENS, sizeof(*tokens));
    if (!tokens) {
        free(js);
        return false;
    }
    m2sdr_json_parser_init(&parser);
    count = m2sdr_json_parse(&parser, js, len, tokens, LAB_MAX_JSON_TOKENS);
    if (count <= 0 || tokens[0].type != M2SDR_JSON_OBJECT) {
        free(tokens);
        free(js);
        return false;
    }

    memset(&report, 0, sizeof(report));
    copy_string_trunc(report.path, sizeof(report.path), path);
    json_get_string(js, tokens, count, 0, "kind", kind_buf, sizeof(kind_buf));
    json_get_string(js, tokens, count, 0, "tool", tool_buf, sizeof(tool_buf));
    if (strcmp(kind_buf, "run-report") == 0) {
        parse_run_report(&report, js, tokens, count, 0);
    } else if (strcmp(kind_buf, "compare") == 0) {
        parse_compare_report(&report, js, tokens, count, 0);
    } else if (strcmp(kind_buf, "verify") == 0) {
        parse_verify_report(&report, js, tokens, count, 0);
    } else if (strcmp(tool_buf, "m2sdr_measure") == 0) {
        parse_measure_report(&report, js, tokens, count, 0);
    } else if (strcmp(tool_buf, "m2sdr_phase") == 0) {
        parse_phase_report(&report, js, tokens, count, 0);
    } else if (strcmp(tool_buf, "m2sdr_timecheck") == 0) {
        parse_timecheck_report(&report, js, tokens, count, 0);
    } else if (strcmp(tool_buf, "m2sdr_cal") == 0) {
        parse_cal_report(&report, js, tokens, count, 0);
    } else if (strcmp(tool_buf, "m2sdr_sweep") == 0) {
        parse_sweep_report(&report, js, tokens, count, 0);
    } else {
        report.kind = REPORT_UNKNOWN;
        snprintf(report.title, sizeof(report.title), "JSON");
    }

    if (report.id[0] == '\0') {
        const char *name = strrchr(path, '/');
        if (name) {
            name++;
        } else {
            name = path;
        }
        copy_string_trunc(report.id, sizeof(report.id), name);
    }
    lab->reports[lab->reports_count++] = report;
    free(tokens);
    free(js);
    return true;
}

static bool load_lab_manifest(struct lab_state *lab, const char *root)
{
    char manifest_path[1200];
    char reports_dir[1200];
    char *js;
    size_t len = 0;
    struct m2sdr_json_parser parser;
    struct m2sdr_json_token *tokens;
    int count;
    int runs_idx;
    int defaults_idx;
    int idx;
    DIR *dir;
    struct dirent *de;

    memset(lab, 0, sizeof(*lab));
    copy_string_trunc(lab->root, sizeof(lab->root), root);
    snprintf(manifest_path, sizeof(manifest_path), "%s/lab.json", root);
    js = load_text_file(manifest_path, &len);
    if (!js) {
        return false;
    }
    tokens = (struct m2sdr_json_token *)calloc(LAB_MAX_JSON_TOKENS, sizeof(*tokens));
    if (!tokens) {
        free(js);
        return false;
    }
    m2sdr_json_parser_init(&parser);
    count = m2sdr_json_parse(&parser, js, len, tokens, LAB_MAX_JSON_TOKENS);
    if (count <= 0 || tokens[0].type != M2SDR_JSON_OBJECT) {
        free(tokens);
        free(js);
        return false;
    }

    json_get_string(js, tokens, count, 0, "title", lab->title, sizeof(lab->title));
    json_get_string(js, tokens, count, 0, "description", lab->description, sizeof(lab->description));
    json_get_string(js, tokens, count, 0, "author", lab->author, sizeof(lab->author));
    json_get_string(js, tokens, count, 0, "created_at", lab->created_at, sizeof(lab->created_at));
    defaults_idx = m2sdr_json_object_get(js, tokens, count, 0, "defaults");
    if (defaults_idx >= 0 && tokens[defaults_idx].type == M2SDR_JSON_OBJECT) {
        json_get_string(js, tokens, count, defaults_idx, "device", lab->default_device, sizeof(lab->default_device));
        json_get_double(js, tokens, count, defaults_idx, "sample_rate", &lab->default_sample_rate);
        json_get_double(js, tokens, count, defaults_idx, "center_freq", &lab->default_center_freq);
    }

    runs_idx = m2sdr_json_object_get(js, tokens, count, 0, "runs");
    if (runs_idx >= 0 && tokens[runs_idx].type == M2SDR_JSON_ARRAY) {
        idx = runs_idx + 1;
        while (idx < count && tokens[idx].start < tokens[runs_idx].end && lab->runs_count < LAB_MAX_RUNS) {
            if (tokens[idx].parent == runs_idx && tokens[idx].type == M2SDR_JSON_OBJECT) {
                if (parse_run_object(js, tokens, count, idx, &lab->runs[lab->runs_count])) {
                    lab->runs_count++;
                }
            }
            idx = m2sdr_json_skip(tokens, count, idx);
        }
    }
    free(tokens);
    free(js);

    snprintf(reports_dir, sizeof(reports_dir), "%s/reports", root);
    dir = opendir(reports_dir);
    if (!dir) {
        return true;
    }
    while ((de = readdir(dir)) != NULL) {
        char path[1200];
        size_t name_len = strlen(de->d_name);

        if (name_len < 6 || strcmp(de->d_name + name_len - 5, ".json") != 0) {
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s", reports_dir, de->d_name) >= (int)sizeof(path)) {
            continue;
        }
        parse_report_file(lab, path);
    }
    closedir(dir);
    return true;
}

static void accent_badge(const char *label, ImU32 color)
{
    ImDrawList *dl;
    ImVec2 pmin, pmax;

    dl = igGetWindowDrawList();
    igGetCursorScreenPos(&pmin);
    igTextUnformatted(label, NULL);
    igGetItemRectMax(&pmax);
    ImDrawList_AddRectFilled(dl,
                             (ImVec2){pmin.x - 6.0f, pmin.y - 2.0f},
                             (ImVec2){pmax.x + 6.0f, pmax.y + 2.0f},
                             color, 6.0f, 0);
    igSetCursorScreenPos((ImVec2){pmin.x, pmin.y});
    igTextColored((ImVec4){1.f, 1.f, 1.f, 1.f}, "%s", label);
}

static void metric_card(const char *title, const char *value, const char *subtext, ImU32 accent)
{
    ImVec2 avail;
    ImDrawList *dl;
    ImVec2 pmin, pmax;

    igGetContentRegionAvail(&avail);
    igBeginChild_Str(title, (ImVec2){avail.x, 74.0f}, true, 0);
    dl = igGetWindowDrawList();
    igGetWindowPos(&pmin);
    pmax = (ImVec2){pmin.x + igGetWindowWidth(), pmin.y + igGetWindowHeight()};
    ImDrawList_AddRectFilled(dl, pmin, pmax, 0xFF1E2329u, 8.0f, 0);
    ImDrawList_AddRectFilled(dl, pmin, (ImVec2){pmin.x + 6.0f, pmax.y}, accent, 8.0f, 0);
    igSetCursorPos((ImVec2){16.0f, 10.0f});
    igTextColored((ImVec4){0.75f, 0.82f, 0.88f, 1.0f}, "%s", title);
    igSetCursorPos((ImVec2){16.0f, 28.0f});
    igText("%s", value);
    igSetCursorPos((ImVec2){16.0f, 50.0f});
    igTextColored((ImVec4){0.60f, 0.66f, 0.71f, 1.0f}, "%s", subtext);
    igEndChild();
}

static ImU32 status_color(const char *status)
{
    if (strcmp(status, "complete") == 0 || strcmp(status, "imported") == 0) {
        return 0xFF2B8A3Eu;
    }
    if (strcmp(status, "planned") == 0) {
        return 0xFFD08700u;
    }
    return 0xFF495057u;
}

static const char *report_kind_name(enum report_kind kind)
{
    switch (kind) {
    case REPORT_RUN: return "run";
    case REPORT_COMPARE: return "compare";
    case REPORT_VERIFY: return "verify";
    case REPORT_MEASURE: return "measure";
    case REPORT_PHASE: return "phase";
    case REPORT_TIMECHECK: return "time";
    case REPORT_CAL: return "cal";
    case REPORT_SWEEP: return "sweep";
    default: return "json";
    }
}

static void draw_runs_panel(const struct lab_state *lab, struct ui_state *ui)
{
    int i;

    igText("Runs");
    igSeparator();
    if (!igBeginChild_Str("##runs_panel", (ImVec2){0.0f, 0.0f}, true, 0)) {
        igEndChild();
        return;
    }
    for (i = 0; i < lab->runs_count; i++) {
        char label[160];
        bool selected = (ui->selected_run == i);

        snprintf(label, sizeof(label), "%s##run%d", lab->runs[i].id, i);
        if (igSelectable_Bool(label, selected, ImGuiSelectableFlags_SpanAvailWidth, (ImVec2){0.0f, 0.0f})) {
            ui->selected_run = i;
        }
        igSameLine(220.0f, 8.0f);
        accent_badge(lab->runs[i].status[0] ? lab->runs[i].status : lab->runs[i].kind,
                     status_color(lab->runs[i].status[0] ? lab->runs[i].status : lab->runs[i].kind));
        igTextColored((ImVec4){0.70f, 0.75f, 0.80f, 1.0f}, "  %s | %.3f MSPS | %.3f MHz",
                      lab->runs[i].kind,
                      lab->runs[i].sample_rate / 1e6,
                      lab->runs[i].center_freq / 1e6);
        igSpacing();
    }
    igEndChild();
}

static void draw_reports_panel(const struct lab_state *lab, struct ui_state *ui)
{
    int i;

    igText("Artifacts");
    igSeparator();
    if (!igBeginChild_Str("##reports_panel", (ImVec2){0.0f, 0.0f}, true, 0)) {
        igEndChild();
        return;
    }
    for (i = 0; i < lab->reports_count; i++) {
        char label[196];
        bool selected = (ui->selected_report == i);

        snprintf(label, sizeof(label), "%s##report%d", lab->reports[i].id, i);
        if (igSelectable_Bool(label, selected, ImGuiSelectableFlags_SpanAvailWidth, (ImVec2){0.0f, 0.0f})) {
            ui->selected_report = i;
        }
        igTextColored((ImVec4){0.62f, 0.70f, 0.78f, 1.0f}, "  %s", report_kind_name(lab->reports[i].kind));
        if (lab->reports[i].summary_a[0]) {
            igTextWrapped("  %s", lab->reports[i].summary_a);
        }
        if (lab->reports[i].summary_b[0]) {
            igTextWrapped("  %s", lab->reports[i].summary_b);
        }
        igSpacing();
    }
    igEndChild();
}

static void draw_overview_cards(const struct lab_state *lab)
{
    char value[96];
    char subtext[128];
    int capture_runs = 0;
    int replay_runs = 0;
    int i;

    for (i = 0; i < lab->runs_count; i++) {
        if (strcmp(lab->runs[i].kind, "capture") == 0) {
            capture_runs++;
        } else if (strcmp(lab->runs[i].kind, "replay") == 0) {
            replay_runs++;
        }
    }

    if (igBeginTable("##overview_cards", 4, ImGuiTableFlags_SizingStretchSame, (ImVec2){0.0f, 0.0f}, 0.0f)) {
        igTableNextColumn();
        snprintf(value, sizeof(value), "%d", lab->runs_count);
        snprintf(subtext, sizeof(subtext), "%d captures / %d replays", capture_runs, replay_runs);
        metric_card("Runs", value, subtext, 0xFF0B7285u);

        igTableNextColumn();
        snprintf(value, sizeof(value), "%d", lab->reports_count);
        snprintf(subtext, sizeof(subtext), "compare / verify / measure / sweep");
        metric_card("Artifacts", value, subtext, 0xFFC92A2Au);

        igTableNextColumn();
        snprintf(value, sizeof(value), "%.2f", lab->default_sample_rate / 1e6);
        snprintf(subtext, sizeof(subtext), "Default MSPS");
        metric_card("Rate", value, subtext, 0xFF2B8A3Eu);

        igTableNextColumn();
        snprintf(value, sizeof(value), "%.2f", lab->default_center_freq / 1e6);
        snprintf(subtext, sizeof(subtext), "Default MHz");
        metric_card("Center Freq", value, subtext, 0xFFD9480Fu);
        igEndTable();
    }
}

static void draw_run_detail(const struct lab_state *lab, const struct ui_state *ui)
{
    const struct lab_run_info *run;

    if (lab->runs_count == 0 || ui->selected_run < 0 || ui->selected_run >= lab->runs_count) {
        igTextDisabled("No run selected.");
        return;
    }
    run = &lab->runs[ui->selected_run];
    igText("Run Detail");
    igSeparator();
    igText("ID              : %s", run->id);
    igText("Kind            : %s", run->kind);
    igText("Status          : %s", run->status);
    igText("Sample Rate     : %.6f MSPS", run->sample_rate / 1e6);
    igText("Center Freq     : %.6f MHz", run->center_freq / 1e6);
    igText("Datatype        : %s", run->datatype[0] ? run->datatype : "n/a");
    igText("Captures        : %u", run->capture_count);
    igText("Annotations     : %u", run->annotation_count);
    if (run->notes[0]) {
        igSpacing();
        igText("Notes");
        igTextWrapped("%s", run->notes);
    }
}

static void draw_report_detail(const struct lab_state *lab, const struct ui_state *ui)
{
    const struct lab_report_info *report;

    if (lab->reports_count == 0 || ui->selected_report < 0 || ui->selected_report >= lab->reports_count) {
        igTextDisabled("No artifact selected.");
        return;
    }
    report = &lab->reports[ui->selected_report];
    igText("Artifact Detail");
    igSeparator();
    igText("ID              : %s", report->id);
    igText("Type            : %s", report_kind_name(report->kind));
    if (report->run_a[0]) {
        igText("Run A           : %s", report->run_a);
    }
    if (report->run_b[0]) {
        igText("Run B           : %s", report->run_b);
    }
    if (report->summary_a[0]) {
        igTextWrapped("%s", report->summary_a);
    }
    if (report->summary_b[0]) {
        igTextWrapped("%s", report->summary_b);
    }
    if (report->path[0]) {
        igSpacing();
        igTextWrapped("Path: %s", report->path);
    }
}

static void draw_dashboard(struct lab_state *lab, struct ui_state *ui)
{
    igSetNextWindowPos((ImVec2){0.0f, 0.0f}, ImGuiCond_Always, (ImVec2){0.0f, 0.0f});
    igSetNextWindowSize(igGetIO()->DisplaySize, ImGuiCond_Always);
    igBegin("##M2SDRLabDashboard", NULL,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse);

    igText("%s", lab->title[0] ? lab->title : "LiteX-M2SDR RF Lab");
    if (lab->description[0]) {
        igTextColored((ImVec4){0.75f, 0.80f, 0.85f, 1.0f}, "%s", lab->description);
    }
    igSameLine(0.0f, 24.0f);
    if (igButton("Refresh", (ImVec2){90.0f, 0.0f})) {
        if (load_lab_manifest(lab, lab->root)) {
            if (lab->runs_count == 0) {
                ui->selected_run = -1;
            } else if (ui->selected_run >= lab->runs_count) {
                ui->selected_run = 0;
            }
            if (lab->reports_count == 0) {
                ui->selected_report = -1;
            } else if (ui->selected_report >= lab->reports_count) {
                ui->selected_report = 0;
            }
            snprintf(ui->status, sizeof(ui->status), "Reloaded lab manifest and artifacts.");
        } else {
            snprintf(ui->status, sizeof(ui->status), "Reload failed.");
        }
    }
    if (ui->status[0]) {
        igSameLine(0.0f, 16.0f);
        igTextColored((ImVec4){0.90f, 0.74f, 0.12f, 1.0f}, "%s", ui->status);
    }
    igSeparator();

    draw_overview_cards(lab);
    igSpacing();

    if (igBeginTable("##main_layout", 3,
                     ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp,
                     (ImVec2){0.0f, 0.0f}, 0.0f)) {
        igTableSetupColumn("Runs", ImGuiTableColumnFlags_WidthStretch, 1.2f, 0);
        igTableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 1.5f, 0);
        igTableSetupColumn("Artifacts", ImGuiTableColumnFlags_WidthStretch, 1.2f, 0);

        igTableNextColumn();
        draw_runs_panel(lab, ui);

        igTableNextColumn();
        if (igBeginTabBar("##detail_tabs", 0)) {
            if (igBeginTabItem("Run", NULL, 0)) {
                draw_run_detail(lab, ui);
                igEndTabItem();
            }
            if (igBeginTabItem("Artifact", NULL, 0)) {
                draw_report_detail(lab, ui);
                igEndTabItem();
            }
            if (igBeginTabItem("Lab", NULL, 0)) {
                igText("Created         : %s", lab->created_at[0] ? lab->created_at : "n/a");
                igText("Author          : %s", lab->author[0] ? lab->author : "n/a");
                igText("Default device  : %s", lab->default_device[0] ? lab->default_device : "n/a");
                igText("Default rate    : %.6f MSPS", lab->default_sample_rate / 1e6);
                igText("Default freq    : %.6f MHz", lab->default_center_freq / 1e6);
                igTextWrapped("Root: %s", lab->root);
                igEndTabItem();
            }
            igEndTabBar();
        }

        igTableNextColumn();
        draw_reports_panel(lab, ui);

        igEndTable();
    }
    igEnd();
}

static void apply_style(void)
{
    ImGuiStyle *style = igGetStyle();

    igStyleColorsDark(style);
    style->WindowRounding = 10.0f;
    style->FrameRounding = 8.0f;
    style->GrabRounding = 8.0f;
    style->TabRounding = 8.0f;
    style->FramePadding = (ImVec2){10.0f, 6.0f};
    style->ItemSpacing = (ImVec2){10.0f, 8.0f};
    style->WindowPadding = (ImVec2){12.0f, 12.0f};
    style->Colors[ImGuiCol_WindowBg] = (ImVec4){0.07f, 0.08f, 0.10f, 1.0f};
    style->Colors[ImGuiCol_ChildBg] = (ImVec4){0.10f, 0.11f, 0.14f, 1.0f};
    style->Colors[ImGuiCol_FrameBg] = (ImVec4){0.14f, 0.16f, 0.19f, 1.0f};
    style->Colors[ImGuiCol_Button] = (ImVec4){0.10f, 0.45f, 0.52f, 1.0f};
    style->Colors[ImGuiCol_ButtonHovered] = (ImVec4){0.14f, 0.57f, 0.65f, 1.0f};
    style->Colors[ImGuiCol_ButtonActive] = (ImVec4){0.08f, 0.37f, 0.44f, 1.0f};
    style->Colors[ImGuiCol_Header] = (ImVec4){0.15f, 0.19f, 0.24f, 1.0f};
    style->Colors[ImGuiCol_HeaderHovered] = (ImVec4){0.21f, 0.26f, 0.32f, 1.0f};
    style->Colors[ImGuiCol_Tab] = (ImVec4){0.11f, 0.14f, 0.18f, 1.0f};
    style->Colors[ImGuiCol_TabHovered] = (ImVec4){0.20f, 0.30f, 0.36f, 1.0f};
    style->Colors[ImGuiCol_TabSelected] = (ImVec4){0.12f, 0.43f, 0.50f, 1.0f};
    style->Colors[ImGuiCol_TitleBgActive] = (ImVec4){0.08f, 0.10f, 0.12f, 1.0f};
}

static void help(void)
{
    printf("LiteX-M2SDR RF Lab Dashboard.\n"
           "usage: m2sdr_lab_gui <lab-dir>\n");
}

int main(int argc, char **argv)
{
    struct lab_state lab;
    struct ui_state ui;
    SDL_Window *window = NULL;
    SDL_GLContext gl_context = NULL;
    SDL_Event event;
    bool done = false;

    if (argc != 2) {
        help();
        return 1;
    }
    if (!load_lab_manifest(&lab, argv[1])) {
        fprintf(stderr, "Could not load lab manifest from %s\n", argv[1]);
        return 1;
    }
    memset(&ui, 0, sizeof(ui));
    ui.selected_run = lab.runs_count > 0 ? 0 : -1;
    ui.selected_report = lab.reports_count > 0 ? 0 : -1;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    window = SDL_CreateWindow("LiteX-M2SDR RF Lab Dashboard",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              1480, 920, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    igCreateContext(NULL);
    apply_style();
    if (!m2sdr_imgui_sdl2_init_for_opengl(window, gl_context) ||
        !m2sdr_imgui_opengl3_init("#version 130")) {
        fprintf(stderr, "Could not initialize ImGui backends\n");
        igDestroyContext(NULL);
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    while (!done) {
        while (SDL_PollEvent(&event)) {
            m2sdr_imgui_sdl2_process_event(&event);
            if (event.type == SDL_QUIT) {
                done = true;
            } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                       event.window.windowID == SDL_GetWindowID(window)) {
                done = true;
            }
        }

        m2sdr_imgui_opengl3_new_frame();
        m2sdr_imgui_sdl2_new_frame();
        igNewFrame();
        draw_dashboard(&lab, &ui);
        igRender();

        glViewport(0, 0, (int)igGetIO()->DisplaySize.x, (int)igGetIO()->DisplaySize.y);
        glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        m2sdr_imgui_opengl3_render_draw_data(igGetDrawData());
        SDL_GL_SwapWindow(window);
    }

    m2sdr_imgui_opengl3_shutdown();
    m2sdr_imgui_sdl2_shutdown();
    igDestroyContext(NULL);
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
