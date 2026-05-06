/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Board Utility.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <getopt.h>

#include "ad9361/util.h"
#include "ad9361/ad9361.h"

#include "liblitepcie.h"
#include "libm2sdr.h"
#include "m2sdr.h"
#include "m2sdr_cli.h"

#include "m2sdr_config.h"

#if defined(__GNUC__) || defined(__clang__)
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x)   (x)
#define unlikely(x) (x)
#endif

/* Parameters */
/*------------*/

#define DMA_CHECK_DATA   /* Enable Data Check when defined */
#define DMA_RANDOM_DATA  /* Enable Random Data when defined */

#define FLASH_WRITE      /* Enable Flash Write when defined */

/* Variables */
/*-----------*/

static struct m2sdr_cli_device g_cli_dev;

sig_atomic_t keep_running = 1;
static bool g_force_flash_write = false;

void intHandler(int dummy) {
    keep_running = 0;
}

static bool confirm_flash_write(void)
{
    char buf[8];
    fprintf(stderr, "WARNING: flash_write can overwrite the FPGA image.\n");
    fprintf(stderr, "Type 'YES' to continue: ");
    if (!fgets(buf, sizeof(buf), stdin))
        return false;
    return (strncmp(buf, "YES", 3) == 0);
}

/* Connection Functions */
/*----------------------*/

static struct m2sdr_dev *g_dev = NULL;

static void m2sdr_print_open_error(const char *device_id, int rc)
{
    fprintf(stderr, "Could not init driver for %s: %s\n",
        device_id ? device_id : "(default)",
        m2sdr_strerror(rc));
}

static struct m2sdr_dev *m2sdr_open_dev(void) {
    int rc;

    if (g_dev)
        return g_dev;
    if (!m2sdr_cli_finalize_device(&g_cli_dev)) {
        exit(1);
    }
    rc = m2sdr_open(&g_dev, m2sdr_cli_device_id(&g_cli_dev));
    if (rc != 0) {
        m2sdr_print_open_error(m2sdr_cli_device_id(&g_cli_dev), rc);
        exit(1);
    }
    return g_dev;
}

static void m2sdr_close_dev(struct m2sdr_dev *dev) {
    (void)dev;
    if (g_dev) {
        m2sdr_close(g_dev);
        g_dev = NULL;
    }
}

static uint32_t m2sdr_read32(struct m2sdr_dev *dev, uint32_t addr)
{
    uint32_t val = 0;
    if (m2sdr_reg_read(dev, addr, &val) != 0) {
        fprintf(stderr, "CSR read failed @0x%08x\n", addr);
        exit(1);
    }
    return val;
}

static void m2sdr_write32(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    if (m2sdr_reg_write(dev, addr, val) != 0) {
        fprintf(stderr, "CSR write failed @0x%08x\n", addr);
        exit(1);
    }
}

static const char *ptp_yes_no(bool value)
{
    return value ? "Yes" : "No";
}

static const char *ptp_state_name(uint8_t state)
{
    switch (state) {
    case 0: return "manual";
    case 1: return "acquire";
    case 2: return "locked";
    case 3: return "holdover";
    default: return "unknown";
    }
}

static void ptp_format_ipv4(char *buf, size_t len, uint32_t ip)
{
    snprintf(buf, len, "%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32,
        (ip >> 24) & 0xffu,
        (ip >> 16) & 0xffu,
        (ip >>  8) & 0xffu,
        (ip >>  0) & 0xffu);
}

static void ptp_format_port_identity(char *buf, size_t len, const struct m2sdr_ptp_port_identity *port)
{
    if (!port || ((port->clock_id == 0) && (port->port_number == 0))) {
        snprintf(buf, len, "n/a");
        return;
    }

    snprintf(buf, len, "%016" PRIx64 ":%u",
        port->clock_id,
        (unsigned)port->port_number);
}

static int parse_bool_arg(const char *arg, bool *value)
{
    if (!arg || !value)
        return -1;

    if ((strcasecmp(arg, "1") == 0) ||
        (strcasecmp(arg, "true") == 0) ||
        (strcasecmp(arg, "yes") == 0) ||
        (strcasecmp(arg, "on") == 0) ||
        (strcasecmp(arg, "enable") == 0) ||
        (strcasecmp(arg, "enabled") == 0)) {
        *value = true;
        return 0;
    }

    if ((strcasecmp(arg, "0") == 0) ||
        (strcasecmp(arg, "false") == 0) ||
        (strcasecmp(arg, "no") == 0) ||
        (strcasecmp(arg, "off") == 0) ||
        (strcasecmp(arg, "disable") == 0) ||
        (strcasecmp(arg, "disabled") == 0)) {
        *value = false;
        return 0;
    }

    return -1;
}

static int parse_u32_arg(const char *arg, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed = 0;

    if (!arg || !value)
        return -1;

    errno = 0;
    parsed = strtoul(arg, &end, 0);
    if ((errno != 0) || (end == arg) || (*end != '\0') || (parsed > 0xfffffffful))
        return -1;

    *value = (uint32_t)parsed;
    return 0;
}

static int parse_double_arg(const char *arg, double *value)
{
    char *end = NULL;
    double parsed = 0.0;

    if (!arg || !value)
        return -1;

    errno = 0;
    parsed = strtod(arg, &end);
    if ((errno != 0) || (end == arg) || (*end != '\0'))
        return -1;

    *value = parsed;
    return 0;
}

static void sleep_seconds(double seconds)
{
    struct timespec req;
    struct timespec rem;

    if (seconds <= 0.0)
        return;

    req.tv_sec  = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1e9);
    if (req.tv_nsec < 0)
        req.tv_nsec = 0;

    while ((nanosleep(&req, &rem) != 0) && (errno == EINTR) && keep_running)
        req = rem;
}

static void ptp_print_status_summary(const struct m2sdr_ptp_status *status)
{
    char master_ip[32];
    char master_port[48];

    ptp_format_ipv4(master_ip, sizeof(master_ip), status->master_ip);
    ptp_format_port_identity(master_port, sizeof(master_port), &status->master_port);

    printf("  PTP Locked     : %s\n", ptp_yes_no(status->ptp_locked));
    printf("  Time Locked    : %s\n", ptp_yes_no(status->time_locked));
    printf("  Time Owner     : %s\n", status->active ? "PTP" : "Host/Free-run");
    printf("  Holdover       : %s\n", ptp_yes_no(status->holdover));
    printf("  State          : %s\n", ptp_state_name(status->state));
    printf("  Master IP      : %s\n", master_ip);
    printf("  Master Port    : %s\n", master_port);
    printf("  Time Inc       : 0x%08" PRIx32 "\n", status->time_inc);
    printf("  Last Error     : %" PRId64 " ns\n", status->last_error_ns);
    printf("  Lock Losses    : PTP=%" PRIu32 " Time=%" PRIu32 "\n",
        status->ptp_lock_losses,
        status->time_lock_losses);
}

static void ptp_print_status_details(const struct m2sdr_ptp_status *status)
{
    char master_ip[32];
    char local_port[48];
    char master_port[48];

    ptp_format_ipv4(master_ip, sizeof(master_ip), status->master_ip);
    ptp_format_port_identity(local_port, sizeof(local_port), &status->local_port);
    ptp_format_port_identity(master_port, sizeof(master_port), &status->master_port);

    printf("Discipline Enable: %s\n", ptp_yes_no(status->enabled));
    printf("Time Owner       : %s\n", status->active ? "PTP" : "Host/Free-run");
    printf("PTP Locked       : %s\n", ptp_yes_no(status->ptp_locked));
    printf("Time Locked      : %s\n", ptp_yes_no(status->time_locked));
    printf("Holdover         : %s\n", ptp_yes_no(status->holdover));
    printf("State            : %s (%u)\n", ptp_state_name(status->state), status->state);
    printf("Master IP        : %s\n", master_ip);
    printf("Local Port       : %s\n", local_port);
    printf("Master Port      : %s\n", master_port);
    printf("Identity Updates : %" PRIu32 "\n", status->identity_updates);
    printf("Time Inc         : 0x%08" PRIx32 "\n", status->time_inc);
    printf("Last Error       : %" PRId64 " ns\n", status->last_error_ns);
    printf("Last PTP Time    : %" PRIu64 " ns\n", status->last_ptp_time_ns);
    printf("Last Local Time  : %" PRIu64 " ns\n", status->last_local_time_ns);
    printf("Coarse Steps     : %" PRIu32 "\n", status->coarse_steps);
    printf("Phase Steps      : %" PRIu32 "\n", status->phase_steps);
    printf("Rate Updates     : %" PRIu32 "\n", status->rate_updates);
    printf("PTP Lock Losses  : %" PRIu32 "\n", status->ptp_lock_losses);
    printf("Time Lock Losses : %" PRIu32 "\n", status->time_lock_losses);
    printf("Invalid Header   : %" PRIu32 "\n", status->invalid_header_count);
    printf("Wrong Peer       : %" PRIu32 "\n", status->wrong_peer_count);
    printf("Wrong Requester  : %" PRIu32 "\n", status->wrong_requester_count);
    printf("RX Timeout       : %" PRIu32 "\n", status->rx_timeout_count);
    printf("Announce Expiry  : %" PRIu32 "\n", status->announce_expiry_count);
}

static void ptp_print_discipline_config(const struct m2sdr_ptp_discipline_config *cfg)
{
    printf("Enable           : %s\n", ptp_yes_no(cfg->enable));
    printf("Holdover         : %s\n", ptp_yes_no(cfg->holdover));
    printf("Update Cycles    : %" PRIu32 "\n", cfg->update_cycles);
    printf("Coarse Threshold : %" PRIu32 " ns\n", cfg->coarse_threshold_ns);
    printf("Phase Threshold  : %" PRIu32 " ns\n", cfg->phase_threshold_ns);
    printf("Lock Window      : %" PRIu32 " ns\n", cfg->lock_window_ns);
    printf("Phase Step Shift : %u\n", cfg->phase_step_shift);
    printf("Phase Step Max   : %" PRIu32 " ns\n", cfg->phase_step_max_ns);
    printf("Trim Limit       : %" PRIu32 "\n", cfg->trim_limit);
    printf("P Gain           : %u\n", cfg->p_gain);
}

static void ptp_status(bool watch, double watch_interval)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();

    keep_running = 1;
    signal(SIGINT, intHandler);

    do {
        struct m2sdr_ptp_status status;
        char refresh_time[64];
        time_t now;
        struct tm tm;
        int ret;

        ret = m2sdr_get_ptp_status(conn, &status);
        if (ret != 0) {
            fprintf(stderr, "Failed to read PTP status: %s\n", m2sdr_strerror(ret));
            m2sdr_close_dev(conn);
            exit(1);
        }

        now = time(NULL);
        localtime_r(&now, &tm);
        strftime(refresh_time, sizeof(refresh_time), "%Y-%m-%d %H:%M:%S", &tm);

        if (watch && isatty(STDOUT_FILENO))
            printf("\033[H\033[J");

        printf("\e[1m[> PTP Status:\e[0m\n");
        printf("--------------\n");
        printf("Refresh Time     : %s\n", refresh_time);
        ptp_print_status_details(&status);
        printf("\n");
        fflush(stdout);

        if (!watch)
            break;
        sleep_seconds(watch_interval);
    } while (keep_running);

    m2sdr_close_dev(conn);
}

static void ptp_config(const char *field, const char *value)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    struct m2sdr_ptp_discipline_config cfg;
    int ret;

    if (field && ((strcmp(field, "clear-counters") == 0) || (strcmp(field, "clear_counters") == 0))) {
        ret = m2sdr_clear_ptp_counters(conn);
        if (ret != 0) {
            fprintf(stderr, "Failed to clear PTP counters: %s\n", m2sdr_strerror(ret));
            m2sdr_close_dev(conn);
            exit(1);
        }
        printf("PTP counters cleared.\n");
        m2sdr_close_dev(conn);
        return;
    }

    ret = m2sdr_get_ptp_discipline_config(conn, &cfg);
    if (ret != 0) {
        fprintf(stderr, "Failed to read PTP discipline config: %s\n", m2sdr_strerror(ret));
        m2sdr_close_dev(conn);
        exit(1);
    }

    if (!field) {
        printf("\e[1m[> PTP Discipline Config:\e[0m\n");
        printf("------------------------\n");
        ptp_print_discipline_config(&cfg);
        printf("\n");
        m2sdr_close_dev(conn);
        return;
    }

    if (!value) {
        fprintf(stderr, "ptp-config requires a value for '%s'\n", field);
        m2sdr_close_dev(conn);
        exit(1);
    }

    if ((strcmp(field, "enable") == 0)) {
        if (parse_bool_arg(value, &cfg.enable) != 0) {
            fprintf(stderr, "Invalid boolean value '%s' for enable\n", value);
            m2sdr_close_dev(conn);
            exit(1);
        }
    } else if ((strcmp(field, "holdover") == 0)) {
        if (parse_bool_arg(value, &cfg.holdover) != 0) {
            fprintf(stderr, "Invalid boolean value '%s' for holdover\n", value);
            m2sdr_close_dev(conn);
            exit(1);
        }
    } else if ((strcmp(field, "update-cycles") == 0) || (strcmp(field, "update_cycles") == 0)) {
        if (parse_u32_arg(value, &cfg.update_cycles) != 0) {
            fprintf(stderr, "Invalid value '%s' for update-cycles\n", value);
            m2sdr_close_dev(conn);
            exit(1);
        }
    } else if ((strcmp(field, "coarse-threshold") == 0) || (strcmp(field, "coarse_threshold") == 0)) {
        if (parse_u32_arg(value, &cfg.coarse_threshold_ns) != 0) {
            fprintf(stderr, "Invalid value '%s' for coarse-threshold\n", value);
            m2sdr_close_dev(conn);
            exit(1);
        }
    } else if ((strcmp(field, "phase-threshold") == 0) || (strcmp(field, "phase_threshold") == 0)) {
        if (parse_u32_arg(value, &cfg.phase_threshold_ns) != 0) {
            fprintf(stderr, "Invalid value '%s' for phase-threshold\n", value);
            m2sdr_close_dev(conn);
            exit(1);
        }
    } else if ((strcmp(field, "lock-window") == 0) || (strcmp(field, "lock_window") == 0)) {
        if (parse_u32_arg(value, &cfg.lock_window_ns) != 0) {
            fprintf(stderr, "Invalid value '%s' for lock-window\n", value);
            m2sdr_close_dev(conn);
            exit(1);
        }
    } else if ((strcmp(field, "phase-step-shift") == 0) || (strcmp(field, "phase_step_shift") == 0)) {
        uint32_t parsed = 0;
        if ((parse_u32_arg(value, &parsed) != 0) || (parsed > 63u)) {
            fprintf(stderr, "Invalid value '%s' for phase-step-shift\n", value);
            m2sdr_close_dev(conn);
            exit(1);
        }
        cfg.phase_step_shift = (uint8_t)parsed;
    } else if ((strcmp(field, "phase-step-max") == 0) || (strcmp(field, "phase_step_max") == 0)) {
        if (parse_u32_arg(value, &cfg.phase_step_max_ns) != 0) {
            fprintf(stderr, "Invalid value '%s' for phase-step-max\n", value);
            m2sdr_close_dev(conn);
            exit(1);
        }
    } else if ((strcmp(field, "trim-limit") == 0) || (strcmp(field, "trim_limit") == 0)) {
        if (parse_u32_arg(value, &cfg.trim_limit) != 0) {
            fprintf(stderr, "Invalid value '%s' for trim-limit\n", value);
            m2sdr_close_dev(conn);
            exit(1);
        }
    } else if ((strcmp(field, "p-gain") == 0) || (strcmp(field, "p_gain") == 0)) {
        uint32_t parsed = 0;
        if ((parse_u32_arg(value, &parsed) != 0) || (parsed > 0xffffu)) {
            fprintf(stderr, "Invalid value '%s' for p-gain\n", value);
            m2sdr_close_dev(conn);
            exit(1);
        }
        cfg.p_gain = (uint16_t)parsed;
    } else {
        fprintf(stderr, "Unknown ptp-config field '%s'\n", field);
        m2sdr_close_dev(conn);
        exit(1);
    }

    ret = m2sdr_set_ptp_discipline_config(conn, &cfg);
    if (ret != 0) {
        fprintf(stderr, "Failed to update PTP discipline config: %s\n", m2sdr_strerror(ret));
        m2sdr_close_dev(conn);
        exit(1);
    }

    printf("\e[1m[> PTP Discipline Config:\e[0m\n");
    printf("------------------------\n");
    ptp_print_discipline_config(&cfg);
    printf("\n");

    m2sdr_close_dev(conn);
}

/* SI5351 */
/*--------*/

#ifdef CSR_SI5351_BASE

static void test_si5351_init(void)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    void *handle = m2sdr_get_handle(conn);

    printf("\e[1m[> SI5351 Init...\e[0m\n");
    m2sdr_si5351_i2c_config(handle, SI5351_I2C_ADDR, si5351_xo_38p4m_config, sizeof(si5351_xo_38p4m_config)/sizeof(si5351_xo_38p4m_config[0]));
    printf("Done.\n");

    m2sdr_close_dev(conn);
}

static void test_si5351_dump(void)
{
    uint8_t value;
    int i;

    struct m2sdr_dev *conn = m2sdr_open_dev();
    void *handle = m2sdr_get_handle(conn);

    printf("\e[1m[> SI5351 Registers Dump:\e[0m\n");
    printf("--------------------------\n");

    for (i = 0; i < 256; i++) {
        if (m2sdr_si5351_i2c_read(handle, SI5351_I2C_ADDR, i, &value, 1, true)) {
            printf("Reg 0x%02x: 0x%02x\n", i, value);
        } else {
            fprintf(stderr, "Failed to read reg 0x%02x\n", i);
        }
    }

    printf("\n");
    m2sdr_close_dev(conn);
}

static void test_si5351_write(uint8_t reg, uint8_t value)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    void *handle = m2sdr_get_handle(conn);

    if (m2sdr_si5351_i2c_write(handle, SI5351_I2C_ADDR, reg, &value, 1)) {
        printf("Wrote 0x%02x to SI5351 reg 0x%02x\n", value, reg);
    } else {
        fprintf(stderr, "Failed to write to SI5351 reg 0x%02x\n", reg);
    }

    m2sdr_close_dev(conn);
}

static void test_si5351_read(uint8_t reg)
{
    uint8_t value;

    struct m2sdr_dev *conn = m2sdr_open_dev();
    void *handle = m2sdr_get_handle(conn);

    if (m2sdr_si5351_i2c_read(handle, SI5351_I2C_ADDR, reg, &value, 1, true)) {
        printf("SI5351 reg 0x%02x: 0x%02x\n", reg, value);
    } else {
        fprintf(stderr, "Failed to read SI5351 reg 0x%02x\n", reg);
    }

    m2sdr_close_dev(conn);
}

#endif

/* AD9361 Dump */
/*-------------*/

static void test_ad9361_dump(void)
{
    int i;

    struct m2sdr_dev *conn = m2sdr_open_dev();
    void *handle = m2sdr_get_handle(conn);

    /* AD9361 SPI Init */
    m2sdr_ad9361_spi_init(handle, 0);

    /* AD9361 SPI Dump of all the Registers */
    for (i=0; i<1024; i++)
        printf("Reg 0x%03x: 0x%04x\n", i, m2sdr_ad9361_spi_read(handle, i));

    printf("\n");

    m2sdr_close_dev(conn);
}

