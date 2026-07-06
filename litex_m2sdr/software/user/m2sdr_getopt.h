/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef M2SDR_GETOPT_H
#define M2SDR_GETOPT_H

/* Only MSVC lacks getopt; MinGW-w64 ships a permuting getopt_long that
 * matches the glibc behavior the tools rely on. */
#if !defined(_MSC_VER)
#include <getopt.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#define no_argument       0
#define required_argument 1
#define optional_argument 2

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

#ifdef __cplusplus
}
#endif

#endif

#endif /* M2SDR_GETOPT_H */
