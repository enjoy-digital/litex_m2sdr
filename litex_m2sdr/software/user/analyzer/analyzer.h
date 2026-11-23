/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: types shared by its files.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_ANALYZER_H
#define M2SDR_ANALYZER_H

#include <complex.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef M2SDR_ANALYZER_GUI
#include <GL/gl.h>
#include "scan_ui/m2sdr_plot.h"
#define igGetIO igGetIO_Nil
#endif

#include "m2sdr.h"
#include "m2sdr_analysis.h"

#define AN_DEFAULT_FREQ_HZ     2400e6
#define AN_DEFAULT_SAMPLE_RATE 7680000.0
#define AN_DEFAULT_FFT_LEN     16384
#define AN_DEFAULT_AVERAGES    16
#define AN_MIN_FFT_LEN         256
#define AN_MAX_FFT_LEN         (1 << 20)
#define AN_MAX_AVERAGES        1024
#define AN_TIME_POINTS         8192          /* Samples kept for the time views. */
#define AN_MAX_PEAKS           8
#define AN_PEAK_EXCURSION_DB   6.0
#define AN_PEAK_MIN_ABOVE_DB    10.0          /* A peak or a tone stands this far above the noise in a bin. */
#define AN_SETTLE_US           30000         /* After a change of the RFIC. */
#define AN_CAPTURE_RETRIES     3             /* Captures redone when samples were lost. */
#define AN_FRAME_HEADER_BYTES  16
#define AN_PN_DEFAULT_LOG2      20            /* Samples of a phase noise capture: 2^20. */
#define AN_PN_MIN_LOG2          16
#define AN_PN_MAX_LOG2          23
#define AN_PN_FLOOR_MARGIN_DB   3.0           /* A reading this close to the floor is the floor. */
#define AN_DEMOD_DEFAULT_LOG2   18            /* Samples of a demodulation capture: 2^18. */
#define AN_DEMOD_MIN_LOG2       14
#define AN_DEMOD_MAX_LOG2       22
#define AN_LTE_SECONDS          0.045         /* Capture of an LTE analysis: a whole frame wherever it starts. */
#define AN_DVBT_SECONDS         0.15          /* Capture of a DVB-T analysis: a TPS frame wherever it starts. */
#define AN_DVBT_FAST_SECONDS    0.04          /* Once the TPS is read: the MER and the channel. */
#define AN_DVBT_FULL_EVERY      300           /* Live, one capture in this many (about 15 s) reads the TPS again. */
#define AN_WATCH_HEADER_EVERY   20            /* Lines of --watch between two headers of the columns. */
#define AN_DVBT_RATE            (64e6 / 7.0)  /* Elementary rate of an 8 MHz DVB-T channel. */
#define AN_DAB_SECONDS          0.2           /* Capture of a DAB analysis: a whole 96 ms frame wherever it starts. */
#define AN_TX_TONE_ATT_DB       60            /* Default attenuation of the test tone. */
#define AN_MIN_PERIOD_MS        40.0          /* Between two results shown. */

/* Settings */
/*----------*/

/* What a capture and its analysis depend on. The UI edits a copy, the worker applies it. */
struct an_settings {
    double freq_hz;                /* Centre of the capture. */
    double sample_rate;
    double bandwidth;              /* RF bandwidth; 0: from the sample rate. */
    bool   agc;
    int    rx_gain;                /* Manual gain, dB. */
    int    channel;                /* 0: RX1, 1: RX2. */
    int    fft_len;
    int    window;                 /* enum m2sdr_window. */
    int    averages;               /* Segments averaged in one capture. */
    double chan_offset_hz;         /* Channel measured: its centre from the centre of the capture. */
    double chan_bw_hz;             /* 0: no channel measurement. */
    double chan_spacing_hz;        /* 0: the bandwidth. */
    double ref_offset_db;          /* Added to the levels: dBFS to dBm at the connector. */
    bool   pn_on;                  /* Phase noise of the strongest carrier. */
    int    pn_log2;                /* Its capture: 2^pn_log2 samples. */
    double pn_lo_hz, pn_hi_hz;     /* Integration limits; 0: the offsets measured. */
    bool   demod_on;               /* Demodulation of the strongest signal. */
    int    demod_log2;             /* Its capture: 2^demod_log2 samples. */
    struct m2sdr_demod_config demod;
    bool   lte_on;                 /* LTE downlink carrier. */
    bool   dvbt_on;                /* DVB-T multiplex. */
    bool   dab_on;                 /* DAB ensemble. */
    /* Set by the worker for a live multiplex whose TPS it read: what it knows of it (fft_len 0:
     * nothing), and whether the capture is a short one. */
    bool   dvbt_fast;
    struct m2sdr_dvbt_config dvbt_hint;
};

