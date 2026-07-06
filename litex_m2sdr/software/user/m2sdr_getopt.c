/* SPDX-License-Identifier: BSD-2-Clause */

#include "m2sdr_getopt.h"

#if defined(_MSC_VER)

#include <stdio.h>
#include <string.h>

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;

static const char *short_pos;

static int short_option_requires_arg(const char *optstring, int opt)
{
    const char *p = strchr(optstring, opt);

    return p && p[1] == ':';
}

static int parse_long_option(int argc, char *const argv[],
                             const struct option *longopts, int *longindex)
{
    const char *arg = argv[optind] + 2;
    const char *value = strchr(arg, '=');
    size_t name_len = value ? (size_t)(value - arg) : strlen(arg);

    for (int i = 0; longopts && longopts[i].name; i++) {
        if (strlen(longopts[i].name) != name_len ||
            strncmp(longopts[i].name, arg, name_len) != 0) {
            continue;
        }

        if (longindex)
            *longindex = i;

        if (longopts[i].has_arg == required_argument) {
            if (value) {
                optarg = (char *)value + 1;
            } else if (optind + 1 < argc) {
                optarg = argv[++optind];
            } else {
                if (opterr)
                    fprintf(stderr, "option '--%s' requires an argument\n", longopts[i].name);
                optind++;
                return '?';
            }
        } else if (longopts[i].has_arg == optional_argument) {
            optarg = value ? (char *)value + 1 : NULL;
        } else {
            optarg = NULL;
            if (value) {
                if (opterr)
                    fprintf(stderr, "option '--%s' does not take an argument\n", longopts[i].name);
                optind++;
                return '?';
            }
        }

        optind++;
        if (longopts[i].flag) {
            *longopts[i].flag = longopts[i].val;
            return 0;
        }
        return longopts[i].val;
    }

    if (opterr)
        fprintf(stderr, "unknown option '--%.*s'\n", (int)name_len, arg);
    optind++;
    return '?';
}

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex)
{
    int opt;

    optarg = NULL;
    if (longindex)
        *longindex = 0;

    if (short_pos && *short_pos) {
        opt = (unsigned char)*short_pos++;
        optopt = opt;
        if (!strchr(optstring, opt)) {
            if (opterr)
                fprintf(stderr, "unknown option '-%c'\n", opt);
            if (!*short_pos) {
                short_pos = NULL;
                optind++;
            }
            return '?';
        }
        if (short_option_requires_arg(optstring, opt)) {
            if (*short_pos) {
                optarg = (char *)short_pos;
                short_pos = NULL;
                optind++;
            } else if (optind + 1 < argc) {
                optarg = argv[++optind];
                short_pos = NULL;
                optind++;
            } else {
                if (opterr)
                    fprintf(stderr, "option '-%c' requires an argument\n", opt);
                short_pos = NULL;
                optind++;
                return '?';
            }
        } else if (!*short_pos) {
            short_pos = NULL;
            optind++;
        }
        return opt;
    }

    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || argv[optind][1] == '\0')
        return -1;

    if (strcmp(argv[optind], "--") == 0) {
        optind++;
        return -1;
    }

    if (argv[optind][1] == '-')
        return parse_long_option(argc, argv, longopts, longindex);

    short_pos = argv[optind] + 1;
    return getopt_long(argc, argv, optstring, longopts, longindex);
}

#endif
