/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility: types shared by its files.
 *
 * m2sdr_analyzer.c   main, command line of the core, help, the registry built from modes.h.
 * modes.h            The registry: one AN_MODE(name) line per view or mode, in tab order.
 * an_source.c        Samples from the RX of a board or from a capture file.
 * an_core.c          Analysis of a capture, reports (text, JSON), --report and --watch.
 * an_gui.c           Window: worker, traces, waterfall, controls, views of the spectrum, tabs.
 * an_trend.c         What the OFDM views share: MER and traces over the results of a tuning.
 * mode_*.c           One mode each: options, help, analysis, report sections, view, figures.
 * mode_*.mk          Its report test, a target of test-analyzer (optional).
 *
 * Adding a mode
 * -------------
 * 1. analyzer/mode_xxx.c, everything of the mode: its hooks, static, and its entry
 *    `const struct an_mode an_mode_xxx` (--view key, tab title, category, flag, result flag,
 *    report rank, options and their parser, defaults, help block...): see mode_dab.c. A mode
 *    that --watch follows has a watch_line hook and an option named as its key (--dab).
 * 2. analyzer/modes.h: one line, AN_MODE(xxx), at its place in the tabs.
 * 3. Here: its flag and settings in struct an_settings, its result and result flag in
 *    struct an_result, and the state of its view in struct an_ui if it keeps one.
 * 4. Optional: analyzer/mode_xxx.mk with its test-analyzer-xxx target (see mode_lte.mk), and its
 *    line in the docs (the manual, the README of the utilities, the changelog).
 * Nothing else: the Makefile builds analyzer/mode_*.c, main takes its options, the help prints
 * its block, the core captures what it needs, analyses while it is on, prints its sections in
 * the reports after the ones of the spectrum, and shows its tab in its category, the mode on
 * while its view is shown.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#ifndef M2SDR_ANALYZER_H
#define M2SDR_ANALYZER_H

#include <complex.h>
#include <getopt.h>
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
#include "m2sdr_analysis_dvbt2.h"
#include "m2sdr_analysis_fm.h"
#include "m2sdr_analysis_monitoring.h"
#include "m2sdr_analysis_cellular.h"
#include "m2sdr_analysis_wireless.h"

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
#define AN_FM_SECONDS           1.0           /* Capture of an FM analysis: about 11 RDS groups. */
/* Cellular: 5G NR SSB, GSM, TETRA. */
#define AN_NR_SECONDS           0.021         /* A burst of SSBs every 20 ms, wherever the capture starts. */
#define AN_NR_RATE              15.36e6       /* 30 kHz SSBs on 512 points, 15 kHz on 1024. */
#define AN_GSM_SECONDS          0.06          /* An FCCH and its SCH, 10 or 11 frames apart. */
#define AN_GSM_RATE             1.92e6
#define AN_TETRA_SECONDS        1.1           /* The synchronisation burst comes once a multiframe (1.02 s). */
#define AN_TETRA_RATE           1e6
#define AN_WIFI_SECONDS         0.105         /* Capture of a Wi-Fi analysis: a beacon interval (102.4 ms). */
#define AN_ADSB_SECONDS         0.5           /* Capture of an ADS-B analysis. */
#define AN_ADSB_SAMPLE_RATE     2.4e6         /* Pulses across two samples decode with less SNR than at 2 MSPS. */
#define AN_LORA_SECONDS         1.0           /* Capture of a LoRa analysis: an SF12 preamble and more. */
#define AN_LORA_SAMPLE_RATE     1e6           /* The three bandwidths divide it by a power of two. */
#define AN_TX_TONE_ATT_DB       60            /* Default attenuation of the test tone. */
#define AN_MIN_PERIOD_MS        40.0          /* Between two results shown. */
/* Monitoring. */
#define AN_PULSE_SECONDS        0.05          /* Capture of a pulse analysis: 3 pulses of PRIs up to 16 ms. */
#define AN_PULSE_ENV_POINTS     2048          /* Envelope of the capture kept for the view (peak per point). */
#define AN_EMC_DEFAULT_LOG2     18            /* Samples of an EMC capture: 2^18, more when the RBW needs them. */
#define AN_EMC_MIN_LOG2         12
#define AN_EMC_MAX_LOG2         22
#define AN_EMC_DEFAULT_CHANNELS 201           /* Default span: 100 RBW (C/D: 12 MHz), for a live speed. */

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
    bool   fm_on;                  /* FM broadcast station. */
    double fm_tone_hz;             /* Its known test tone (stereo separation); 0: none. */
    int    fm_deemph_us;           /* De-emphasis of its audio SNR: 0: 50 us, -1: none. */
    /* Monitoring: pulses and EMC pre-compliance. */
    bool   pulse_on;               /* Pulsed signals. */
    double pulse_threshold_db;     /* Over the noise floor; 0: the default of libm2sdr. */
    double pulse_seconds;          /* Capture; 0: AN_PULSE_SECONDS. */
    bool   emc_on;                 /* EMC pre-compliance. */
    int    emc_log2;               /* Its capture: 2^emc_log2 samples; 0: the default. */
    double emc_span_hz;            /* Channels over this span around the centre; 0: the default, < 0: all. */
    struct m2sdr_emc_config emc;   /* Band, unit offset, factors and limit lines (centre and channels set per capture). */
    /* Cellular. nr_scs_hz: spacing of the SSBs, 0 to find it; nr_scs_known: the one found
     * on this tuning, set by the worker (0: none). */
    bool   nr_on, gsm_on, tetra_on;
    double nr_scs_hz, nr_scs_known;
    bool   wifi_on;                /* Wi-Fi channel. */
    bool   adsb_on;                /* ADS-B at 1090 MHz. */
    struct m2sdr_adsb_config adsb; /* Its receiver position. */
    bool   lora_on;                /* LoRa channel. */
    /* Set by the worker for a live multiplex whose TPS it read: what it knows of it (fft_len 0:
     * nothing), and whether the capture is a short one. */
    bool   dvbt_fast;
    struct m2sdr_dvbt_config dvbt_hint;
    /* DVB-T2 multiplex; set by the worker once its mode and pattern are known: short captures. */
    bool   dvbt2_on;
    bool   dvbt2_fast;
    struct m2sdr_dvbt2_config dvbt2_hint;
};

