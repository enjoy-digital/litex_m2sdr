/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Reference Clock Calibration Utility.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 * Description:
 * Measures the error of the reference clock of the board against cellular downlink signals,
 * whose carriers are within 0.05 ppm for a macro base station: the frequency offset of LTE
 * carriers (cyclic prefix and PSS) and of GSM carriers (FCCH bursts). A scan lists the carriers
 * received in a band. The result can be applied
 * to the board (SI5351 trim) and stored, in a file and in the SPI flash of the board.
 *
 * Usage Example:
 *     ./m2sdr_cal scan
 *     ./m2sdr_cal measure lte:816e6 earfcn:1300 arfcn:85
 *     ./m2sdr_cal measure --apply --store lte:816e6 lte:1815e6
 *     ./m2sdr_cal show
 *     ./m2sdr_rf --refclk-ppm stored
 *
 */

#include <complex.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "m2sdr_cal.h"

/* Constants */
/*-----------*/

#define CAL_MAX_SIGNALS    32
#define CAL_MAX_REPEAT     64
#define CAL_SETTLE_US      50000   /* After a retune: PLL lock and AGC. */
#define CAL_BANDWIDTH_HZ   1400000
#define CAL_OUTLIER_PPM     1.0     /* A capture further than this from the median of its signal is left out. */

struct cal_signal {
    enum m2sdr_cal_signal kind;
    double freq_hz;
    char   name[32];
    /* Results. */
    int    found;                  /* Captures in which the signal was found. */
    double ppm;                    /* Median of them. */
    double spread_ppm;             /* Their standard deviation. */
    double power_db;
};

struct cal_options {
    double duration_s;
    int    repeat;
    double max_ppm;
    bool   manual_gain;
    int    rx_gain;
    unsigned channel;              /* 0: RX1, 1: RX2. */
    bool   apply;
    bool   store_file;
    bool   store_flash;
    bool   yes;
    bool   ppm_only;
    const char *file;
    uint32_t flash_offset;
    const char *sync;
    int64_t refclk_freq;
    bool   trim_stored;            /* --refclk-ppm stored. */
    double trim_ppm;               /* Trim of the reference during the measure. */
};

/* Helpers */
/*---------*/

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median_of(double *v, int n)
{
    qsort(v, (size_t)n, sizeof(*v), cmp_double);
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static double stddev_of(const double *v, int n)
{
    double sum = 0, sum2 = 0;
    int i;

    for (i = 0; i < n; i++) {
        sum  += v[i];
        sum2 += v[i] * v[i];
    }
    return n > 1 ? sqrt(fmax(0.0, sum2 / n - (sum / n) * (sum / n))) : 0.0;
}

static int parse_signal(const char *text, struct cal_signal *sig)
{
    const char *colon = strchr(text, ':');
    double value;

    if (!colon || m2sdr_cli_parse_double(colon + 1, &value) != 0)
        return -1;
    memset(sig, 0, sizeof(*sig));
    if (strncmp(text, "lte:", 4) == 0) {
        sig->kind    = M2SDR_CAL_SIGNAL_LTE;
        sig->freq_hz = value;
    } else if (strncmp(text, "gsm:", 4) == 0) {
        sig->kind    = M2SDR_CAL_SIGNAL_GSM;
        sig->freq_hz = value;
    } else if (strncmp(text, "earfcn:", 7) == 0) {
        sig->kind    = M2SDR_CAL_SIGNAL_LTE;
        sig->freq_hz = m2sdr_cal_earfcn_to_hz((unsigned)value);
    } else if (strncmp(text, "arfcn:", 6) == 0) {
        sig->kind    = M2SDR_CAL_SIGNAL_GSM;
        sig->freq_hz = m2sdr_cal_arfcn_to_hz((unsigned)value);
    } else {
        return -1;
    }
    if (sig->freq_hz < 70e6 || sig->freq_hz > 6e9)
        return -1;
    snprintf(sig->name, sizeof(sig->name), "%s %.1f MHz",
             sig->kind == M2SDR_CAL_SIGNAL_LTE ? "LTE" : "GSM", sig->freq_hz / 1e6);
    return 0;
}

static void print_record(const char *where, const struct m2sdr_cal_record *rec)
{
    char date[32] = "unknown date";
    const time_t t = (time_t)rec->unix_time;
    struct tm tm;

    if (rec->unix_time && gmtime_r(&t, &tm))
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M UTC", &tm);
    printf("%s: %+.3f ppm (spread %.3f ppm over %u signal%s), %s, FPGA at %.1f C, board %014llx\n",
           where, rec->refclk_ppm, rec->spread_ppm, rec->signals, rec->signals == 1 ? "" : "s",
           date, rec->temperature_c, (unsigned long long)rec->fpga_dna);
    if (rec->note[0])
        printf("  signals: %s\n", rec->note);
}

static bool confirm(const char *question, bool yes)
{
    char answer[16];

    if (yes)
        return true;
    printf("%s [y/N] ", question);
    fflush(stdout);
    if (!fgets(answer, sizeof(answer), stdin))
        return false;
    return answer[0] == 'y' || answer[0] == 'Y';
}

/* Capture and measure */
/*---------------------*/

/* RFIC and RX stream for the captures. The reference is used without a trim, so that what is
 * measured is its error, or with the trim of --refclk-ppm, and what is measured is what remains. */
static int rf_setup(struct m2sdr_dev *dev, const struct cal_options *opt, double sample_rate,
                    int64_t bandwidth, double freq_hz)
{
    const unsigned per_buf = m2sdr_bytes_to_samples(M2SDR_FORMAT_SC16_Q11, M2SDR_BUFFER_BYTES);
    struct m2sdr_config cfg;
    int rc;

    m2sdr_config_init(&cfg);
    cfg.sample_rate    = (int64_t)sample_rate;
    cfg.bandwidth      = bandwidth;
    cfg.rx_freq        = (int64_t)llround(freq_hz);
    cfg.tx_att         = 89;
    cfg.refclk_ppm     = opt->trim_ppm;
    cfg.channel_layout = M2SDR_CHANNEL_LAYOUT_2T2R;
    if (opt->refclk_freq)
        cfg.refclk_freq = opt->refclk_freq;
    if (opt->sync) {
        if (strcmp(opt->sync, "internal") == 0) {
            cfg.clock_source = M2SDR_CLOCK_SOURCE_INTERNAL;
        } else if (strcmp(opt->sync, "external") == 0) {
            cfg.clock_source = M2SDR_CLOCK_SOURCE_EXTERNAL;
        } else if (strcmp(opt->sync, "fpga") == 0) {
            cfg.clock_source = M2SDR_CLOCK_SOURCE_SI5351C_FPGA;
        } else {
            m2sdr_cli_invalid_choice("sync", opt->sync, "internal, external or fpga");
            return -1;
        }
    }
    if (opt->manual_gain) {
        cfg.rx_gain1 = cfg.rx_gain2 = opt->rx_gain;
    } else {
        cfg.program_rx_gains      = false;
        cfg.program_rx_gain_modes = true;
        cfg.rx_gain_mode1 = cfg.rx_gain_mode2 = M2SDR_RX_GAIN_MODE_SLOW_ATTACK_AGC;
    }
    rc = m2sdr_apply_config(dev, &cfg);
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_apply_config failed: %s\n", m2sdr_strerror(rc));
        return -1;
    }
    rc = m2sdr_sync_config(dev, M2SDR_RX, M2SDR_FORMAT_SC16_Q11, 0, per_buf, 0, 2000);
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_sync_config failed: %s\n", m2sdr_strerror(rc));
        return -1;
    }
    return 0;
}