static void test_ad9361_write(uint16_t reg, uint16_t value)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    void *handle = m2sdr_get_handle(conn);

    /* AD9361 SPI Init */
    m2sdr_ad9361_spi_init(handle, 0);

    m2sdr_ad9361_spi_write(handle, reg, value);
    printf("Wrote 0x%04x to AD9361 reg 0x%03x\n", value, reg);

    m2sdr_close_dev(conn);
}

static void test_ad9361_read(uint16_t reg)
{
    uint16_t value;

    struct m2sdr_dev *conn = m2sdr_open_dev();
    void *handle = m2sdr_get_handle(conn);

    /* AD9361 SPI Init */
    m2sdr_ad9361_spi_init(handle, 0);

    value = m2sdr_ad9361_spi_read(handle, reg);
    printf("AD9361 reg 0x%03x: 0x%04x\n", reg, value);

    m2sdr_close_dev(conn);
}

/* AD9361 Dump Utilities */
/*-----------------------*/

static void print_table_row(const char *c1, const char *c2, const char *c3, const char *c4, const char *c5, const char *c6)
{
    printf("|%-9s|%-6s|%-5s|%-35s|%-8s|%-35s|\n",
           c1 ? c1 : "", c2 ? c2 : "", c3 ? c3 : "", c4 ? c4 : "", c5 ? c5 : "", c6 ? c6 : "");
}

static void print_separator(void)
{
    printf("+---------+------+-----+-----------------------------------+--------+-----------------------------------+\n");
}

/* AD9361 Port Dump */
/*------------------*/
static void test_ad9361_port_dump(void)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    void *handle = m2sdr_get_handle(conn);
    /* AD9361 SPI Init */
    m2sdr_ad9361_spi_init(handle, 0);
    uint8_t reg010 = m2sdr_ad9361_spi_read(handle, 0x010);
    uint8_t reg011 = m2sdr_ad9361_spi_read(handle, 0x011);
    uint8_t reg012 = m2sdr_ad9361_spi_read(handle, 0x012);
    printf("\e[1m[> AD9361 Parallel Port Configuration Dump:\e[0m\n");
    printf("-------------------------------------------\n");

    /* Separator */
    print_separator();

    /* Table Header */
    print_table_row("Register", "Hex", "Bits", "Field Name", "Value", "Decoding");

    /* Separator */
    print_separator();

    /* Register 0x010 */
    {
        char hex010[7];
        snprintf(hex010, sizeof(hex010), "0x%02X", reg010);
        print_table_row("0x010", hex010, "", "", "", "");
        print_table_row("", "", "D7", "PP Tx Swap IQ",       ((reg010 >> 7) & 0x01) ? "1" : "0", ((reg010 >> 7) & 0x01) ? "No Swap"          : "Swap Enabled (Spectral Inversion)");
        print_table_row("", "", "D6", "PP Rx Swap IQ",       ((reg010 >> 6) & 0x01) ? "1" : "0", ((reg010 >> 6) & 0x01) ? "No Swap"          : "Swap Enabled (Spectral Inversion)");
        print_table_row("", "", "D5", "Tx Channel Swap",     ((reg010 >> 5) & 0x01) ? "1" : "0", ((reg010 >> 5) & 0x01) ? "Swap Enabled"     : "No Swap");
        print_table_row("", "", "D4", "Rx Channel Swap",     ((reg010 >> 4) & 0x01) ? "1" : "0", ((reg010 >> 4) & 0x01) ? "Swap Enabled"     : "No Swap");
        print_table_row("", "", "D3", "Rx Frame Pulse Mode", ((reg010 >> 3) & 0x01) ? "1" : "0", ((reg010 >> 3) & 0x01) ? "Pulse (50% duty)" : "Level (stays high)");
        print_table_row("", "", "D2", "2R2T Timing",         ((reg010 >> 2) & 0x01) ? "1" : "0", ((reg010 >> 2) & 0x01) ? "Always 2R2T"      : "Auto (based on paths)");
        print_table_row("", "", "D1", "Invert Data Bus",     ((reg010 >> 1) & 0x01) ? "1" : "0", ((reg010 >> 1) & 0x01) ? "Enabled ([0:11])" : "Disabled ([11:0])");
        print_table_row("", "", "D0", "Invert DATA CLK",     (reg010 & 0x01)        ? "1" : "0", (reg010 & 0x01)        ? "Enabled"          : "Disabled");
    }

    /* Separator */
    print_separator();

    /* Register 0x011 */
    {
        char hex011[7];
        snprintf(hex011, sizeof(hex011), "0x%02X", reg011);
        print_table_row("0x011", hex011, "", "", "", "");
        print_table_row("", "", "D7", "FDD Alt Word Order", ((reg011 >> 7) & 0x01) ? "1" : "0", ((reg011 >> 7) & 0x01) ? "Enabled (6-bit split)" : "Disabled");
        {
            char mustbe_val[6];
            snprintf(mustbe_val, sizeof(mustbe_val), "0x%X", (reg011 >> 5) & 0x03);
            char mustbe_desc[50];
            snprintf(mustbe_desc, sizeof(mustbe_desc), "%s", ((reg011 >> 5) & 0x03) ? "Warning: Should be 0x0" : "Clear");
            print_table_row("", "", "D6:5", "Must be 0", mustbe_val, mustbe_desc);
        }
        print_table_row("", "", "D4", "Invert Tx1",      ((reg011 >> 4) & 0x01) ? "1" : "0", ((reg011 >> 4) & 0x01) ? "Enabled (Multiply by -1)" : "Normal");
        print_table_row("", "", "D3", "Invert Tx2",      ((reg011 >> 3) & 0x01) ? "1" : "0", ((reg011 >> 3) & 0x01) ? "Enabled (Multiply by -1)" : "Normal");
        print_table_row("", "", "D2", "Invert Rx Frame", ((reg011 >> 2) & 0x01) ? "1" : "0", ((reg011 >> 2) & 0x01) ? "Enabled"                  : "Disabled");
        {
            char delay_val[6];
            snprintf(delay_val, sizeof(delay_val), "0x%X", reg011 & 0x03);
            char delay_desc[64];
            snprintf(delay_desc, sizeof(delay_desc), "%u (1/4 clk cycles for DDR)", reg011 & 0x03);
            print_table_row("", "", "D1:0", "Delay Rx Data", delay_val, delay_desc);
        }
    }

    /* Separator */
    print_separator();

    /* Register 0x012 */
    {
        char hex012[7];
        snprintf(hex012, sizeof(hex012), "0x%02X", reg012);
        print_table_row("0x012", hex012, "", "", "", "");
        print_table_row("", "", "D7", "FDD Rx Rate = 2*Tx Rate", ((reg012 >> 7) & 0x01) ? "1" : "0", ((reg012 >> 7) & 0x01) ? "Enabled (Rx 2x Tx)"           : "Disabled (Rx = Tx)");
        print_table_row("", "", "D6", "Swap Ports",              ((reg012 >> 6) & 0x01) ? "1" : "0", ((reg012 >> 6) & 0x01) ? "Enabled (P0 <-> P1)"          : "Disabled");
        print_table_row("", "", "D5", "Single Data Rate",        ((reg012 >> 5) & 0x01) ? "1" : "0", ((reg012 >> 5) & 0x01) ? "SDR (one edge)"               : "DDR (both edges)");
        print_table_row("", "", "D4", "LVDS Mode",               ((reg012 >> 4) & 0x01) ? "1" : "0", ((reg012 >> 4) & 0x01) ? "Enabled (LVDS)"               : "Disabled (CMOS)");
        print_table_row("", "", "D3", "Half-Duplex Mode",        ((reg012 >> 3) & 0x01) ? "1" : "0", ((reg012 >> 3) & 0x01) ? "Enabled (TDD)"                : "Disabled (FDD)");
        print_table_row("", "", "D2", "Single Port Mode",        ((reg012 >> 2) & 0x01) ? "1" : "0", ((reg012 >> 2) & 0x01) ? "Enabled (1 port)"             : "Disabled (2 ports)");
        print_table_row("", "", "D1", "Full Port",               ((reg012 >> 1) & 0x01) ? "1" : "0", ((reg012 >> 1) & 0x01) ? "Enabled (Rx/Tx separated)"    : "Disabled (Mixed)");
        print_table_row("", "", "D0", "Full Duplex Swap Bit",    (reg012 & 0x01)        ? "1" : "0", (reg012 & 0x01)        ? "Enabled (Toggle Rx/Tx bits)"  : "Disabled");
    }

    /* Final Separator */
    print_separator();
    printf("\n");

    m2sdr_close_dev(conn);
}

/* AD9361 ENSM Dump */
/*-----------------*/

static const char* decode_cal_state(uint8_t state)
{
    switch (state) {
        case 0x0: return "Calibrations Done";
        case 0x1: return "Baseband DC Offset Cal";
        case 0x2: return "RF DC Offset Cal";
        case 0x3: return "Tx1 Quadrature Cal";
        case 0x4: return "Tx2 Quadrature Cal";
        case 0x5: return "Receiver Gain Step Cal";
        case 0x9: return "Baseband Cal Flush";
        case 0xA: return "RF Cal Flush";
        case 0xB: return "Tx Quad Cal Flush";
        case 0xC: return "Tx Power Detector Cal Flush";
        case 0xE: return "Rx Gain Step Cal Flush";
        case 0xF: return "Unknown";
        default: return "Reserved";
    }
}

static const char* decode_ensm_state(uint8_t state)
{
    switch (state) {
        case 0x0: return "Sleep (Clocks/BB PLL disabled)";
        case 0x1: return "Wait";
        case 0x5: return "Alert (Synthesizers enabled)";
        case 0x6: return "Tx (Tx signal chain enabled)";
        case 0x7: return "Tx Flush";
        case 0x8: return "Rx (Rx signal chain enabled)";
        case 0x9: return "Rx Flush";
        case 0xA: return "FDD (Tx and Rx enabled)";
        case 0xB: return "FDD Flush";
        default: return "Unknown";
    }
}



