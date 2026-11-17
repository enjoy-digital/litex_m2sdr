/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR FM Transmitter Utility.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 * Description:
 * This utility FM modulates a WAV audio file, or raw PCM from stdin, into interleaved I/Q
 * samples for the M2SDR. The audio is resampled to the rate of the modulator of libm2sdr
 * (m2sdr_fm.h), which does the pre-emphasis, the stereo multiplex, the RDS subcarrier and the
 * modulation, and the I/Q samples are written to a file or to stdout.
 *
 * Note: Only the formats of libsndfile are read. Convert the others with ffmpeg:
 *     ffmpeg -i input.mp3 input.wav
 *
 * Usage Example:
 *     ./m2sdr_rf --sample-rate 1e6 --tx-freq 100e6 --tx-att 10 --chan 1t1r
 *     ./m2sdr_fm_tx --sample-rate 1000000 --bits 12 music.wav - | ./m2sdr_play -
 *     ffmpeg -i music.mp3 -f s16le -ac 2 -ar 44100 - | ./m2sdr_fm_tx --sample-rate 1000000 --mode stereo --input-channels 2 --input-sample-rate 44100 - - | ./m2sdr_play -
 *     ./m2sdr_fm_tx --mode stereo --rds-pi 0x1234 --rds-ps "M2SDR FM" --rds-rt "Hello from the LiteX-M2SDR" --rds-ct now music.wav - | ./m2sdr_play -
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sndfile.h>
#include <samplerate.h>
#include <stdint.h>
#include <inttypes.h>
#include <getopt.h>
#include <time.h>

#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "m2sdr_fm.h"

/* Constants */
/*-----------*/

#define IN_FRAMES   1024   /* Audio frames read from the source per pass. */
#define MPX_FRAMES  4096   /* Resampled audio frames given to the modulator per pass. */

#define DEFAULT_INPUT_RATE 44100.0  /* Raw PCM from stdin. */

#define DEFAULT_RDS_PI 0x1000       /* Not an allocated code: set the one of the programme. */
#define DEFAULT_RDS_PS "M2SDR"

/* Sample conversion */
/*-------------------*/

/* A sample of `bits` significant bits to SC8, rounded to nearest. */
static int8_t sc8_from_bits(int16_t sample, int bits) {
    int shift = bits - 8;
    int32_t v = sample;
    if (shift > 0) {
        int32_t half = 1 << (shift - 1);
        v = (v >= 0) ? ((v + half) >> shift) : -((-v + half) >> shift);
    } else if (shift < 0) {
        v *= 1 << -shift;
    }
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (int8_t)v;
}

/* FM Modulation */
/*---------------*/

struct fm_tx_source {
    SNDFILE *wav;      /* WAV file, or NULL for raw PCM from `raw`. */
    FILE    *raw;
    int      channels;
    double   rate;
};

/* Reads up to IN_FRAMES frames, converted to the channels of the modulator. */
static size_t read_audio(const struct fm_tx_source *src, float *in, float *audio, int audio_channels) {
    size_t frames, k;

    if (src->wav) {
        sf_count_t n = sf_readf_float(src->wav, in, IN_FRAMES);
        frames = (n > 0) ? (size_t)n : 0;
    } else {
        static int16_t raw[IN_FRAMES * 2];
        frames = fread(raw, sizeof(int16_t) * src->channels, IN_FRAMES, src->raw);
        for (k = 0; k < frames * src->channels; k++)
            in[k] = raw[k] / 32768.0f;
    }

    for (k = 0; k < frames; k++) {
        const float *x = &in[k * src->channels];
        if (audio_channels == 2) {
            audio[2 * k + 0] = x[0];
            audio[2 * k + 1] = (src->channels >= 2) ? x[1] : x[0];
        } else {
            audio[k] = (src->channels >= 2) ? 0.5f * (x[0] + x[1]) : x[0];
        }
    }
    return frames;
}

static int write_iq(FILE *out, const int16_t *iq, size_t count, int bits, enum m2sdr_format format) {
    if (format == M2SDR_FORMAT_SC8_Q7) {
        static int8_t iq8[2 * 4096];
        size_t done = 0;
        while (done < count) {
            size_t n = count - done, k;
            if (n > sizeof(iq8) / 2) n = sizeof(iq8) / 2;
            for (k = 0; k < 2 * n; k++)
                iq8[k] = sc8_from_bits(iq[2 * done + k], bits);
            if (fwrite(iq8, 2, n, out) != n)
                return -1;
            done += n;
        }
        return 0;
    }
    return (fwrite(iq, 2 * sizeof(int16_t), count, out) == count) ? 0 : -1;
}