/* One capture of `count` samples of the selected channel, tuned on freq_hz. */
static int capture(struct m2sdr_dev *dev, const struct cal_options *opt, double freq_hz,
                   int16_t *raw, float complex *x, size_t count)
{
    const unsigned per_buf = m2sdr_bytes_to_samples(M2SDR_FORMAT_SC16_Q11, M2SDR_BUFFER_BYTES);
    const size_t bufs = (count * 2 + per_buf - 1) / per_buf;   /* 2T2R: two channels per sample period. */
    size_t b;
    int rc;

    rc = m2sdr_set_rx_frequency(dev, (uint64_t)llround(freq_hz));
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr, "Could not tune to %.3f MHz: %s\n", freq_hz / 1e6, m2sdr_strerror(rc));
        return -1;
    }
    usleep(CAL_SETTLE_US);
    (void)m2sdr_sync_rx_flush(dev);
    for (b = 0; b < bufs; b++) {
        rc = m2sdr_sync_rx(dev, raw + b * per_buf * 2, per_buf, NULL, 2000);
        if (rc != M2SDR_ERR_OK) {
            fprintf(stderr, "RX failed: %s\n", m2sdr_strerror(rc));
            return -1;
        }
    }
    m2sdr_cal_iq_to_complex(raw + 2 * opt->channel, count, 4, 12, x);
    return 0;
}

/* Measure a signal opt->repeat times; fills its results. */
static int measure_signal(struct m2sdr_dev *dev, const struct cal_options *opt, struct cal_signal *sig,
                          int16_t *raw, float complex *x, size_t count, bool verbose)
{
    const double max_cfo_hz = opt->max_ppm * 1e-6 * sig->freq_hz;
    double ppm[CAL_MAX_REPEAT];
    double power = 0;
    int r;

    sig->found = 0;
    for (r = 0; r < opt->repeat; r++) {
        struct m2sdr_cal_estimate est;
        int rc;

        if (capture(dev, opt, sig->freq_hz, raw, x, count) != 0)
            return -1;
        rc = sig->kind == M2SDR_CAL_SIGNAL_LTE
            ? m2sdr_cal_lte_estimate(x, count, max_cfo_hz, &est)
            : m2sdr_cal_gsm_estimate(x, count, max_cfo_hz, &est);
        if (rc != 0) {
            fprintf(stderr, "Estimate failed on %s\n", sig->name);
            return -1;
        }
        power += est.power_db;
        if (verbose) {
            if (!est.found)
                printf("  %-16s #%d: not found (%.1f dBFS, %s %.1f)\n", sig->name, r + 1, est.power_db,
                       sig->kind == M2SDR_CAL_SIGNAL_LTE ? "PSS metric" : "FCCH bursts",
                       sig->kind == M2SDR_CAL_SIGNAL_LTE ? est.quality : (double)est.count);
            else if (sig->kind == M2SDR_CAL_SIGNAL_LTE)
                printf("  %-16s #%d: offset %+9.1f Hz (+-%.1f) = %+.3f ppm | %.1f dBFS, PSS root %d, metric %.0f\n",
                       sig->name, r + 1, est.cfo_hz, est.spread_hz,
                       m2sdr_cal_refclk_ppm(est.cfo_hz, sig->freq_hz), est.power_db, est.id, est.quality);
            else
                printf("  %-16s #%d: offset %+9.1f Hz (+-%.1f) = %+.3f ppm | %.1f dBFS, %d FCCH bursts\n",
                       sig->name, r + 1, est.cfo_hz, est.spread_hz,
                       m2sdr_cal_refclk_ppm(est.cfo_hz, sig->freq_hz), est.power_db, est.count);
        }
        if (est.found)
            ppm[sig->found++] = m2sdr_cal_refclk_ppm(est.cfo_hz, sig->freq_hz);
    }
    sig->power_db = power / opt->repeat;
    if (sig->found) {
        /* A capture can pick the offset one LTE subcarrier away, 15 kHz: the captures far from
         * the median are left out, and the count shows it. */
        const double centre = median_of(ppm, sig->found);
        int used = 0;

        for (r = 0; r < sig->found; r++) {
            if (fabs(ppm[r] - centre) < CAL_OUTLIER_PPM)
                ppm[used++] = ppm[r];
        }
        sig->found      = used;
        sig->spread_ppm = stddev_of(ppm, used);
        sig->ppm        = median_of(ppm, used);
    }
    return 0;
}

/* Commands */
/*----------*/

static int cmd_measure(struct m2sdr_dev *dev, const struct cal_options *opt,
                       struct cal_signal *signals, int nsignals)
{
    const size_t count = (size_t)(opt->duration_s * M2SDR_CAL_SAMPLE_RATE);
    const unsigned per_buf = m2sdr_bytes_to_samples(M2SDR_FORMAT_SC16_Q11, M2SDR_BUFFER_BYTES);
    struct m2sdr_fpga_sensors sensors;
    struct m2sdr_cal_record rec;
    double ppm[CAL_MAX_SIGNALS];
    int16_t *raw;
    float complex *x;
    int used = 0, i, rc, status = 1;
    double result, spread;

