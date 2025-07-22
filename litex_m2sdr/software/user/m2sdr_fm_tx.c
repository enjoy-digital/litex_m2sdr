/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR FM Transmitter Utility.
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
 *
 * Description:
 * This utility FM modulates a WAV audio file into interleaved 16-bit I/Q samples for
 * use with the M2SDR software-defined radio. It reads a WAV input file, resamples it to the
 * specified sample rate, applies FM modulation, and writes the I/Q samples to an output file or
 * stdout.
 *
 * Note: Only WAV files are supported. To use MP3 files, convert them to WAV using ffmpeg:
 *     ffmpeg -i input.mp3 input.wav
 *
 * Usage Example:
 *     ./m2sdr_rf -samplerate 1e6 -tx_freq 100e6 -tx_gain -10 -chan 1t1r
 *     ./m2sdr_fm_tx -s 1000000 -b 12 music.wav - | ./m2sdr_play -
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

/* Constants */
/*-----------*/

#define SINE_TABLE_SIZE 4096
#define CHUNK_SIZE 512

/* FM Modulation Functions */
/*------------------------*/

static void generate_sine_table(int16_t *sine_table, int bits) {
    /* Generate sine table for FM modulation */
    double scale = ((1LL << (bits - 1)) - 1);
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        sine_table[i] = (int16_t) round( sin( i * 2 * M_PI / SINE_TABLE_SIZE ) * scale );
    }
}

