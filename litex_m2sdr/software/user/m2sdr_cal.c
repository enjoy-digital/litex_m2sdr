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
 * carriers (PSS and cyclic prefix) and of GSM carriers (FCCH bursts). The result can be applied
 * to the board (SI5351 trim) and stored, in a file and in the SPI flash of the board.
 *
 * Usage Example:
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "m2sdr_cal_dsp.h"
#include "m2sdr_cal_store.h"

/* Constants */
/*-----------*/

#define CAL_MAX_SIGNALS    32
#define CAL_MAX_REPEAT     64
#define CAL_SETTLE_US      50000   /* After a retune: PLL lock and AGC. */
#define CAL_BANDWIDTH_HZ   1400000

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
        sig->spread_ppm = stddev_of(ppm, sig->found);
        sig->ppm        = median_of(ppm, sig->found);
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
    struct m2sdr_config cfg;
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

    /* The reference is measured without a trim: the result is its error. */
    m2sdr_config_init(&cfg);
    cfg.sample_rate    = (int64_t)M2SDR_CAL_SAMPLE_RATE;
    cfg.bandwidth      = CAL_BANDWIDTH_HZ;
    cfg.rx_freq        = (int64_t)llround(signals[0].freq_hz);
    cfg.tx_att         = 89;
    cfg.refclk_ppm     = 0.0;
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
            goto out;
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
        goto out;
    }
    rc = m2sdr_sync_config(dev, M2SDR_RX, M2SDR_FORMAT_SC16_Q11, 0, per_buf, 0, 2000);
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_sync_config failed: %s\n", m2sdr_strerror(rc));
        goto out;
    }

    printf("Measuring %d signal%s, %d capture%s of %.2f s each, RX%u, %s:\n",
           nsignals, nsignals == 1 ? "" : "s", opt->repeat, opt->repeat == 1 ? "" : "s",
           opt->duration_s, opt->channel + 1, opt->manual_gain ? "manual gain" : "AGC");
    for (i = 0; i < nsignals; i++) {
        if (measure_signal(dev, opt, &signals[i], raw, x, count, true) != 0)
            goto out;
        if (signals[i].found)
            ppm[used++] = signals[i].ppm;
    }

    printf("\n%-18s %10s %10s %9s %s\n", "Signal", "Error", "Spread", "Power", "Found");
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
    printf("\nReference clock error: %+.3f ppm (%d signal%s, spread %.3f ppm). Positive: the clock runs fast.\n",
           result, used, used == 1 ? "" : "s", spread);
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
           "usage: m2sdr_cal [options] measure SIGNAL...\n"
           "       m2sdr_cal [options] show\n"
           "       m2sdr_cal [options] clear\n"
           "\n"
           "Measures the error of the reference clock against cellular downlink carriers.\n"
           "\n"
           "Signals:\n"
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
        { NULL, 0, NULL, 0 }
    };
    struct m2sdr_cli_device cli_dev;
    struct cal_options opt;
    struct cal_signal signals[CAL_MAX_SIGNALS];
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
    } else if (strcmp(cmd, "show") != 0 && strcmp(cmd, "clear") != 0) {
        m2sdr_cli_error("unknown command '%s' (expected measure, show or clear)", cmd);
        return 1;
    }

    if (!m2sdr_cli_finalize_device(&cli_dev))
        return 1;
    if (m2sdr_open(&dev, m2sdr_cli_device_id(&cli_dev)) != 0) {
        fprintf(stderr, "Could not open device: %s\n", m2sdr_cli_device_id(&cli_dev));
        return 1;
    }

    if (strcmp(cmd, "measure") == 0)
        status = cmd_measure(dev, &opt, signals, nsignals);
    else if (strcmp(cmd, "show") == 0)
        status = cmd_show(dev, &opt);
    else
        status = cmd_clear(dev, &opt);

    m2sdr_close(dev);
    return status;
}