    raw = malloc(((count * 2 + per_buf - 1) / per_buf + 1) * per_buf * 2 * sizeof(*raw));
    x   = malloc(count * sizeof(*x));
    if (!raw || !x) {
        fprintf(stderr, "Out of memory\n");
        goto out;
    }

    if (rf_setup(dev, opt, M2SDR_CAL_SAMPLE_RATE, CAL_BANDWIDTH_HZ, signals[0].freq_hz) != 0)
        goto out;

    printf("Measuring %d signal%s, %d capture%s of %.2f s each, RX%u, %s:\n",
           nsignals, nsignals == 1 ? "" : "s", opt->repeat, opt->repeat == 1 ? "" : "s",
           opt->duration_s, opt->channel + 1, opt->manual_gain ? "manual gain" : "AGC");
    for (i = 0; i < nsignals; i++) {
        if (measure_signal(dev, opt, &signals[i], raw, x, count, true) != 0)
            goto out;
        if (signals[i].found)
            ppm[used++] = signals[i].ppm;
    }

    printf("\n%-18s %10s %10s %9s %s\n", "Signal", "Error", "Spread", "Power", "Used");
    for (i = 0; i < nsignals; i++) {
        if (signals[i].found)
            printf("%-18s %+9.3f ppm %6.3f ppm %6.1f dBFS %d/%d\n", signals[i].name, signals[i].ppm,
                   signals[i].spread_ppm, signals[i].power_db, signals[i].found, opt->repeat);
        else
            printf("%-18s %10s %10s %6.1f dBFS 0/%d\n", signals[i].name, "-", "-",
                   signals[i].power_db, opt->repeat);
    }
    if (!used) {
        printf("\nNo signal found: check the frequencies (centre of an LTE carrier, GSM carrier with a BCCH), "
               "the antenna and --max-ppm.\n");
        goto out;
    }
    spread = stddev_of(ppm, used);
    result = median_of(ppm, used);
    if (opt->trim_ppm != 0.0) {
        /* The errors are small: the one of the reference is the trim plus what remains. */
        printf("\nResidual error with a trim of %+.3f ppm: %+.3f ppm (%d signal%s, spread %.3f ppm).\n",
               opt->trim_ppm, result, used, used == 1 ? "" : "s", spread);
        result += opt->trim_ppm;
        printf("Reference clock error: %+.3f ppm. Positive: the clock runs fast.\n", result);
    } else {
        printf("\nReference clock error: %+.3f ppm (%d signal%s, spread %.3f ppm). Positive: the clock runs fast.\n",
               result, used, used == 1 ? "" : "s", spread);
    }
    if (used == 1)
        printf("One signal only: a second one, of another operator or band, confirms the result.\n");
    else if (spread > 0.5)
        printf("The signals disagree by more than 0.5 ppm: one of them may be a repeater or a wrong carrier.\n");

    if (opt->apply) {
        struct cal_signal check = signals[0];
        struct cal_options once = *opt;

        for (i = 0; i < nsignals && !signals[i].found; i++)
            ;
        check = signals[i < nsignals ? i : 0];
        rc = m2sdr_set_refclk_ppm(dev, result);
        if (rc != M2SDR_ERR_OK) {
            fprintf(stderr, "m2sdr_set_refclk_ppm(%.3f) failed: %s\n", result, m2sdr_strerror(rc));
            goto out;
        }
        usleep(200000);
        once.repeat = 1;
        if (measure_signal(dev, &once, &check, raw, x, count, false) != 0)
            goto out;
        if (check.found)
            printf("Trim of %+.3f ppm applied; residual error on %s: %+.3f ppm.\n", result, check.name, check.ppm);
        else
            printf("Trim of %+.3f ppm applied; %s not found again for the check.\n", result, check.name);
        printf("The trim lasts until the next RF initialisation: pass --refclk-ppm to m2sdr_rf, or 'stored' once it is stored.\n");
    }

    if (opt->store_file || opt->store_flash) {
        size_t len = 0;

        memset(&rec, 0, sizeof(rec));
        memset(&sensors, 0, sizeof(sensors));
        (void)m2sdr_get_fpga_sensors(dev, &sensors);
        (void)m2sdr_get_fpga_dna(dev, &rec.fpga_dna);
        rec.refclk_ppm    = result;
        rec.spread_ppm    = spread;
        rec.temperature_c = sensors.temperature_c;
        rec.unix_time     = (uint64_t)time(NULL);
        rec.signals       = (uint32_t)used;
        for (i = 0; i < nsignals; i++) {
            if (signals[i].found && len < sizeof(rec.note) - 1)
                len += (size_t)snprintf(rec.note + len, sizeof(rec.note) - len, "%s%s", len ? ", " : "", signals[i].name);
        }

        if (opt->store_file) {
            char path[512];
            if (opt->file)
                snprintf(path, sizeof(path), "%s", opt->file);
            else if (m2sdr_cal_default_path(rec.fpga_dna, path, sizeof(path)) != 0)
                path[0] = '\0';
            if (path[0] && m2sdr_cal_file_save(path, &rec) == 0)
                printf("Stored in %s.\n", path);
            else {
                fprintf(stderr, "Could not store the record in a file: %s\n", strerror(errno));
                goto out;
            }
        }
        if (opt->store_flash) {
            char question[128];
            snprintf(question, sizeof(question), "Erase and write 64 KiB of the SPI flash at 0x%08x?", opt->flash_offset);
            if (!confirm(question, opt->yes)) {
                printf("Flash not written.\n");
            } else if (m2sdr_cal_flash_save(dev, opt->flash_offset, &rec) == 0) {
                printf("Stored in the SPI flash at 0x%08x.\n", opt->flash_offset);
            } else {
                fprintf(stderr, "Could not store the record in the SPI flash.\n");
                goto out;
            }
        }
    }
    status = 0;

out:
    (void)m2sdr_stream_deactivate(dev, M2SDR_RX);
    free(raw);
    free(x);
    return status;
}

/* Scan */
/*------*/

#define SCAN_SAMPLE_RATE  (M2SDR_CAL_SAMPLE_RATE * M2SDR_CAL_SCAN_DECIM)   /* 7.68 MSPS. */
#define SCAN_BANDWIDTH_HZ 7000000
#define SCAN_STEP_HZ      5e6       /* Between two captures: the carriers 2.5 MHz around each. */
#define SCAN_LTE_S        0.10      /* Capture for LTE carriers. */
#define SCAN_GSM_S        0.30      /* Capture for GSM carriers: six FCCH bursts. */
#define SCAN_MAX_FOUND    1024
#define SCAN_LTE_MERGE_HZ 450e3     /* Detections closer than this are one carrier. */
#define SCAN_AGREE_PPM    0.75      /* Detections within this of each other agree. */