extern const char *const window_names[];  /* enum m2sdr_window. */

/* Source */
/*--------*/

/* Time of the first sample of a capture, and what it is read from. */
enum an_clock {
    AN_CLOCK_NONE    = 0,
    AN_CLOCK_SAMPLES = 1,          /* File: its sample count, exact; consecutive captures are contiguous. */
    AN_CLOCK_BOARD   = 2,          /* Board: the timestamp of its DMA header. */
    AN_CLOCK_HOST    = 3,          /* Board without timestamps: the host clock, within a millisecond or so. */
};

struct an_capture_time {
    double t_s;
    enum an_clock clock;
};

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
    struct an_capture_time capture;  /* Of the last capture. */
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
    struct m2sdr_fm_result fm;
    bool   fm_valid;               /* Measured: the sample rate was 240 kSPS or more; fm.found says the rest. */
    struct m2sdr_pulse_result pulse;
    bool   pulse_valid;            /* Measured; pulse.found says whether there were pulses. */
    float  pulse_env_db[AN_PULSE_ENV_POINTS];   /* Highest power per point of the capture, dBFS. */
    double pulse_env_s;            /* Time of the capture over them. */
    struct m2sdr_emc_result emc;
    bool   emc_valid;              /* Measured: the band fits the sample rate and the capture. */
    /* Cellular: measured (rate and length right); found says the rest. */
    struct m2sdr_nr_result nr;
    bool   nr_valid;
    struct m2sdr_gsm_result gsm;
    bool   gsm_valid;
    struct m2sdr_tetra_result tetra;
    bool   tetra_valid;
    struct m2sdr_wifi_result wifi;
    bool   wifi_valid;             /* Measured: the sample rate was 20 MSPS. */
    bool   wifi_no_psdu;           /* The data was not decoded: it took longer than the capture. */
    struct m2sdr_adsb_result adsb;
    struct m2sdr_adsb_state adsb_state;   /* Aircraft of the captures so far. */
    bool   adsb_valid;             /* Measured: the sample rate was 2 MSPS or more. */
    struct m2sdr_lora_result lora;
    bool   lora_valid;             /* Measured: a LoRa bandwidth divides the sample rate by a power of two. */
    struct an_capture_time capture;  /* Set from the source before the analysis. */
    float complex time[AN_TIME_POINTS];
    size_t time_count;
    bool   lost;                   /* Samples were lost in the capture. */
    struct m2sdr_dvbt2_result dvbt2;
    bool   dvbt2_valid;            /* Measured: the sample rate was one of DVB-T2; dvbt2.found says the rest. */
};

size_t capture_samples(const struct an_settings *set);
int    analyse(const float complex *x, size_t count, const struct an_settings *set, struct an_result *res);

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