static void test_ad9361_ensm_dump(void)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    void *handle = m2sdr_get_handle(conn);
    /* AD9361 SPI Init */
    m2sdr_ad9361_spi_init(handle, 0);
    uint8_t reg013 = m2sdr_ad9361_spi_read(handle, 0x013);
    uint8_t reg014 = m2sdr_ad9361_spi_read(handle, 0x014);
    uint8_t reg015 = m2sdr_ad9361_spi_read(handle, 0x015);
    uint8_t reg016 = m2sdr_ad9361_spi_read(handle, 0x016);
    uint8_t reg017 = m2sdr_ad9361_spi_read(handle, 0x017);
    printf("\e[1m[> AD9361 ENSM Dump:\e[0m\n");
    printf("--------------------\n");
    /* Separator */
    print_separator();
    /* Table Header */
    print_table_row("Register", "Hex", "Bits", "Field Name", "Value", "Decoding");
    /* Separator */
    print_separator();

    /* Register 0x013 */
    {
        char hex013[7];
        snprintf(hex013, sizeof(hex013), "0x%02X", reg013);
        print_table_row("0x013", hex013, "", "", "", "");
        print_table_row("", "", "D7", "Open",     ((reg013 >> 7) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D6", "Reserved", ((reg013 >> 6) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D5", "Reserved", ((reg013 >> 5) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D4", "Reserved", ((reg013 >> 4) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D3", "Reserved", ((reg013 >> 3) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D2", "Reserved", ((reg013 >> 2) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D1", "Reserved", ((reg013 >> 1) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D0", "FDD Mode", ((reg013 >> 0) & 0x01) ? "1" : "0", ((reg013 >> 0) & 0x01) ? "FDD" : "TDD");
    }
    /* Separator */
    print_separator();

    /* Register 0x014 */
    {
        char hex014[7];
        snprintf(hex014, sizeof(hex014), "0x%02X", reg014);
        print_table_row("0x014", hex014, "", "", "", "");
        print_table_row("", "", "D7", "Enable Rx Data Port for Cal", ((reg014 >> 7) & 0x01) ? "1" : "0", ((reg014 >> 7) & 0x01) ? "Enabled"             : "Disabled");
        print_table_row("", "", "D6", "Force Rx On",                 ((reg014 >> 6) & 0x01) ? "1" : "0", ((reg014 >> 6) & 0x01) ? "Force Rx State"      : "Normal");
        print_table_row("", "", "D5", "Force Tx On",                 ((reg014 >> 5) & 0x01) ? "1" : "0", ((reg014 >> 5) & 0x01) ? "Force Tx/FDD State"  : "Normal");
        print_table_row("", "", "D4", "ENSM Pin Control",            ((reg014 >> 4) & 0x01) ? "1" : "0", ((reg014 >> 4) & 0x01) ? "Pin Controlled"      : "SPI Controlled");
        print_table_row("", "", "D3", "Level Mode",                  ((reg014 >> 3) & 0x01) ? "1" : "0", ((reg014 >> 3) & 0x01) ? "Level"               : "Pulse");
        print_table_row("", "", "D2", "Force Alert State",           ((reg014 >> 2) & 0x01) ? "1" : "0", ((reg014 >> 2) & 0x01) ? "Force to Alert/Wait" : "Normal");
        print_table_row("", "", "D1", "Auto Gain Lock",              ((reg014 >> 1) & 0x01) ? "1" : "0", ((reg014 >> 1) & 0x01) ? "Enabled"             : "Disabled");
        print_table_row("", "", "D0", "To Alert",                    ((reg014 >> 0) & 0x01) ? "1" : "0", ((reg014 >> 0) & 0x01) ? "To Alert"            : "To Wait");
    }
    /* Separator */
    print_separator();

    /* Register 0x015 */
    {
        char hex015[7];
        snprintf(hex015, sizeof(hex015), "0x%02X", reg015);
        print_table_row("0x015", hex015, "", "", "", "");
        print_table_row("", "", "D7", "FDD External Control Enable", ((reg015 >> 7) & 0x01) ? "1" : "0", ((reg015 >> 7) & 0x01) ? "Enabled (Independent)" : "Disabled");
        print_table_row("", "", "D6", "Power Down Rx Synth",         ((reg015 >> 6) & 0x01) ? "1" : "0", ((reg015 >> 6) & 0x01) ? "Powered Down"          : "Normal");
        print_table_row("", "", "D5", "Power Down Tx Synth",         ((reg015 >> 5) & 0x01) ? "1" : "0", ((reg015 >> 5) & 0x01) ? "Powered Down"          : "Normal");
        print_table_row("", "", "D4", "TXNRX SPI Control",           ((reg015 >> 4) & 0x01) ? "1" : "0", ((reg015 >> 4) & 0x01) ? "TXNRX/ENRX Control"    : "ENRX/ENTX Control");
        print_table_row("", "", "D3", "Synth Pin Control Mode",      ((reg015 >> 3) & 0x01) ? "1" : "0", ((reg015 >> 3) & 0x01) ? "TXNRX Controls Synth"  : "Bit D4 Controls");
        print_table_row("", "", "D2", "Dual Synth Mode",             ((reg015 >> 2) & 0x01) ? "1" : "0", ((reg015 >> 2) & 0x01) ? "Both Synths Always On" : "Single Synth");
        print_table_row("", "", "D1", "Rx Synth Ready Mask",         ((reg015 >> 1) & 0x01) ? "1" : "0", ((reg015 >> 1) & 0x01) ? "Ignore VCO Cal"        : "Wait for Lock");
        print_table_row("", "", "D0", "Tx Synth Ready Mask",         ((reg015 >> 0) & 0x01) ? "1" : "0", ((reg015 >> 0) & 0x01) ? "Ignore VCO Cal"        : "Wait for Lock");
    }
    /* Separator */
    print_separator();

    /* Register 0x016 */
    {
        char hex016[7];
        snprintf(hex016, sizeof(hex016), "0x%02X", reg016);
        print_table_row("0x016", hex016, "", "", "", "");
        print_table_row("", "", "D7", "Rx BB Tune",       ((reg016 >> 7) & 0x01) ? "1" : "0", ((reg016 >> 7) & 0x01) ? "Start Rx BB Filter Cal" : "Idle");
        print_table_row("", "", "D6", "Tx BB Tune",       ((reg016 >> 6) & 0x01) ? "1" : "0", ((reg016 >> 6) & 0x01) ? "Start Tx BB Filter Cal" : "Idle");
        print_table_row("", "", "D5", "Must be 0",        ((reg016 >> 5) & 0x01) ? "1" : "0", ((reg016 >> 5) & 0x01) ? "Warning: Should be 0"   : "Clear");
        print_table_row("", "", "D4", "Tx Quad Cal",      ((reg016 >> 4) & 0x01) ? "1" : "0", ((reg016 >> 4) & 0x01) ? "Start Tx Quad Cal"      : "Idle");
        print_table_row("", "", "D3", "Rx Gain Step Cal", ((reg016 >> 3) & 0x01) ? "1" : "0", ((reg016 >> 3) & 0x01) ? "Start Rx Gain Step Cal" : "Idle");
        print_table_row("", "", "D2", "Must be 0",        ((reg016 >> 2) & 0x01) ? "1" : "0", ((reg016 >> 2) & 0x01) ? "Warning: Should be 0"   : "Clear");
        print_table_row("", "", "D1", "DC Cal RF Start",  ((reg016 >> 1) & 0x01) ? "1" : "0", ((reg016 >> 1) & 0x01) ? "Start RF DC Cal"        : "Idle");
        print_table_row("", "", "D0", "DC Cal BB Start",  ((reg016 >> 0) & 0x01) ? "1" : "0", ((reg016 >> 0) & 0x01) ? "Start BB DC Cal"        : "Idle");
    }
    /* Separator */
    print_separator();

    /* Register 0x017 */
    {
        char hex017[7];
        snprintf(hex017, sizeof(hex017), "0x%02X", reg017);
        print_table_row("0x017", hex017, "", "", "", "");
        char cal_val[6];
        snprintf(cal_val, sizeof(cal_val), "0x%X", (reg017 >> 4) & 0x0F);
        print_table_row("", "", "D7:4", "Cal Sequence State", cal_val, decode_cal_state((reg017 >> 4) & 0x0F));
        char ensm_val[6];
        snprintf(ensm_val, sizeof(ensm_val), "0x%X", reg017 & 0x0F);
        print_table_row("", "", "D3:0", "ENSM State", ensm_val, decode_ensm_state(reg017 & 0x0F));
    }

    /* Final Separator */
    print_separator();
    printf("\n");

    m2sdr_close_dev(conn);
}

/* Info */
/*------*/

static uint32_t icap_read(void *conn, uint32_t reg)
{
    m2sdr_write32(conn, CSR_ICAP_ADDR_ADDR, reg);
    m2sdr_write32(conn, CSR_ICAP_READ_ADDR, 1);
    while (m2sdr_read32(conn, CSR_ICAP_DONE_ADDR) == 0)
        usleep(1000);
    m2sdr_write32(conn, CSR_ICAP_READ_ADDR, 0);
    return m2sdr_read32(conn, CSR_ICAP_DATA_ADDR);
}

static void info(void)
{
    int i;
    unsigned char soc_identifier[256];

    struct m2sdr_dev *conn = m2sdr_open_dev();
    void *handle = m2sdr_get_handle(conn);

    printf("\e[1m[> SoC Info:\e[0m\n");
    printf("------------\n");

    for (i = 0; i < 256; i ++)
        soc_identifier[i] = m2sdr_read32(conn, CSR_IDENTIFIER_MEM_BASE + 4 * i);
    printf("SoC Identifier   : %s.\n", soc_identifier);

#ifdef CSR_CAPABILITY_BASE
    struct m2sdr_capabilities caps;
    if (m2sdr_get_capabilities(conn, &caps) != 0) {
        fprintf(stderr, "Failed to read capabilities\n");
        m2sdr_close_dev(conn);
        exit(1);
    }
    int major = caps.api_version >> 16;
    int minor = caps.api_version & 0xffff;
    printf("API Version      : %d.%d\n", major, minor);

    bool pcie_enabled = (caps.features >> CSR_CAPABILITY_FEATURES_PCIE_OFFSET) & ((1 << CSR_CAPABILITY_FEATURES_PCIE_SIZE) - 1);
    bool eth_enabled  = (caps.features >> CSR_CAPABILITY_FEATURES_ETH_OFFSET)  & ((1 << CSR_CAPABILITY_FEATURES_ETH_SIZE)  - 1);
    bool sata_enabled = (caps.features >> CSR_CAPABILITY_FEATURES_SATA_OFFSET) & ((1 << CSR_CAPABILITY_FEATURES_SATA_SIZE) - 1);
    bool gpio_enabled = (caps.features >> CSR_CAPABILITY_FEATURES_GPIO_OFFSET) & ((1 << CSR_CAPABILITY_FEATURES_GPIO_SIZE) - 1);
    bool wr_enabled   = (caps.features >> CSR_CAPABILITY_FEATURES_WR_OFFSET)   & ((1 << CSR_CAPABILITY_FEATURES_WR_SIZE)   - 1);
    bool jtagbone_enabled = (caps.features >> CSR_CAPABILITY_FEATURES_JTAGBONE_OFFSET) & ((1 << CSR_CAPABILITY_FEATURES_JTAGBONE_SIZE) - 1);
#ifdef CSR_CAPABILITY_FEATURES_ETH_PTP_OFFSET
    bool eth_ptp_enabled = (caps.features >> CSR_CAPABILITY_FEATURES_ETH_PTP_OFFSET) & ((1 << CSR_CAPABILITY_FEATURES_ETH_PTP_SIZE) - 1);
#else
    bool eth_ptp_enabled = false;
#endif

    {
        int variant = (caps.board_info >> CSR_CAPABILITY_BOARD_INFO_VARIANT_OFFSET) & ((1 << CSR_CAPABILITY_BOARD_INFO_VARIANT_SIZE) - 1);
        const char *variant_str[] = {"M.2", "Baseboard", "Reserved", "Reserved"};
        const char *variant_name  = (variant < 4) ? variant_str[variant] : "Unknown";
        printf("Board:\n");
        printf("  Variant        : %s\n", variant_name);
    }

    printf("Features:\n");
    printf("  PCIe           : %s\n", pcie_enabled ? "Yes" : "No");
    printf("  Ethernet       : %s\n", eth_enabled  ? "Yes" : "No");
    printf("  SATA           : %s\n", sata_enabled ? "Yes" : "No");
    printf("  GPIO           : %s\n", gpio_enabled ? "Yes" : "No");
    printf("  White Rabbit   : %s\n", wr_enabled   ? "Yes" : "No");
    printf("  JTAGBone       : %s\n", jtagbone_enabled ? "Yes" : "No");
    printf("  Ethernet PTP   : %s\n", eth_ptp_enabled ? "Yes" : "No");

    if (pcie_enabled) {
        int pcie_speed = (caps.pcie_config >> CSR_CAPABILITY_PCIE_CONFIG_SPEED_OFFSET) & ((1 << CSR_CAPABILITY_PCIE_CONFIG_SPEED_SIZE) - 1);
        int pcie_lanes = (caps.pcie_config >> CSR_CAPABILITY_PCIE_CONFIG_LANES_OFFSET) & ((1 << CSR_CAPABILITY_PCIE_CONFIG_LANES_SIZE) - 1);
        bool pcie_ptm  = (caps.pcie_config >> CSR_CAPABILITY_PCIE_CONFIG_PTM_OFFSET)   & ((1 << CSR_CAPABILITY_PCIE_CONFIG_PTM_SIZE) - 1);
        const char *pcie_speed_str[] = {"Gen1", "Gen2"};
        const char *pcie_lanes_str[] = {"x1", "x2", "x4"};
        printf("  PCIe Speed     : %s\n", pcie_speed_str[pcie_speed]);
        printf("  PCIe Lanes     : %s\n", pcie_lanes_str[pcie_lanes]);
        printf("  PCIe PTM       : %s\n", pcie_ptm ? "Enabled" : "Disabled");
    }

    if (eth_enabled) {
        int eth_speed = (caps.eth_config >> CSR_CAPABILITY_ETH_CONFIG_SPEED_OFFSET) & ((1 << CSR_CAPABILITY_ETH_CONFIG_SPEED_SIZE) - 1);
        const char *eth_speed_str[] = {"1Gbps", "2.5Gbps"};
        printf("  Ethernet Speed : %s\n", eth_speed_str[eth_speed]);
        {
            int eth_sfp  = (caps.board_info >> CSR_CAPABILITY_BOARD_INFO_ETH_SFP_OFFSET) & ((1 << CSR_CAPABILITY_BOARD_INFO_ETH_SFP_SIZE) - 1);
            printf("  Ethernet SFP   : %d\n", eth_sfp);
        }
        if (eth_ptp_enabled) {
            struct m2sdr_ptp_status ptp_status;
            if (m2sdr_get_ptp_status(conn, &ptp_status) == 0)
                ptp_print_status_summary(&ptp_status);
        }
    }

    if (sata_enabled) {
        int sata_gen = (caps.sata_config >> CSR_CAPABILITY_SATA_CONFIG_GEN_OFFSET) & ((1 << CSR_CAPABILITY_SATA_CONFIG_GEN_SIZE) - 1);
        const char *sata_gen_str[] = {"Gen1", "Gen2", "Gen3"};
        printf("  SATA Gen       : %s\n", sata_gen_str[sata_gen]);
        int sata_mode = (caps.sata_config >> CSR_CAPABILITY_SATA_CONFIG_MODE_OFFSET) & ((1 << CSR_CAPABILITY_SATA_CONFIG_MODE_SIZE) - 1);
        const char *sata_mode_str[] = {"Read-only", "Write-only", "Read+Write", "Reserved"};
        const char *sata_mode_name = (sata_mode < 4) ? sata_mode_str[sata_mode] : "Unknown";
        printf("  SATA Mode      : %s\n", sata_mode_name);
    }

    if (wr_enabled) {
        int wr_sfp   = (caps.board_info >> CSR_CAPABILITY_BOARD_INFO_WR_SFP_OFFSET)  & ((1 << CSR_CAPABILITY_BOARD_INFO_WR_SFP_SIZE)  - 1);
        printf("  WR SFP         : %d\n", wr_sfp);
    }
#endif
    printf("\n");

    printf("\e[1m[> FPGA Info:\e[0m\n");
    printf("-------------\n");

#ifdef CSR_DNA_BASE
    printf("FPGA DNA         : 0x%08x%08x\n",
        m2sdr_read32(conn, CSR_DNA_ID_ADDR + 4 * 0),
        m2sdr_read32(conn, CSR_DNA_ID_ADDR + 4 * 1)
    );
#endif
#ifdef CSR_XADC_BASE
    printf("FPGA Temperature : %0.1f °C\n",
           (double)m2sdr_read32(conn, CSR_XADC_TEMPERATURE_ADDR) * 503.975/4096 - 273.15);
    printf("FPGA VCC-INT     : %0.2f V\n",
           (double)m2sdr_read32(conn, CSR_XADC_VCCINT_ADDR) / 4096 * 3);
    printf("FPGA VCC-AUX     : %0.2f V\n",
           (double)m2sdr_read32(conn, CSR_XADC_VCCAUX_ADDR) / 4096 * 3);
    printf("FPGA VCC-BRAM    : %0.2f V\n",
           (double)m2sdr_read32(conn, CSR_XADC_VCCBRAM_ADDR) / 4096 * 3);
#endif
#ifdef CSR_ICAP_BASE
    uint32_t status;
    status = icap_read(conn, ICAP_BOOTSTS_REG);
    printf("FPGA Status      : %s\n", (status & ICAP_BOOTSTS_FALLBACK)? "Fallback" : "Operational");
#endif
    printf("\n");

#ifdef CSR_SI5351_BASE
    printf("\e[1m[> SI5351 Info:\e[0m\n");
    printf("---------------\n");
    if (m2sdr_si5351_i2c_check_litei2c(handle)) {
        bool si5351_present = m2sdr_si5351_i2c_poll(handle, SI5351_I2C_ADDR);
        printf("SI5351 Presence  : %s\n", si5351_present ? "Yes" : "No");
        if (si5351_present) {
            uint8_t status;
            uint32_t control = m2sdr_read32(conn, CSR_SI5351_CONTROL_ADDR);
            bool si5351c = ((control >> CSR_SI5351_CONTROL_VERSION_OFFSET) & ((1u << CSR_SI5351_CONTROL_VERSION_SIZE) - 1u)) != 0;
            bool clkin_ufl = ((control >> CSR_SI5351_CONTROL_CLKIN_SRC_OFFSET) & ((1u << CSR_SI5351_CONTROL_CLKIN_SRC_SIZE) - 1u)) != 0;
            const char *configured_mode = si5351c ? (clkin_ufl ? "SI5351C / 10MHz from uFL" : "SI5351C / 10MHz from FPGA") : "SI5351B / internal XO";

            printf("Configured Mode  : %s\n", configured_mode);

            m2sdr_si5351_i2c_read(handle, SI5351_I2C_ADDR, 0x00, &status, 1, true);
            printf("Device Status    : 0x%02x\n", status);
            printf("  SYS_INIT       : %s\n", (status & 0x80) ? "Initializing"   : "Ready");
            printf("  LOL_B          : %s\n", (status & 0x40) ? "Unlocked"       : "Locked");
            printf("  LOL_A          : %s\n", (status & 0x20) ? "Unlocked"       : "Locked");
            printf("  LOS            : %s\n", (status & 0x10) ? "Loss of Signal" : "Valid Signal");
            printf("  REVID          : 0x%01x\n", status & 0x03);

            uint8_t rev;
            m2sdr_si5351_i2c_read(handle, SI5351_I2C_ADDR, 0x0F, &rev, 1, true);
            printf("PLL Input Source : 0x%02x\n", rev);
            printf("  PLLB_SRC       : %s\n", (rev & 0x08) ? "CLKIN" : "XTAL");
            printf("  PLLA_SRC       : %s\n", (rev & 0x04) ? "CLKIN" : "XTAL");
        }
    } else {
        printf("Old gateware detected: SI5351 Software I2C access is not supported. Please update gateware.\n");
    }
    printf("\n");
#endif

    printf("\e[1m[> AD9361 Info:\e[0m\n");
    printf("---------------\n");
    m2sdr_ad9361_spi_init(handle, 0);
    uint16_t product_id = m2sdr_ad9361_spi_read(handle, REG_PRODUCT_ID);
    bool ad9361_present = (product_id == 0xa);
    printf("AD9361 Presence    : %s\n", ad9361_present ? "Yes" : "No");
    if (ad9361_present) {
        printf("AD9361 Product ID  : %04x \n", product_id);
        printf("AD9361 Temperature : %0.1f °C\n",
            (double)DIV_ROUND_CLOSEST(m2sdr_ad9361_spi_read(handle, REG_TEMPERATURE) * 1000000, 1140)/1000);
    }

    printf("\n\e[1m[> Board Time:\e[0m\n");
    printf("--------------\n");
    uint32_t ctrl = m2sdr_read32(conn, CSR_TIME_GEN_CONTROL_ADDR);
    m2sdr_write32(conn, CSR_TIME_GEN_CONTROL_ADDR, ctrl | 0x2);
    m2sdr_write32(conn, CSR_TIME_GEN_CONTROL_ADDR, ctrl & ~0x2);
    uint64_t ts    = (
        ((uint64_t) m2sdr_read32(conn, CSR_TIME_GEN_READ_TIME_ADDR + 0)) << 32 |
        ((uint64_t) m2sdr_read32(conn, CSR_TIME_GEN_READ_TIME_ADDR + 4)) <<  0
    );
    time_t seconds = ts / 1000000000ULL;
    uint32_t ms    = (ts % 1000000000ULL) / 1000000;
    struct tm tm;
    localtime_r(&seconds, &tm);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
    printf("Board Time : %s.%03u\n", time_str, ms);

    m2sdr_close_dev(conn);
}


/* FPGA Reg Access */
/*-----------------*/

static void test_reg_write(uint32_t offset, uint32_t value)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();

    m2sdr_write32(conn, offset, value);
    printf("Wrote 0x%08x to reg 0x%08x\n", value, offset);

    m2sdr_close_dev(conn);
}

static void test_reg_read(uint32_t offset)
{
    uint32_t value;

    struct m2sdr_dev *conn = m2sdr_open_dev();

    value = m2sdr_read32(conn, offset);
    printf("Reg 0x%08x: 0x%08x\n", offset, value);

    m2sdr_close_dev(conn);
}

/* Scratch */
/*---------*/

void scratch_test(void)
{
    printf("\e[1m[> Scratch register test:\e[0m\n");
    printf("-------------------------\n");

    struct m2sdr_dev *conn = m2sdr_open_dev();

    /* Write to scratch register. */
    printf("Write 0x12345678 to Scratch register:\n");
    m2sdr_write32(conn, CSR_CTRL_SCRATCH_ADDR, 0x12345678);
    printf("Read: 0x%08x\n", m2sdr_read32(conn, CSR_CTRL_SCRATCH_ADDR));

    /* Read from scratch register. */
    printf("Write 0xdeadbeef to Scratch register:\n");
    m2sdr_write32(conn, CSR_CTRL_SCRATCH_ADDR, 0xdeadbeef);
    printf("Read: 0x%08x\n", m2sdr_read32(conn, CSR_CTRL_SCRATCH_ADDR));

    m2sdr_close_dev(conn);
}

/* SPI Flash */
/*-----------*/

#ifdef CSR_FLASH_BASE

#ifdef FLASH_WRITE

static void flash_progress(void *opaque, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    fflush(stdout);
    va_end(ap);
}

static void flash_program(uint32_t base, const uint8_t *buf1, int size1)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    void *handle = m2sdr_get_handle(conn);
    uint32_t size;
    uint8_t *buf;
    int sector_size;
    int errors;

    /* Get flash sector size and pad size to it. */
    sector_size = m2sdr_flash_get_erase_block_size(handle);
    size = ((size1 + sector_size - 1) / sector_size) * sector_size;

    /* Alloc buffer and copy data to it. */
    buf = calloc(1, size);
    if (!buf) {
        fprintf(stderr, "%d: alloc failed\n", __LINE__);
        exit(1);
    }
    memcpy(buf, buf1, size1);

    /* Program flash. */
    printf("Programming (%d bytes at 0x%08x)...\n", size, base);
    errors = m2sdr_flash_write(handle, buf, base, size, flash_progress, NULL);
    if (errors) {
        printf("Failed %d errors.\n", errors);
        exit(1);
    } else {
        printf("Success.\n");
    }

    /* Free buffer and close connection. */
    free(buf);
    m2sdr_close_dev(conn);
}

static void flash_write(const char *filename, uint32_t offset)
{
    uint8_t *data;
    int size;
    FILE * f;

    /* Open data source file. */
    f = fopen(filename, "rb");
    if (!f) {
        perror(filename);
        exit(1);
    }

    /* Get size, alloc buffer and copy data to it. */
    fseek(f, 0L, SEEK_END);
    size = ftell(f);
    fseek(f, 0L, SEEK_SET);
    data = malloc(size);
    if (!data) {
        fprintf(stderr, "%d: malloc failed\n", __LINE__);
        exit(1);
    }
    ssize_t ret = fread(data, size, 1, f);
    fclose(f);

    /* Program file to flash */
    if (ret != 1)
        perror(filename);
    else
        flash_program(offset, data, size);

    /* Free buffer */
    free(data);
}

#endif /* FLASH_WRITE */

static void flash_read(const char *filename, uint32_t size, uint32_t offset)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    void *handle = m2sdr_get_handle(conn);
    FILE * f;
    uint32_t base;
    uint32_t sector_size;
    uint8_t byte;
    int i;

    /* Open data destination file. */
    f = fopen(filename, "wb");
    if (!f) {
        perror(filename);
        exit(1);
    }

    /* Get flash sector size. */
    sector_size = m2sdr_flash_get_erase_block_size(handle);

    /* Read flash and write to destination file. */
    base = offset;
    for (i = 0; i < size; i++) {
        if ((i % sector_size) == 0) {
            printf("Reading 0x%08x\r", base + i);
            fflush(stdout);
        }
        byte = m2sdr_flash_read(handle, base + i);
        fwrite(&byte, 1, 1, f);
    }

    /* Close destination file and connection. */
    fclose(f);
    m2sdr_close_dev(conn);
}

static void flash_reload(void)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();

    /* Reload FPGA through ICAP.*/
    m2sdr_write32(conn, CSR_ICAP_ADDR_ADDR, ICAP_CMD_REG);
    m2sdr_write32(conn, CSR_ICAP_DATA_ADDR, ICAP_CMD_IPROG);
    m2sdr_write32(conn, CSR_ICAP_WRITE_ADDR, 1);

    /* Notice user to reboot/rescan the hardware.*/
    printf("===========================================================================\n");
    printf("= PLEASE REBOOT YOUR HARDWARE OR RESCAN PCIe BUS TO USE NEW FPGA GATEWARE =\n");
    printf("===========================================================================\n");

    m2sdr_close_dev(conn);
}

#endif /* CSR_FLASH_BASE */

/* DMA */
/*-----*/

#ifdef USE_LITEPCIE

#if (DMA_BUFFER_SIZE & (DMA_BUFFER_SIZE - 1)) == 0
static inline int64_t add_mod_int(int64_t a, int64_t b, int64_t m)
{
    /* Optimized for power of 2 */
    int64_t result;
    result = a + b;
    return result & (m - 1);
}
#else
static inline int64_t add_mod_int(int64_t a, int64_t b, int64_t m)
{
    /* Generic */
    a += b;
    if (a >= m)
        a -= m;
    return a;
}
#endif

static inline int get_next_pow2(int data_width)
{
    int x = 1;
    while (x < data_width)
        x <<= 1;
    return x;
}

#ifdef DMA_CHECK_DATA
typedef uint32_t u32x4 __attribute__((vector_size(16)));

static inline uint32_t seed_to_data(uint32_t seed)
{
#ifdef DMA_RANDOM_DATA
    /* Return pseudo random data from seed. */
    return seed * 69069 + 1;
#else
    /* Return seed. */
    return seed;
#endif
}