/* Source */
/*--------*/

struct an_source {
    bool   live;
    char   name[1024];
    /* Board. */
    struct m2sdr_dev *dev;
    double trim_ppm;
    const char *sync;
    int64_t refclk_freq;
    bool   no_rf_init;             /* The RFIC is left as another utility set it. */
    bool   tx_tone;                /* Test tone on TX, from the RFIC itself. */
    int    tx_att;
    uint64_t dna;
    struct an_settings applied;    /* What the RFIC is set to. */
    bool   configured;
    /* File. */
    FILE  *file;
    int    sample_bytes;           /* 1: SC8, 2: SC16. */
    int    nbits;
    int    nchannels;
    bool   frame_header;
    size_t frame_size;
    uint64_t data_start;           /* Bytes. */
    uint64_t total_sets;           /* Sample periods in the file. */
    uint64_t position;             /* Next sample period read. */
    bool   loop;
    double file_rate;
    double file_freq;
    /* Buffer of raw samples. */
    void  *raw;
    size_t raw_bytes;
    unsigned lost;                 /* Captures with samples lost, since the start. */
};

int  file_open(struct an_source *src, const char *path, int capture_index, char *err, size_t err_len);
int  source_capture(struct an_source *src, const struct an_settings *set, float complex *x,
                    size_t count, char *err, size_t err_len);
void source_close(struct an_source *src);

/* Analysis */
/*----------*/

struct an_result {
    struct an_settings set;        /* Settings of the capture. */
    struct m2sdr_psd psd;
    struct m2sdr_psd tone_psd;     /* Worker only: spectrum behind the tone figures. */
    struct m2sdr_iq_stats stats;
    struct m2sdr_tone tone;
    bool   tone_valid;
    struct m2sdr_psd_peak peaks[AN_MAX_PEAKS];
    int    npeaks;
    bool   chan_valid;
    double chan_power_db;
    struct m2sdr_acpr acpr;
    bool   acpr_valid;
    double obw_lo_hz, obw_hi_hz;
    bool   obw_valid;
    double noise_db_hz;
    struct m2sdr_phase_noise pn;
    bool   pn_valid;               /* Measured, with a carrier. */
    struct m2sdr_demod_result demod;
    bool   demod_valid;            /* Measured; demod.signal and demod.locked say what was found. */
    struct m2sdr_lte_result lte;
    bool   lte_valid;              /* Measured: the sample rate was one of LTE; lte.found says the rest. */
    struct m2sdr_dvbt_result dvbt;
    bool   dvbt_valid;             /* Measured: the sample rate was one of DVB-T; dvbt.found says the rest. */
    struct m2sdr_dab_result dab;
    bool   dab_valid;              /* Measured: the sample rate was 2.048 MSPS; dab.found says the rest. */
    float complex time[AN_TIME_POINTS];
    size_t time_count;
    bool   lost;                   /* Samples were lost in the capture. */
};

/* Phase noise floor */
/*-------------------*/

/* Phase noise read with a source cleaner than the board, or with its own test tone: what the
 * board adds. Kept per board, in a text file. */