struct scan_range {
    enum m2sdr_cal_signal kind;
    double start_hz, stop_hz;
    char   name[24];
};

struct scan_found {
    enum m2sdr_cal_signal kind;
    double freq_hz;
    double ppm;
    double power_db;
    double quality;
    int    count;
    int    id;
    bool   confirmed;              /* Another carrier gives the same error. */
};

static const struct { const char *name; enum m2sdr_cal_signal kind; double start_mhz, stop_mhz; } scan_bands[] = {
    { "lte28",   M2SDR_CAL_SIGNAL_LTE,  758.0,  803.0 },
    { "lte20",   M2SDR_CAL_SIGNAL_LTE,  791.0,  821.0 },
    { "lte8",    M2SDR_CAL_SIGNAL_LTE,  925.0,  960.0 },
    { "lte3",    M2SDR_CAL_SIGNAL_LTE, 1805.0, 1880.0 },
    { "lte1",    M2SDR_CAL_SIGNAL_LTE, 2110.0, 2170.0 },
    { "lte7",    M2SDR_CAL_SIGNAL_LTE, 2620.0, 2690.0 },
    { "lte5",    M2SDR_CAL_SIGNAL_LTE,  869.0,  894.0 },
    { "lte2",    M2SDR_CAL_SIGNAL_LTE, 1930.0, 1990.0 },
    { "lte4",    M2SDR_CAL_SIGNAL_LTE, 2110.0, 2155.0 },
    { "lte12",   M2SDR_CAL_SIGNAL_LTE,  729.0,  746.0 },
    { "lte13",   M2SDR_CAL_SIGNAL_LTE,  746.0,  756.0 },
    { "lte38",   M2SDR_CAL_SIGNAL_LTE, 2570.0, 2620.0 },
    { "lte40",   M2SDR_CAL_SIGNAL_LTE, 2300.0, 2400.0 },
    { "gsm900",  M2SDR_CAL_SIGNAL_GSM,  925.0,  960.0 },
    { "gsm1800", M2SDR_CAL_SIGNAL_GSM, 1805.0, 1880.0 },
};
#define SCAN_BANDS (sizeof(scan_bands) / sizeof(scan_bands[0]))

/* Bands scanned without an argument: the ones in use in Europe. */
static const char *const scan_default[] = { "lte28", "lte20", "lte8", "gsm900", "lte3", "gsm1800", "lte1", "lte7" };

/* A band name, or lte:START-STOP / gsm:START-STOP in Hz. */
static int parse_scan_range(const char *text, struct scan_range *range)
{
    const char *colon = strchr(text, ':');
    size_t i;

    memset(range, 0, sizeof(*range));
    for (i = 0; i < SCAN_BANDS; i++) {
        if (strcmp(text, scan_bands[i].name) == 0) {
            range->kind     = scan_bands[i].kind;
            range->start_hz = scan_bands[i].start_mhz * 1e6;
            range->stop_hz  = scan_bands[i].stop_mhz * 1e6;
            snprintf(range->name, sizeof(range->name), "%s", text);
            return 0;
        }
    }
    if (colon && (strncmp(text, "lte:", 4) == 0 || strncmp(text, "gsm:", 4) == 0)) {
        char buf[64];
        char *dash;

        snprintf(buf, sizeof(buf), "%s", colon + 1);
        dash = strchr(buf + 1, '-');
        if (!dash)
            return -1;
        *dash = '\0';
        if (m2sdr_cli_parse_double(buf, &range->start_hz) != 0 ||
            m2sdr_cli_parse_double(dash + 1, &range->stop_hz) != 0)
            return -1;
        if (range->start_hz < 70e6 || range->stop_hz > 6e9 || range->stop_hz - range->start_hz < 1e6)
            return -1;
        range->kind = text[0] == 'l' ? M2SDR_CAL_SIGNAL_LTE : M2SDR_CAL_SIGNAL_GSM;
        snprintf(range->name, sizeof(range->name), "%s %.0f-%.0f MHz", text[0] == 'l' ? "LTE" : "GSM",
                 range->start_hz / 1e6, range->stop_hz / 1e6);
        return 0;
    }
    return -1;
}

static int cmp_found(const void *a, const void *b)
{
    const struct scan_found *x = a, *y = b;
    if (x->kind != y->kind)
        return (int)x->kind - (int)y->kind;
    return (x->freq_hz > y->freq_hz) - (x->freq_hz < y->freq_hz);
}

/* Points of one capture, shared by the threads that test them. */
#define SCAN_MAX_POINTS  64
#define SCAN_MAX_THREADS 32

struct scan_job {
    const float complex *x;
    size_t count;
    bool   lte;
    double max_ppm;
    struct {
        int    index;
        double freq_hz, offset_hz;
        struct m2sdr_cal_estimate est;
    } point[SCAN_MAX_POINTS];
    int npoints;
    int next;
    bool failed;
    pthread_mutex_t lock;
};

static void *scan_worker(void *arg)
{
    struct scan_job *job = arg;
    float complex *y = malloc((job->count / M2SDR_CAL_SCAN_DECIM + 1) * sizeof(*y));

    for (;;) {
        size_t ny = 0;
        int k, rc;

        pthread_mutex_lock(&job->lock);
        k = job->next < job->npoints ? job->next++ : -1;
        if (!y)
            job->failed = true;
        pthread_mutex_unlock(&job->lock);
        if (k < 0 || !y)
            break;
        rc = m2sdr_cal_channelize(job->x, job->count, job->point[k].offset_hz, y, &ny);
        if (rc == 0) {
            const double max_cfo_hz = job->max_ppm * 1e-6 * job->point[k].freq_hz;
            rc = job->lte ? m2sdr_cal_lte_estimate(y, ny, max_cfo_hz, &job->point[k].est)
                          : m2sdr_cal_gsm_estimate(y, ny, max_cfo_hz, &job->point[k].est);
        }
        if (rc != 0)
            job->point[k].est.found = false;
    }
    free(y);
    return NULL;
}

