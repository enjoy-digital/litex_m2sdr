/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR FM Receiver Utility.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 * Description:
 * This utility FM demodulates interleaved I/Q samples into audio. It reads the I/Q samples
 * from a file or stdin, demodulates them with the demodulator of libm2sdr (m2sdr_fm.h: channel
 * filter, discriminator, stereo decoder, de-emphasis), resamples the audio to 44.1 kHz and
 * writes a WAV file, or raw 16-bit PCM to stdout. With --rds, it prints what the station
 * sends on its RDS subcarrier.
 *
 * Usage Example:
 *     ./m2sdr_rf --sample-rate 1e6 --rx-freq 100e6 --rx-gain 20 --chan 1t1r
 *     ./m2sdr_record - | ./m2sdr_fm_rx --sample-rate 1000000 --deviation 75000 --bits 12 - music.wav
 *     ./m2sdr_record - | ./m2sdr_fm_rx --sample-rate 1000000 --deviation 75000 --bits 12 --emphasis eu --mode stereo - - | ffmpeg -f s16le -ac 2 -ar 44100 -i - -f alsa default
 *     ./m2sdr_record - | ./m2sdr_fm_rx --sample-rate 1000000 --mode stereo --rds - station.wav
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <getopt.h>
#include <samplerate.h>
#include <time.h>

#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "m2sdr_fm.h"

/* Constants */
/*-----------*/

#define IQ_FRAMES  16384    /* I/Q samples read per pass. */
#define AUDIO_RATE 44100.0  /* Output audio sample rate. */

/* WAV header */
/*------------*/

static void put_le(uint8_t *p, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; i++)
        p[i] = (uint8_t)(value >> (8 * i));
}

/* 16-bit PCM header; written first with no data, then again with the final size. */
static int write_wav_header(FILE *f, int channels, uint32_t data_bytes) {
    uint8_t h[44];

    memcpy(h + 0, "RIFF", 4);
    put_le(h + 4, 36 + data_bytes, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);                                  /* Size of the fmt chunk. */
    put_le(h + 20, 1, 2);                                   /* PCM. */
    put_le(h + 22, (uint32_t)channels, 2);
    put_le(h + 24, (uint32_t)AUDIO_RATE, 4);
    put_le(h + 28, (uint32_t)AUDIO_RATE * channels * 2, 4); /* Bytes per second. */
    put_le(h + 32, (uint32_t)channels * 2, 2);              /* Bytes per frame. */
    put_le(h + 34, 16, 2);                                  /* Bits per sample. */
    memcpy(h + 36, "data", 4);
    put_le(h + 40, data_bytes, 4);
    return (fwrite(h, 1, sizeof(h), f) == sizeof(h)) ? 0 : -1;
}

/* RDS */
/*-----*/

/* Text for a terminal: the codes above ASCII belong to the RDS character table, not to the
 * one of the terminal. */
static const char *rds_ascii(const char *text, char *out, size_t size) {
    size_t i;
    for (i = 0; text[i] && i + 1 < size; i++)
        out[i] = ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e) ? '?' : text[i];
    out[i] = '\0';
    return out;
}

/* What was decoded so far, on one line of stderr, when it differs from the last line. */
static void print_rds(const struct m2sdr_fm_rds_info *info) {
    static char last[256];
    char line[256], text[M2SDR_FM_RDS_RT_LEN + 1];
    size_t n = 0;

    n += (size_t)snprintf(line + n, sizeof(line) - n, "RDS: PI=%04X PTY=%u (%s)%s%s", info->pi, info->pty,
        m2sdr_fm_rds_pty_name(info->pty), info->tp ? " TP" : "", info->ta ? " TA" : "");
    if (info->ps_valid)
        n += (size_t)snprintf(line + n, sizeof(line) - n, " PS=\"%s\"", rds_ascii(info->ps, text, sizeof(text)));
    if (info->rt_valid)
        n += (size_t)snprintf(line + n, sizeof(line) - n, " RT=\"%s\"", rds_ascii(info->rt, text, sizeof(text)));
    if (info->ct_valid) {
        /* Local time of the station, and its offset from UTC. */
        const int minutes = info->ct_offset * 30;
        time_t t = (time_t)(info->ct_unix_time + minutes * 60);
        struct tm tm;
        if (gmtime_r(&t, &tm))
            snprintf(line + n, sizeof(line) - n, " CT=%04d-%02d-%02d %02d:%02d %c%02d:%02d", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, minutes < 0 ? '-' : '+',
                abs(minutes) / 60, abs(minutes) % 60);
    }
    if (strcmp(line, last) != 0) {
        fprintf(stderr, "%s\n", line);
        memcpy(last, line, sizeof(last));
    }
}