int  floor_default_path(uint64_t dna, char *path, size_t len);
int  floor_load(struct an_floor *fl, const char *path);
int  floor_save(struct an_floor *fl, const char *path, const struct an_result *res);

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

/* Command line: what the options of the modes set. A mode option is given to getopt_long with
 * the id AN_OPT_BASE + AN_OPT_IDS * (index of the mode in an_modes[]) + (its id in the mode). */
#define AN_OPT_BASE  1000
#define AN_OPT_IDS   32            /* Ids of the options of a mode: 0 to AN_OPT_IDS - 1. */

struct an_cli {
    struct an_settings *set;
    struct an_source   *src;
    const char *floor_arg;         /* --pn-floor: file of the floor of the board. */
    bool   floor_store;            /* --pn-floor-save. */
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
    double default_freq;           /* Centre frequency when it is on and none is given; 0: none. */
    bool   psd_short;              /* Its capture is long: the spectrum only takes its own averages of it. */
    /* Command line: its defaults in the settings, set before the options are read; its long
     * options, their .val its own ids, NULL-name terminated; their parser, that returns 0, or -1
     * after its message; and its block of the help. */
    void   (*defaults)(struct an_settings *set);
    const struct option *options;
    int    (*option)(int id, const char *arg, struct an_cli *cli);
    void   (*help)(void);             /* Its block of the help, ending with an empty line. */
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

/* The flag of a mode in settings; NULL for a view of the spectrum. */
static inline bool *an_mode_flag(const struct an_mode *m, struct an_settings *set)
{
    return m->on == AN_NONE ? NULL : (bool *)((char *)set + m->on);
}

static inline bool an_mode_on(const struct an_mode *m, const struct an_settings *set)
{
    return m->on != AN_NONE && *(const bool *)((const char *)set + m->on);
}

/* Reports */
/*---------*/

void   print_json_number(const char *name, double value, bool last);
int    export_csv(const char *path, const struct an_result *res);
int    run_report(struct an_source *src, const struct an_settings *set, bool json, const char *csv_path,
                  struct an_floor *fl, const char *floor_save_path);

/* --watch */
double now_ms(void);
int    watch_modes(const struct an_settings *set, const struct an_mode **mode);
int    run_watch(struct an_source *src, const struct an_settings *base, bool json);

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
    int    tab;                    /* View shown: its index in an_modes[]. */
    /* Phase noise. */
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
    struct an_fm_ui *fm;           /* FM: its view, see mode_fm.c. */
    /* NR: grid of the strongest SSB as a texture. */
    GLuint nr_grid_tex;
    float  wf_floor_db, wf_ceil_db;
    char   message[256];
    /* DVB-T2: MER over the results of one tuning, and the last capture with a P1. */
    struct an_trend dvbt2;
    struct m2sdr_dvbt2_result *dvbt2_p1;
};

int  run_gui(struct an_source *src, const struct an_settings *set, int first_view, struct an_floor *fl,
             const char *floor_path);

/* Views of the spectrum. */
void draw_spectrum(struct an_ui *ui, bool live, bool *changed);
void draw_time(struct an_ui *ui, bool live, bool *changed);
void draw_statistics(struct an_ui *ui, bool live, bool *changed);

/* Widgets of the views. */
uint32_t palette_color(int palette, float t);
void legend_item(ImU32 color, const char *name, float right);
void label_value(const char *label, const char *fmt, ...);
bool input_scaled(const char *label, double *value, double scale, const char *fmt);
void same_line_or_wrap(float spacing, float width);

/* Span and traces. */
void view_full(struct an_ui *ui);
void traces_reset(struct an_ui *ui);
void view_retune(struct an_ui *ui, double freq_hz, bool *changed);
void view_rate(struct an_ui *ui, double rate, bool *changed);

/* OFDM views: an_trend.c. */
bool   trend_add(struct an_trend *tr, const struct an_settings *set, bool ok, double mer, const float *mer_bin,
                 const float *channel_bin, int bins, const float *echo, int echo_bins);
double trend_mer_shown(const struct an_trend *tr, double last);
void   draw_trend_mer(const char *id, const struct an_trend *tr, bool ok, double last, double needed, const char *line,
                      float width, float height);
void   draw_trend_points(const char *id, ImVec2 size, const float complex *points, int n, enum m2sdr_modulation mod);
void   draw_trend_traces(const char *id, const struct an_trend *tr, int bins, double half_hz, double echo_start_us,
                         double echo_step_us, int echo_bins, double guard_us, double needed);

#endif /* M2SDR_ANALYZER_GUI */

#endif /* M2SDR_ANALYZER_H */