/* The PSS of an LTE carrier is also seen, weaker, on the raster points next to its centre: of
 * the detections close to each other, the strongest PSS is the carrier. Detections of the same
 * frequency are one. Returns the number kept, sorted, at the start of the table. */
static int scan_merge(struct scan_found *found, int nfound)
{
    int kept = 0, i, j;

    qsort(found, (size_t)nfound, sizeof(*found), cmp_found);
    for (i = 0; i < nfound; i++)
        found[i].confirmed = true;                     /* Used as "kept" in this pass. */
    for (i = 0; i < nfound; i++) {
        for (j = 0; j < nfound; j++) {
            if (j == i || found[j].kind != found[i].kind)
                continue;
            if (found[i].kind == M2SDR_CAL_SIGNAL_LTE && fabs(found[j].freq_hz - found[i].freq_hz) < SCAN_LTE_MERGE_HZ &&
                (found[j].quality > found[i].quality || (found[j].quality == found[i].quality && j < i)))
                found[i].confirmed = false;
            if (fabs(found[j].freq_hz - found[i].freq_hz) < 1.0 && j < i)
                found[i].confirmed = false;
        }
    }
    for (i = 0; i < nfound; i++) {
        if (found[i].confirmed) {
            found[kept] = found[i];
            found[kept++].confirmed = false;
        }
    }
    return kept;
}

static int cmd_scan(struct m2sdr_dev *dev, const struct cal_options *opt,
                    const struct scan_range *ranges, int nranges)
{
    const unsigned per_buf = m2sdr_bytes_to_samples(M2SDR_FORMAT_SC16_Q11, M2SDR_BUFFER_BYTES);
    const size_t max_count = (size_t)(SCAN_GSM_S * SCAN_SAMPLE_RATE);
    struct scan_found *found;
    int16_t *raw;
    float complex *x;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    const int nthreads = cores < 1 ? 1 : cores > SCAN_MAX_THREADS ? SCAN_MAX_THREADS : (int)cores;
    int nfound = 0, kept = 0, status = 1;
    int r, i, j;

    raw   = malloc(((max_count * 2 + per_buf - 1) / per_buf + 1) * per_buf * 2 * sizeof(*raw));
    x     = malloc(max_count * sizeof(*x));
    found = calloc(SCAN_MAX_FOUND, sizeof(*found));
    if (!raw || !x || !found) {
        fprintf(stderr, "Out of memory\n");
        goto out;
    }
    if (rf_setup(dev, opt, SCAN_SAMPLE_RATE, SCAN_BANDWIDTH_HZ, ranges[0].start_hz) != 0)
        goto out;

    for (r = 0; r < nranges; r++) {
        const struct scan_range *range = &ranges[r];
        const bool   lte    = range->kind == M2SDR_CAL_SIGNAL_LTE;
        const double raster = lte ? 100e3 : 200e3;
        /* A carrier has to lie in the band: half of the narrowest LTE carrier, one GSM channel. */
        const double first  = ceil((range->start_hz + (lte ? 0.7e6 : 0.2e6)) / raster - 1e-6) * raster;
        const double last   = floor((range->stop_hz - (lte ? 0.7e6 : 0.2e6)) / raster + 1e-6) * raster;
        const size_t count  = (size_t)((lte ? SCAN_LTE_S : SCAN_GSM_S) * SCAN_SAMPLE_RATE);
        const int    before = nfound;
        double centre;

        printf("Scanning %s, %.1f to %.1f MHz ", range->name, first / 1e6, last / 1e6);
        fflush(stdout);
        for (centre = first + SCAN_STEP_HZ / 2; centre - SCAN_STEP_HZ / 2 <= last; centre += SCAN_STEP_HZ) {
            double f;

            if (capture(dev, opt, centre, raw, x, count) != 0)
                goto out;
            /* LTE: every third raster point first. A carrier is also seen, weaker, from the
             * points next to its centre, so one of them finds it; the points around a detection
             * are then tested, and the merge keeps the strongest. GSM: every channel. */
            {
                const int npoints = (int)llround(SCAN_STEP_HZ / raster);
                const int margin  = lte ? 2 : 0;
                bool tested[SCAN_MAX_POINTS] = { false };
                bool hit[SCAN_MAX_POINTS]    = { false };
                struct scan_job job;
                int pass, k, t;

                for (pass = 0; pass < (lte ? 2 : 1); pass++) {
                    pthread_t threads[SCAN_MAX_THREADS];
                    int started = 0;

                    memset(&job, 0, sizeof(job));
                    job.x = x;
                    job.count = count;
                    job.lte = lte;
                    job.max_ppm = opt->max_ppm;
                    pthread_mutex_init(&job.lock, NULL);
                    for (k = -margin; k < npoints + margin; k++) {
                        bool want;

                        f = centre - SCAN_STEP_HZ / 2 + k * raster;
                        if (f < first - 1.0 || f > last + 1.0 || tested[k + margin])
                            continue;
                        if (!lte) {
                            want = true;
                        } else if (pass == 0) {
                            want = k >= 0 && k < npoints && (llround(f / raster) % 3) == 0;
                        } else {
                            int d;
                            want = false;
                            for (d = -2; d <= 2; d++) {
                                if (k + d >= -margin && k + d < npoints + margin && hit[k + d + margin])
                                    want = true;
                            }
                        }
                        if (!want)
                            continue;
                        tested[k + margin] = true;
                        job.point[job.npoints].index     = k + margin;
                        job.point[job.npoints].freq_hz   = f;
                        job.point[job.npoints].offset_hz = f - centre;
                        job.npoints++;
                    }
                    /* The points of a capture are independent: one thread each, up to the cores. */
                    for (t = 0; t < nthreads && t < job.npoints; t++) {
                        if (pthread_create(&threads[started], NULL, scan_worker, &job) == 0)
                            started++;
                    }
                    if (!started)
                        (void)scan_worker(&job);
                    for (t = 0; t < started; t++)
                        pthread_join(threads[t], NULL);
                    pthread_mutex_destroy(&job.lock);
                    if (job.failed)
                        goto out;
                    for (k = 0; k < job.npoints; k++) {
                        const struct m2sdr_cal_estimate *est = &job.point[k].est;

                        if (!est->found || nfound >= SCAN_MAX_FOUND)
                            continue;
                        if (pass == 0)
                            hit[job.point[k].index] = true;
                        found[nfound].kind     = range->kind;
                        found[nfound].freq_hz  = job.point[k].freq_hz;
                        found[nfound].ppm      = m2sdr_cal_refclk_ppm(est->cfo_hz, job.point[k].freq_hz);
                        found[nfound].power_db = est->power_db;
                        found[nfound].quality  = est->quality;
                        found[nfound].count    = est->count;
                        found[nfound].id       = est->id;
                        nfound++;
                    }
                }
            }
            printf(".");
            fflush(stdout);
        }
        /* Keep the carriers of the band only: the table stays small over a long scan. */
        nfound = before + scan_merge(found + before, nfound - before);
        printf(" %d\n", nfound - before);
    }

    /* Bands that overlap give the same carrier twice. */
    kept = scan_merge(found, nfound);

    /* One clock, one error. An LTE carrier whose centre was not identified leaves a detection
     * next to it that reads wrong by 5 or 10 kHz: the carriers that agree with another one are
     * confirmed, the ones far from them are left out. GSM carriers have no such ambiguity. */
    {
        int best = -1, best_votes = 0, dropped = 0;

        for (i = 0; i < kept; i++) {
            int votes = 0;
            for (j = 0; j < kept; j++)
                votes += j != i && fabs(found[j].ppm - found[i].ppm) < SCAN_AGREE_PPM;
            if (votes > best_votes || (votes == best_votes && votes > 0 && best >= 0 &&
                found[i].kind == M2SDR_CAL_SIGNAL_GSM && found[best].kind != M2SDR_CAL_SIGNAL_GSM)) {
                best_votes = votes;
                best = i;
            }
        }
        if (best >= 0) {
            const double consensus = found[best].ppm;
            for (i = 0, j = 0; i < kept; i++) {
                if (fabs(found[i].ppm - consensus) < 2 * SCAN_AGREE_PPM) {
                    found[j] = found[i];
                    found[j++].confirmed = true;
                } else {
                    dropped++;
                }
            }
            kept = j;
            if (dropped)
                printf("%d detection%s left out: more than %.1f ppm from the carriers that agree.\n", dropped,
                       dropped == 1 ? "" : "s", 2 * SCAN_AGREE_PPM);
        }
    }

    if (!kept) {
        printf("\nNo LTE or GSM carrier found: check the antenna, the bands of the region and --max-ppm.\n");
        goto out;
    }
    printf("\n%d carrier%s usable for the calibration:\n", kept, kept == 1 ? "" : "s");
    printf("%-4s %12s %-16s %10s %11s  %s\n", "Type", "Frequency", "Channel", "Error", "Power", "Detection");
    for (i = 0; i < kept; i++) {
        char channel[32];
        if (found[i].kind == M2SDR_CAL_SIGNAL_LTE) {
            unsigned band = 0;
            const int earfcn = m2sdr_cal_hz_to_earfcn(found[i].freq_hz, &band);
            if (earfcn >= 0)
                snprintf(channel, sizeof(channel), "B%u EARFCN %d", band, earfcn);
            else
                snprintf(channel, sizeof(channel), "-");
            printf("%-4s %8.1f MHz %-16s %+6.3f ppm %6.1f dBFS  PSS root %d, metric %.0f\n", "LTE",
                   found[i].freq_hz / 1e6, channel, found[i].ppm, found[i].power_db, found[i].id, found[i].quality);
        } else {
            const int arfcn = m2sdr_cal_hz_to_arfcn(found[i].freq_hz);
            if (arfcn >= 0)
                snprintf(channel, sizeof(channel), "ARFCN %d", arfcn);
            else
                snprintf(channel, sizeof(channel), "-");
            printf("%-4s %8.1f MHz %-16s %+6.3f ppm %6.1f dBFS  %d FCCH bursts\n", "GSM",
                   found[i].freq_hz / 1e6, channel, found[i].ppm, found[i].power_db, found[i].count);
        }
    }
    if (!found[0].confirmed)
        printf("\nNo two carriers agree: an LTE entry may be next to the centre of its carrier and read wrong\n"
               "by several ppm. Scan more bands, or check the carriers with measure.\n");
    printf("\nThe error of a scan is a first value, from a short capture. To measure on these carriers:\n  m2sdr_cal measure");
    for (i = 0; i < kept && i < CAL_MAX_SIGNALS; i++)
        printf(" %s:%.1fe6", found[i].kind == M2SDR_CAL_SIGNAL_LTE ? "lte" : "gsm", found[i].freq_hz / 1e6);
    printf("\n");
    status = 0;

out:
    (void)m2sdr_stream_deactivate(dev, M2SDR_RX);
    free(raw);
    free(x);
    free(found);
    return status;
}