static void modulate_audio(SNDFILE *infile, SF_INFO *sfinfo, FILE *outfile, double samplerate, double deviation, int bits, double tau, const char *mode) {
    /* Modulate audio to interleaved 16-bit I/Q samples in streaming fashion */
    int N = SINE_TABLE_SIZE;
    int shift = 32;
    int64_t multiplier = (int64_t)((deviation * N * (1LL << shift)) / samplerate);
    int64_t phase_int = 0;

    /* Initialize sine table */
    int16_t sine_table[SINE_TABLE_SIZE];
    generate_sine_table(sine_table, bits);

    /* Assume audio is normalized to [-1, 1] for streaming */
    const int64_t max_val = 32767LL;

    /* Determine audio channels */
    int audio_channels = (strcmp(mode, "stereo") == 0) ? 2 : 1;

    /* Pre-emphasis */
    double b0 = 0.0, b1 = 0.0;
    if (tau > 0.0) {
        b0 = 1.0 + 2.0 * tau * samplerate;
        b1 = 1.0 - 2.0 * tau * samplerate;
    }
    static double prev_x[2] = {0.0, 0.0};
    static double prev_y[2] = {0.0, 0.0};

    /* Stereo parameters */
    static double pilot_phase = 0.0;
    double pilot_inc = 2 * M_PI * 19000.0 / samplerate;

    /* Setup buffers */
    float *in_chunk = malloc(sizeof(float) * CHUNK_SIZE * sfinfo->channels);
    if (!in_chunk) {
        fprintf(stderr, "Error: Memory allocation failed for input chunk\n");
        exit(1);
    }
    float *audio_chunk = malloc(sizeof(float) * CHUNK_SIZE * audio_channels);
    if (!audio_chunk) {
        fprintf(stderr, "Error: Memory allocation failed for audio chunk\n");
        free(in_chunk);
        exit(1);
    }

    /* Setup resampling if input sample rate differs from desired sample rate */
    int resampling_needed = (sfinfo->samplerate != (int)samplerate);
    SRC_STATE *src_state = NULL;
    SRC_DATA src_data;
    float *out_chunk = NULL;
    int max_out_frames = 0;
    double src_ratio = 1.0;
    if (resampling_needed) {
        src_ratio = samplerate / sfinfo->samplerate;
        max_out_frames = (int)(CHUNK_SIZE * src_ratio) + 1024; /* Generous margin for filter state */
        out_chunk = malloc(sizeof(float) * max_out_frames * audio_channels);
        if (!out_chunk) {
            fprintf(stderr, "Error: Memory allocation failed for output chunk\n");
            free(in_chunk);
            free(audio_chunk);
            exit(1);
        }

        /* Initialize resampler */
        int error;
        src_state = src_new(SRC_SINC_BEST_QUALITY, audio_channels, &error);
        if (!src_state) {
            fprintf(stderr, "Error: Failed to initialize resampler\n");
            free(in_chunk);
            free(audio_chunk);
            free(out_chunk);
            exit(1);
        }
    }

    /* Process in chunks */
    while (1) {
        sf_count_t frames_read = sf_readf_float(infile, in_chunk, CHUNK_SIZE);
        if (frames_read == 0) break;

        /* Convert to audio_channels */
        for (sf_count_t k = 0; k < frames_read; k++) {
            if (sfinfo->channels == audio_channels) {
                for (int ch = 0; ch < audio_channels; ch++) {
                    audio_chunk[k * audio_channels + ch] = in_chunk[k * sfinfo->channels + ch];
                }
            } else if (sfinfo->channels == 1 && audio_channels == 2) {
                float val = in_chunk[k];
                audio_chunk[k * 2] = val;
                audio_chunk[k * 2 + 1] = val;
            } else if (sfinfo->channels == 2 && audio_channels == 1) {
                float val = (in_chunk[k * 2] + in_chunk[k * 2 + 1]) * 0.5f;
                audio_chunk[k] = val;
            } else {
                fprintf(stderr, "Error: Unsupported number of channels (%d). Only mono or stereo supported.\n", sfinfo->channels);
                free(in_chunk);
                free(audio_chunk);
                if (resampling_needed) {
                    free(out_chunk);
                    src_delete(src_state);
                }
                exit(1);
            }
        }

        float *process_buffer;
        sf_count_t process_frames;

        if (resampling_needed) {
            src_data.data_in = audio_chunk;
            src_data.input_frames = frames_read;
            src_data.data_out = out_chunk;
            src_data.output_frames = max_out_frames;
            src_data.src_ratio = src_ratio;
            src_data.end_of_input = (frames_read < CHUNK_SIZE) ? 1 : 0;

            int err = src_process(src_state, &src_data);
            if (err) {
                fprintf(stderr, "Error: Resampling failed: %s\n", src_strerror(err));
                free(in_chunk);
                free(audio_chunk);
                free(out_chunk);
                src_delete(src_state);
                exit(1);
            }

            process_frames = src_data.output_frames_gen;
            process_buffer = out_chunk;
        } else {
            process_buffer = audio_chunk;
            process_frames = frames_read;
        }

        /* Apply pre-emphasis if enabled */
        for (sf_count_t i = 0; i < process_frames; i++) {
            for (int ch = 0; ch < audio_channels; ch++) {
                double x = process_buffer[i * audio_channels + ch];
                double y = x;
                if (tau > 0.0) {
                    y = b0 * x + b1 * prev_x[ch] - prev_y[ch];
                    prev_x[ch] = x;
                    prev_y[ch] = y;
                }
                process_buffer[i * audio_channels + ch] = y;
            }
        }

        /* Modulate and write directly */
        for (sf_count_t i = 0; i < process_frames; i++) {
            double composite;
            if (audio_channels == 1) {
                composite = process_buffer[i];
            } else {
                double L = process_buffer[i * 2];
                double R = process_buffer[i * 2 + 1];
                double mono = (L + R) / 2.0;
                double diff = (L - R) / 2.0;
                double pilot = 0.1 * sin(pilot_phase);
                double diff_mod = diff * sin(2.0 * pilot_phase);
                composite = 0.9 * (mono + diff_mod) + pilot;
                pilot_phase += pilot_inc;
                pilot_phase = fmod(pilot_phase, 2.0 * M_PI);
            }
            int64_t sample = llround(composite * 32767.0);
            int64_t phase_increment = (sample * multiplier) / (max_val * (1LL << shift));
            phase_int += phase_increment;
            phase_int %= N;
            if (phase_int < 0) phase_int += N;
            int index = (int)phase_int;
            int16_t i_val = sine_table[(index + N / 4) % N];
            int16_t q_val = sine_table[index];
            fwrite(&i_val, sizeof(int16_t), 1, outfile);
            fwrite(&q_val, sizeof(int16_t), 1, outfile);
        }
    }

    /* Flush any remaining resampled data */
    if (resampling_needed) {
        src_data.input_frames = 0;
        src_data.end_of_input = 1;
        src_data.data_out = out_chunk;
        src_data.output_frames = max_out_frames;

        int err = src_process(src_state, &src_data);
        if (err) {
            fprintf(stderr, "Error: Resampling flush failed: %s\n", src_strerror(err));
            free(in_chunk);
            free(audio_chunk);
            free(out_chunk);
            src_delete(src_state);
            exit(1);
        }

        sf_count_t process_frames = src_data.output_frames_gen;
        float *process_buffer = out_chunk;

        /* Apply pre-emphasis if enabled */
        for (sf_count_t i = 0; i < process_frames; i++) {
            for (int ch = 0; ch < audio_channels; ch++) {
                double x = process_buffer[i * audio_channels + ch];
                double y = x;
                if (tau > 0.0) {
                    y = b0 * x + b1 * prev_x[ch] - prev_y[ch];
                    prev_x[ch] = x;
                    prev_y[ch] = y;
                }
                process_buffer[i * audio_channels + ch] = y;
            }
        }

        /* Modulate and write remaining */
        for (sf_count_t i = 0; i < process_frames; i++) {
            double composite;
            if (audio_channels == 1) {
                composite = process_buffer[i];
            } else {
                double L = process_buffer[i * 2];
                double R = process_buffer[i * 2 + 1];
                double mono = (L + R) / 2.0;
                double diff = (L - R) / 2.0;
                double pilot = 0.1 * sin(pilot_phase);
                double diff_mod = diff * sin(2.0 * pilot_phase);
                composite = 0.9 * (mono + diff_mod) + pilot;
                pilot_phase += pilot_inc;
                pilot_phase = fmod(pilot_phase, 2.0 * M_PI);
            }
            int64_t sample = llround(composite * 32767.0);
            int64_t phase_increment = (sample * multiplier) / (max_val * (1LL << shift));
            phase_int += phase_increment;
            phase_int %= N;
            if (phase_int < 0) phase_int += N;
            int index = (int)phase_int;
            int16_t i_val = sine_table[(index + N / 4) % N];
            int16_t q_val = sine_table[index];
            fwrite(&i_val, sizeof(int16_t), 1, outfile);
            fwrite(&q_val, sizeof(int16_t), 1, outfile);
        }
    }

    if (outfile == stdout) fflush(outfile);

    /* Cleanup */
    free(in_chunk);
    free(audio_chunk);
    if (resampling_needed) {
        free(out_chunk);
        src_delete(src_state);
    }
}

