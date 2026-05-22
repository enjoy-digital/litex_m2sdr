/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef M2SDR_CLI_H
#define M2SDR_CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

#include "m2sdr.h"

enum m2sdr_cli_transport {
    M2SDR_CLI_TRANSPORT_DEFAULT = 0,
    M2SDR_CLI_TRANSPORT_LITEPCIE,
    M2SDR_CLI_TRANSPORT_LITEETH,
};

struct m2sdr_cli_device {
    char device_id[128];
    bool use_explicit_device_id;
    enum m2sdr_cli_transport transport;
    char pcie_path[64];
    int device_num;
    char ip_address[1024];
    char port[16];
};

void m2sdr_cli_device_init(struct m2sdr_cli_device *dev);
int m2sdr_cli_handle_device_option(struct m2sdr_cli_device *dev, int opt, const char *optarg);
int m2sdr_cli_set_device_id(struct m2sdr_cli_device *dev, const char *device_id);
bool m2sdr_cli_finalize_device(struct m2sdr_cli_device *dev);
const char *m2sdr_cli_device_id(const struct m2sdr_cli_device *dev);
const char *m2sdr_cli_pcie_path(const struct m2sdr_cli_device *dev);
int m2sdr_cli_parse_int64(const char *text, int64_t *value);
int m2sdr_cli_parse_double(const char *text, double *value);
int m2sdr_cli_parse_u64(const char *text, uint64_t *value);
int m2sdr_cli_parse_u32(const char *text, uint32_t *value);
int m2sdr_cli_parse_u16(const char *text, uint16_t *value);
int m2sdr_cli_parse_u8(const char *text, uint8_t *value);
int m2sdr_cli_parse_bool(const char *text, bool *value);
int m2sdr_cli_parse_uint_range(const char *text, unsigned min, unsigned max, unsigned *value);
int m2sdr_cli_parse_int_range(const char *text, int min, int max, int *value);
int m2sdr_cli_parse_double_range(const char *text, double min, double max, double *value);
int m2sdr_cli_parse_format(const char *text, enum m2sdr_format *format);
const char *m2sdr_cli_format_name(enum m2sdr_format format);
void m2sdr_cli_print_device_help(void);
void m2sdr_cli_error(const char *fmt, ...);
void m2sdr_cli_invalid_choice(const char *what, const char *value, const char *expected);
void m2sdr_cli_unknown_option(const char *opt);

#endif /* M2SDR_CLI_H */