static int cmd_show(struct m2sdr_dev *dev, const struct cal_options *opt)
{
    struct m2sdr_cal_record rec;
    uint64_t dna = 0;
    char path[512];
    bool any = false;

    (void)m2sdr_get_fpga_dna(dev, &dna);
    if (opt->ppm_only) {
        /* The value that the other tools use: the flash first, then the file. */
        if (opt->file ? m2sdr_cal_file_load(opt->file, &rec) != 0
                      : m2sdr_cal_load(dev, opt->flash_offset, &rec) == M2SDR_CAL_SOURCE_NONE)
            return 1;
        printf("%.3f\n", rec.refclk_ppm);
        return 0;
    }
    if (m2sdr_cal_flash_load(dev, opt->flash_offset, &rec) == 0) {
        print_record("SPI flash", &rec);
        if (rec.fpga_dna && rec.fpga_dna != dna)
            printf("  This record is of another board (%014llx here): it is not used.\n", (unsigned long long)dna);
        any = true;
    } else {
        printf("SPI flash: no record at 0x%08x.\n", opt->flash_offset);
    }
    if (opt->file)
        snprintf(path, sizeof(path), "%s", opt->file);
    else if (m2sdr_cal_default_path(dna, path, sizeof(path)) != 0)
        path[0] = '\0';
    if (path[0] && m2sdr_cal_file_load(path, &rec) == 0) {
        print_record(path, &rec);
        any = true;
    } else if (path[0]) {
        printf("%s: no record.\n", path);
    }
    return any ? 0 : 1;
}