static int modulate_audio(const struct fm_tx_source *src, FILE *out, const struct m2sdr_fm_config *cfg, enum m2sdr_format format) {
    struct m2sdr_fm_tx *tx = m2sdr_fm_tx_new(cfg);
    SRC_STATE *resampler = NULL;
    SRC_DATA data;
    float *in = NULL, *audio = NULL, *mpx = NULL;
    int16_t *iq = NULL;
    int audio_channels = cfg->stereo ? 2 : 1;
    int ret = -1;
    int eof = 0;

    if (!tx) {
        m2sdr_cli_error("unsupported FM settings (sample rate %.0f SPS: at least %.0f in %s, %.0f with RDS, deviation at most 0.4 x sample rate)",
            cfg->sample_rate, cfg->stereo ? M2SDR_FM_MIN_RATE_STEREO : M2SDR_FM_MIN_RATE_MONO,
            cfg->stereo ? "stereo" : "mono", M2SDR_FM_MIN_RATE_RDS);
        return -1;
    }

    in    = malloc(sizeof(float) * IN_FRAMES * src->channels);
    audio = malloc(sizeof(float) * IN_FRAMES * audio_channels);
    mpx   = malloc(sizeof(float) * MPX_FRAMES * audio_channels);
    iq    = malloc(sizeof(int16_t) * 2 * MPX_FRAMES * m2sdr_fm_tx_interpolation(tx));
    if (!in || !audio || !mpx || !iq) {
        m2sdr_cli_error("memory allocation failed");
        goto cleanup;
    }

    /* The modulator takes the audio at its own rate: resample unless the source is already
     * at this rate. */
    memset(&data, 0, sizeof(data));
    data.src_ratio = m2sdr_fm_tx_audio_rate(tx) / src->rate;
    if (fabs(data.src_ratio - 1.0) > 1e-9) {
        int error;
        if (!src_is_valid_ratio(data.src_ratio)) {
            m2sdr_cli_error("cannot resample %.0f Hz audio to %.0f Hz", src->rate, m2sdr_fm_tx_audio_rate(tx));
            goto cleanup;
        }
        resampler = src_new(SRC_SINC_MEDIUM_QUALITY, audio_channels, &error);
        if (!resampler) {
            m2sdr_cli_error("resampler: %s", src_strerror(error));
            goto cleanup;
        }
    }

    while (!eof) {
        size_t frames = read_audio(src, in, audio, audio_channels);
        eof = (frames < IN_FRAMES);

        if (!resampler) {
            size_t n = m2sdr_fm_tx_process(tx, audio, frames, iq);
            if (write_iq(out, iq, n, cfg->bits, format) != 0)
                goto write_error;
            continue;
        }

        /* The resampler takes the input in several passes when its output is full, and
         * gives its last samples once the end of the input is flagged. */
        data.data_in      = audio;
        data.input_frames = (long)frames;
        data.end_of_input = eof;
        for (;;) {
            size_t n;
            int err;

            data.data_out      = mpx;
            data.output_frames = MPX_FRAMES;
            err = src_process(resampler, &data);
            if (err) {
                m2sdr_cli_error("resampler: %s", src_strerror(err));
                goto cleanup;
            }
            data.data_in      += data.input_frames_used * audio_channels;
            data.input_frames -= data.input_frames_used;
            n = m2sdr_fm_tx_process(tx, mpx, (size_t)data.output_frames_gen, iq);
            if (write_iq(out, iq, n, cfg->bits, format) != 0)
                goto write_error;
            if (data.input_frames == 0 && (!eof || data.output_frames_gen == 0))
                break;
        }
    }

    if (fflush(out) != 0)
        goto write_error;
    if (m2sdr_fm_tx_clipped(tx))
        fprintf(stderr, "Warning: %" PRIu64 " audio samples limited to full scale after pre-emphasis; lower the audio level.\n",
            m2sdr_fm_tx_clipped(tx));
    ret = 0;
    goto cleanup;

write_error:
    m2sdr_cli_error("could not write the I/Q samples");

cleanup:
    free(in);
    free(audio);
    free(mpx);
    free(iq);
    if (resampler) src_delete(resampler);
    m2sdr_fm_tx_free(tx);
    return ret;
}