static inline uint32_t get_data_mask(int data_width)
{
    int i;
    uint32_t mask;
    mask = 0;
    for (i = 0; i < 32/get_next_pow2(data_width); i++) {
        mask <<= get_next_pow2(data_width);
        mask |= ((uint64_t) 1 << data_width) - 1;
    }
    return mask;
}

static inline void write_pn_data(uint32_t * restrict buf, int count, uint32_t *pseed, uint32_t mask, int dma_word_count)
{
    int i;
    uint32_t seed;

    seed = *pseed;
    for (i = 0; i + 4 <= count; i += 4) {
        uint32_t d0 = seed_to_data(seed) & mask; seed = add_mod_int(seed, 1, dma_word_count);
        uint32_t d1 = seed_to_data(seed) & mask; seed = add_mod_int(seed, 1, dma_word_count);
        uint32_t d2 = seed_to_data(seed) & mask; seed = add_mod_int(seed, 1, dma_word_count);
        uint32_t d3 = seed_to_data(seed) & mask; seed = add_mod_int(seed, 1, dma_word_count);
        u32x4 vec = {d0, d1, d2, d3};
        memcpy(&buf[i], &vec, sizeof(vec));
    }

    for (; i < count; i++) {
        buf[i] = (seed_to_data(seed) & mask);
        seed = add_mod_int(seed, 1, dma_word_count);
    }
    *pseed = seed;
}

static inline int check_pn_data(const uint32_t * restrict buf, int count, uint32_t *pseed, uint32_t mask, int dma_word_count)
{
    int i, errors;
    uint32_t seed;
    u32x4 mask_vec = {mask, mask, mask, mask};

    errors = 0;
    seed = *pseed;
    for (i = 0; i + 4 <= count; i += 4) {
        uint32_t e0 = seed_to_data(seed) & mask; seed = add_mod_int(seed, 1, dma_word_count);
        uint32_t e1 = seed_to_data(seed) & mask; seed = add_mod_int(seed, 1, dma_word_count);
        uint32_t e2 = seed_to_data(seed) & mask; seed = add_mod_int(seed, 1, dma_word_count);
        uint32_t e3 = seed_to_data(seed) & mask; seed = add_mod_int(seed, 1, dma_word_count);
        u32x4 expected = {e0, e1, e2, e3};
        u32x4 actual;
        u32x4 diff;

        memcpy(&actual, &buf[i], sizeof(actual));
        diff = (actual & mask_vec) ^ expected;
        errors += (diff[0] != 0) + (diff[1] != 0) + (diff[2] != 0) + (diff[3] != 0);
    }

    for (; i < count; i++) {
        if (unlikely((buf[i] & mask) != (seed_to_data(seed) & mask))) {
            errors ++;
        }
        seed = add_mod_int(seed, 1, dma_word_count);
    }
    *pseed = seed;
    return errors;
}

static void find_best_rx_delay(const uint32_t * restrict buf, uint32_t mask, int dma_word_count, uint32_t *best_delay, uint32_t *best_errors)
{
    uint32_t seed_rd;
    uint32_t errors;
    int count = dma_word_count;
    const int coarse_stride = 8;
    const int refine_radius = 8;
    int refine_start;
    int refine_stop;

    *best_delay  = 0;
    *best_errors = UINT32_MAX;

    /* Coarse sweep to quickly localize a good candidate. */
    for (int delay = 0; delay < count; delay += coarse_stride) {
        seed_rd = delay;
        errors = check_pn_data(buf, count, &seed_rd, mask, dma_word_count);
        if (errors < *best_errors) {
            *best_errors = errors;
            *best_delay  = delay;
            if (errors == 0)
                return;
        }
    }

    /* Local refinement around the best coarse candidate. */
    refine_start = (int)*best_delay - refine_radius;
    refine_stop  = (int)*best_delay + refine_radius;
    if (refine_start < 0)
        refine_start = 0;
    if (refine_stop >= count)
        refine_stop = count - 1;

    for (int delay = refine_start; delay <= refine_stop; delay++) {
        seed_rd = delay;
        errors = check_pn_data(buf, count, &seed_rd, mask, dma_word_count);
        if (errors < *best_errors) {
            *best_errors = errors;
            *best_delay  = delay;
            if (errors == 0)
                return;
        }
    }
}
#endif

static int dma_test(uint8_t zero_copy, uint8_t external_loopback, int data_width, int auto_rx_delay, int duration, int warmup_buffers)
{
    static struct litepcie_dma_ctrl dma = {.use_reader = 1, .use_writer = 1};
    dma.loopback = external_loopback ? 0 : 1;
    keep_running = 1;

    if (!m2sdr_cli_finalize_device(&g_cli_dev))
        exit(1);

    if (unlikely(data_width > 32 || data_width < 1)) {
        fprintf(stderr, "Invalid data width %d\n", data_width);
        exit(1);
    }

    /* Statistics */
    int i = 0;
    int64_t reader_sw_count_last = 0;
    int64_t last_time;
    uint32_t errors = 0;
    uint64_t total_data_errors = 0;
    int64_t end_time = (duration > 0) ? get_time_ms() + duration * 1000 : 0;
    int status = 0;

#ifdef DMA_CHECK_DATA
    uint32_t seed_wr = 0;
    uint32_t seed_rd = 0;
    const int dma_word_count = DMA_BUFFER_SIZE / sizeof(uint32_t);
    const uint32_t data_mask = get_data_mask(data_width);
    uint64_t validated_buffers = 0;
    uint8_t  run = (auto_rx_delay == 0);
    const uint32_t rx_delay_errors_threshold = dma_word_count / 8;
    const int rx_delay_confirmations_needed = 3;
    const int rx_delay_max_attempts = 128;
    uint32_t rx_delay_candidate = UINT32_MAX;
    int rx_delay_candidate_confirmations = 0;
    int rx_delay_attempts = 0;
    uint32_t rx_delay_best_overall = UINT32_MAX;
    uint32_t rx_delay_best_overall_delay = 0;
#else
    uint8_t run = 1;
#endif

    signal(SIGINT, intHandler);

    printf("\e[1m[> DMA loopback test:\e[0m\n");
    printf("---------------------\n");

    if (unlikely(litepcie_dma_init(&dma, m2sdr_cli_pcie_path(&g_cli_dev), zero_copy)))
        exit(1);

    dma.reader_enable = 1;
    dma.writer_enable = 1;

    /* Test loop. */
    last_time = get_time_ms();
    for (;;) {
        int work_done = 0;

        /* Exit loop on CTRL+C or when the duration is over. */
        if (!keep_running || (duration > 0 && get_time_ms() >= end_time))
            break;

        /* Update DMA status. */
        litepcie_dma_process(&dma);

#ifdef DMA_CHECK_DATA
        char *buf_wr;
        char *buf_rd;

        /* DMA-TX Write. */
        while (1) {
            /* Get Write buffer. */
            buf_wr = litepcie_dma_next_write_buffer(&dma);
            /* Break when no buffer available for Write. */
            if (!buf_wr)
                break;
            work_done = 1;
            /* Write data to buffer. */
            write_pn_data((uint32_t *) buf_wr, dma_word_count, &seed_wr, data_mask, dma_word_count);
        }

        /* DMA-RX Read/Check */
        while (1) {
            /* Get Read buffer. */
            buf_rd = litepcie_dma_next_read_buffer(&dma);
            /* Break when no buffer available for Read. */
            if (!buf_rd)
                break;
            work_done = 1;
            /* Skip the first 128 DMA loops. */
            if (dma.writer_hw_count < warmup_buffers)
                continue;
            /* When running... */
            if (run) {
                /* Check data in Read buffer. */
                errors += check_pn_data((uint32_t *) buf_rd, dma_word_count, &seed_rd, data_mask, dma_word_count);
                validated_buffers++;
            } else {
                /* Find and confirm initial RX delay/seed over multiple buffers. */
                uint32_t best_delay;
                uint32_t best_errors;

                find_best_rx_delay((const uint32_t *)buf_rd, data_mask, dma_word_count, &best_delay, &best_errors);
                rx_delay_attempts++;

                if (best_errors < rx_delay_best_overall) {
                    rx_delay_best_overall = best_errors;
                    rx_delay_best_overall_delay = best_delay;
                }

                if (best_errors <= rx_delay_errors_threshold) {
                    if (best_delay == rx_delay_candidate) {
                        rx_delay_candidate_confirmations++;
                    } else {
                        rx_delay_candidate = best_delay;
                        rx_delay_candidate_confirmations = 1;
                    }

                    if (rx_delay_candidate_confirmations >= rx_delay_confirmations_needed) {
                        seed_rd = best_delay;
                        run = 1;
                        errors = 0;
                        printf("RX_DELAY: %d (errors: %d, confirmations: %d)\n",
                            best_delay,
                            best_errors,
                            rx_delay_candidate_confirmations);
                    }
                } else {
                    rx_delay_candidate = UINT32_MAX;
                    rx_delay_candidate_confirmations = 0;
                }

                if (unlikely(!run && (rx_delay_attempts >= rx_delay_max_attempts))) {
                    printf("Unable to find DMA RX_DELAY (best: delay=%d, errors=%d/%d, attempts=%d), exiting.\n",
                        rx_delay_best_overall_delay,
                        rx_delay_best_overall,
                        dma_word_count,
                        rx_delay_attempts);
                    status = 1;
                    goto end;
                }
            }

        }
#endif

        /* Statistics every 200ms. */
        int64_t duration_ms = get_time_ms() - last_time;
        if (run && (duration_ms > 200)) {
            /* Print banner every 10 lines. */
            if (i % 10 == 0)
                printf("\e[1mDMA_SPEED(Gbps)\tTX_BUFFERS\tRX_BUFFERS\tDIFF\tERRORS\e[0m\n");
            i++;
            /* Print statistics. */
            printf("%14.2f\t%10" PRIu64 "\t%10" PRIu64 "\t%4" PRIu64 "\t%6u\n",
                   (double)(dma.reader_sw_count - reader_sw_count_last) * DMA_BUFFER_SIZE * 8 * data_width / (get_next_pow2(data_width) * (double)duration_ms * 1e6),
                   dma.reader_sw_count,
                   dma.writer_sw_count,
                   (uint64_t) llabs(dma.reader_sw_count - dma.writer_sw_count),
                   errors);
            /* Update errors/time/count. */
            total_data_errors += errors;
            errors = 0;
            last_time = get_time_ms();
            reader_sw_count_last = dma.reader_sw_count;
        }

        if (!work_done)
            usleep(100);
    }

#ifdef DMA_CHECK_DATA
    total_data_errors += errors;

    if (total_data_errors != 0 && keep_running) {
        printf("DMA data validation errors detected: %" PRIu64 "\n", total_data_errors);
        status = 1;
    }

    if (validated_buffers == 0 && keep_running) {
        printf("DMA data validation did not run (warmup not completed before timeout), exiting.\n");
        status = 1;
    }

    if (auto_rx_delay && !run && keep_running) {
        printf("DMA RX_DELAY calibration did not complete before timeout, exiting.\n");
        status = 1;
    }
#endif

    /* Cleanup DMA. */
#ifdef DMA_CHECK_DATA
end:
#endif
    litepcie_dma_cleanup(&dma);
    return status;
}

#endif

/* Clk Measurement */
/*-----------------*/

#define N_CLKS 5

static const uint32_t latch_addrs[N_CLKS] =
{
    CSR_CLK_MEASUREMENT_CLK0_LATCH_ADDR,
    CSR_CLK_MEASUREMENT_CLK1_LATCH_ADDR,
    CSR_CLK_MEASUREMENT_CLK2_LATCH_ADDR,
    CSR_CLK_MEASUREMENT_CLK3_LATCH_ADDR,
    CSR_CLK_MEASUREMENT_CLK4_LATCH_ADDR,
};

static const uint32_t value_addrs[N_CLKS] =
{
    CSR_CLK_MEASUREMENT_CLK0_VALUE_ADDR,
    CSR_CLK_MEASUREMENT_CLK1_VALUE_ADDR,
    CSR_CLK_MEASUREMENT_CLK2_VALUE_ADDR,
    CSR_CLK_MEASUREMENT_CLK3_VALUE_ADDR,
    CSR_CLK_MEASUREMENT_CLK4_VALUE_ADDR,
};

static const char* clk_names[N_CLKS] = {
    "       Sys Clk",
    "      PCIe Clk",
    "AD9361 Ref Clk",
    "AD9361 Dat Clk",
    "  Time Ref Clk",
};

static uint64_t read_64bit_register(void *conn, uint32_t addr)
{
    uint32_t lower = m2sdr_read32(conn, addr + 4);
    uint32_t upper = m2sdr_read32(conn, addr + 0);
    return ((uint64_t)upper << 32) | lower;
}

static void latch_all_clocks(void *conn)
{
    for (int i = 0; i < N_CLKS; i++) {
        m2sdr_write32(conn, latch_addrs[i], 1);
    }
}

static void read_all_clocks(void *conn, uint64_t *values)
{
    for (int i = 0; i < N_CLKS; i++) {
        values[i] = read_64bit_register(conn, value_addrs[i]);
    }
}

static void clk_test(int num_measurements, int delay_between_tests)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();

    printf("\e[1m[> Clk Measurement Test:\e[0m\n");
    printf("-------------------------\n");

    uint64_t previous_values[N_CLKS], current_values[N_CLKS];
    struct timespec start_time, current_time;
    double elapsed_time;

    printf("\e[1m%-8s", "Meas.");
    for (int i = 0; i < N_CLKS; i++) {
        printf("  %-15s", clk_names[i]);
    }
    printf(" (MHz)\e[0m\n");
    printf("--------");
    for (int i = 0; i < N_CLKS; i++) {
        printf("  ---------------");
    }
    printf("\n");

    latch_all_clocks(conn);
    read_all_clocks(conn, previous_values);
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (int i = 0; i < num_measurements; i++) {
        sleep(delay_between_tests);

        latch_all_clocks(conn);
        read_all_clocks(conn, current_values);
        clock_gettime(CLOCK_MONOTONIC, &current_time);

        elapsed_time = (current_time.tv_sec - start_time.tv_sec) +
                       (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
        start_time = current_time;

        printf("%-8d", i + 1);
        for (int clk_index = 0; clk_index < N_CLKS; clk_index++) {
            uint64_t delta_value = current_values[clk_index] - previous_values[clk_index];
            double frequency_mhz = delta_value / (elapsed_time * 1e6);
            printf("  %15.2f", frequency_mhz);
            previous_values[clk_index] = current_values[clk_index];
        }
        printf("\n");
    }

    m2sdr_close_dev(conn);
}

/* VCXO Test */
/*-----------*/

#ifdef CSR_SI5351_BASE

#define VCXO_TEST_PWM_PERIOD             4096   /* PWM period */
#define VCXO_TEST_MEASUREMENT_SAMPLES    10     /* Number of samples for averaging frequency measurements */
#define VCXO_TEST_STABILIZATION_DELAY_MS 100    /* Milliseconds to wait for stabilization */
#define VCXO_TEST_MEASUREMENT_DELAY_MS   100    /* Milliseconds for measurement period */
#define VCXO_TEST_DETECTION_THRESHOLD_HZ 1000.0 /* Threshold for detecting VCXO presence (SI5351B) */

static double measure_frequency(void *conn, int clk_index)
{
    uint64_t previous_value, current_value;
    struct timespec start_time, current_time;
    double elapsed_time;
    double freq_sum = 0.0;

    for (int sample = 0; sample < VCXO_TEST_MEASUREMENT_SAMPLES; sample++) {
        latch_all_clocks(conn);
        previous_value = read_64bit_register(conn, value_addrs[clk_index]);
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        struct timespec ts;
        ts.tv_sec = VCXO_TEST_MEASUREMENT_DELAY_MS / 1000;
        ts.tv_nsec = (VCXO_TEST_MEASUREMENT_DELAY_MS % 1000) * 1000000L;
        nanosleep(&ts, NULL);

        latch_all_clocks(conn);
        current_value = read_64bit_register(conn, value_addrs[clk_index]);
        clock_gettime(CLOCK_MONOTONIC, &current_time);

        elapsed_time = (current_time.tv_sec - start_time.tv_sec) +
                       (current_time.tv_nsec - start_time.tv_nsec) / 1e9;

        uint64_t delta_value = current_value - previous_value;
        freq_sum += delta_value / elapsed_time;
    }

    return freq_sum / VCXO_TEST_MEASUREMENT_SAMPLES;
}

static void vcxo_test(void)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();

    printf("\e[1m[> VCXO Test:\e[0m\n");
    printf("-------------\n");

    /* Find clock index for AD9361 Ref Clk. */
    int clk_index = -1;
    for (int i = 0; i < N_CLKS; i++) {
        if (strcmp(clk_names[i], "AD9361 Ref Clk") == 0) {
            clk_index = i;
            break;
        }
    }
    if (clk_index == -1) {
        fprintf(stderr, "Error: Clock 'AD9361 Ref Clk' not found in clk_names\n");
        m2sdr_close_dev(conn);
        exit(1);
    }

    /* Set PWM period. */
    m2sdr_write32(conn, CSR_SI5351_PWM_PERIOD_ADDR, VCXO_TEST_PWM_PERIOD);
    /* Enable PWM. */
    m2sdr_write32(conn, CSR_SI5351_PWM_ENABLE_ADDR, 1);

    /* Set PWM to 0% and wait for stabilization. */
    m2sdr_write32(conn, CSR_SI5351_PWM_WIDTH_ADDR, 0);
    struct timespec ts_stab;
    ts_stab.tv_sec = VCXO_TEST_STABILIZATION_DELAY_MS / 1000;
    ts_stab.tv_nsec = (VCXO_TEST_STABILIZATION_DELAY_MS % 1000) * 1000000L;
    nanosleep(&ts_stab, NULL);

    /* Detection phase for SI5351B (VCXO) vs SI5351C. */
    double freq_0 = measure_frequency(conn, clk_index);
    m2sdr_write32(conn, CSR_SI5351_PWM_WIDTH_ADDR, VCXO_TEST_PWM_PERIOD / 2);  /* 50% */
    nanosleep(&ts_stab, NULL);
    double freq_50 = measure_frequency(conn, clk_index);
    m2sdr_write32(conn, CSR_SI5351_PWM_WIDTH_ADDR, VCXO_TEST_PWM_PERIOD);  /* 100% */
    nanosleep(&ts_stab, NULL);
    double freq_100 = measure_frequency(conn, clk_index);

    double max_diff = fabs(freq_0 - freq_50) + fabs(freq_100 - freq_50);
    int is_vcxo = (max_diff >= VCXO_TEST_DETECTION_THRESHOLD_HZ);

    if (!is_vcxo) {
        printf("Detected SI5351C (no VCXO), exiting.\n");
        /* Set back PWM to nominal width. */
        m2sdr_write32(conn, CSR_SI5351_PWM_WIDTH_ADDR, VCXO_TEST_PWM_PERIOD / 2);
        m2sdr_close_dev(conn);
        return;
    }

    printf("Detected SI5351B (with VCXO): Max frequency variation %.2f Hz >= threshold %.2f Hz.\n\n", max_diff, VCXO_TEST_DETECTION_THRESHOLD_HZ);

    /* Full test: Reset to 0% and stabilize again. */
    m2sdr_write32(conn, CSR_SI5351_PWM_WIDTH_ADDR, 0);
    nanosleep(&ts_stab, NULL);

    double nominal_frequency_hz = 0.0;
    double min_frequency = 1e12;
    double max_frequency = 0.0;
    double previous_frequency_hz = 0.0;

    /* Print table header. */
    printf("\e[1m%-12s  %-15s  %-15s\e[0m\n", "PWM Width (%)", "Frequency (MHz)", "Variation (Hz)");
    printf("------------  ---------------  ---------------\n");

    for (double pwm_width_percent = 0.0; pwm_width_percent <= 100.0; pwm_width_percent += 10.0) {
        uint32_t pwm_width = (uint32_t)((pwm_width_percent / 100.0) * VCXO_TEST_PWM_PERIOD);

        /* Set PWM width. */
        m2sdr_write32(conn, CSR_SI5351_PWM_WIDTH_ADDR, pwm_width);
        nanosleep(&ts_stab, NULL);

        double frequency_hz = measure_frequency(conn, clk_index);

        if (pwm_width == VCXO_TEST_PWM_PERIOD / 2) {
            nominal_frequency_hz = frequency_hz;
        }

        double variation_hz = (pwm_width_percent != 0.0) ? frequency_hz - previous_frequency_hz : 0.0;

        printf("%-12.2f  %15.6f  %c%14.2f\n",
               pwm_width_percent, frequency_hz / 1e6,
               (variation_hz >= 0 ? '+' : '-'), fabs(variation_hz));

        if (frequency_hz < min_frequency) min_frequency = frequency_hz;
        if (frequency_hz > max_frequency) max_frequency = frequency_hz;

        previous_frequency_hz = frequency_hz;
    }

    /* Set back PWM to nominal width. */
    m2sdr_write32(conn, CSR_SI5351_PWM_WIDTH_ADDR, VCXO_TEST_PWM_PERIOD / 2);

    m2sdr_close_dev(conn);

    /* Calculate PPM and Hz variation from nominal at 50% PWM. */
    double hz_variation_from_nominal_max = max_frequency - nominal_frequency_hz;
    double hz_variation_from_nominal_min = nominal_frequency_hz - min_frequency;
    double ppm_variation_from_nominal_max = (hz_variation_from_nominal_max / nominal_frequency_hz) * 1e6;
    double ppm_variation_from_nominal_min = (hz_variation_from_nominal_min / nominal_frequency_hz) * 1e6;

    printf("\n\e[1m[> Report:\e[0m\n");
    printf("----------\n");
    printf(" Hz Variation from Nominal (50%% PWM): -%10.2f Hz / +%10.2f Hz\n",
           hz_variation_from_nominal_min, hz_variation_from_nominal_max);
    printf("PPM Variation from Nominal (50%% PWM): -%10.2f PPM / +%10.2f PPM\n",
           ppm_variation_from_nominal_min, ppm_variation_from_nominal_max);
}