static int cmd_clear(struct m2sdr_dev *dev, const struct cal_options *opt)
{
    struct m2sdr_cal_record rec;
    uint64_t dna = 0;
    char path[512];
    char question[128];

    (void)m2sdr_get_fpga_dna(dev, &dna);
    if (opt->file)
        snprintf(path, sizeof(path), "%s", opt->file);
    else if (m2sdr_cal_default_path(dna, path, sizeof(path)) != 0)
        path[0] = '\0';
    if (path[0] && unlink(path) == 0)
        printf("Removed %s.\n", path);

    /* Only a block that holds a record is erased. */
    if (m2sdr_cal_flash_load(dev, opt->flash_offset, &rec) != 0) {
        printf("SPI flash: no record at 0x%08x, nothing erased.\n", opt->flash_offset);
        return 0;
    }
    snprintf(question, sizeof(question), "Erase 64 KiB of the SPI flash at 0x%08x?", opt->flash_offset);
    if (!confirm(question, opt->yes)) {
        printf("Flash not erased.\n");
        return 0;
    }
    if (m2sdr_cal_flash_erase(dev, opt->flash_offset) != 0) {
        fprintf(stderr, "Could not erase the record in the SPI flash.\n");
        return 1;
    }
    printf("Record erased from the SPI flash.\n");
    return 0;
}

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR Reference Clock Calibration Utility.\n"
           "usage: m2sdr_cal [options] scan [BAND...]\n"
           "       m2sdr_cal [options] measure SIGNAL...\n"
           "       m2sdr_cal [options] show\n"
           "       m2sdr_cal [options] clear\n"
           "\n"
           "Measures the error of the reference clock against cellular downlink carriers.\n"
           "\n"
           "Bands (scan): lte1 lte2 lte3 lte4 lte5 lte7 lte8 lte12 lte13 lte20 lte28 lte38 lte40,\n"
           "  gsm900 gsm1800, or a range: lte:START-STOP, gsm:START-STOP in Hz (lte:791e6-821e6).\n"
           "  Without a band: lte28 lte20 lte8 gsm900 lte3 gsm1800 lte1 lte7.\n"
           "\n"
           "Signals (measure):\n"
           "  lte:FREQ       LTE carrier, FREQ its centre in Hz (816e6).\n"
           "  earfcn:N       LTE carrier, by its downlink EARFCN.\n"
           "  gsm:FREQ       GSM carrier with a BCCH, FREQ in Hz (952e6).\n"
           "  arfcn:N        GSM carrier, by its ARFCN (GSM 900 and DCS 1800).\n"
           "\n"
           "Device options:\n");
    m2sdr_cli_print_device_help();
    printf("\n"
           "Measure options:\n"
           "      --duration S       Length of a capture in seconds (default: 0.5).\n"
           "      --repeat N         Captures per signal (default: 3).\n"
           "      --max-ppm PPM      Largest clock error searched (default: 15).\n"
           "      --rx-gain DB       Manual RX gain (default: slow AGC).\n"
           "      --channel N        RX channel, 1 or 2 (default: 1).\n"
           "      --sync MODE        Clock source: internal, external or fpga (default: internal).\n"
           "      --refclk-freq HZ   Reference frequency in Hz (default: 38400000).\n"
           "      --refclk-ppm PPM   measure: trim the reference by PPM first, to check a value;\n"
           "                         'stored': the value of the calibration record of the board.\n"
           "      --apply            Trim the reference by the result and measure again.\n"
           "\n"
           "Storage options:\n"
           "      --store            Store the result in the file of the board.\n"
           "      --store-flash      Store the result in the SPI flash of the board.\n"
           "      --file PATH        File of the record (default: ~/.config/litex_m2sdr/cal_<dna>.bin).\n"
           "      --flash-offset N   Flash offset of the record (default: 0x%08x).\n"
           "  -y, --yes              Do not ask before a flash erase.\n"
           "      --ppm              show: print the stored value only, for scripts.\n"
           "  -h, --help             Show this help message.\n",
           M2SDR_CAL_FLASH_OFFSET);
}

/* Main */
/*------*/