/* Help */
/*------*/

static void help(int status) {
    /* Display usage information */
    fprintf(status ? stderr : stdout,
           "M2SDR FM Transmitter Utility\n"
           "usage: m2sdr_fm_tx [options] input output\n"
           "\n"
           "Options:\n"
           "-h, --help            Display this help message.\n"
           "-s, --sample-rate sps  Set I/Q sample rate in SPS (default: 1000000).\n"
           "-d, --deviation dev   Set FM deviation of full-scale audio in Hz (default: 75000).\n"
           "-b, --bits bits       Set bits per I/Q sample (2 to 16, default: 12).\n"
           "    --format FMT      Sample format: sc16 or sc8 (default: sc16).\n"
           "-8, --sc8             Output 8-bit I/Q samples (SC8 Q7).\n"
           "-e, --emphasis type   Set pre-emphasis to us (75 us), eu (50 us) or none (default: eu).\n"
           "-m, --mode mode       Set mode to mono or stereo (default: mono).\n"
           "-i, --input-channels channels  Set input channels for stdin (1 or 2).\n"
           "-f, --input-sample-rate sps  Set input sample rate for stdin (default: 44100).\n"
           "\n"
           "RDS (57 kHz subcarrier; any of these options enables it):\n"
           "    --rds             Enable RDS with the default identification and name.\n"
           "    --rds-pi PI       Programme Identification, four hexadecimal digits (default: 1000).\n"
           "    --rds-ps NAME     Programme Service name, up to 8 characters (default: M2SDR).\n"
           "    --rds-rt TEXT     RadioText, up to 64 characters.\n"
           "    --rds-pty N       Programme Type, 0 to 31 (default: 0, none).\n"
           "    --rds-ct TIME     Send the clock time each minute: 'now' for the system clock at the\n"
           "                      first sample, or a UTC time in seconds since 1970.\n"
           "\n"
           "Arguments:\n"
           "input                 Input WAV file or '-' for raw 16-bit PCM from stdin (MP3 not supported; convert using: ffmpeg -i input.mp3 input.wav).\n"
           "output                Output file for I/Q samples ('-' for stdout).\n"
           "\n"
           "Example:\n"
           "m2sdr_fm_tx --sample-rate 1000000 --deviation 75000 --bits 12 input.wav output.bin\n"
           "ffmpeg -i input.mp3 -f s16le -ac 2 -ar 44100 - | m2sdr_fm_tx --sample-rate 1000000 --deviation 75000 --bits 12 --emphasis eu --mode stereo --input-channels 2 --input-sample-rate 44100 - - | m2sdr_play -\n"
           "Note: Full-scale audio gives the full deviation. The audio is limited to full scale after the\n"
           "      pre-emphasis, which raises the high frequencies: keep some headroom in the source.\n");
    exit(status);
}

/* Main */
/*------*/

static struct option options[] = {
    { "help",       no_argument,       NULL, 'h' },
    { "sample-rate", required_argument, NULL, 's' },
    { "samplerate", required_argument, NULL, 's' },
    { "deviation",  required_argument, NULL, 'd' },
    { "bits",       required_argument, NULL, 'b' },
    { "format",     required_argument, NULL, 1 },
    { "sc8",        no_argument,       NULL, '8' },
    { "emphasis",   required_argument, NULL, 'e' },
    { "mode",       required_argument, NULL, 'm' },
    { "input-channels", required_argument, NULL, 'i' },
    { "input-sample-rate", required_argument, NULL, 'f' },
    { "input-samplerate", required_argument, NULL, 'f' },
    { "rds",        no_argument,       NULL, 2 },
    { "rds-pi",     required_argument, NULL, 3 },
    { "rds-ps",     required_argument, NULL, 4 },
    { "rds-rt",     required_argument, NULL, 5 },
    { "rds-pty",    required_argument, NULL, 6 },
    { "rds-ct",     required_argument, NULL, 7 },
    { NULL,         0,                 NULL, 0 }
};