#endif

#ifdef CSR_LEDS_BASE

/* LEDs */
/*------*/

static uint32_t csr_field_get(uint32_t value, unsigned int offset, unsigned int size)
{
    uint32_t mask;

    if (size >= 32)
        mask = 0xffffffffu;
    else
        mask = (1u << size) - 1u;

    return (value >> offset) & mask;
}

static void led_print_control(uint32_t control)
{
    printf("control raw        0x%08" PRIx32 "\n", control);
    printf("  manual_enable    %" PRIu32 "\n", csr_field_get(control, CSR_LEDS_CONTROL_MANUAL_ENABLE_OFFSET, CSR_LEDS_CONTROL_MANUAL_ENABLE_SIZE));
    printf("  time_running     %" PRIu32 "\n", csr_field_get(control, CSR_LEDS_CONTROL_TIME_RUNNING_OFFSET, CSR_LEDS_CONTROL_TIME_RUNNING_SIZE));
    printf("  time_valid       %" PRIu32 "\n", csr_field_get(control, CSR_LEDS_CONTROL_TIME_VALID_OFFSET, CSR_LEDS_CONTROL_TIME_VALID_SIZE));
    printf("  pcie_present     %" PRIu32 "\n", csr_field_get(control, CSR_LEDS_CONTROL_PCIE_PRESENT_OFFSET, CSR_LEDS_CONTROL_PCIE_PRESENT_SIZE));
    printf("  pcie_link_up     %" PRIu32 "\n", csr_field_get(control, CSR_LEDS_CONTROL_PCIE_LINK_UP_OFFSET, CSR_LEDS_CONTROL_PCIE_LINK_UP_SIZE));
    printf("  dma_synced       %" PRIu32 "\n", csr_field_get(control, CSR_LEDS_CONTROL_DMA_SYNCED_OFFSET, CSR_LEDS_CONTROL_DMA_SYNCED_SIZE));
    printf("  eth_present      %" PRIu32 "\n", csr_field_get(control, CSR_LEDS_CONTROL_ETH_PRESENT_OFFSET, CSR_LEDS_CONTROL_ETH_PRESENT_SIZE));
    printf("  eth_link_up      %" PRIu32 "\n", csr_field_get(control, CSR_LEDS_CONTROL_ETH_LINK_UP_OFFSET, CSR_LEDS_CONTROL_ETH_LINK_UP_SIZE));
    printf("  tx_activity      %" PRIu32 "\n", csr_field_get(control, CSR_LEDS_CONTROL_TX_ACTIVITY_OFFSET, CSR_LEDS_CONTROL_TX_ACTIVITY_SIZE));
    printf("  rx_activity      %" PRIu32 "\n", csr_field_get(control, CSR_LEDS_CONTROL_RX_ACTIVITY_OFFSET, CSR_LEDS_CONTROL_RX_ACTIVITY_SIZE));
    printf("  pps_level        %" PRIu32 "\n", csr_field_get(control, CSR_LEDS_CONTROL_PPS_LEVEL_OFFSET, CSR_LEDS_CONTROL_PPS_LEVEL_SIZE));
}

static void led_print_status(uint32_t status)
{
    printf("status raw         0x%08" PRIx32 "\n", status);
    printf("  level            %" PRIu32 "\n", csr_field_get(status, CSR_LEDS_STATUS_LEVEL_OFFSET, CSR_LEDS_STATUS_LEVEL_SIZE));
    printf("  output           %" PRIu32 "\n", csr_field_get(status, CSR_LEDS_STATUS_OUTPUT_OFFSET, CSR_LEDS_STATUS_OUTPUT_SIZE));
}

static void led_status(void)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    uint32_t control = m2sdr_read32(conn, CSR_LEDS_CONTROL_ADDR);
    uint32_t status  = m2sdr_read32(conn, CSR_LEDS_STATUS_ADDR);

    printf("\e[1m[> LED Status:\e[0m\n");
    printf("-------------\n");
    led_print_control(control);
    led_print_status(status);

    m2sdr_close_dev(conn);
}

static void led_control(uint32_t control)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    uint32_t status;

    m2sdr_write32(conn, CSR_LEDS_CONTROL_ADDR, control);
    status = m2sdr_read32(conn, CSR_LEDS_STATUS_ADDR);

    printf("LED control written.\n");
    led_print_control(control);
    led_print_status(status);

    m2sdr_close_dev(conn);
}

static void led_pulse(uint32_t pulse)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    uint32_t status;

    m2sdr_write32(conn, CSR_LEDS_PULSE_ADDR, pulse);
    status = m2sdr_read32(conn, CSR_LEDS_STATUS_ADDR);

    printf("LED pulse written.\n");
    printf("pulse raw          0x%08" PRIx32 "\n", pulse);
    printf("  tx_activity      %" PRIu32 "\n", csr_field_get(pulse, CSR_LEDS_PULSE_TX_ACTIVITY_OFFSET, CSR_LEDS_PULSE_TX_ACTIVITY_SIZE));
    printf("  rx_activity      %" PRIu32 "\n", csr_field_get(pulse, CSR_LEDS_PULSE_RX_ACTIVITY_OFFSET, CSR_LEDS_PULSE_RX_ACTIVITY_SIZE));
    printf("  pps              %" PRIu32 "\n", csr_field_get(pulse, CSR_LEDS_PULSE_PPS_OFFSET, CSR_LEDS_PULSE_PPS_SIZE));
    led_print_status(status);

    m2sdr_close_dev(conn);
}

static void led_release(void)
{
    led_control(0);
}

#endif

#if defined(USE_LITEETH) || defined(USE_LITEPCIE)
enum stream_loopback_pace {
    STREAM_LOOPBACK_PACE_RX,
    STREAM_LOOPBACK_PACE_RATE,
    STREAM_LOOPBACK_PACE_NONE,
};

#define STREAM_LOOPBACK_PREFILL_BUFS       24u
#define STREAM_LOOPBACK_RX_TIMEOUT_MS     200u
#define STREAM_LOOPBACK_STALL_US      1000000u

static int64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static const char *stream_loopback_pace_name(enum stream_loopback_pace pace)
{
    switch (pace) {
    case STREAM_LOOPBACK_PACE_RX:
        return "rx";
    case STREAM_LOOPBACK_PACE_RATE:
        return "rate";
    case STREAM_LOOPBACK_PACE_NONE:
        return "none";
    default:
        return "unknown";
    }
}

static int stream_loopback_parse_pace(const char *s, enum stream_loopback_pace *pace)
{
    if (!s || !pace)
        return -1;
    if (!strcmp(s, "rx")) {
        *pace = STREAM_LOOPBACK_PACE_RX;
        return 0;
    }
    if (!strcmp(s, "rate")) {
        *pace = STREAM_LOOPBACK_PACE_RATE;
        return 0;
    }
    if (!strcmp(s, "none")) {
        *pace = STREAM_LOOPBACK_PACE_NONE;
        return 0;
    }
    return -1;
}

static uint32_t stream_data_mask(int data_width)
{
    if (data_width >= 32)
        return 0xffffffffu;
    return (1u << data_width) - 1u;
}

static uint32_t stream_pn_word(uint32_t seed)
{
    return seed * 69069u + 1u;
}

static uint32_t stream_pn_seed_from_word(uint32_t word)
{
    return (word - 1u) * UINT32_C(0xa5e2a705);
}

static void stream_write_pn_data(uint32_t *buf, unsigned words, uint32_t *seed, uint32_t mask)
{
    for (unsigned i = 0; i < words; i++) {
        buf[i] = stream_pn_word(*seed) & mask;
        *seed += 1;
    }
}

static uint32_t stream_check_pn_data(const uint32_t *buf, unsigned words, uint32_t *seed, uint32_t mask, bool verbose)
{
    uint32_t errors = 0;

    for (unsigned i = 0; i < words; i++) {
        uint32_t expected = stream_pn_word(*seed) & mask;
        if (unlikely((buf[i] & mask) != expected)) {
            if (verbose && errors < 8) {
                fprintf(stderr, "loopback mismatch[%u]: got=0x%08x expected=0x%08x\n",
                    i, buf[i] & mask, expected);
            }
            errors++;
        }
        *seed += 1;
    }

    return errors;
}

static void stream_loopback_rate_wait(int64_t start_us,
                                      uint64_t tx_buffers,
                                      unsigned samples_per_buf,
                                      int64_t sample_rate)
{
    if (sample_rate <= 0)
        return;

    double target_us_d = ((double)tx_buffers * (double)samples_per_buf * 1e6) /
                         (double)sample_rate;
    int64_t target_us = start_us + (int64_t)target_us_d;

    for (;;) {
        int64_t now = get_time_us();
        if (now >= target_us)
            break;
        int64_t delta = target_us - now;
        if (delta > 1000)
            delta = 1000;
        usleep((useconds_t)delta);
    }
}

static uint64_t stream_loopback_rx_observed_buffers(struct m2sdr_dev *dev, uint64_t fallback)
{
    struct m2sdr_liteeth_udp_stats stats;

    if (m2sdr_liteeth_get_udp_stats(dev, &stats) == M2SDR_ERR_OK &&
        stats.rx_buffers > fallback) {
        return stats.rx_buffers;
    }

    return fallback;
}

static int stream_loopback_test(int data_width,
                                int duration,
                                enum stream_loopback_pace pace,
                                int64_t sample_rate,
                                unsigned window)
{
    struct m2sdr_dev *dev = NULL;
    enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
    const unsigned samples_per_buf = m2sdr_bytes_to_samples(format, M2SDR_BUFFER_BYTES);
    const unsigned words_per_buf = M2SDR_BUFFER_BYTES / sizeof(uint32_t);
    uint8_t *tx_buf = NULL;
    uint8_t *rx_buf = NULL;
    uint32_t seed_wr = 0;
    uint32_t seed_rd = 0;
    uint32_t mask;
    uint64_t tx_buffers = 0;
    uint64_t rx_buffers = 0;
    uint64_t checked_buffers = 0;
    uint64_t total_errors = 0;
    uint64_t startup_skip_words = 0;
    uint64_t startup_discard_buffers = 0;
    uint64_t last_checked_buffers = 0;
    unsigned prefill_buffers;
    bool seed_synced = false;
    int64_t last_time;
    int64_t start_us;
    int64_t last_progress_us;
    int64_t end_time = 0;
    int i = 0;
    int rc;
    int status = 1;

    if (samples_per_buf == 0) {
        fprintf(stderr, "Invalid stream loopback buffer size.\n");
        return 1;
    }
    if (data_width < 1 || data_width > 32) {
        fprintf(stderr, "Invalid data width %d\n", data_width);
        return 1;
    }
    if (pace == STREAM_LOOPBACK_PACE_RATE && sample_rate <= 0) {
        fprintf(stderr, "--pace=rate requires a positive --sample-rate\n");
        return 1;
    }
    if (window == 0)
        window = 1;
    prefill_buffers = window < STREAM_LOOPBACK_PREFILL_BUFS ? window : STREAM_LOOPBACK_PREFILL_BUFS;
    mask = stream_data_mask(data_width);

    if (!m2sdr_cli_finalize_device(&g_cli_dev))
        return 1;
    rc = m2sdr_open(&dev, m2sdr_cli_device_id(&g_cli_dev));
    if (rc != M2SDR_ERR_OK) {
        m2sdr_print_open_error(m2sdr_cli_device_id(&g_cli_dev), rc);
        return 1;
    }

    tx_buf = aligned_alloc(64, M2SDR_BUFFER_BYTES);
    rx_buf = aligned_alloc(64, M2SDR_BUFFER_BYTES);
    if (!tx_buf || !rx_buf) {
        fprintf(stderr, "buffer allocation failed\n");
        goto cleanup;
    }

    signal(SIGINT, intHandler);
    keep_running = 1;

    printf("\e[1m[> Stream loopback test:\e[0m\n");
    printf("-------------------------\n");
    printf("Device      : %s\n", m2sdr_cli_device_id(&g_cli_dev));
    printf("Buffer      : %u bytes / %u samples\n", M2SDR_BUFFER_BYTES, samples_per_buf);
    printf("Data width  : %d bits\n", data_width);
    printf("Pace        : %s\n", stream_loopback_pace_name(pace));
    if (pace == STREAM_LOOPBACK_PACE_RATE)
        printf("Sample rate : %" PRId64 " S/s\n", sample_rate);
    printf("TX window   : %u buffers\n", window);
    printf("TX prefill  : %u buffers\n", prefill_buffers);
    printf("Loopback    : TX stream -> RX stream inside FPGA\n");

    (void)m2sdr_liteeth_rx_stream_deactivate(dev);
    (void)m2sdr_liteeth_tx_stream_deactivate(dev);

    if (m2sdr_set_txrx_loopback(dev, true) != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_set_txrx_loopback(enable) failed\n");
        goto cleanup;
    }
    if (m2sdr_set_rx_header(dev, false, false) != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_set_rx_header failed\n");
        goto cleanup_disable_loopback;
    }
    if (m2sdr_set_tx_header(dev, false) != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_set_tx_header failed\n");
        goto cleanup_disable_loopback;
    }
    if (m2sdr_sync_config(dev, M2SDR_RX, format, 64, samples_per_buf, 0, 1000) != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_sync_config(RX) failed\n");
        goto cleanup_disable_loopback;
    }
    if (m2sdr_sync_config(dev, M2SDR_TX, format, 64, samples_per_buf, 0, 1000) != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_sync_config(TX) failed\n");
        goto cleanup_disable_loopback;
    }

    last_time = get_time_ms();
    start_us = get_time_us();
    last_progress_us = start_us;

    for (unsigned n = 0; n < prefill_buffers; n++) {
        stream_write_pn_data((uint32_t *)tx_buf, words_per_buf, &seed_wr, mask);
        rc = m2sdr_sync_tx(dev, tx_buf, samples_per_buf, NULL, 1000);
        if (rc != M2SDR_ERR_OK) {
            fprintf(stderr, "m2sdr_sync_tx prefill failed: %s\n", m2sdr_strerror(rc));
            goto cleanup_disable_loopback;
        }
        tx_buffers++;
    }

    end_time = (duration > 0) ? last_time + duration * 1000 : 0;
    while (keep_running && (duration <= 0 || get_time_ms() < end_time)) {
        int did_work = 0;
        uint64_t outstanding = tx_buffers - rx_buffers;

        while (outstanding < window) {
            int rc;

            if (pace == STREAM_LOOPBACK_PACE_RX && outstanding > 0)
                break;
            if (pace == STREAM_LOOPBACK_PACE_RATE)
                stream_loopback_rate_wait(start_us, tx_buffers, samples_per_buf, sample_rate);

            stream_write_pn_data((uint32_t *)tx_buf, words_per_buf, &seed_wr, mask);
            rc = m2sdr_sync_tx(dev, tx_buf, samples_per_buf, NULL, 1000);
            if (rc != M2SDR_ERR_OK) {
                fprintf(stderr, "m2sdr_sync_tx failed: %s\n", m2sdr_strerror(rc));
                goto cleanup_disable_loopback;
            }
            tx_buffers++;
            outstanding++;
            did_work = 1;

            if (pace != STREAM_LOOPBACK_PACE_NONE)
                break;
        }

        if (outstanding > 0) {
            int timeout_ms = (pace == STREAM_LOOPBACK_PACE_RX || outstanding >= window) ? 1000 : 0;
            int rc = m2sdr_sync_rx(dev, rx_buf, samples_per_buf, NULL, timeout_ms);
            if (rc == M2SDR_ERR_OK) {
                uint32_t errors;

                if (!seed_synced && mask == 0xffffffffu) {
                    uint32_t observed_seed = stream_pn_seed_from_word(((const uint32_t *)rx_buf)[0]);
                    uint32_t tmp_seed = observed_seed;

                    if (observed_seed >= seed_wr ||
                        stream_check_pn_data((const uint32_t *)rx_buf, words_per_buf, &tmp_seed, mask, false) != 0) {
                        rx_buffers++;
                        outstanding--;
                        startup_discard_buffers++;
                        did_work = 1;
                        last_progress_us = get_time_us();
                        continue;
                    }
                    if (observed_seed > seed_rd) {
                        startup_skip_words = observed_seed - seed_rd;
                        seed_rd = observed_seed;
                    }
                    seed_synced = true;
                }

                rx_buffers++;
                outstanding--;
                errors = stream_check_pn_data((const uint32_t *)rx_buf, words_per_buf, &seed_rd, mask,
                    checked_buffers == 0);
                total_errors += errors;
                checked_buffers++;
                did_work = 1;
                last_progress_us = get_time_us();
                if (errors)
                    status = 1;
            } else if (rc != M2SDR_ERR_TIMEOUT) {
                fprintf(stderr, "m2sdr_sync_rx failed: %s\n", m2sdr_strerror(rc));
                goto cleanup_disable_loopback;
            } else if (outstanding >= window) {
                fprintf(stderr, "m2sdr_sync_rx failed: timeout with TX window full\n");
                goto cleanup_disable_loopback;
            } else if ((get_time_us() - last_progress_us) > 1000000) {
                fprintf(stderr, "m2sdr_sync_rx failed: timeout waiting for loopback data\n");
                goto cleanup_disable_loopback;
            }
        }

        int64_t now = get_time_ms();
        int64_t elapsed = now - last_time;
        if (elapsed > 500) {
            struct m2sdr_liteeth_udp_stats stats;
            uint64_t delta = checked_buffers - last_checked_buffers;
            double gbps = (double)delta * M2SDR_BUFFER_BYTES * 8.0 / ((double)elapsed * 1e6);

            if (i % 10 == 0)
                printf("\e[1mSTREAM_Gbps\tTX_BUFFERS\tRX_BUFFERS\tERRORS\tSTART_SKIP\tSTART_DROP\tKDROP\tSRC_DROP\tRING_FULL\tTX_SEND_ERR\e[0m\n");
            i++;

            if (m2sdr_liteeth_get_udp_stats(dev, &stats) == M2SDR_ERR_OK) {
                printf("%14.2f\t%10" PRIu64 "\t%10" PRIu64 "\t%6" PRIu64 "\t%10" PRIu64 "\t%10" PRIu64 "\t%5" PRIu64 "\t%8" PRIu64 "\t%9" PRIu64 "\t%11" PRIu64 "\n",
                    gbps,
                    tx_buffers,
                    rx_buffers,
                    total_errors,
                    startup_skip_words,
                    startup_discard_buffers,
                    stats.rx_kernel_drops,
                    stats.rx_source_drops,
                    stats.rx_ring_full_events,
                    stats.tx_send_errors);
            } else {
                printf("%14.2f\t%10" PRIu64 "\t%10" PRIu64 "\t%6" PRIu64 "\t%10" PRIu64 "\t%10" PRIu64 "\t%5u\t%8u\t%9u\t%11u\n",
                    gbps, tx_buffers, rx_buffers, total_errors, startup_skip_words, startup_discard_buffers, 0u, 0u, 0u, 0u);
            }

            last_time = now;
            last_checked_buffers = checked_buffers;
        }

        if (!did_work)
            usleep(1000);
    }

    if (checked_buffers > 0 && total_errors == 0) {
        printf("Stream loopback test passed: checked %" PRIu64 " buffers", checked_buffers);
        if (startup_skip_words)
            printf(" after skipping %" PRIu64 " startup words", startup_skip_words);
        if (startup_discard_buffers)
            printf(" and discarding %" PRIu64 " stale startup buffers", startup_discard_buffers);
        printf(".\n");
        status = 0;
    } else {
        if (checked_buffers == 0)
            printf("Stream loopback test failed: no synchronized RX data after discarding %" PRIu64 " stale startup buffers.\n",
                startup_discard_buffers);
        else
            printf("Stream loopback test failed: %" PRIu64 " data errors over %" PRIu64 " buffers.\n",
                total_errors, checked_buffers);
    }

cleanup_disable_loopback:
    (void)m2sdr_liteeth_rx_stream_deactivate(dev);
    (void)m2sdr_liteeth_tx_stream_deactivate(dev);
    if (m2sdr_set_txrx_loopback(dev, false) != M2SDR_ERR_OK)
        fprintf(stderr, "m2sdr_set_txrx_loopback(disable) failed\n");
cleanup:
    free(tx_buf);
    free(rx_buf);
    m2sdr_close(dev);
    return status;
}

