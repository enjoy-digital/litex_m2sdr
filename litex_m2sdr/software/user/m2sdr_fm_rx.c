/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR FM Receiver Utility.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 * Description:
 * This utility FM demodulates interleaved 16-bit I/Q samples into a WAV audio file.
 * It reads I/Q samples from an input file or stdin, applies FM demodulation, and writes the audio samples to an output file or stdout.
 *
 * Usage Example:
 *     ./m2sdr_rf -samplerate 1e6 -rx_freq 100e6 -rx_gain 20 -chan 1t1r
 *     ./m2sdr_record - | ./m2sdr_fm_rx -s 1000000 -d 75000 -b 12 -o music.wav
 *     ./m2sdr_record - | ./m2sdr_fm_rx -s 1000000 -d 75000 -b 12 -e eu -m stereo - - | ffmpeg -f s16le -ac 2 -ar 44100 -i - -f alsa default
 *
 */

/* Dependencies: libsamplerate, math.h */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <getopt.h>
#include <samplerate.h>

/* Constants */
/*-----------*/

#define CHUNK_SIZE 512
#define AUDIO_RATE 44100.0  /* Default output audio sample rate */

/* FM Demodulation Functions */
/*---------------------------*/

static void demodulate_fm(FILE *infile, FILE *outfile, double samplerate, double deviation, int bits, double tau, const char *mode) {
    /* Demodulate interleaved 16-bit I/Q samples to audio in streaming fashion */
    int audio_channels = (strcmp(mode, "stereo") == 0) ? 2 : 1;
    const double max_val = 32767.0;

    /* De-emphasis constants */
    double a1 = 0.0, b0 = 0.0, b1 = 0.0;
    if (tau > 0.0) {
        double alpha = exp(-1.0 / (AUDIO_RATE * tau));
        a1 = alpha;
        b0 = 1.0 - alpha;
        b1 = 0.0;
    }
    static double prev_y[1] = {0.0};

    /* Setup buffers */
    int16_t *iq_chunk = malloc(sizeof(int16_t) * CHUNK_SIZE * 2);
    if (!iq_chunk) {
        fprintf(stderr, "Error: Memory allocation failed for I/Q chunk\n");
        exit(1);
    }
    float *audio_chunk = malloc(sizeof(float) * CHUNK_SIZE);
    if (!audio_chunk) {
        fprintf(stderr, "Error: Memory allocation failed for audio chunk\n");
        free(iq_chunk);
        exit(1);
    }

    /* Resampling for decimation from samplerate to AUDIO_RATE */
    int resampling_needed = (samplerate != AUDIO_RATE);
    SRC_STATE *src_state = NULL;
    SRC_DATA src_data;
    float *resampled_chunk = NULL;
    int max_out_frames = 0;
    double src_ratio = AUDIO_RATE / samplerate;
    if (resampling_needed) {
        max_out_frames = (int)(CHUNK_SIZE * src_ratio) + 1024;
        resampled_chunk = malloc(sizeof(float) * max_out_frames);
        if (!resampled_chunk) {
            fprintf(stderr, "Error: Memory allocation failed for resampled chunk\n");
            free(iq_chunk);
            free(audio_chunk);
            exit(1);
        }

        int error;
        src_state = src_new(SRC_SINC_BEST_QUALITY, 1, &error);
        if (!src_state) {
            fprintf(stderr, "Error: Failed to initialize resampler\n");
            free(iq_chunk);
            free(audio_chunk);
            free(resampled_chunk);
            exit(1);
        }
    }

    static double prev_phase = 0.0;

    /* Process in chunks */
    while (1) {
        size_t bytes_read = fread(iq_chunk, sizeof(int16_t), CHUNK_SIZE * 2, infile);
        size_t frames_read = bytes_read / 2;
        if (frames_read == 0) break;

        for (size_t k = 0; k < frames_read; k++) {
            double I = iq_chunk[k * 2] / (double)(1 << (bits - 1));
            double Q = iq_chunk[k * 2 + 1] / (double)(1 << (bits - 1));
            double phase = atan2(Q, I);
            double phase_diff = phase - prev_phase;
            if (phase_diff > M_PI) phase_diff -= 2 * M_PI;
            if (phase_diff < -M_PI) phase_diff += 2 * M_PI;
            prev_phase = phase;
            audio_chunk[k] = (phase_diff * samplerate) / (2 * M_PI * deviation);
        }

        float *process_buffer = audio_chunk;
        size_t process_frames = frames_read;

        if (resampling_needed) {
            src_data.data_in = audio_chunk;
            src_data.input_frames = frames_read;
            src_data.data_out = resampled_chunk;
            src_data.output_frames = max_out_frames;
            src_data.src_ratio = src_ratio;
            src_data.end_of_input = (frames_read < CHUNK_SIZE) ? 1 : 0;

            int err = src_process(src_state, &src_data);
            if (err) {
                fprintf(stderr, "Error: Resampling failed: %s\n", src_strerror(err));
                free(iq_chunk);
                free(audio_chunk);
                free(resampled_chunk);
                src_delete(src_state);
                exit(1);
            }

            process_frames = src_data.output_frames_gen;
            process_buffer = resampled_chunk;
        }

        /* Apply de-emphasis if enabled */
        for (size_t i = 0; i < process_frames; i++) {
            double x = process_buffer[i];
            double y = b0 * x + b1 * prev_y[0] + a1 * prev_y[0];
            prev_y[0] = y;
            process_buffer[i] = y;
        }

        /* Write audio to output */
        for (size_t i = 0; i < process_frames; i++) {
            int16_t audio_sample = (int16_t)(process_buffer[i] * max_val);
            fwrite(&audio_sample, sizeof(int16_t), 1, outfile);
            if (audio_channels == 2) fwrite(&audio_sample, sizeof(int16_t), 1, outfile); /* Duplicate for stereo */
        }
    }

    /* Flush any remaining resampled data */
    if (resampling_needed) {
        src_data.input_frames = 0;
        src_data.end_of_input = 1;
        src_data.data_out = resampled_chunk;
        src_data.output_frames = max_out_frames;

        int err = src_process(src_state, &src_data);
        if (err) {
            fprintf(stderr, "Error: Resampling flush failed: %s\n", src_strerror(err));
            free(iq_chunk);
            free(audio_chunk);
            free(resampled_chunk);
            src_delete(src_state);
            exit(1);
        }

        size_t process_frames = src_data.output_frames_gen;
        float *process_buffer = resampled_chunk;

        /* Apply de-emphasis if enabled */
        for (size_t i = 0; i < process_frames; i++) {
            double x = process_buffer[i];
            double y = b0 * x + b1 * prev_y[0] + a1 * prev_y[0];
            prev_y[0] = y;
            process_buffer[i] = y;
        }

        /* Write audio to output */
        for (size_t i = 0; i < process_frames; i++) {
            int16_t audio_sample = (int16_t)(process_buffer[i] * max_val);
            fwrite(&audio_sample, sizeof(int16_t), 1, outfile);
            if (audio_channels == 2) fwrite(&audio_sample, sizeof(int16_t), 1, outfile); /* Duplicate for stereo */
        }
    }

    if (outfile == stdout) fflush(outfile);

    /* Cleanup */
    free(iq_chunk);
    free(audio_chunk);
    if (resampling_needed) {
        free(resampled_chunk);
        src_delete(src_state);
    }
}