struct an_floor {
    int    points;
    double offset_hz[M2SDR_PN_MAX_POINTS];
    double l_dbc_hz[M2SDR_PN_MAX_POINTS];
    double freq_hz, sample_rate;   /* Of the capture it comes from. */
    char   path[1024];
};

/* Modes */
/*-------*/

/* Groups of the views, in the order of their tabs. */
enum an_category {
    AN_CAT_SPECTRUM,
    AN_CAT_SIGNAL,
    AN_CAT_CELLULAR,
    AN_CAT_BROADCAST,
    AN_CAT_WIRELESS,
    AN_CAT_LAB,
    AN_CAT_TWO_CHANNEL,
    AN_CAT_TIMING,
    AN_CAT_MONITORING,
    AN_CAT_COUNT
};

#define AN_NONE      ((size_t)-1)  /* Offset of a flag a mode does not have. */
#define AN_MODES_MAX 64            /* Views and modes. */

/* What the reports of the modes get besides the result. */
struct an_report {
    double off;                    /* Added to the levels: dBFS to dBm. */
    const char *unit;              /* Of the levels: "dBm" with an offset, "dBFS" without. */
    const struct an_floor *fl;     /* Phase noise floor of the board; NULL or empty: none. */
};

struct an_ui;

/* A view of the window, and the measurement behind it. The views of the spectrum are measured
 * with every capture and only have their view; a mode has its flag in the settings, is measured
 * while it is on, and has its section in the reports. A hook left NULL is not called. */
struct an_mode {
    const char *key;               /* --view name: "dvbt". */
    const char *title;             /* Its tab: "DVB-T". */
    enum an_category category;
    size_t on;                     /* offsetof(struct an_settings, <flag>); AN_NONE: always measured. */
    size_t valid;                  /* offsetof(struct an_result, <flag>): cleared before each analysis. */
    int    report_rank;            /* Place of its section in the reports: lower first. */
    double default_rate;           /* Sample rate when it is on and none is given; 0: none. */
    bool   psd_short;              /* Its capture is long: the spectrum only takes its own averages of it. */
    /* Capture and analysis. */
    size_t (*samples)(const struct an_settings *set);             /* Samples it needs. */
    void   (*analyse)(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res);
    /* Its section of the reports, after the ones of the spectrum. JSON: its "key": {...} block,
     * from its first line to its closing brace, with no comma nor newline after it. */
    void   (*report_text)(const struct an_result *res, const struct an_report *rep);
    void   (*report_json)(const struct an_result *res, const struct an_report *rep);
    /* --watch on a board: before each capture, then one line per result (now: seconds since the
     * start); < 0: stop on an error. */
    void   (*watch_prepare)(struct an_settings *set);
    int    (*watch_line)(const struct an_settings *set, const struct an_result *res, double now, bool json);
#ifdef M2SDR_ANALYZER_GUI
    /* Worker: settings of a capture from the results before, and each result. */
    void   (*worker_prepare)(struct an_settings *set, bool live);
    void   (*worker_result)(const struct an_settings *set, const struct an_result *res);
    /* Window: its view (live: the RFIC can be tuned from it), its figures (NULL: the ones of the
     * spectrum), each new result, and the end. */
    void   (*draw)(struct an_ui *ui, bool live, bool *changed);
    void   (*results)(struct an_ui *ui);
    void   (*keep)(struct an_ui *ui);
    void   (*close)(struct an_ui *ui);
#endif
};

/* The views and modes, in the order of their tabs, NULL terminated. */
extern const struct an_mode *const an_modes[];

extern const struct an_mode an_mode_spectrum, an_mode_time, an_mode_statistics;
extern const struct an_mode an_mode_pn, an_mode_demod, an_mode_lte, an_mode_dvbt, an_mode_dab;

/* The flag of a mode in settings; NULL for a view of the spectrum. */
static inline bool *an_mode_flag(const struct an_mode *m, struct an_settings *set)
{
    return m->on == AN_NONE ? NULL : (bool *)((char *)set + m->on);
}