#ifdef USE_LITEETH
static int eth_rfic_rx_sweep(int duration)
{
    static const int64_t rates[] = {
        1920000,
        3840000,
        7680000,
        15360000,
        30720000,
        61440000,
    };
    enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
    const unsigned samples_per_buf = m2sdr_bytes_to_samples(format, M2SDR_BUFFER_BYTES);
    uint8_t *rx_buf = NULL;
    int status = 0;

    if (duration <= 0)
        duration = 1;
    if (!m2sdr_cli_finalize_device(&g_cli_dev))
        return 1;

    rx_buf = aligned_alloc(64, M2SDR_BUFFER_BYTES);
    if (!rx_buf) {
        fprintf(stderr, "buffer allocation failed\n");
        return 1;
    }

    signal(SIGINT, intHandler);
    keep_running = 1;

    printf("\e[1m[> LiteEth RFIC RX sweep:\e[0m\n");
    printf("--------------------------\n");
    printf("Device      : %s\n", m2sdr_cli_device_id(&g_cli_dev));
    printf("Duration    : %d s/rate\n", duration);
    printf("Buffer      : %u bytes / %u samples\n", M2SDR_BUFFER_BYTES, samples_per_buf);
    printf("\e[1mRATE(MSPS)\tRX_Gbps\tBUFFERS\tKDROP\tSRC_DROP\tRING_FULL\tRECOVERIES\tSTATUS\e[0m\n");

    for (unsigned i = 0; keep_running && i < (sizeof(rates) / sizeof(rates[0])); i++) {
        struct m2sdr_dev *dev = NULL;
        struct m2sdr_config cfg;
        struct m2sdr_liteeth_udp_stats stats;
        uint64_t buffers = 0;
        int64_t start_ms;
        int rc;
        const char *rate_status = "ok";

        rc = m2sdr_open(&dev, m2sdr_cli_device_id(&g_cli_dev));
        if (rc != M2SDR_ERR_OK) {
            m2sdr_print_open_error(m2sdr_cli_device_id(&g_cli_dev), rc);
            free(rx_buf);
            return 1;
        }

        m2sdr_config_init(&cfg);
        cfg.sample_rate = (double)rates[i];
        cfg.bandwidth = (double)rates[i];
        cfg.loopback = 0;

        rc = m2sdr_apply_config(dev, &cfg);
        if (rc != M2SDR_ERR_OK) {
            printf("%10.2f\t%7.2f\t%7u\t%5u\t%8u\t%9u\t%10u\t%s\n",
                (double)rates[i] / 1e6, 0.0, 0u, 0u, 0u, 0u, 0u, m2sdr_strerror(rc));
            m2sdr_close(dev);
            status = 1;
            continue;
        }

        rc = m2sdr_sync_config(dev, M2SDR_RX, format, 64, samples_per_buf, 0, 1000);
        if (rc != M2SDR_ERR_OK) {
            printf("%10.2f\t%7.2f\t%7u\t%5u\t%8u\t%9u\t%10u\t%s\n",
                (double)rates[i] / 1e6, 0.0, 0u, 0u, 0u, 0u, 0u, m2sdr_strerror(rc));
            m2sdr_close(dev);
            status = 1;
            continue;
        }

        start_ms = get_time_ms();
        while (keep_running && (get_time_ms() - start_ms) < (int64_t)duration * 1000) {
            rc = m2sdr_sync_rx(dev, rx_buf, samples_per_buf, NULL, 1000);
            if (rc != M2SDR_ERR_OK) {
                rate_status = m2sdr_strerror(rc);
                status = 1;
                break;
            }
            buffers++;
        }

        memset(&stats, 0, sizeof(stats));
        (void)m2sdr_liteeth_get_udp_stats(dev, &stats);
        double elapsed_ms = (double)(get_time_ms() - start_ms);
        double gbps = elapsed_ms > 0.0
            ? (double)buffers * M2SDR_BUFFER_BYTES * 8.0 / (elapsed_ms * 1e6)
            : 0.0;

        printf("%10.2f\t%7.2f\t%7" PRIu64 "\t%5" PRIu64 "\t%8" PRIu64 "\t%9" PRIu64 "\t%10" PRIu64 "\t%s\n",
            (double)rates[i] / 1e6,
            gbps,
            buffers,
            stats.rx_kernel_drops,
            stats.rx_source_drops,
            stats.rx_ring_full_events,
            stats.rx_timeout_recoveries,
            rate_status);

        m2sdr_close(dev);
    }

    free(rx_buf);
    return status;
}
#endif

#define RFIC_LOOPBACK_SYNC_LANES       32u
#define RFIC_LOOPBACK_SEARCH_LANES   1024u
#define RFIC_LOOPBACK_LANES_PER_WORD    4u
#define RFIC_LOOPBACK_MAX_DELAY_BUFS  128u
#define RFIC_LOOPBACK_SEARCH_BUFS     128u
#define RFIC_LOOPBACK_PREFILL_BUFS     STREAM_LOOPBACK_PREFILL_BUFS
#define RFIC_LOOPBACK_WARMUP_MS      4000u
#define RFIC_LOOPBACK_MIN_DURATION_S    8u
#define RFIC_PRBS_SEED             0x0a54u
#define RFIC_PRBS_LEN               65535u
#define RFIC_PRBS_SYNC_LANES          128u
#define RFIC_PRBS_MAX_STALE_BUFS      128u

static uint32_t rfic_loopback_mix32(uint64_t x)
{
    x += UINT64_C(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return (uint32_t)(x ^ (x >> 32));
}

static uint16_t rfic_loopback_lane12(uint32_t run_seed, uint64_t lane)
{
    uint64_t word = lane / RFIC_LOOPBACK_LANES_PER_WORD;
    uint64_t key = ((uint64_t)run_seed << 32) ^ word;
    return (uint16_t)(rfic_loopback_mix32(key) & 0x0fffu);
}

static int16_t rfic_loopback_sign_extend12(uint16_t v)
{
    v &= 0x0fffu;
    return (int16_t)((v & 0x0800u) ? (v | 0xf000u) : v);
}

static void rfic_loopback_fill(int16_t *buf, unsigned lanes, uint64_t *lane_wr, uint32_t run_seed)
{
    for (unsigned i = 0; i < lanes; i++) {
        buf[i] = rfic_loopback_sign_extend12(rfic_loopback_lane12(run_seed, *lane_wr));
        *lane_wr += 1;
    }
}

static uint64_t rfic_loopback_check(const int16_t *buf,
                                    unsigned lanes,
                                    uint64_t *lane_rd,
                                    uint32_t run_seed,
                                    bool verbose)
{
    uint64_t errors = 0;

    for (unsigned i = 0; i < lanes; i++) {
        uint16_t got = (uint16_t)buf[i] & 0x0fffu;
        uint16_t expected = rfic_loopback_lane12(run_seed, *lane_rd);

        if (unlikely(got != expected)) {
            if (verbose && errors < 8) {
                fprintf(stderr, "rfic loopback mismatch[%u]: got=0x%03x expected=0x%03x\n",
                    i, got, expected);
            }
            errors++;
        }
        *lane_rd += 1;
    }

    return errors;
}

static bool rfic_loopback_match_at(const int16_t *buf,
                                   unsigned lanes,
                                   unsigned sync_lanes,
                                   uint64_t lane,
                                   uint32_t run_seed)
{
    if (lanes < sync_lanes || sync_lanes < RFIC_LOOPBACK_SYNC_LANES)
        return false;

    for (unsigned i = 0; i < sync_lanes; i++) {
        uint16_t got = (uint16_t)buf[i] & 0x0fffu;
        uint16_t expected = rfic_loopback_lane12(run_seed, lane + i);

        if (got != expected)
            return false;
    }

    return true;
}

static bool rfic_loopback_find_sync(const int16_t *buf,
                                    unsigned lanes,
                                    uint64_t lane_wr,
                                    uint64_t *lane_rd,
                                    uint32_t run_seed)
{
    unsigned sync_lanes = lanes < RFIC_LOOPBACK_SEARCH_LANES ? lanes : RFIC_LOOPBACK_SEARCH_LANES;
    uint64_t search_lanes = (uint64_t)RFIC_LOOPBACK_SEARCH_BUFS * lanes;
    uint64_t start_lane = 0;

    if (lane_wr < sync_lanes || sync_lanes < RFIC_LOOPBACK_SYNC_LANES)
        return false;

    if (lane_wr > search_lanes)
        start_lane = lane_wr - search_lanes;
    start_lane &= ~(uint64_t)(RFIC_LOOPBACK_LANES_PER_WORD - 1u);

    for (uint64_t lane = start_lane; lane + sync_lanes <= lane_wr; lane += RFIC_LOOPBACK_LANES_PER_WORD) {
        if (rfic_loopback_match_at(buf, lanes, sync_lanes, lane, run_seed)) {
            *lane_rd = lane;
            return true;
        }
    }

    return false;
}

static void rfic_loopback_print_rx_preview(const int16_t *buf, unsigned lanes)
{
    unsigned preview = lanes < 16 ? lanes : 16;

    fprintf(stderr, "RFIC loopback RX preview:");
    for (unsigned i = 0; i < preview; i++)
        fprintf(stderr, " %03x", (uint16_t)buf[i] & 0x0fffu);
    fprintf(stderr, "\n");
}

static int rfic_loopback_test(int duration,
                              enum stream_loopback_pace pace,
                              int64_t sample_rate,
                              unsigned window,
                              bool fpga_data_loopback)
{
    struct m2sdr_dev *dev = NULL;
    struct m2sdr_config cfg;
    enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
    const unsigned samples_per_buf = m2sdr_bytes_to_samples(format, M2SDR_BUFFER_BYTES);
    const unsigned lanes_per_buf = M2SDR_BUFFER_BYTES / sizeof(int16_t);
    const unsigned stream_words_per_buf = lanes_per_buf / RFIC_LOOPBACK_LANES_PER_WORD;
    int16_t *tx_buf = NULL;
    int16_t *rx_buf = NULL;
    uint32_t run_seed = (uint32_t)get_time_us();
    uint64_t lane_wr = 0;
    uint64_t lane_rd = 0;
    uint64_t tx_buffers = 0;
    uint64_t rx_buffers = 0;
    uint64_t checked_buffers = 0;
    uint64_t stale_buffers = 0;
    uint64_t warmup_buffers = 0;
    uint64_t startup_skip_lanes = 0;
    uint64_t total_errors = 0;
    uint64_t last_checked_buffers = 0;
    uint64_t last_rx_timeout_recoveries = 0;
    unsigned prefill_buffers;
    uint64_t restart_window;
    bool restart_refilling = false;
    bool restart_waiting = false;
    bool warmup_done = fpga_data_loopback;
    bool seed_synced = false;
    int64_t start_us;
    int64_t last_time;
    int64_t last_rx_progress_us;
    int64_t warmup_end_time = 0;
    int64_t end_time;
    int i = 0;
    int rc;
    int status = 1;

    if (duration <= 0)
        duration = fpga_data_loopback ? 1 : RFIC_LOOPBACK_MIN_DURATION_S;
    if (!fpga_data_loopback && duration < (int)RFIC_LOOPBACK_MIN_DURATION_S)
        duration = RFIC_LOOPBACK_MIN_DURATION_S;
    if (window == 0)
        window = 1;
    if (sample_rate <= 0)
        sample_rate = DEFAULT_SAMPLERATE;
    prefill_buffers = window < RFIC_LOOPBACK_PREFILL_BUFS ? window : RFIC_LOOPBACK_PREFILL_BUFS;
    restart_window = window + prefill_buffers;

    if (!m2sdr_cli_finalize_device(&g_cli_dev))
        return 1;

    rc = m2sdr_open(&dev, m2sdr_cli_device_id(&g_cli_dev));
    if (rc != M2SDR_ERR_OK) {
        m2sdr_print_open_error(m2sdr_cli_device_id(&g_cli_dev), rc);
        return 1;
    }

    tx_buf = aligned_alloc(64, M2SDR_BUFFER_BYTES);
    rx_buf = aligned_alloc(64, M2SDR_BUFFER_BYTES);
    if (!tx_buf || !rx_buf) {
        fprintf(stderr, "buffer allocation failed\n");
        goto cleanup;
    }

    signal(SIGINT, intHandler);
    keep_running = 1;

    printf("\e[1m[> AD9361 TX/RX digital loopback test:\e[0m\n");
    printf("--------------------------------------\n");
    printf("Device      : %s\n", m2sdr_cli_device_id(&g_cli_dev));
    printf("Duration    : %d s\n", duration);
    printf("Sample rate : %" PRId64 " S/s\n", sample_rate);
    printf("Pace        : %s\n", stream_loopback_pace_name(pace));
    printf("Buffer      : %u bytes / %u samples / %u int16 lanes\n",
        M2SDR_BUFFER_BYTES, samples_per_buf, lanes_per_buf);
    printf("RFIC words  : %u per buffer\n", stream_words_per_buf);
    printf("Pattern seed: 0x%08x\n", run_seed);
    printf("Loopback    : host TX -> %s -> host RX\n",
        fpga_data_loopback ? "FPGA RFIC data loopback" : "AD9361 digital loopback");
    printf("Precision   : 12-bit compare on lane[11:0]\n");
    printf("TX prefill  : %u buffers\n", prefill_buffers);
    printf("Restart fill: %" PRIu64 " outstanding buffers\n", restart_window);
    if (!fpga_data_loopback)
        printf("Warmup      : %u ms\n", RFIC_LOOPBACK_WARMUP_MS);

    m2sdr_config_init(&cfg);
    cfg.sample_rate = sample_rate;
    cfg.bandwidth = sample_rate;
    cfg.loopback = fpga_data_loopback ? 0 : 1;
    cfg.enable_8bit_mode = false;
    rc = m2sdr_apply_config(dev, &cfg);
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_apply_config failed: %s\n", m2sdr_strerror(rc));
        goto cleanup;
    }

    (void)m2sdr_set_txrx_loopback(dev, false);
    (void)m2sdr_set_fpga_prbs_tx(dev, false);
    if (m2sdr_set_rx_header(dev, false, false) != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_set_rx_header failed\n");
        goto cleanup_disable_loopback;
    }
    if (m2sdr_set_tx_header(dev, false) != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_set_tx_header failed\n");
        goto cleanup_disable_loopback;
    }
    if (fpga_data_loopback && m2sdr_set_rfic_data_loopback(dev, true) != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_set_rfic_data_loopback(enable) failed\n");
        goto cleanup_disable_loopback;
    }
    rc = m2sdr_sync_config(dev, M2SDR_RX, format, 64, samples_per_buf, 0, 1000);
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_sync_config(RX) failed: %s\n", m2sdr_strerror(rc));
        goto cleanup_disable_loopback;
    }
    rc = m2sdr_sync_config(dev, M2SDR_TX, format, 64, samples_per_buf, 0, 1000);
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_sync_config(TX) failed: %s\n", m2sdr_strerror(rc));
        goto cleanup_disable_loopback;
    }

#ifdef CSR_CROSSBAR_MUX_SEL_ADDR
    /* Keep LiteEth TX packets in the Ethernet-side FIFO during the explicit
     * test prefill. Releasing the crossbar after prefill avoids startup gaps
     * in the RFIC TX stream caused by host packet scheduling latency. */
    if (m2sdr_reg_write(dev, CSR_CROSSBAR_MUX_SEL_ADDR, 0) != 0) {
        fprintf(stderr, "m2sdr TX prefill hold failed\n");
        goto cleanup_disable_loopback;
    }
