/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Signal Analyzer Utility.
 *
 * Spectrum, markers, channel power and time-domain measurements on the RX of a board or on a
 * capture file. With SDL2 and cimgui it opens a Dear ImGui window; --report prints the
 * measurements of one capture without a display.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <getopt.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "m2sdr.h"
#include "m2sdr_analysis.h"
#include "m2sdr_cal.h"
#include "m2sdr_cli.h"

#include "analyzer/analyzer.h"

/* Modes */
/*-------*/

/* Views of the spectrum: measured with every capture. */
const struct an_mode an_mode_spectrum = {
    .key      = "spectrum",
    .title    = "Spectrum",
    .category = AN_CAT_SPECTRUM,
    .on       = AN_NONE,
    .valid    = AN_NONE,
#ifdef M2SDR_ANALYZER_GUI
    .draw     = draw_spectrum,
#endif
};

const struct an_mode an_mode_time = {
    .key      = "time",
    .title    = "Time",
    .category = AN_CAT_SPECTRUM,
    .on       = AN_NONE,
    .valid    = AN_NONE,
#ifdef M2SDR_ANALYZER_GUI
    .draw     = draw_time,
#endif
};

const struct an_mode an_mode_statistics = {
    .key      = "statistics",
    .title    = "Statistics",
    .category = AN_CAT_SPECTRUM,
    .on       = AN_NONE,
    .valid    = AN_NONE,
#ifdef M2SDR_ANALYZER_GUI
    .draw     = draw_statistics,
#endif
};

/* Registry: the views and modes, in the order of the tabs (a mode: one line here, see
 * analyzer/analyzer.h). */
const struct an_mode *const an_modes[] = {
    &an_mode_spectrum,
    &an_mode_time,
    &an_mode_statistics,
    &an_mode_pn,
    &an_mode_demod,
    &an_mode_lte,
    &an_mode_dvbt,
    &an_mode_dab,
    NULL
};

/* Help */
/*------*/

#define AN_HELP_COLUMNS 94         /* The longest line of the help. */

/* The names of the views, as "a, b or c". */
static char *view_names(char *text, size_t len)
{
    size_t n = 0;
    int m;

    text[0] = '\0';
    for (m = 0; an_modes[m] && n < len; m++)
        n += (size_t)snprintf(text + n, len - n, "%s%s", m == 0 ? "" : an_modes[m + 1] ? ", " : " or ", an_modes[m]->key);
    return text;
}

/* --view: the names of the views, on lines as long as the other ones of the help. */
static void help_views(void)
{
    char names[512], *word;
    int col;

    view_names(names, sizeof(names) - 1);
    strcat(names, ".");
    col = printf("      --view NAME        View shown first:");
    for (word = strtok(names, " "); word; word = strtok(NULL, " ")) {
        if (col + 1 + (int)strlen(word) > AN_HELP_COLUMNS)
            col = printf("\n%25s%s", "", word) - 1;
        else
            col += printf(" %s", word);
    }
    printf("\n");
}