/* Help */
/*------*/

static void help(void) {
    /* Display usage information */
    printf("M2SDR FM Transmitter Utility\n"
           "usage: m2sdr_fm_tx [options] input output\n"
           "\n"
           "Options:\n"
           "-h, --help            Display this help message.\n"
           "-s, --samplerate sps  Set sample rate in SPS (default: 500000).\n"
           "-d, --deviation dev   Set FM deviation in Hz (default: 75000).\n"
           "-b, --bits bits       Set bits per I/Q sample (≤16, default: 12).\n"
           "-e, --emphasis type   Set pre-emphasis to us, eu or none (default: eu).\n"
           "-m, --mode mode       Set mode to mono or stereo (default: mono).\n"
           "\n"
           "Arguments:\n"
           "input                 Input WAV file (MP3 not supported; convert using: ffmpeg -i input.mp3 input.wav).\n"
           "output                Output file for I/Q samples ('-' for stdout).\n"
           "\n"
           "Example:\n"
           "m2sdr_fm_tx -s 500000 -d 75000 -b 12 input.wav output.bin\n"
           "Note: Convert MP3 to WAV using: ffmpeg -i input.mp3 input.wav\n"
           "Note: This version processes in streaming mode without global normalization, assuming audio is normalized to full scale.\n");
    exit(1);
}