#endif

    if (prefill_buffers > 0) {
        for (unsigned n = 0; n < prefill_buffers; n++) {
            rfic_loopback_fill(tx_buf, lanes_per_buf, &lane_wr, run_seed);
            rc = m2sdr_sync_tx(dev, tx_buf, samples_per_buf, NULL, 1000);
            if (rc != M2SDR_ERR_OK) {
                fprintf(stderr, "m2sdr_sync_tx prefill failed: %s\n", m2sdr_strerror(rc));
                goto cleanup_disable_loopback;
            }
            tx_buffers++;
        }
    }

#ifdef CSR_CROSSBAR_MUX_SEL_ADDR
    if (m2sdr_reg_write(dev, CSR_CROSSBAR_MUX_SEL_ADDR, 1) != 0) {
        fprintf(stderr, "m2sdr TX prefill release failed\n");
        goto cleanup_disable_loopback;
    }
#endif

    last_time = get_time_ms();
    warmup_end_time = last_time + (fpga_data_loopback ? 0 : RFIC_LOOPBACK_WARMUP_MS);
    start_us = get_time_us();
    last_rx_progress_us = start_us;
    end_time = last_time + (int64_t)duration * 1000;
    while (keep_running && get_time_ms() < end_time) {
        int did_work = 0;
        uint64_t rx_observed_buffers = stream_loopback_rx_observed_buffers(dev, rx_buffers);
        uint64_t outstanding = tx_buffers > rx_observed_buffers ? tx_buffers - rx_observed_buffers : 0;
        uint64_t tx_target = window;

        if (pace == STREAM_LOOPBACK_PACE_RX)
            tx_target = restart_refilling ? restart_window : window;

        while (outstanding < tx_target) {
            if (pace == STREAM_LOOPBACK_PACE_RATE && tx_buffers >= prefill_buffers) {
                uint64_t paced_tx_buffers = tx_buffers - prefill_buffers + 1;
                stream_loopback_rate_wait(start_us, paced_tx_buffers, stream_words_per_buf, sample_rate);
            }
            rfic_loopback_fill(tx_buf, lanes_per_buf, &lane_wr, run_seed);
            rc = m2sdr_sync_tx(dev, tx_buf, samples_per_buf, NULL, 1000);
            if (rc != M2SDR_ERR_OK) {
                fprintf(stderr, "m2sdr_sync_tx failed: %s\n", m2sdr_strerror(rc));
                goto cleanup_disable_loopback;
            }
            tx_buffers++;
            outstanding++;
            did_work = 1;
        }

        if (restart_refilling && outstanding >= tx_target) {
            restart_refilling = false;
            restart_waiting = true;
        }

        if (outstanding > 0) {
            int timeout_ms = (pace == STREAM_LOOPBACK_PACE_RX) ?
                STREAM_LOOPBACK_RX_TIMEOUT_MS : 1000;
            rc = m2sdr_sync_rx(dev, rx_buf, samples_per_buf, NULL, timeout_ms);
            if (rc != M2SDR_ERR_OK) {
                if (rc == M2SDR_ERR_TIMEOUT && pace == STREAM_LOOPBACK_PACE_RX) {
                    if (!restart_refilling && !restart_waiting) {
                        restart_refilling = true;
                        seed_synced = false;
                        did_work = 1;
                        continue;
                    }
                    if (restart_waiting &&
                        (get_time_us() - last_rx_progress_us) > STREAM_LOOPBACK_STALL_US) {
                        fprintf(stderr, "m2sdr_sync_rx failed: stalled after TX refill\n");
                        goto cleanup_disable_loopback;
                    }
                    continue;
                }
                fprintf(stderr, "m2sdr_sync_rx failed: %s\n", m2sdr_strerror(rc));
                goto cleanup_disable_loopback;
            }
            rx_buffers++;
            if (outstanding > 0)
                outstanding--;
            did_work = 1;
            restart_waiting = false;

            struct m2sdr_liteeth_udp_stats stats;
            int64_t now_us = get_time_us();
            last_rx_progress_us = now_us;

            if (m2sdr_liteeth_get_udp_stats(dev, &stats) == M2SDR_ERR_OK &&
                stats.rx_timeout_recoveries != last_rx_timeout_recoveries) {
                last_rx_timeout_recoveries = stats.rx_timeout_recoveries;
                if (pace == STREAM_LOOPBACK_PACE_RX)
                    restart_refilling = true;
                seed_synced = false;
            }

            if (!warmup_done) {
                warmup_buffers++;
                if (get_time_ms() < warmup_end_time)
                    continue;
                warmup_done = true;
            }

            if (!seed_synced) {
                if (!rfic_loopback_find_sync(rx_buf, lanes_per_buf, lane_wr, &lane_rd, run_seed)) {
                    stale_buffers++;
                    if (stale_buffers == 1)
                        rfic_loopback_print_rx_preview(rx_buf, lanes_per_buf);
                    if (stale_buffers > RFIC_LOOPBACK_MAX_DELAY_BUFS) {
                        fprintf(stderr, "RFIC loopback sync failed after %" PRIu64 " stale buffers.\n",
                            stale_buffers);
                        goto cleanup_disable_loopback;
                    }
                    continue;
                }
                startup_skip_lanes = lane_rd;
                seed_synced = true;
            }

            uint64_t errors = rfic_loopback_check(rx_buf, lanes_per_buf, &lane_rd, run_seed,
                total_errors == 0);
            total_errors += errors;
            checked_buffers++;
            if (errors)
                status = 1;
        }

        int64_t now = get_time_ms();
        int64_t elapsed = now - last_time;
        if (elapsed > 500) {
            struct m2sdr_liteeth_udp_stats stats;
            uint64_t delta = checked_buffers - last_checked_buffers;
            double gbps = (double)delta * M2SDR_BUFFER_BYTES * 8.0 / ((double)elapsed * 1e6);
            uint64_t rx_kernel_drops = 0;
            uint64_t rx_source_drops = 0;
            uint64_t rx_ring_full = 0;
            uint64_t rx_recoveries = 0;
            uint64_t tx_send_errors = 0;

            if (m2sdr_liteeth_get_udp_stats(dev, &stats) == M2SDR_ERR_OK) {
                rx_kernel_drops = stats.rx_kernel_drops;
                rx_source_drops = stats.rx_source_drops;
                rx_ring_full = stats.rx_ring_full_events;
                rx_recoveries = stats.rx_timeout_recoveries;
                tx_send_errors = stats.tx_send_errors;
            }

            if (i % 10 == 0)
                printf("\e[1mRFIC_LB_Gbps\tTX_BUFFERS\tRX_BUFFERS\tCHECKED\tERRORS\tSTART_SKIP\tWARMUP\tSTALE\tKDROP\tSRC_DROP\tRING_FULL\tRECOV\tTX_SEND_ERR\e[0m\n");
            i++;
            printf("%12.2f\t%10" PRIu64 "\t%10" PRIu64 "\t%7" PRIu64 "\t%6" PRIu64 "\t%10" PRIu64 "\t%6" PRIu64 "\t%5" PRIu64 "\t%5" PRIu64 "\t%8" PRIu64 "\t%9" PRIu64 "\t%5" PRIu64 "\t%11" PRIu64 "\n",
                gbps, tx_buffers, rx_buffers, checked_buffers, total_errors,
                startup_skip_lanes, warmup_buffers, stale_buffers,
                rx_kernel_drops, rx_source_drops, rx_ring_full,
                rx_recoveries, tx_send_errors);

            last_time = now;
            last_checked_buffers = checked_buffers;
        }

        if (!did_work)
            usleep(1000);
    }

    if (!seed_synced) {
        fprintf(stderr, "RFIC loopback test failed: no synchronized RX data.\n");
    } else if (total_errors == 0) {
        printf("RFIC loopback test passed: checked %" PRIu64 " buffers", checked_buffers);
        if (startup_skip_lanes)
            printf(" after skipping %" PRIu64 " startup lanes", startup_skip_lanes);
        if (warmup_buffers)
            printf(", warming up %" PRIu64 " buffers", warmup_buffers);
        if (stale_buffers)
            printf("%sdiscarding %" PRIu64 " stale startup buffers",
                warmup_buffers ? ", and " : " after ", stale_buffers);
        printf(".\n");
        status = 0;
    } else {
        printf("RFIC loopback test failed: %" PRIu64 " lane errors over %" PRIu64 " buffers.\n",
            total_errors, checked_buffers);
    }

cleanup_disable_loopback:
    (void)m2sdr_liteeth_rx_stream_deactivate(dev);
    (void)m2sdr_liteeth_tx_stream_deactivate(dev);
    if (m2sdr_set_rfic_data_loopback(dev, false) != M2SDR_ERR_OK)
        fprintf(stderr, "m2sdr_set_rfic_data_loopback(disable) failed\n");
    if (m2sdr_set_rfic_loopback(dev, 0) != M2SDR_ERR_OK)
        fprintf(stderr, "m2sdr_set_rfic_loopback(disable) failed\n");
cleanup:
    free(tx_buf);
    free(rx_buf);
    m2sdr_close(dev);
    return status;
}

#ifdef USE_LITEETH
struct rfic_prbs_phase {
    unsigned phase;
    unsigned lane_mod;
};

static uint16_t rfic_prbs_next(uint16_t state)
{
    uint16_t feedback =
        ((state >> 1)  ^ (state >> 2)  ^ (state >> 4)  ^ (state >> 5)  ^
         (state >> 6)  ^ (state >> 7)  ^ (state >> 8)  ^ (state >> 9)  ^
         (state >> 10) ^ (state >> 11) ^ (state >> 12) ^ (state >> 13) ^
         (state >> 14) ^ (state >> 15)) & 0x1u;

    return (uint16_t)(((state << 1) & 0xfffeu) | feedback);
}

static void rfic_prbs_build_sequence(uint16_t *seq)
{
    uint16_t state = RFIC_PRBS_SEED;

    for (unsigned i = 0; i < RFIC_PRBS_LEN; i++) {
        seq[i] = state & 0x0fffu;
        state = rfic_prbs_next(state);
    }
}

static uint16_t rfic_prbs_expected(const uint16_t *seq, const struct rfic_prbs_phase *phase, unsigned lane)
{
    unsigned word_advance = (phase->lane_mod + lane) / RFIC_LOOPBACK_LANES_PER_WORD;
    return seq[(phase->phase + word_advance) % RFIC_PRBS_LEN];
}

static void rfic_prbs_advance(struct rfic_prbs_phase *phase, unsigned lanes)
{
    unsigned total = phase->lane_mod + lanes;

    phase->phase = (phase->phase + total / RFIC_LOOPBACK_LANES_PER_WORD) % RFIC_PRBS_LEN;
    phase->lane_mod = total % RFIC_LOOPBACK_LANES_PER_WORD;
}

static bool rfic_prbs_match_at(const int16_t *buf,
                               unsigned lanes,
                               const uint16_t *seq,
                               const struct rfic_prbs_phase *phase)
{
    unsigned sync_lanes = lanes < RFIC_PRBS_SYNC_LANES ? lanes : RFIC_PRBS_SYNC_LANES;

    if (sync_lanes == 0)
        return false;

    for (unsigned i = 0; i < sync_lanes; i++) {
        uint16_t got = (uint16_t)buf[i] & 0x0fffu;
        uint16_t expected = rfic_prbs_expected(seq, phase, i);

        if (got != expected)
            return false;
    }

    return true;
}

static bool rfic_prbs_find_sync(const int16_t *buf,
                                unsigned lanes,
                                const uint16_t *seq,
                                struct rfic_prbs_phase *phase)
{
    struct rfic_prbs_phase candidate;

    if (lanes < RFIC_PRBS_SYNC_LANES)
        return false;

    for (candidate.phase = 0; candidate.phase < RFIC_PRBS_LEN; candidate.phase++) {
        for (candidate.lane_mod = 0; candidate.lane_mod < RFIC_LOOPBACK_LANES_PER_WORD; candidate.lane_mod++) {
            if (rfic_prbs_match_at(buf, lanes, seq, &candidate)) {
                *phase = candidate;
                return true;
            }
        }
    }

    return false;
}

static uint64_t rfic_prbs_check(const int16_t *buf,
                                unsigned lanes,
                                const uint16_t *seq,
                                struct rfic_prbs_phase *phase,
                                bool verbose)
{
    uint64_t errors = 0;

    for (unsigned i = 0; i < lanes; i++) {
        uint16_t got = (uint16_t)buf[i] & 0x0fffu;
        uint16_t expected = rfic_prbs_expected(seq, phase, i);

        if (unlikely(got != expected)) {
            if (verbose && errors < 8) {
                fprintf(stderr, "rfic prbs mismatch[%u]: got=0x%03x expected=0x%03x phase=%u lane_mod=%u\n",
                    i, got, expected, phase->phase, phase->lane_mod);
            }
            errors++;
        }
    }

    rfic_prbs_advance(phase, lanes);
    return errors;
}

static void rfic_prbs_print_rx_preview(const int16_t *buf, unsigned lanes)
{
    unsigned preview = lanes < 16 ? lanes : 16;

    fprintf(stderr, "RFIC PRBS RX preview:");
    for (unsigned i = 0; i < preview; i++)
        fprintf(stderr, " %03x", (uint16_t)buf[i] & 0x0fffu);
    fprintf(stderr, "\n");
}

static int rfic_prbs_loopback_test(int duration, int64_t sample_rate)
{
    struct m2sdr_dev *dev = NULL;
    struct m2sdr_config cfg;
    enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
    const unsigned samples_per_buf = m2sdr_bytes_to_samples(format, M2SDR_BUFFER_BYTES);
    const unsigned lanes_per_buf = M2SDR_BUFFER_BYTES / sizeof(int16_t);
    struct rfic_prbs_phase phase = {0, 0};
    uint16_t *seq = NULL;
    int16_t *rx_buf = NULL;
    uint64_t rx_buffers = 0;
    uint64_t checked_buffers = 0;
    uint64_t stale_buffers = 0;
    uint64_t total_errors = 0;
    uint64_t last_checked_buffers = 0;
    bool host_synced = false;
    bool fpga_synced = false;
    int64_t last_time;
    int64_t end_time;
    int i = 0;
    int rc;
    int status = 1;

    if (duration <= 0)
        duration = 1;
    if (sample_rate <= 0)
        sample_rate = DEFAULT_SAMPLERATE;

    if (!m2sdr_cli_finalize_device(&g_cli_dev))
        return 1;

    rc = m2sdr_open(&dev, m2sdr_cli_device_id(&g_cli_dev));
    if (rc != M2SDR_ERR_OK) {
        m2sdr_print_open_error(m2sdr_cli_device_id(&g_cli_dev), rc);
        return 1;
    }

    seq = malloc(RFIC_PRBS_LEN * sizeof(*seq));
    rx_buf = aligned_alloc(64, M2SDR_BUFFER_BYTES);
    if (!seq || !rx_buf) {
        fprintf(stderr, "buffer allocation failed\n");
        goto cleanup;
    }
    rfic_prbs_build_sequence(seq);

    signal(SIGINT, intHandler);
    keep_running = 1;

    printf("\e[1m[> RFIC PRBS loopback test:\e[0m\n");
    printf("----------------------------\n");
    printf("Device      : %s\n", m2sdr_cli_device_id(&g_cli_dev));
    printf("Duration    : %d s\n", duration);
    printf("Sample rate : %" PRId64 " S/s\n", sample_rate);
    printf("Buffer      : %u bytes / %u samples / %u int16 lanes\n",
        M2SDR_BUFFER_BYTES, samples_per_buf, lanes_per_buf);
    printf("Loopback    : FPGA PRBS TX -> AD9361 internal loopback -> FPGA RX -> LiteEth RX\n");
    printf("Precision   : 12-bit compare on lane[11:0]\n");

    m2sdr_config_init(&cfg);
    cfg.sample_rate = sample_rate;
    cfg.bandwidth = sample_rate;
    cfg.loopback = 1;
    cfg.enable_8bit_mode = false;
    rc = m2sdr_apply_config(dev, &cfg);
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_apply_config failed: %s\n", m2sdr_strerror(rc));
        goto cleanup;
    }

    (void)m2sdr_set_txrx_loopback(dev, false);
    if (m2sdr_set_rx_header(dev, false, false) != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_set_rx_header failed\n");
        goto cleanup_disable_prbs;
    }
    rc = m2sdr_sync_config(dev, M2SDR_RX, format, 64, samples_per_buf, 0, 1000);
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_sync_config(RX) failed: %s\n", m2sdr_strerror(rc));
        goto cleanup_disable_prbs;
    }
    rc = m2sdr_set_fpga_prbs_tx(dev, true);
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr, "m2sdr_set_fpga_prbs_tx(enable) failed: %s\n", m2sdr_strerror(rc));
        goto cleanup_disable_prbs;
    }

    last_time = get_time_ms();
    end_time = last_time + (int64_t)duration * 1000;
    while (keep_running && get_time_ms() < end_time) {
        rc = m2sdr_sync_rx(dev, rx_buf, samples_per_buf, NULL, 1000);
        if (rc != M2SDR_ERR_OK) {
            fprintf(stderr, "m2sdr_sync_rx failed: %s\n", m2sdr_strerror(rc));
            goto cleanup_disable_prbs;
        }
        rx_buffers++;

        (void)m2sdr_get_fpga_prbs_rx_synced(dev, &fpga_synced);

        if (!host_synced) {
            if (!rfic_prbs_find_sync(rx_buf, lanes_per_buf, seq, &phase)) {
                stale_buffers++;
                if (stale_buffers == 1)
                    rfic_prbs_print_rx_preview(rx_buf, lanes_per_buf);
                if (stale_buffers > RFIC_PRBS_MAX_STALE_BUFS) {
                    fprintf(stderr, "RFIC PRBS host sync failed after %" PRIu64 " stale buffers (FPGA synced: %s).\n",
                        stale_buffers, fpga_synced ? "yes" : "no");
                    goto cleanup_disable_prbs;
                }
                continue;
            }
            printf("Host PRBS sync: phase=%u lane_mod=%u after %" PRIu64 " stale buffers (FPGA synced: %s).\n",
                phase.phase, phase.lane_mod, stale_buffers, fpga_synced ? "yes" : "no");
            host_synced = true;
        }

        uint64_t errors = rfic_prbs_check(rx_buf, lanes_per_buf, seq, &phase, checked_buffers == 0);
        total_errors += errors;
        checked_buffers++;
        if (errors)
            status = 1;

        int64_t now = get_time_ms();
        int64_t elapsed = now - last_time;
        if (elapsed > 500) {
            uint64_t delta = checked_buffers - last_checked_buffers;
            double gbps = (double)delta * M2SDR_BUFFER_BYTES * 8.0 / ((double)elapsed * 1e6);

            if (i % 10 == 0)
                printf("\e[1mPRBS_Gbps\tRX_BUFFERS\tCHECKED\tERRORS\tSTALE\tFPGA_SYNC\tPHASE\tLANE_MOD\e[0m\n");
            i++;
            printf("%9.2f\t%10" PRIu64 "\t%7" PRIu64 "\t%6" PRIu64 "\t%5" PRIu64 "\t%9s\t%5u\t%8u\n",
                gbps, rx_buffers, checked_buffers, total_errors, stale_buffers,
                fpga_synced ? "yes" : "no", phase.phase, phase.lane_mod);

            last_time = now;
            last_checked_buffers = checked_buffers;
        }
    }

    (void)m2sdr_get_fpga_prbs_rx_synced(dev, &fpga_synced);
    if (!host_synced) {
        fprintf(stderr, "RFIC PRBS loopback test failed: no synchronized host RX data (FPGA synced: %s).\n",
            fpga_synced ? "yes" : "no");
    } else if (!fpga_synced) {
        fprintf(stderr, "RFIC PRBS loopback test failed: FPGA PRBS checker is not synced.\n");
    } else if (total_errors == 0) {
        printf("RFIC PRBS loopback test passed: checked %" PRIu64 " buffers", checked_buffers);
        if (stale_buffers)
            printf(" after discarding %" PRIu64 " stale startup buffers", stale_buffers);
        printf(".\n");
        status = 0;
    } else {
        printf("RFIC PRBS loopback test failed: %" PRIu64 " lane errors over %" PRIu64 " buffers (FPGA synced: %s).\n",
            total_errors, checked_buffers, fpga_synced ? "yes" : "no");
    }