int main(int argc, char **argv)
{
    static struct option options[] = {
        { "help",         no_argument,       NULL, 'h' },
        { "device",       required_argument, NULL, 'd' },
        { "device-num",   required_argument, NULL, 'c' },
        { "ip",           required_argument, NULL, 'i' },
        { "port",         required_argument, NULL, 'p' },
        { "yes",          no_argument,       NULL, 'y' },
        { "duration",     required_argument, NULL, 1 },
        { "repeat",       required_argument, NULL, 2 },
        { "max-ppm",      required_argument, NULL, 3 },
        { "rx-gain",      required_argument, NULL, 4 },
        { "channel",      required_argument, NULL, 5 },
        { "apply",        no_argument,       NULL, 6 },
        { "store",        no_argument,       NULL, 7 },
        { "store-flash",  no_argument,       NULL, 8 },
        { "file",         required_argument, NULL, 9 },
        { "flash-offset", required_argument, NULL, 10 },
        { "ppm",          no_argument,       NULL, 11 },
        { "sync",         required_argument, NULL, 12 },
        { "refclk-freq",  required_argument, NULL, 13 },
        { "refclk-ppm",   required_argument, NULL, 14 },
        { NULL, 0, NULL, 0 }
    };
    struct m2sdr_cli_device cli_dev;
    struct cal_options opt;
    struct cal_signal signals[CAL_MAX_SIGNALS];
    struct scan_range ranges[CAL_MAX_SIGNALS];
    int nranges = 0;
    struct m2sdr_dev *dev = NULL;
    const char *cmd;
    int nsignals = 0;
    int c, status;

    m2sdr_cli_handle_version(argc, argv);
    m2sdr_cli_device_init(&cli_dev);
    memset(&opt, 0, sizeof(opt));
    opt.duration_s   = 0.5;
    opt.repeat       = 3;
    opt.max_ppm      = 15.0;
    opt.flash_offset = M2SDR_CAL_FLASH_OFFSET;

    while ((c = getopt_long(argc, argv, "hd:c:i:p:y", options, NULL)) != -1) {
        int v;
        int64_t v64;

        switch (c) {
        case 'h':
            help();
            return 0;
        case 'd': case 'c': case 'i': case 'p':
            if (m2sdr_cli_handle_device_option(&cli_dev, c, optarg) != 0)
                return 1;
            break;
        case 'y':
            opt.yes = true;
            break;
        case 1:
            if (m2sdr_cli_parse_double_range(optarg, 0.05, 10.0, &opt.duration_s) != 0) {
                m2sdr_cli_error("invalid duration '%s' (expected 0.05 to 10)", optarg);
                return 1;
            }
            break;
        case 2:
            if (m2sdr_cli_parse_int_range(optarg, 1, CAL_MAX_REPEAT, &opt.repeat) != 0) {
                m2sdr_cli_error("invalid repeat '%s' (expected 1 to %d)", optarg, CAL_MAX_REPEAT);
                return 1;
            }
            break;
        case 3:
            if (m2sdr_cli_parse_double_range(optarg, 0.1, M2SDR_REFCLK_PPM_MAX, &opt.max_ppm) != 0) {
                m2sdr_cli_error("invalid max ppm '%s'", optarg);
                return 1;
            }
            break;
        case 4:
            if (m2sdr_cli_parse_int_range(optarg, -10, 77, &opt.rx_gain) != 0) {
                m2sdr_cli_error("invalid RX gain '%s'", optarg);
                return 1;
            }
            opt.manual_gain = true;
            break;
        case 5:
            if (m2sdr_cli_parse_int_range(optarg, 1, 2, &v) != 0) {
                m2sdr_cli_error("invalid channel '%s' (expected 1 or 2)", optarg);
                return 1;
            }
            opt.channel = (unsigned)(v - 1);
            break;
        case 6:  opt.apply = true; break;
        case 7:  opt.store_file = true; break;
        case 8:  opt.store_flash = true; break;
        case 9:  opt.file = optarg; break;
        case 10:
            if (m2sdr_cli_parse_u32(optarg, &opt.flash_offset) != 0 || (opt.flash_offset & 0xffffu)) {
                m2sdr_cli_error("invalid flash offset '%s' (expected a multiple of 64 KiB)", optarg);
                return 1;
            }
            break;
        case 11: opt.ppm_only = true; break;
        case 12: opt.sync = optarg; break;
        case 13:
            if (m2sdr_cli_parse_int64(optarg, &v64) != 0 || v64 <= 0) {
                m2sdr_cli_error("invalid refclk frequency '%s'", optarg);
                return 1;
            }
            opt.refclk_freq = v64;
            break;
        case 14:
            if (strcmp(optarg, "stored") == 0) {
                opt.trim_stored = true;
            } else if (m2sdr_cli_parse_double_range(optarg, -M2SDR_REFCLK_PPM_MAX, M2SDR_REFCLK_PPM_MAX, &opt.trim_ppm) != 0) {
                m2sdr_cli_error("invalid refclk ppm '%s' (expected 'stored' or -%g to %g)", optarg,
                                (double)M2SDR_REFCLK_PPM_MAX, (double)M2SDR_REFCLK_PPM_MAX);
                return 1;
            }
            break;
        default:
            return 1;
        }
    }

    if (optind >= argc) {
        help();
        return 1;
    }
    cmd = argv[optind++];
    if (strcmp(cmd, "measure") == 0) {
        for (; optind < argc; optind++) {
            if (nsignals >= CAL_MAX_SIGNALS) {
                m2sdr_cli_error("too many signals (%d at most)", CAL_MAX_SIGNALS);
                return 1;
            }
            if (parse_signal(argv[optind], &signals[nsignals]) != 0) {
                m2sdr_cli_error("invalid signal '%s' (expected lte:FREQ, earfcn:N, gsm:FREQ or arfcn:N)", argv[optind]);
                return 1;
            }
            nsignals++;
        }
        if (!nsignals) {
            m2sdr_cli_error("measure needs at least one signal");
            return 1;
        }
    } else if (strcmp(cmd, "scan") == 0) {
        for (; optind < argc; optind++) {
            if (nranges >= CAL_MAX_SIGNALS) {
                m2sdr_cli_error("too many bands (%d at most)", CAL_MAX_SIGNALS);
                return 1;
            }
            if (parse_scan_range(argv[optind], &ranges[nranges]) != 0) {
                m2sdr_cli_error("invalid band '%s' (expected a band as lte20 or gsm900, or lte:START-STOP, gsm:START-STOP)", argv[optind]);
                return 1;
            }
            nranges++;
        }
        if (!nranges) {
            for (; nranges < (int)(sizeof(scan_default) / sizeof(scan_default[0])); nranges++)
                (void)parse_scan_range(scan_default[nranges], &ranges[nranges]);
        }
    } else if (strcmp(cmd, "show") != 0 && strcmp(cmd, "clear") != 0) {
        m2sdr_cli_error("unknown command '%s' (expected scan, measure, show or clear)", cmd);
        return 1;
    }

    if (!m2sdr_cli_finalize_device(&cli_dev))
        return 1;
    if ((opt.trim_stored || opt.trim_ppm != 0.0) && strcmp(cmd, "measure") != 0) {
        m2sdr_cli_error("--refclk-ppm is for measure");
        return 1;
    }

    if (m2sdr_open(&dev, m2sdr_cli_device_id(&cli_dev)) != 0) {
        fprintf(stderr, "Could not open device: %s\n", m2sdr_cli_device_id(&cli_dev));
        return 1;
    }

    if (opt.trim_stored) {
        struct m2sdr_cal_record rec;
        enum m2sdr_cal_source source = M2SDR_CAL_SOURCE_NONE;

        if (opt.file) {
            if (m2sdr_cal_file_load(opt.file, &rec) == 0)
                source = M2SDR_CAL_SOURCE_FILE;
        } else {
            source = m2sdr_cal_load(dev, opt.flash_offset, &rec);
        }
        if (source == M2SDR_CAL_SOURCE_NONE) {
            fprintf(stderr, "No calibration record for this board: run m2sdr_cal measure --store.\n");
            m2sdr_close(dev);
            return 1;
        }
        opt.trim_ppm = rec.refclk_ppm;
        printf("RefClk trim %+.3f ppm, from the calibration record in %s.\n", rec.refclk_ppm,
               source == M2SDR_CAL_SOURCE_FLASH ? "the SPI flash" : "its file");
    }

    if (strcmp(cmd, "scan") == 0)
        status = cmd_scan(dev, &opt, ranges, nranges);
    else if (strcmp(cmd, "measure") == 0)
        status = cmd_measure(dev, &opt, signals, nsignals);
    else if (strcmp(cmd, "show") == 0)
        status = cmd_show(dev, &opt);
    else
        status = cmd_clear(dev, &opt);

    m2sdr_close(dev);
    return status;
}