static void help(void)
{
    printf("M2SDR Signal Analyzer Utility.\n"
           "usage: m2sdr_analyzer [options]                  Board RX\n"
           "       m2sdr_analyzer [options] --file PATH      SigMF or raw capture\n"
           "\n"
           "Spectrum with markers, channel power, occupied bandwidth and ACPR, the figures of a\n"
           "tone, time-domain statistics and phase noise. It only receives, unless --tx-tone is given.\n"
           "\n");
    m2sdr_cli_print_device_help();
    printf("\n"
           "Board options:\n"
           "      --freq HZ          Centre frequency (default: %.0f).\n"
           "      --sample-rate HZ   Sample rate (default: %.0f).\n"
           "      --bandwidth HZ     RF bandwidth (default: 0.9 x the sample rate).\n"
           "      --rx-gain DB       Manual RX gain (default: slow AGC).\n"
           "      --channel N        RX channel, 1 or 2 (default: 1).\n"
           "      --refclk-ppm V     Reference trim in ppm, 'stored' or 'none' (default: the\n"
           "                         calibration record of the board when it has one, see m2sdr_cal).\n"
           "      --sync MODE        Clock source: internal, external or fpga (default: internal).\n"
           "      --refclk-freq HZ   Reference frequency in Hz (default: 38400000).\n"
           "\n"
           "File options:\n"
           "      --file PATH        SigMF metadata or data file, or raw interleaved samples.\n"
           "      --format FMT       Raw file: sc16 or sc8 (default: sc16).\n"
           "      --nbits N          Raw file: significant bits of a sample (default: 12, 8 for sc8).\n"
           "      --nchannels N      Raw file: channels interleaved (default: 2).\n"
           "      --frame-header     Raw file: a 16-byte header starts every frame.\n"
           "      --frame-size N     Raw file: bytes of a frame with its header (default: %u).\n"
           "      --capture-index N  SigMF: capture to open (default: 0).\n"
           "      --offset S         Start, in seconds from the start of the file (default: 0).\n"
           "      --freq HZ, --sample-rate HZ   When the file does not give them.\n"
           "\n"
           "Measurement options:\n"
           "      --fft-len N        FFT length, a power of two (default: %d).\n"
           "      --window NAME      hann, flattop, blackman-harris or rect (default: hann).\n"
           "      --averages N       Segments averaged in a capture (default: %d).\n"
           "      --channel-bw HZ    Channel measured: power, ACPR, occupied bandwidth.\n"
           "      --channel-offset HZ   Its centre, from the centre of the capture (default: 0).\n"
           "      --channel-spacing HZ  Spacing of the adjacent channels (default: the bandwidth).\n"
           "      --ref-offset DB    Added to the levels, to read dBm at the connector: the board\n"
           "                         has no absolute level calibration.\n"
           "\n"
           "Modulation options:\n"
           "      --demod            With --report: demodulate the strongest signal, a single carrier\n"
           "                         with PSK or QAM: constellation, EVM, MER, I/Q errors.\n"
           "      --modulation NAME  auto, bpsk, qpsk, 8psk, 16qam, 32qam, 64qam, 128qam or 256qam\n"
           "                         (default: auto).\n"
           "      --symbol-rate HZ   Symbols per second (default: found from the signal).\n"
           "      --rolloff A        Roll-off of the pulse (default: found from the signal).\n"
           "      --pulse NAME       Transmit pulse: rrc, rc or rect (default: rrc).\n"
           "      --demod-samples N  Samples of its capture (default: %d).\n"
           "      --no-rf-init       Board: leave the RFIC as it is, set by m2sdr_rf, and only receive;\n"
           "                         --freq and --sample-rate then only label the results.\n"
           "\n"
           "LTE options:\n"
           "      --lte              With --report: analyse an LTE downlink carrier, FDD with a normal\n"
           "                         cyclic prefix, tuned on its centre: cell, MIB, EVM, data blocks.\n"
           "                         The sample rate is 1.92, 3.84, 7.68, 15.36 or 30.72 MSPS, for\n"
           "                         6, 15, 25, 50 or 100 resource blocks around the centre.\n"
           "\n"
           "DVB-T options:\n"
           "      --dvbt             With --report: reception of a DVB-T multiplex tuned near its centre:\n"
           "                         mode, TPS, MER, channel and echoes. The sample rate is the one of\n"
           "                         the channel (default with --dvbt: 9.142857 MSPS, 8 MHz).\n"
           "      --watch            With --report --dvbt or --dab, on a board: one line per capture until\n"
           "                         Ctrl-C (MER, margin, level, offset, echo); with --json, one JSON\n"
           "                         object per line.\n"
           "\n"
           "DAB options:\n"
           "      --dab              With --report: reception of a DAB/DAB+ ensemble (mode I) tuned near\n"
           "                         its centre: MER, SNR, carrier offset, channel and echoes. The sample\n"
           "                         rate is 2.048 MSPS (the default with --dab). --watch works too.\n"
           "\n"
           "Phase noise options:\n"
           "      --phase-noise      With --report: phase noise of the strongest carrier.\n"
           "      --pn-samples N     Samples of its capture (default: %d): more reach lower offsets.\n"
           "      --pn-from HZ, --pn-to HZ   Integration limits (default: the offsets measured).\n"
           "      --pn-floor PATH    File of the floor of the board (default: one per board,\n"
           "                         ~/.config/litex_m2sdr/pn_floor_<dna>.csv).\n"
           "      --pn-floor-save    With --report: store the phase noise read as this floor.\n"
           "      --tx-tone          Board: send the test tone of the RFIC on TX, at the centre plus a\n"
           "                         thirty-second of the sample rate. For a cable from TX to RX.\n"
           "      --tx-att DB        Attenuation of the test tone (default: %d).\n"
           "\n"
           "Output options:\n",
           AN_DEFAULT_FREQ_HZ, AN_DEFAULT_SAMPLE_RATE, M2SDR_BUFFER_BYTES, AN_DEFAULT_FFT_LEN, AN_DEFAULT_AVERAGES,
           1 << AN_DEMOD_DEFAULT_LOG2, 1 << AN_PN_DEFAULT_LOG2, AN_TX_TONE_ATT_DB);
    help_views();
    printf("      --report           Measure one capture and print the result, without a display.\n"
           "      --json             With --report: JSON.\n"
           "      --export-csv PATH  With --report: write the spectrum to a CSV file.\n"
           "  -h, --help             Show this help message.\n"
           "\n"
           "Window: left click places the selected marker, the wheel zooms, a right drag moves the\n"
           "span, a double click shows it all. Space: run or stop. P: marker to the peak. F: full\n"
           "span. R: restart the traces.\n");
}