static inline bool an_mode_on(const struct an_mode *m, const struct an_settings *set)
{
    return m->on != AN_NONE && *(const bool *)((const char *)set + m->on);
}

/* GUI */
/*-----*/

#ifdef M2SDR_ANALYZER_GUI

#define UI_MARKERS       4
#define UI_WF_COLS       1024
#define UI_WF_ROWS       256
#define UI_TRACES        4

enum { TRACE_LIVE = 0, TRACE_AVG = 1, TRACE_MAX = 2, TRACE_MIN = 3 };

#define AN_DVBT_HISTORY    300       /* Captures of the MER trace: about 13 s. */
#define AN_TREND_BINS      512       /* At least the per-carrier and echo bins of DVB-T and DAB. */

/* MER of each result and traces averaged over the results of one tuning (DVB-T, DAB): what to
 * follow when moving an antenna. */
struct an_trend {
    float  mer[AN_DVBT_HISTORY];   /* Oldest first. */
    int    nmer;
    float  mer_bin[AN_TREND_BINS], channel_bin[AN_TREND_BINS], echo[AN_TREND_BINS];
    int    navg;
    double freq_hz, rate;
    int    channel;
};

/* Shared between the worker, which captures and analyses, and the UI, which draws. */
struct an_shared {
    pthread_mutex_t lock;
    struct an_settings set;        /* Wanted by the UI. */
    bool   run;
    bool   single;                 /* One capture, then stop. */
    bool   quit;
    struct an_result res;          /* Last result; its psd.power is a copy owned here. */
    unsigned seq;                  /* Counts the results. */
    char   status[256];
    double capture_ms, analysis_ms;
    struct an_source *src;
};

struct an_marker {
    bool   on;
    double offset_hz;
};

struct an_ui {
    struct an_settings set;        /* Edited by the controls. */
    struct an_result res;          /* Result being drawn. */
    unsigned seq;
    /* Traces, in dB, of res.psd.bins points. */
    float *trace[UI_TRACES];
    double *avg_lin;
    size_t bins;
    bool   trace_on[UI_TRACES];
    int    avg_count, avg_target;
    /* View. */
    double view_lo, view_hi;       /* Offsets from the centre, Hz. */
    double ref_db, range_db;
    struct an_marker markers[UI_MARKERS];
    int    marker_sel;
    /* Waterfall. */
    GLuint wf_tex;
    uint32_t *wf_rgba;
    int    wf_palette;
    bool   wf_on;
    bool   wf_auto;                /* Levels to be set from the next result. */
    /* Phase noise. */
    int    tab;                    /* View shown: 0 spectrum, 1 time, 2 statistics, 3 phase noise, 4 modulation, 5 LTE, 6 DVB-T, 7 DAB. */
    bool   pn_am;
    struct an_floor *fl;
    const char *floor_path;        /* Where "Store as floor" writes; NULL: nowhere. */
    /* LTE: resource grid and blocks as textures, redrawn with each result that has a cell. */
    GLuint lte_grid_tex, lte_block_tex;
    uint32_t *lte_rgba;
    /* Last frame with a cell, and symbols of the last one with data read: held, see lte_keep(). */
    struct m2sdr_lte_result *lte_last;
    unsigned lte_last_seq;
    bool   lte_last_ok;
    double lte_freq_hz, lte_rate;  /* What they were read on: nothing is held for another signal. */
    int    lte_channel;
    float complex lte_points[M2SDR_LTE_MAX_POINTS];
    uint8_t lte_points_mod[M2SDR_LTE_MAX_POINTS];
    int    lte_npoints, lte_points_sfn;
    unsigned lte_points_seq;
    /* DVB-T and DAB: MER and traces over the results of one tuning. */
    struct an_trend dvbt, dab;
    /* Last capture whose TPS was read: the short ones between carry none. */
    struct m2sdr_dvbt_result *dvbt_tps;
    float  wf_floor_db, wf_ceil_db;
    char   message[256];
};

#endif /* M2SDR_ANALYZER_GUI */

#endif /* M2SDR_ANALYZER_H */