/* Help */
/*------*/

static void help(void) {
    /* Display usage information */
    printf("M2SDR FM Receiver Utility\n"
           "usage: m2sdr_fm_rx [options] input output\n"
           "\n"
           "Options:\n"
           "-h, --help            Display this help message.\n"
           "-s, --samplerate sps  Set I/Q sample rate in SPS (default: 500000).\n"
           "-d, --deviation dev   Set FM deviation in Hz (default: 75000).\n"
           "-b, --bits bits       Set bits per I/Q sample (≤16, default: 12).\n"
           "-e, --emphasis type   Set de-emphasis to us, eu or none (default: eu).\n"
           "-m, --mode mode       Set mode to mono or stereo (default: mono).\n"
           "\n"
           "Arguments:\n"
           "input                 Input I/Q bin file ('-' for stdin).\n"
           "output                Output WAV file ('-' for stdout).\n"
           "\n"
           "Example:\n"
           "m2sdr_fm_rx -s 500000 -d 75000 -b 12 input.bin output.wav\n"
           "Note: This version processes in streaming mode without global normalization.\n");
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
    double samplerate = 1000000.0;
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
    int audio_channels = (strcmp(mode, "stereo") == 0) ? 2 : 1;
    if (outfile != stdout) {
        char riff[4] = {'R', 'I', 'F', 'F'};
        fwrite(riff, 1, 4, outfile);
        uint32_t file_size = 0;  // Placeholder
        fwrite(&file_size, 1, 4, outfile);
        char wave[4] = {'W', 'A', 'V', 'E'};
        fwrite(wave, 1, 4, outfile);
        char fmt[4] = {'f', 'm', 't', ' '};
        fwrite(fmt, 1, 4, outfile);
        uint32_t fmt_size = 16;
        fwrite(&fmt_size, 1, 4, outfile);
        uint16_t audio_format = 1;  // PCM
        fwrite(&audio_format, 1, 2, outfile);
        uint16_t num_channels = audio_channels;
        fwrite(&num_channels, 1, 2, outfile);
        uint32_t sample_rate = (uint32_t)AUDIO_RATE;
        fwrite(&sample_rate, 1, 4, outfile);
        uint32_t byte_rate = sample_rate * num_channels * 2;
        fwrite(&byte_rate, 1, 4, outfile);
        uint16_t block_align = num_channels * 2;
        fwrite(&block_align, 1, 2, outfile);
        uint16_t bits_per_sample = 16;
        fwrite(&bits_per_sample, 1, 2, outfile);
        char data[4] = {'d', 'a', 't', 'a'};
        fwrite(data, 1, 4, outfile);
        uint32_t data_size = 0;  // Placeholder
        fwrite(&data_size, 1, 4, outfile);
    }

    /* Perform FM demodulation */
    demodulate_fm(infile, outfile, samplerate, deviation, bits, tau, mode);

    /* Update WAV header sizes if output is a file */
    if (outfile != stdout) {
        fflush(outfile);
        long total_size = ftell(outfile);
        // Update RIFF chunk size
        fseek(outfile, 4, SEEK_SET);
        uint32_t riff_size = total_size - 8;
        fwrite(&riff_size, 1, 4, outfile);
        // Update data subchunk size
        fseek(outfile, 40, SEEK_SET);
        uint32_t data_size = total_size - 44;
        fwrite(&data_size, 1, 4, outfile);
    }

    /* Cleanup */
    fclose(infile);
    if (outfile != stdout) fclose(outfile);

    /* Report completion */
    printf("✓ wrote %s\n", (strcmp(output_file, "-") == 0) ? "stdout" : output_file);

    return 0;
}