/* Main */
/*------*/

static struct option options[] = {
    { "help",       no_argument,       NULL, 'h' },
    { "samplerate", required_argument, NULL, 's' },
    { "deviation",  required_argument, NULL, 'd' },
    { "bits",       required_argument, NULL, 'b' },
    { "emphasis",   required_argument, NULL, 'e' },
    { "mode",       required_argument, NULL, 'm' },
    { NULL,         0,                 NULL, 0 }
};

int main(int argc, char **argv) {
    int c;
    int option_index;

    char *input_file = NULL;
    char *output_file = NULL;
    double samplerate = 500000.0;
    double deviation = 75000.0;
    int bits = 12;
    char *emphasis_type = "eu";
    char *mode = "mono";

    /* Parse command-line options */
    for (;;) {
        c = getopt_long(argc, argv, "hs:d:b:e:m:", options, &option_index);
        if (c == -1) break;
        switch (c) {
        case 'h':
            help();
            break;
        case 's':
            samplerate = atof(optarg);
            break;
        case 'd':
            deviation = atof(optarg);
            break;
        case 'b':
            bits = atoi(optarg);
            break;
        case 'e':
            emphasis_type = optarg;
            break;
        case 'm':
            mode = optarg;
            break;
        default:
            exit(1);
        }
    }

    /* Set tau based on emphasis type */
    double tau = 0.0;
    if (strcmp(emphasis_type, "us") == 0) {
        tau = 75e-6;
    } else if (strcmp(emphasis_type, "eu") == 0) {
        tau = 50e-6;
    } else if (strcmp(emphasis_type, "none") == 0) {
        tau = 0.0;
    } else {
        fprintf(stderr, "Error: Invalid emphasis type '%s'. Must be 'us', 'eu', or 'none'.\n", emphasis_type);
        exit(1);
    }

    /* Validate mode */
    if (strcmp(mode, "mono") != 0 && strcmp(mode, "stereo") != 0) {
        fprintf(stderr, "Error: Invalid mode '%s'. Must be 'mono' or 'stereo'.\n", mode);
        exit(1);
    }

    /* Validate positional arguments */
    if (optind + 2 != argc) {
        fprintf(stderr, "Error: Both input and output arguments are required\n");
        help();
    }
    input_file = argv[optind];
    output_file = argv[optind + 1];

    /* Validate options */
    if (bits > 16) {
        fprintf(stderr, "Error: Bits per sample must be <= 16\n");
        exit(1);
    }

    /* Debug: Print input file path */
    printf("Attempting to open input file: %s\n", input_file);

    /* Open input audio file */
    SF_INFO sfinfo = {0};
    SNDFILE *infile = sf_open(input_file, SFM_READ, &sfinfo);
    if (!infile) {
        fprintf(stderr, "Error: Could not open input file %s: %s\n", input_file, sf_strerror(NULL));
        fprintf(stderr, "Note: Only WAV files are supported. Convert MP3 to WAV with: ffmpeg -i %s %s.wav\n", input_file, input_file);
        exit(1);
    }

    /* Open output file or use stdout */
    FILE *outfile = (strcmp(output_file, "-") == 0) ? stdout : fopen(output_file, "wb");
    if (!outfile) {
        fprintf(stderr, "Error: Could not open output file %s\n", output_file);
        sf_close(infile);
        exit(1);
    }

    /* Perform FM modulation */
    modulate_audio(infile, &sfinfo, outfile, samplerate, deviation, bits, tau, mode);

    /* Cleanup */
    sf_close(infile);
    if (outfile != stdout) fclose(outfile);

    /* Report completion */
    printf("✓ wrote %s\n", (strcmp(output_file, "-") == 0) ? "stdout" : output_file);

    return 0;
}