/* Command line */
/*--------------*/

#define AN_OPTIONS_MAX 512

/* Options of the core; the ones of the modes are in their files. */
static const struct option core_options[] = {
    { "help",            no_argument,       NULL, 'h' },
    { "device",          required_argument, NULL, 'd' },
    { "freq",            required_argument, NULL, 1 },
    { "sample-rate",     required_argument, NULL, 2 },
    { "bandwidth",       required_argument, NULL, 3 },
    { "rx-gain",         required_argument, NULL, 4 },
    { "channel",         required_argument, NULL, 5 },
    { "refclk-ppm",      required_argument, NULL, 6 },
    { "sync",            required_argument, NULL, 7 },
    { "refclk-freq",     required_argument, NULL, 8 },
    { "file",            required_argument, NULL, 9 },
    { "format",          required_argument, NULL, 10 },
    { "nbits",           required_argument, NULL, 11 },
    { "nchannels",       required_argument, NULL, 12 },
    { "frame-header",    no_argument,       NULL, 13 },
    { "frame-size",      required_argument, NULL, 14 },
    { "capture-index",   required_argument, NULL, 15 },
    { "offset",          required_argument, NULL, 16 },
    { "fft-len",         required_argument, NULL, 17 },
    { "window",          required_argument, NULL, 18 },
    { "averages",        required_argument, NULL, 19 },
    { "channel-bw",      required_argument, NULL, 20 },
    { "channel-offset",  required_argument, NULL, 21 },
    { "channel-spacing", required_argument, NULL, 22 },
    { "ref-offset",      required_argument, NULL, 23 },
    { "report",          no_argument,       NULL, 24 },
    { "json",            no_argument,       NULL, 25 },
    { "export-csv",      required_argument, NULL, 26 },
    { "view",            required_argument, NULL, 27 },
    { "tx-tone",         no_argument,       NULL, 34 },
    { "tx-att",          required_argument, NULL, 35 },
    { "no-rf-init",      no_argument,       NULL, 42 },
    { "watch",           no_argument,       NULL, 45 },
    { NULL, 0, NULL, 0 }
};