/* FM Demodulation */
/*-----------------*/

/* Audio to 16-bit PCM, limited to full scale. */
static int write_pcm(FILE *out, const float *audio, size_t samples, int16_t *pcm, uint64_t *clipped) {
    for (size_t k = 0; k < samples; k++) {
        float v = audio[k] * 32767.0f;
        if (v > 32767.0f) {
            v = 32767.0f;
            (*clipped)++;
        } else if (v < -32767.0f) {
            v = -32767.0f;
            (*clipped)++;
        }
        pcm[k] = (int16_t)lrintf(v);
    }
    return (fwrite(pcm, sizeof(int16_t), samples, out) == samples) ? 0 : -1;
}

static int demodulate_fm(FILE *infile, FILE *outfile, const struct m2sdr_fm_config *cfg, enum m2sdr_format format, uint64_t *pcm_bytes) {
    struct m2sdr_fm_rx *rx = m2sdr_fm_rx_new(cfg);
    SRC_STATE *resampler = NULL;
    SRC_DATA data;
    int16_t *iq = NULL, *pcm = NULL;
    int8_t *iq8 = NULL;
    float *audio = NULL, *resampled = NULL;
    int audio_channels = cfg->stereo ? 2 : 1;
    size_t audio_frames, out_frames = 0;
    uint64_t clipped = 0;
    struct m2sdr_fm_rds_info rds;
    unsigned rds_updates = 0;
    bool pilot = false;
    int ret = -1;
    int eof = 0;

    *pcm_bytes = 0;
    if (!rx) {
        m2sdr_cli_error("unsupported FM settings (sample rate %.0f SPS: at least %.0f in %s, %.0f with RDS, deviation at most 0.4 x sample rate)",
            cfg->sample_rate, cfg->stereo ? M2SDR_FM_MIN_RATE_STEREO : M2SDR_FM_MIN_RATE_MONO,
            cfg->stereo ? "stereo" : "mono", M2SDR_FM_MIN_RATE_RDS);
        return -1;
    }

    /* The demodulator gives the audio at its own rate: resample to the output rate. */
    memset(&data, 0, sizeof(data));
    data.src_ratio = AUDIO_RATE / m2sdr_fm_rx_audio_rate(rx);
    audio_frames   = IQ_FRAMES / m2sdr_fm_rx_decimation(rx) + 1;
    out_frames     = audio_frames;
    if (fabs(data.src_ratio - 1.0) > 1e-9) {
        int error;
        if (!src_is_valid_ratio(data.src_ratio)) {
            m2sdr_cli_error("cannot resample %.0f Hz audio to %.0f Hz", m2sdr_fm_rx_audio_rate(rx), AUDIO_RATE);
            goto cleanup;
        }
        resampler = src_new(SRC_SINC_MEDIUM_QUALITY, audio_channels, &error);
        if (!resampler) {
            m2sdr_cli_error("resampler: %s", src_strerror(error));
            goto cleanup;
        }
        out_frames = (size_t)(audio_frames * data.src_ratio) + 1024;
    }

    iq        = malloc(sizeof(int16_t) * 2 * IQ_FRAMES);
    iq8       = malloc(sizeof(int8_t) * 2 * IQ_FRAMES);
    audio     = malloc(sizeof(float) * audio_frames * audio_channels);
    resampled = malloc(sizeof(float) * out_frames * audio_channels);
    pcm       = malloc(sizeof(int16_t) * out_frames * audio_channels);
    if (!iq || !iq8 || !audio || !resampled || !pcm) {
        m2sdr_cli_error("memory allocation failed");
        goto cleanup;
    }

    while (!eof) {
        size_t count, frames;

        if (format == M2SDR_FORMAT_SC8_Q7) {
            count = fread(iq8, 2 * sizeof(int8_t), IQ_FRAMES, infile);
            for (size_t k = 0; k < 2 * count; k++)
                iq[k] = iq8[k];
        } else {
            count = fread(iq, 2 * sizeof(int16_t), IQ_FRAMES, infile);
        }
        eof = (count < IQ_FRAMES);

        frames = m2sdr_fm_rx_process(rx, iq, count, audio);
        if (cfg->stereo && m2sdr_fm_rx_pilot_locked(rx) != pilot) {
            pilot = !pilot;
            fprintf(stderr, "Stereo pilot %s\n", pilot ? "locked" : "lost");
        }
        if (cfg->rds.enable) {
            m2sdr_fm_rx_rds(rx, &rds);
            if (rds.updates != rds_updates) {
                rds_updates = rds.updates;
                print_rds(&rds);
            }
        }

        if (!resampler) {
            if (write_pcm(outfile, audio, frames * audio_channels, pcm, &clipped) != 0)
                goto write_error;
            *pcm_bytes += frames * audio_channels * sizeof(int16_t);
        } else {
            /* The resampler gives its last samples once the end of the input is flagged. */
            data.data_in      = audio;
            data.input_frames = (long)frames;
            data.end_of_input = eof;
            for (;;) {
                size_t n;
                int err;

                data.data_out      = resampled;
                data.output_frames = (long)out_frames;
                err = src_process(resampler, &data);
                if (err) {
                    m2sdr_cli_error("resampler: %s", src_strerror(err));
                    goto cleanup;
                }
                data.data_in      += data.input_frames_used * audio_channels;
                data.input_frames -= data.input_frames_used;
                n = (size_t)data.output_frames_gen * audio_channels;
                if (write_pcm(outfile, resampled, n, pcm, &clipped) != 0)
                    goto write_error;
                *pcm_bytes += n * sizeof(int16_t);
                if (data.input_frames == 0 && (!eof || data.output_frames_gen == 0))
                    break;
            }
        }

        /* In a pipe, give the audio to the player as it comes. */
        if (outfile == stdout && fflush(outfile) != 0)
            goto write_error;
    }

    if (clipped)
        fprintf(stderr, "Warning: %" PRIu64 " audio samples limited to full scale; check --deviation.\n", clipped);
    if (cfg->rds.enable) {
        m2sdr_fm_rx_rds(rx, &rds);
        fprintf(stderr, "RDS: %" PRIu64 " groups, %" PRIu64 " wrong blocks of %" PRIu64 "\n",
            rds.groups, rds.block_errors, rds.blocks);
    }
    ret = 0;
    goto cleanup;

write_error:
    m2sdr_cli_error("could not write the audio");

cleanup:
    free(iq);
    free(iq8);
    free(audio);
    free(resampled);
    free(pcm);
    if (resampler) src_delete(resampler);
    m2sdr_fm_rx_free(rx);
    return ret;
}