int main(int argc, char **argv) {
    m2sdr_cli_handle_version(argc, argv);
    int c;
    int option_index;

    char *input_file = NULL;
    char *output_file = NULL;
    struct m2sdr_fm_config cfg;
    int bits = 12;
    bool bits_set = false;
    enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
    char *emphasis_type = "eu";
    char *mode = "mono";
    int input_channels = 0;
    double input_srate = DEFAULT_INPUT_RATE;
    struct fm_tx_source src = {0};
    SF_INFO sfinfo = {0};
    int ret;

    m2sdr_fm_config_init(&cfg);
    cfg.rds.pi    = DEFAULT_RDS_PI;
    cfg.rds.music = true;
    snprintf(cfg.rds.ps, sizeof(cfg.rds.ps), "%s", DEFAULT_RDS_PS);

    /* Parse command-line options */
    for (;;) {
        c = getopt_long(argc, argv, "hs:d:b:8e:m:i:f:", options, &option_index);
        if (c == -1) break;
        switch (c) {
        case 'h':
            help(0);
            break;
        case 's':
            if (m2sdr_cli_parse_double_range(optarg, 1.0, 1.0e9, &cfg.sample_rate) != 0) {
                m2sdr_cli_error("invalid sample rate '%s'", optarg);
                return 1;
            }
            break;
        case 'd':
            if (m2sdr_cli_parse_double_range(optarg, 1.0, 1.0e9, &cfg.deviation) != 0) {
                m2sdr_cli_error("invalid FM deviation '%s'", optarg);
                return 1;
            }
            break;
        case 'b':
            if (m2sdr_cli_parse_int_range(optarg, 2, 16, &bits) != 0) {
                m2sdr_cli_error("invalid bits value '%s' (expected 2 to 16)", optarg);
                return 1;
            }
            bits_set = true;
            break;
        case 1:
            if (m2sdr_cli_parse_format(optarg, &format) != 0 || format == M2SDR_FORMAT_BFP8_Q11) {
                m2sdr_cli_invalid_choice("format", optarg, "sc16 or sc8");
                return 1;
            }
            if (format == M2SDR_FORMAT_SC8_Q7 && !bits_set)
                bits = 8;
            break;
        case '8':
            format = M2SDR_FORMAT_SC8_Q7;
            if (!bits_set) bits = 8;
            break;
        case 'e':
            emphasis_type = optarg;
            break;
        case 'm':
            mode = optarg;
            break;
        case 'i':
            if (m2sdr_cli_parse_int_range(optarg, 1, 2, &input_channels) != 0) {
                m2sdr_cli_error("invalid input channel count '%s' (expected 1 or 2)", optarg);
                return 1;
            }
            break;
        case 'f':
            if (m2sdr_cli_parse_double_range(optarg, 1000.0, 1.0e7, &input_srate) != 0) {
                m2sdr_cli_error("invalid input sample rate '%s'", optarg);
                return 1;
            }
            break;
        case 2:
            cfg.rds.enable = true;
            break;
        case 3: {
            /* A PI is written in hexadecimal, with or without 0x. */
            char *end;
            unsigned long pi = strtoul(optarg, &end, 16);
            if (end == optarg || *end != '\0' || pi > 0xffff) {
                m2sdr_cli_error("invalid RDS PI '%s' (expected four hexadecimal digits, for instance F212)", optarg);
                return 1;
            }
            cfg.rds.pi     = (uint16_t)pi;
            cfg.rds.enable = true;
            break;
        }
        case 4:
            if (strlen(optarg) > M2SDR_FM_RDS_PS_LEN) {
                m2sdr_cli_error("RDS PS '%s' is longer than %d characters", optarg, M2SDR_FM_RDS_PS_LEN);
                return 1;
            }
            snprintf(cfg.rds.ps, sizeof(cfg.rds.ps), "%s", optarg);
            cfg.rds.enable = true;
            break;
        case 5:
            if (strlen(optarg) > M2SDR_FM_RDS_RT_LEN) {
                m2sdr_cli_error("RDS RT is longer than %d characters", M2SDR_FM_RDS_RT_LEN);
                return 1;
            }
            snprintf(cfg.rds.rt, sizeof(cfg.rds.rt), "%s", optarg);
            cfg.rds.enable = true;
            break;
        case 6: {
            int pty;
            if (m2sdr_cli_parse_int_range(optarg, 0, 31, &pty) != 0) {
                m2sdr_cli_error("invalid RDS PTY '%s' (expected 0 to 31)", optarg);
                return 1;
            }
            cfg.rds.pty    = (uint8_t)pty;
            cfg.rds.enable = true;
            break;
        }
        case 7: {
            time_t t;
            struct tm local;
            if (strcmp(optarg, "now") == 0) {
                cfg.rds.ct_unix_time = (int64_t)time(NULL);
            } else if (m2sdr_cli_parse_int64(optarg, &cfg.rds.ct_unix_time) != 0) {
                m2sdr_cli_error("invalid RDS clock time '%s' (expected 'now' or seconds since 1970)", optarg);
                return 1;
            }
            /* Local time offset of the host at this date, in half hours. */
            t = (time_t)cfg.rds.ct_unix_time;
            if (localtime_r(&t, &local))
                cfg.rds.ct_offset = (int)lround((double)local.tm_gmtoff / 1800.0);
            cfg.rds.ct     = true;
            cfg.rds.enable = true;
            break;
        }
        default:
            exit(1);
        }
    }
    cfg.bits = (unsigned)bits;

    /* Set tau based on emphasis type */
    if (strcmp(emphasis_type, "us") == 0) {
        cfg.tau = M2SDR_FM_TAU_US;
    } else if (strcmp(emphasis_type, "eu") == 0) {
        cfg.tau = M2SDR_FM_TAU_EU;
    } else if (strcmp(emphasis_type, "none") == 0) {
        cfg.tau = 0.0;
    } else {
        m2sdr_cli_invalid_choice("emphasis", emphasis_type, "us, eu or none");
        exit(1);
    }

    /* Validate mode */
    if (strcmp(mode, "mono") != 0 && strcmp(mode, "stereo") != 0) {
        m2sdr_cli_invalid_choice("mode", mode, "mono or stereo");
        exit(1);
    }
    cfg.stereo = (strcmp(mode, "stereo") == 0);

    /* Validate positional arguments */
    if (optind + 2 != argc) {
        fprintf(stderr, "Error: Both input and output arguments are required\n");
        help(1);
    }
    input_file = argv[optind];
    output_file = argv[optind + 1];

    /* Handle input */
    /* Messages go to stderr: stdout carries the I/Q samples when the tool is used in a pipe. */
    if (strcmp(input_file, "-") == 0) {
        if (input_channels == 0) {
            fprintf(stderr, "Error: -i/--input-channels required for stdin input.\n");
            help(1);
        }
        src.raw      = stdin;
        src.channels = input_channels;
        src.rate     = input_srate;
    } else {
        src.wav = sf_open(input_file, SFM_READ, &sfinfo);
        if (!src.wav) {
            fprintf(stderr, "Error: Could not open input file %s: %s\n", input_file, sf_strerror(NULL));
            fprintf(stderr, "Note: Only WAV files are supported. Convert MP3 to WAV with: ffmpeg -i %s %s.wav\n", input_file, input_file);
            exit(1);
        }
        if (sfinfo.channels < 1 || sfinfo.samplerate < 1) {
            fprintf(stderr, "Error: Unsupported audio in %s\n", input_file);
            sf_close(src.wav);
            exit(1);
        }
        /* Files with more than two channels: the first two are used. */
        src.channels = sfinfo.channels;
        src.rate     = sfinfo.samplerate;
    }

    /* Open output file or use stdout */
    FILE *outfile = (strcmp(output_file, "-") == 0) ? stdout : fopen(output_file, "wb");
    if (!outfile) {
        fprintf(stderr, "Error: Could not open output file %s\n", output_file);
        if (src.wav) sf_close(src.wav);
        exit(1);
    }

    /* Perform FM modulation */
    ret = modulate_audio(&src, outfile, &cfg, format);

    /* Cleanup */
    if (src.wav) sf_close(src.wav);
    if (outfile != stdout && fclose(outfile) != 0 && ret == 0) {
        m2sdr_cli_error("could not write %s", output_file);
        ret = -1;
    }
    if (ret != 0)
        return 1;

    /* Report completion */
    /* Keep stdout reserved for IQ samples when the tool is used in a pipe. */
    fprintf(stderr, "Wrote %s\n", (strcmp(output_file, "-") == 0) ? "stdout" : output_file);

    return 0;
}