cleanup_disable_prbs:
    if (m2sdr_set_fpga_prbs_tx(dev, false) != M2SDR_ERR_OK)
        fprintf(stderr, "m2sdr_set_fpga_prbs_tx(disable) failed\n");
    if (m2sdr_set_rfic_loopback(dev, 0) != M2SDR_ERR_OK)
        fprintf(stderr, "m2sdr_set_rfic_loopback(disable) failed\n");
    (void)m2sdr_liteeth_rx_stream_deactivate(dev);
cleanup:
    free(seq);
    free(rx_buf);
    m2sdr_close(dev);
    return status;
}
#endif
#endif

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR Board Utility\n"
           "usage: m2sdr_util [options] cmd [args...]\n"
           "\n"
           "options:\n"
           "  -h, --help                       Show this help message.\n"
           "  -d, --device DEV                 Use explicit device id.\n"
           "      --watch                      Refresh supported status commands until Ctrl-C.\n"
           "      --watch-interval SEC         Refresh interval for --watch (default: 1.0).\n"
           "      --data-width N               Width of generated test data (default: 32).\n"
           "      --duration SEC               Duration of loopback tests (default: 0, infinite).\n"
#ifdef USE_LITEPCIE
           "  -c, --device-num N               Select the device (default: 0).\n"
           "  -y, --force                      Skip confirmation prompts for destructive commands.\n"
           "      --zero-copy                  Enable zero-copy DMA mode.\n"
           "      --external-loopback          Use external loopback (default: internal).\n"
           "      --warmup-buffers N           Number of DMA buffers to skip before validation.\n"
           "      --auto-rx-delay              Automatic DMA RX-delay calibration.\n"
#endif
#ifdef USE_LITEETH
           "  -i, --ip ADDR                    Target IP address for Etherbone.\n"
           "  -p, --port PORT                  Port number (default: 1234).\n"
#endif
#if defined(USE_LITEETH) || defined(USE_LITEPCIE)
           "      --pace MODE                  TX pace: rx, rate, none (default: rx).\n"
           "      --sample-rate RATE           Sample rate for --pace=rate.\n"
           "      --window N                   TX buffers allowed ahead of RX (default: 64).\n"
#endif
           "\n"
           "device commands:\n"
           "  info | show-info\n"
           "      Show board information.\n"
           "  reg-read OFFSET\n"
           "      Read an FPGA register.\n"
           "  reg-write OFFSET VALUE\n"
           "      Write an FPGA register.\n"
           "\n"
           "ptp commands:\n"
           "  ptp-status\n"
           "      Show Ethernet PTP lock, identity, and counter status.\n"
           "  ptp-config\n"
           "      Show PTP discipline configuration.\n"
           "  ptp-config FIELD VALUE\n"
           "      Update one PTP discipline field (enable, holdover, update-cycles, ...).\n"
           "  ptp-config clear-counters\n"
           "      Clear PTP discipline and identity counters.\n"
           "\n"
#ifdef USE_LITEPCIE
           "test commands:\n"
           "  dma-test\n"
           "      Run the DMA test.\n"
#else
           "test commands:\n"
#endif
#if defined(USE_LITEETH) || defined(USE_LITEPCIE)
           "  stream-loopback-test\n"
           "      Run a host TX/RX FPGA stream loopback test. Use --pace to compare pacing modes.\n"
           "  rfic-loopback-test\n"
           "      Run host TX through AD9361 loopback and compare host RX data.\n"
           "  rfic-data-loopback-test\n"
           "      Run host TX through the FPGA RFIC data loopback and compare host RX data.\n"
#endif
#ifdef USE_LITEETH
           "  eth-loopback-test\n"
           "      Alias for stream-loopback-test.\n"
           "  eth-rfic-rx-sweep\n"
           "      Sweep RFIC sample rates and measure LiteEth RX throughput.\n"
           "  rfic-prbs-loopback-test\n"
           "      Run FPGA PRBS TX through AD9361 loopback and compare LiteEth RX data.\n"
#endif
           "  scratch-test\n"
           "      Test the scratch register.\n"
           "  clk-test [COUNT] [DELAY]\n"
           "      Measure on-board clock frequencies.\n"
#ifdef CSR_LEDS_BASE
           "  led-status\n"
           "      Read and decode LED control/status CSRs.\n"
           "  led-control VALUE\n"
           "      Write raw LED control bits.\n"
           "  led-pulse VALUE\n"
           "      Trigger raw LED pulse bits.\n"
           "  led-release\n"
           "      Return LED ownership to the design (control=0).\n"
#endif
#ifdef  CSR_SI5351_BASE
           "  vcxo-test\n"
           "      Measure VCXO frequency variation.\n"
#endif
           "\n"
#ifdef  CSR_SI5351_BASE
           "si5351 commands:\n"
           "  si5351-init\n"
           "      Initialize the SI5351.\n"
           "  si5351-dump\n"
           "      Dump SI5351 registers.\n"
           "  si5351-write REG VALUE\n"
           "      Write a SI5351 register.\n"
           "  si5351-read REG\n"
           "      Read a SI5351 register.\n"
           "\n"
#endif
           "ad9361 commands:\n"
           "  ad9361-dump\n"
           "      Dump AD9361 registers.\n"
           "  ad9361-write REG VALUE\n"
           "      Write an AD9361 register.\n"
           "  ad9361-read REG\n"
           "      Read an AD9361 register.\n"
           "  ad9361-port-dump\n"
           "      Dump AD9361 port configuration.\n"
           "  ad9361-ensm-dump\n"
           "      Dump AD9361 ENSM state.\n"
           "\n"
#ifdef CSR_FLASH_BASE
           "flash commands:\n"
#ifdef FLASH_WRITE
           "  flash-write FILE [OFFSET]\n"
           "      Write a file to SPI flash.\n"
#endif
           "  flash-read FILE SIZE [OFFSET]\n"
           "      Read from SPI flash to a file.\n"
           "  flash-reload\n"
           "      Reload the FPGA image.\n"
#endif
           );
    exit(1);
}

static bool cmd_is(const char *cmd, const char *name, const char *alias)
{
    return (!strcmp(cmd, name) || (alias && !strcmp(cmd, alias)));
}

/* Main */
/*------*/

int main(int argc, char **argv)
{
    enum {
        OPTION_WATCH = 256,
        OPTION_WATCH_INTERVAL,
        OPTION_PACE,
        OPTION_SAMPLE_RATE,
        OPTION_WINDOW,
    };
    const char *cmd;
    int c;
    int option_index = 0;
    bool ptp_watch = false;
    double ptp_watch_interval = 1.0;
    static int test_data_width = 32;
    static int test_duration = 0; /* Default to 0 for infinite duration. */
#if defined(USE_LITEETH) || defined(USE_LITEPCIE)
    static enum stream_loopback_pace stream_pace = STREAM_LOOPBACK_PACE_RX;
    static int64_t stream_sample_rate = 30720000;
    static unsigned stream_window = 64;
#endif
#ifdef USE_LITEPCIE
    static uint8_t m2sdr_device_zero_copy = 0;
    static uint8_t m2sdr_device_external_loopback = 0;
    static int litepcie_warmup_buffers = 128 * DMA_BUFFER_COUNT;
    static int litepcie_auto_rx_delay = 0;
#endif
    static struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { "device", required_argument, NULL, 'd' },
        { "device-num", required_argument, NULL, 'c' },
        { "ip", required_argument, NULL, 'i' },
        { "port", required_argument, NULL, 'p' },
        { "watch", no_argument, NULL, OPTION_WATCH },
        { "watch-interval", required_argument, NULL, OPTION_WATCH_INTERVAL },
        { "data-width", required_argument, NULL, 'w' },
        { "duration", required_argument, NULL, 't' },
#if defined(USE_LITEETH) || defined(USE_LITEPCIE)
        { "pace", required_argument, NULL, OPTION_PACE },
        { "sample-rate", required_argument, NULL, OPTION_SAMPLE_RATE },
        { "window", required_argument, NULL, OPTION_WINDOW },
#endif
#ifdef USE_LITEPCIE
        { "warmup-buffers", required_argument, NULL, 'W' },
        { "force", no_argument, NULL, 'y' },
        { "zero-copy", no_argument, NULL, 'z' },
        { "external-loopback", no_argument, NULL, 'e' },
        { "auto-rx-delay", no_argument, NULL, 'a' },
#endif
        { NULL, 0, NULL, 0 }
    };

    /* Parameters. */
    m2sdr_cli_device_init(&g_cli_dev);
    for (;;) {
#ifdef USE_LITEPCIE
        c = getopt_long(argc, argv, "hd:c:i:p:w:W:yzeat:", options, &option_index);
#else
        c = getopt_long(argc, argv, "hd:c:i:p:w:t:", options, &option_index);
#endif
        if (c == -1)
            break;
        switch(c) {
        case 'h':
            help();
            return 0;
        case 'd':
        case 'c':
        case 'i':
        case 'p':
            if (m2sdr_cli_handle_device_option(&g_cli_dev, c, optarg) != 0)
                exit(1);
            break;
        case OPTION_WATCH:
            ptp_watch = true;
            break;
        case OPTION_WATCH_INTERVAL:
            if ((parse_double_arg(optarg, &ptp_watch_interval) != 0) || (ptp_watch_interval <= 0.0)) {
                fprintf(stderr, "Invalid --watch-interval value '%s'\n", optarg);
                exit(1);
            }
            break;
#if defined(USE_LITEETH) || defined(USE_LITEPCIE)
        case OPTION_PACE:
            if (stream_loopback_parse_pace(optarg, &stream_pace) != 0) {
                fprintf(stderr, "Invalid --pace '%s' (expected rx, rate, or none)\n", optarg);
                exit(1);
            }
            break;
        case OPTION_SAMPLE_RATE:
            if (m2sdr_cli_parse_int64(optarg, &stream_sample_rate) != 0 || stream_sample_rate <= 0) {
                fprintf(stderr, "Invalid --sample-rate '%s'\n", optarg);
                exit(1);
            }
            break;
        case OPTION_WINDOW:
            stream_window = (unsigned)strtoul(optarg, NULL, 0);
            if (stream_window == 0) {
                fprintf(stderr, "Invalid --window '%s'\n", optarg);
                exit(1);
            }
            break;
#endif
        case 'w':
            test_data_width = atoi(optarg);
            break;
        case 't':
            test_duration = atoi(optarg);
            break;
#ifdef USE_LITEPCIE
        case 'y':
            g_force_flash_write = true;
            break;
        case 'W':
            litepcie_warmup_buffers = atoi(optarg);
            if (litepcie_warmup_buffers < 0)
                litepcie_warmup_buffers = 0;
            break;
        case 'z':
            m2sdr_device_zero_copy = 1;
            break;
        case 'e':
            m2sdr_device_external_loopback = 1;
            break;
        case 'a':
            litepcie_auto_rx_delay = 1;
            break;
#endif
        default:
            exit(1);
        }
    }

    /* Show help when too much args. */
    if (optind >= argc)
        help();

    cmd = argv[optind++];

    /* Info cmds. */
    if (cmd_is(cmd, "info", "show-info"))
        info();

    /* PTP cmds. */
    else if (cmd_is(cmd, "ptp_status", "ptp-status"))
        ptp_status(ptp_watch, ptp_watch_interval);
    else if (cmd_is(cmd, "ptp_config", "ptp-config")) {
        const char *field = NULL;
        const char *value = NULL;

        if (optind < argc)
            field = argv[optind++];
        if (optind < argc)
            value = argv[optind++];
        if (optind < argc)
            goto show_help;
        ptp_config(field, value);
    }

    /* Reg cmds. */
    else if (cmd_is(cmd, "reg_write", "reg-write")) {
        if (optind + 2 > argc) goto show_help;
        uint32_t offset = strtoul(argv[optind++], NULL, 0);
        uint32_t value = strtoul(argv[optind++], NULL, 0);
        test_reg_write(offset, value);
    }
    else if (cmd_is(cmd, "reg_read", "reg-read")) {
        if (optind + 1 > argc) goto show_help;
        uint32_t offset = strtoul(argv[optind++], NULL, 0);
        test_reg_read(offset);
    }

    /* Scratch cmds. */
    else if (cmd_is(cmd, "scratch_test", "scratch-test"))
        scratch_test();

    /* Clk cmds. */
    else if (cmd_is(cmd, "clk_test", "clk-test")) {
        int num_measurements = 10;
        int delay_between_tests = 1;

        if (optind < argc)
            num_measurements = atoi(argv[optind++]);
        if (optind < argc)
            delay_between_tests = atoi(argv[optind++]);

        clk_test(num_measurements, delay_between_tests);
    }

#ifdef CSR_LEDS_BASE
    /* LED cmds. */
    else if (cmd_is(cmd, "led_status", "led-status"))
        led_status();
    else if (cmd_is(cmd, "led_control", "led-control")) {
        if (optind + 1 > argc) goto show_help;
        uint32_t control = strtoul(argv[optind++], NULL, 0);
        led_control(control);
    }
    else if (cmd_is(cmd, "led_pulse", "led-pulse")) {
        if (optind + 1 > argc) goto show_help;
        uint32_t pulse = strtoul(argv[optind++], NULL, 0);
        led_pulse(pulse);
    }
    else if (cmd_is(cmd, "led_release", "led-release"))
        led_release();
#endif

#ifdef  CSR_SI5351_BASE
    /* VCXO test cmd. */
    else if (cmd_is(cmd, "vcxo_test", "vcxo-test")) {
        vcxo_test();
    }
#endif

    /* SI5351 cmds. */
#ifdef CSR_SI5351_BASE
    else if (cmd_is(cmd, "si5351_init", "si5351-init"))
        test_si5351_init();
    else if (cmd_is(cmd, "si5351_dump", "si5351-dump"))
        test_si5351_dump();
    else if (cmd_is(cmd, "si5351_write", "si5351-write")) {
        if (optind + 2 > argc) goto show_help;
        uint8_t reg = strtoul(argv[optind++], NULL, 0);
        uint8_t value = strtoul(argv[optind++], NULL, 0);
        test_si5351_write(reg, value);
    }
    else if (cmd_is(cmd, "si5351_read", "si5351-read")) {
        if (optind + 1 > argc) goto show_help;
        uint8_t reg = strtoul(argv[optind++], NULL, 0);
        test_si5351_read(reg);
    }
#endif

    /* AD9361 cmds. */
    else if (cmd_is(cmd, "ad9361_dump", "ad9361-dump"))
        test_ad9361_dump();
    else if (cmd_is(cmd, "ad9361_write", "ad9361-write")) {
        if (optind + 2 > argc) goto show_help;
        uint16_t reg = strtoul(argv[optind++], NULL, 0);
        uint16_t value = strtoul(argv[optind++], NULL, 0);
        test_ad9361_write(reg, value);
    }
    else if (cmd_is(cmd, "ad9361_read", "ad9361-read")) {
        if (optind + 1 > argc) goto show_help;
        uint16_t reg = strtoul(argv[optind++], NULL, 0);
        test_ad9361_read(reg);
    }
    else if (cmd_is(cmd, "ad9361_port_dump", "ad9361-port-dump"))
        test_ad9361_port_dump();
    else if (cmd_is(cmd, "ad9361_ensm_dump", "ad9361-ensm-dump"))
        test_ad9361_ensm_dump();

    /* SPI Flash cmds. */
#if CSR_FLASH_BASE
#ifdef FLASH_WRITE
    else if (cmd_is(cmd, "flash_write", "flash-write")) {
        const char *filename;
        uint32_t offset = CONFIG_FLASH_IMAGE_SIZE;  /* Operational */
        if (optind + 1 > argc)
            goto show_help;
        filename = argv[optind++];
        if (optind < argc)
            offset = strtoul(argv[optind++], NULL, 0);
        if (!g_force_flash_write && !confirm_flash_write()) {
            fprintf(stderr, "Aborted.\n");
            exit(1);
        }
        flash_write(filename, offset);
    }
#endif
    else if (cmd_is(cmd, "flash_read", "flash-read")) {
        const char *filename;
        uint32_t size = 0;
        uint32_t offset = CONFIG_FLASH_IMAGE_SIZE; /* Operational */
        if (optind + 2 > argc)
            goto show_help;
        filename = argv[optind++];
        size = strtoul(argv[optind++], NULL, 0);
        if (optind < argc)
            offset = strtoul(argv[optind++], NULL, 0);
        flash_read(filename, size, offset);
    }
    else if (cmd_is(cmd, "flash_reload", "flash-reload"))
        flash_reload();
#endif

#ifdef USE_LITEPCIE
    /* DMA cmds. */
    else if (cmd_is(cmd, "dma_test", "dma-test"))
        return dma_test(
            m2sdr_device_zero_copy,
            m2sdr_device_external_loopback,
            test_data_width,
            litepcie_auto_rx_delay,
            test_duration,
            litepcie_warmup_buffers);
#endif

#if defined(USE_LITEETH) || defined(USE_LITEPCIE)
    else if (cmd_is(cmd, "stream_loopback_test", "stream-loopback-test")
#ifdef USE_LITEETH
             || cmd_is(cmd, "eth_loopback_test", "eth-loopback-test")
#endif
             )
        return stream_loopback_test(test_data_width, test_duration,
                                    stream_pace, stream_sample_rate, stream_window);
    else if (cmd_is(cmd, "rfic_loopback_test", "rfic-loopback-test"))
        return rfic_loopback_test(test_duration, stream_pace, stream_sample_rate, stream_window, false);
    else if (cmd_is(cmd, "rfic_data_loopback_test", "rfic-data-loopback-test"))
        return rfic_loopback_test(test_duration, stream_pace, stream_sample_rate, stream_window, true);
#endif
#ifdef USE_LITEETH
    else if (cmd_is(cmd, "eth_rfic_rx_sweep", "eth-rfic-rx-sweep"))
        return eth_rfic_rx_sweep(test_duration);
    else if (cmd_is(cmd, "rfic_prbs_loopback_test", "rfic-prbs-loopback-test"))
        return rfic_prbs_loopback_test(test_duration, stream_sample_rate);
#endif

    /* Show help otherwise. */
    else
        goto show_help;

    return 0;

show_help:
    help();

    return 0;
}