/* The options of the core, then the ones of each mode, their ids moved to its range; NULL when a
 * mode has an option without a parser, of a name already taken or of an id out of its range. */
static const struct option *build_options(void)
{
    static struct option table[AN_OPTIONS_MAX];
    const struct option *o;
    int n = 0, m, k;

    for (o = core_options; o->name; o++)
        table[n++] = *o;
    for (m = 0; an_modes[m]; m++) {
        for (o = an_modes[m]->options; o && o->name; o++) {
            for (k = 0; k < n && strcmp(table[k].name, o->name) != 0; k++)
                ;
            if (!an_modes[m]->option || k < n || o->val < 0 || o->val >= AN_OPT_IDS || n + 1 >= AN_OPTIONS_MAX) {
                fprintf(stderr, "Option --%s of the mode %s: no parser, name taken, id out of 0 to %d, or too many options.\n",
                        o->name, an_modes[m]->key, AN_OPT_IDS - 1);
                return NULL;
            }
            table[n]     = *o;
            table[n].val = AN_OPT_BASE + AN_OPT_IDS * m + o->val;
            n++;
        }
    }
    memset(&table[n], 0, sizeof(table[n]));
    return table;
}

/* Main */
/*------*/

int main(int argc, char **argv)
{
    struct m2sdr_cli_device cli_dev;
    struct an_settings set;
    struct an_source src;
    const struct option *options = build_options();
    struct an_cli cli;
    const char *file = NULL, *csv_path = NULL, *trim = NULL, *floor_path = NULL;
    static struct an_floor fl;
    static char floor_default[1024];
    bool report = false, json = false, watch = false, freq_given = false, rate_given = false, nbits_given = false;
    double offset_s = 0.0;
    int capture_index = 0, view = 0;
    char err[256];
    int c, m, status;

    m2sdr_cli_handle_version(argc, argv);
    m2sdr_cli_device_init(&cli_dev);
    memset(&set, 0, sizeof(set));
    memset(&src, 0, sizeof(src));
    set.freq_hz      = AN_DEFAULT_FREQ_HZ;
    set.sample_rate  = AN_DEFAULT_SAMPLE_RATE;
    set.agc          = true;
    set.rx_gain      = 40;
    set.fft_len      = AN_DEFAULT_FFT_LEN;
    set.window       = M2SDR_WINDOW_HANN;
    set.averages     = AN_DEFAULT_AVERAGES;
    src.sample_bytes = 2;
    src.nbits        = 12;
    src.nchannels    = 2;
    src.frame_size   = M2SDR_BUFFER_BYTES;
    src.loop         = true;
    src.tx_att       = AN_TX_TONE_ATT_DB;
    memset(&cli, 0, sizeof(cli));
    cli.set = &set;
    cli.src = &src;
    for (m = 0; an_modes[m]; m++) {
        if (an_modes[m]->defaults)
            an_modes[m]->defaults(&set);
    }
    if (!options)
        return 1;

    while ((c = getopt_long(argc, argv, "hd:c:i:p:", options, NULL)) != -1) {
        double v;
        int n;

        switch (c) {
        case 'h':
            help();
            return 0;
        case 'd': case 'c': case 'i': case 'p':
            if (m2sdr_cli_handle_device_option(&cli_dev, c, optarg) != 0)
                return 1;
            break;
        case 1:
            if (m2sdr_cli_parse_double(optarg, &set.freq_hz) != 0 || set.freq_hz < 0) {
                m2sdr_cli_error("invalid frequency '%s'", optarg);
                return 1;
            }
            freq_given = true;
            break;
        case 2:
            if (m2sdr_cli_parse_double(optarg, &set.sample_rate) != 0 || !(set.sample_rate > 0)) {
                m2sdr_cli_error("invalid sample rate '%s'", optarg);
                return 1;
            }
            rate_given = true;
            break;
        case 3:
            if (m2sdr_cli_parse_double(optarg, &set.bandwidth) != 0 || set.bandwidth < 0) {
                m2sdr_cli_error("invalid bandwidth '%s'", optarg);
                return 1;
            }
            break;
        case 4:
            if (m2sdr_cli_parse_int_range(optarg, -10, 77, &set.rx_gain) != 0) {
                m2sdr_cli_error("invalid RX gain '%s'", optarg);
                return 1;
            }
            set.agc = false;
            break;
        case 5:
            if (m2sdr_cli_parse_int_range(optarg, 1, 2, &n) != 0) {
                m2sdr_cli_error("invalid channel '%s' (expected 1 or 2)", optarg);
                return 1;
            }
            set.channel = n - 1;
            break;
        case 6:  trim = optarg; break;
        case 7:  src.sync = optarg; break;
        case 8:
            if (m2sdr_cli_parse_double(optarg, &v) != 0 || !(v > 0)) {
                m2sdr_cli_error("invalid refclk frequency '%s'", optarg);
                return 1;
            }
            src.refclk_freq = (int64_t)v;
            break;
        case 9:  file = optarg; break;
        case 10:
            if (strcmp(optarg, "sc16") == 0) {
                src.sample_bytes = 2;
            } else if (strcmp(optarg, "sc8") == 0) {
                src.sample_bytes = 1;
                if (!nbits_given)
                    src.nbits = 8;
            } else {
                m2sdr_cli_invalid_choice("format", optarg, "sc16 or sc8");
                return 1;
            }
            break;
        case 11:
            if (m2sdr_cli_parse_int_range(optarg, 2, 16, &src.nbits) != 0) {
                m2sdr_cli_error("invalid nbits '%s' (expected 2 to 16)", optarg);
                return 1;
            }
            nbits_given = true;
            break;
        case 12:
            if (m2sdr_cli_parse_int_range(optarg, 1, 8, &src.nchannels) != 0) {
                m2sdr_cli_error("invalid nchannels '%s' (expected 1 to 8)", optarg);
                return 1;
            }
            break;
        case 13: src.frame_header = true; break;
        case 14:
            if (m2sdr_cli_parse_int_range(optarg, 32, 1 << 26, &n) != 0) {
                m2sdr_cli_error("invalid frame size '%s'", optarg);
                return 1;
            }
            src.frame_size = (size_t)n;
            break;
        case 15:
            if (m2sdr_cli_parse_int_range(optarg, 0, 1 << 20, &capture_index) != 0) {
                m2sdr_cli_error("invalid capture index '%s'", optarg);
                return 1;
            }
            break;
        case 16:
            if (m2sdr_cli_parse_double(optarg, &offset_s) != 0 || offset_s < 0) {
                m2sdr_cli_error("invalid offset '%s'", optarg);
                return 1;
            }
            break;
        case 17:
            if (m2sdr_cli_parse_int_range(optarg, AN_MIN_FFT_LEN, AN_MAX_FFT_LEN, &set.fft_len) != 0 ||
                (set.fft_len & (set.fft_len - 1))) {
                m2sdr_cli_error("invalid FFT length '%s' (expected a power of two from %d to %d)", optarg,
                                AN_MIN_FFT_LEN, AN_MAX_FFT_LEN);
                return 1;
            }
            break;
        case 18:
            if (strcmp(optarg, "hann") == 0) {
                set.window = M2SDR_WINDOW_HANN;
            } else if (strcmp(optarg, "flattop") == 0) {
                set.window = M2SDR_WINDOW_FLATTOP;
            } else if (strcmp(optarg, "blackman-harris") == 0) {
                set.window = M2SDR_WINDOW_BLACKMAN_HARRIS;
            } else if (strcmp(optarg, "rect") == 0) {
                set.window = M2SDR_WINDOW_RECTANGULAR;
            } else {
                m2sdr_cli_invalid_choice("window", optarg, "hann, flattop, blackman-harris or rect");
                return 1;
            }
            break;
        case 19:
            if (m2sdr_cli_parse_int_range(optarg, 1, AN_MAX_AVERAGES, &set.averages) != 0) {
                m2sdr_cli_error("invalid averages '%s' (expected 1 to %d)", optarg, AN_MAX_AVERAGES);
                return 1;
            }
            break;
        case 20:
            if (m2sdr_cli_parse_double(optarg, &set.chan_bw_hz) != 0 || set.chan_bw_hz < 0) {
                m2sdr_cli_error("invalid channel bandwidth '%s'", optarg);
                return 1;
            }
            break;
        case 21:
            if (m2sdr_cli_parse_double(optarg, &set.chan_offset_hz) != 0) {
                m2sdr_cli_error("invalid channel offset '%s'", optarg);
                return 1;
            }
            break;
        case 22:
            if (m2sdr_cli_parse_double(optarg, &set.chan_spacing_hz) != 0 || set.chan_spacing_hz < 0) {
                m2sdr_cli_error("invalid channel spacing '%s'", optarg);
                return 1;
            }
            break;
        case 23:
            if (m2sdr_cli_parse_double(optarg, &set.ref_offset_db) != 0) {
                m2sdr_cli_error("invalid reference offset '%s'", optarg);
                return 1;
            }
            break;
        case 24: report = true; break;
        case 25: json = true; break;
        case 26: csv_path = optarg; break;
        case 27:
            for (view = 0; an_modes[view] && strcmp(optarg, an_modes[view]->key) != 0; view++)
                ;
            if (!an_modes[view]) {
                char names[512];

                m2sdr_cli_invalid_choice("view", optarg, view_names(names, sizeof(names)));
                return 1;
            }
            break;
        case 42: src.no_rf_init = true; break;
        case 45: watch = true; break;
        case 34: src.tx_tone = true; break;
        case 35:
            if (m2sdr_cli_parse_int_range(optarg, 0, 89, &src.tx_att) != 0) {
                m2sdr_cli_error("invalid TX attenuation '%s' (expected 0 to 89)", optarg);
                return 1;
            }
            break;
        default:
            /* An option of a mode: its index in the registry and its own id. */
            if (c < AN_OPT_BASE ||
                an_modes[(c - AN_OPT_BASE) / AN_OPT_IDS]->option((c - AN_OPT_BASE) % AN_OPT_IDS, optarg, &cli) != 0)
                return 1;
            break;
        }
    }
    if (optind < argc) {
        m2sdr_cli_error("unexpected argument '%s'", argv[optind]);
        return 1;
    }
    if (watch && (!report || watch_modes(&set, NULL) != 1 || file || csv_path || cli.floor_store)) {
        m2sdr_cli_error("--watch goes with --report and one of --dvbt and --dab, on a board");
        return 1;
    }
    if ((json || csv_path || cli.floor_store) && !report) {
        m2sdr_cli_error("--json, --export-csv and --pn-floor-save go with --report");
        return 1;
    }
    /* The mode of the view shown first is measured from the first capture. */
    if (an_mode_flag(an_modes[view], &set))
        *an_mode_flag(an_modes[view], &set) = true;
    /* The sample rate of a mode, unless one is given; a file gives its own. */
    for (m = 0; an_modes[m]; m++) {
        if (an_modes[m]->default_rate > 0 && an_mode_on(an_modes[m], &set) && !rate_given)
            set.sample_rate = an_modes[m]->default_rate;
    }
    if (src.no_rf_init && (file || src.tx_tone)) {
        m2sdr_cli_error("--no-rf-init is for a board, without --tx-tone");
        return 1;
    }
    if (src.tx_tone && file) {
        m2sdr_cli_error("--tx-tone is for a board");
        return 1;
    }

    if (file) {
        if (src.frame_header && src.frame_size <= AN_FRAME_HEADER_BYTES) {
            m2sdr_cli_error("the frame size must be above %d bytes", AN_FRAME_HEADER_BYTES);
            return 1;
        }
        if (file_open(&src, file, capture_index, err, sizeof(err)) != 0) {
            fprintf(stderr, "Could not open the capture: %s\n", err);
            source_close(&src);
            return 1;
        }
        if (!rate_given && src.file_rate > 0)
            set.sample_rate = src.file_rate;
        if (!freq_given)
            set.freq_hz = src.file_freq;
        if (!rate_given && !(src.file_rate > 0))
            fprintf(stderr, "The file does not give its sample rate: %.0f assumed (--sample-rate).\n", set.sample_rate);
        if (set.channel >= src.nchannels) {
            fprintf(stderr, "The file has %d channel%s.\n", src.nchannels, src.nchannels == 1 ? "" : "s");
            source_close(&src);
            return 1;
        }
        src.position = (uint64_t)(offset_s * set.sample_rate);
        if (src.position >= src.total_sets)
            src.position = 0;
        if (report)
            src.loop = false;
    } else {
        struct m2sdr_cal_record rec;

        if (!m2sdr_cli_finalize_device(&cli_dev))
            return 1;
        if (m2sdr_open(&src.dev, m2sdr_cli_device_id(&cli_dev)) != 0) {
            fprintf(stderr, "Could not open device: %s\n", m2sdr_cli_device_id(&cli_dev));
            return 1;
        }
        src.live = true;
        if (json)
            (void)m2sdr_set_log_enabled(false);     /* The JSON is alone on the output. */
        snprintf(src.name, sizeof(src.name), "%s", m2sdr_cli_device_id(&cli_dev));

        /* Reference trim: the calibration record of the board unless told otherwise. */
        if (trim && strcmp(trim, "none") == 0) {
            src.trim_ppm = 0.0;
        } else if (trim && strcmp(trim, "stored") != 0) {
            if (m2sdr_cli_parse_double(trim, &src.trim_ppm) != 0 || fabs(src.trim_ppm) > M2SDR_REFCLK_PPM_MAX) {
                m2sdr_cli_error("invalid refclk ppm '%s' (expected 'stored', 'none' or -%g to %g)", trim,
                                (double)M2SDR_REFCLK_PPM_MAX, (double)M2SDR_REFCLK_PPM_MAX);
                source_close(&src);
                return 1;
            }
        } else if (m2sdr_cal_load(src.dev, M2SDR_CAL_FLASH_OFFSET, &rec) != M2SDR_CAL_SOURCE_NONE) {
            src.trim_ppm = rec.refclk_ppm;
            fprintf(stderr, "RefClk trim %+.3f ppm, from the calibration record of the board.\n", rec.refclk_ppm);
        } else if (trim) {
            fprintf(stderr, "No calibration record for this board: run m2sdr_cal measure --store.\n");
            source_close(&src);
            return 1;
        }
    }

    /* Floor of the phase noise: the file given, or the one of the board. */
    if (cli.floor_arg) {
        floor_path = cli.floor_arg;
    } else if (src.live && m2sdr_get_fpga_dna(src.dev, &src.dna) == M2SDR_ERR_OK &&
               floor_default_path(src.dna, floor_default, sizeof(floor_default)) == 0) {
        floor_path = floor_default;
    }
    if (floor_path && floor_load(&fl, floor_path) != 0 && cli.floor_arg && !cli.floor_store)
        fprintf(stderr, "No phase noise floor in %s.\n", floor_path);
    if (cli.floor_store && !floor_path) {
        m2sdr_cli_error("--pn-floor-save needs a board or --pn-floor PATH");
        source_close(&src);
        return 1;
    }

    (void)view;
    if (watch) {
        status = run_watch(&src, &set, json);
    } else if (report) {
        status = run_report(&src, &set, json, csv_path, &fl, cli.floor_store ? floor_path : NULL);
    } else {
#ifdef M2SDR_ANALYZER_GUI
        status = run_gui(&src, &set, view, &fl, floor_path);
#else
        fprintf(stderr, "This build has no window (SDL2 or cimgui was missing): use --report.\n");
        status = 1;
#endif
    }
    source_close(&src);
    return status;
}