/* Help */
/*------*/

static void help(int status) {
    /* Display usage information */
    fprintf(status ? stderr : stdout,
           "M2SDR FM Receiver Utility\n"
           "usage: m2sdr_fm_rx [options] input output\n"
           "\n"
           "Options:\n"
           "-h, --help            Display this help message.\n"
           "-s, --sample-rate sps  Set I/Q sample rate in SPS (default: 1000000).\n"
           "-d, --deviation dev   Set FM deviation of full-scale audio in Hz (default: 75000).\n"
           "-b, --bits bits       Set bits per I/Q sample (accepted for symmetry with m2sdr_fm_tx; the level of the samples does not matter).\n"
           "    --format FMT      Sample format: sc16 or sc8 (default: sc16).\n"
           "-8, --sc8             Input is 8-bit I/Q samples (SC8 Q7).\n"
           "-e, --emphasis type   Set de-emphasis to us (75 us), eu (50 us) or none (default: eu).\n"
           "-m, --mode mode       Set mode to mono or stereo (default: mono).\n"
           "-r, --rds             Decode the RDS subcarrier and print it on stderr: PI, PTY, PS, RT, CT.\n"
           "\n"
           "Arguments:\n"
           "input                 Input I/Q bin file ('-' for stdin).\n"
           "output                Output WAV file ('-' for raw 16-bit PCM at 44100 Hz on stdout).\n"
           "\n"
           "Example:\n"
           "m2sdr_fm_rx --sample-rate 1000000 --deviation 75000 --bits 12 input.bin output.wav\n"
           "Note: In stereo, the two channels are decoded when the 19 kHz pilot is received, and carry\n"
           "      the mono signal otherwise.\n");
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
    { "rds",        no_argument,       NULL, 'r' },
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
    enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
    char *emphasis_type = "eu";
    char *mode = "mono";
    uint64_t pcm_bytes = 0;
    int ret;

    m2sdr_fm_config_init(&cfg);

    /* Parse command-line options */
    for (;;) {
        c = getopt_long(argc, argv, "hs:d:b:8e:m:r", options, &option_index);
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
            if (m2sdr_cli_parse_int_range(optarg, 1, 16, &bits) != 0) {
                m2sdr_cli_error("invalid bits value '%s' (expected 1 to 16)", optarg);
                return 1;
            }
            break;
        case 1:
            if (m2sdr_cli_parse_format(optarg, &format) != 0 || format == M2SDR_FORMAT_BFP8_Q11) {
                m2sdr_cli_invalid_choice("format", optarg, "sc16 or sc8");
                return 1;
            }
            break;
        case '8':
            format = M2SDR_FORMAT_SC8_Q7;
            break;
        case 'e':
            emphasis_type = optarg;
            break;
        case 'm':
            mode = optarg;
            break;
        case 'r':
            cfg.rds.enable = true;
            break;
        default:
            exit(1);
        }
    }

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

    /* Open input file or use stdin */
    FILE *infile = (strcmp(input_file, "-") == 0) ? stdin : fopen(input_file, "rb");
    if (!infile) {
        fprintf(stderr, "Error: Could not open input file %s\n", input_file);
        exit(1);
    }

    /* Open output file or use stdout */
    FILE *outfile = (strcmp(output_file, "-") == 0) ? stdout : fopen(output_file, "wb");
    if (!outfile) {
        fprintf(stderr, "Error: Could not open output file %s\n", output_file);
        fclose(infile);
        exit(1);
    }

    /* Write WAV header if output is a file */
    int audio_channels = cfg.stereo ? 2 : 1;
    if (outfile != stdout && write_wav_header(outfile, audio_channels, 0) != 0) {
        fprintf(stderr, "Error: Could not write %s\n", output_file);
        exit(1);
    }

    /* Perform FM demodulation */
    ret = demodulate_fm(infile, outfile, &cfg, format, &pcm_bytes);

    /* Update WAV header sizes if output is a file */
    if (outfile != stdout) {
        /* A WAV file holds 4 GiB at most: longer recordings keep the largest size. */
        uint32_t data_bytes = (pcm_bytes > 0xffffffffull - 36) ? 0xffffffffu - 36 : (uint32_t)pcm_bytes;
        if (fseek(outfile, 0, SEEK_SET) != 0 || write_wav_header(outfile, audio_channels, data_bytes) != 0) {
            m2sdr_cli_error("could not update the header of %s", output_file);
            ret = -1;
        }
    }

    /* Cleanup */
    if (infile != stdin) fclose(infile);
    if (outfile != stdout && fclose(outfile) != 0 && ret == 0) {
        m2sdr_cli_error("could not write %s", output_file);
        ret = -1;
    }
    if (ret != 0)
        return 1;

    /* Report completion */
    /* Keep stdout reserved for PCM samples when the tool is used in a pipe. */
    fprintf(stderr, "Wrote %s\n", (strcmp(output_file, "-") == 0) ? "stdout" : output_file);

    return 0;
}
