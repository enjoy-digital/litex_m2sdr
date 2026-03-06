/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef M2SDR_CLI_H
#define M2SDR_CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

struct m2sdr_cli_device {
    char device_id[128];
    bool use_explicit_device_id;
#ifdef USE_LITEPCIE
    char pcie_path[64];
    int device_num;
#elif defined(USE_LITEETH)
    char ip_address[1024];
    char port[16];
#endif
};

void m2sdr_cli_device_init(struct m2sdr_cli_device *dev);
int m2sdr_cli_handle_device_option(struct m2sdr_cli_device *dev, int opt, const char *optarg);
int m2sdr_cli_set_device_id(struct m2sdr_cli_device *dev, const char *device_id);
bool m2sdr_cli_finalize_device(struct m2sdr_cli_device *dev);
const char *m2sdr_cli_device_id(const struct m2sdr_cli_device *dev);
const char *m2sdr_cli_pcie_path(const struct m2sdr_cli_device *dev);
void m2sdr_cli_error(const char *fmt, ...);
void m2sdr_cli_invalid_choice(const char *what, const char *value, const char *expected);
void m2sdr_cli_unknown_option(const char *opt);

#endif /* M2SDR_CLI_H */
