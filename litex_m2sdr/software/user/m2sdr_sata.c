/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR SATA Utility.
 *
 * Configure Crossbar routing and drive LiteSATA streamers:
 * - record:  RX stream -> SATA_RX_STREAMER (Stream2Sectors)
 * - play:    SATA_TX_STREAMER -> TX stream (Sectors2Stream)
 * - replay:  SATA_TX_STREAMER -> TXRX loopback -> RX destination (pcie/eth/sata)
 * - copy:    SATA_TX_STREAMER -> TXRX loopback -> SATA_RX_STREAMER (SSD -> SSD)
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <math.h>
#include <ctype.h>

#include "liblitepcie.h"
#include "m2sdr.h"
#include "m2sdr_cli.h"
#include "m2sdr_sata_catalog.h"
#include "m2sdr_sata_lowlevel.h"
#include "m2sdr_sata_sigmf.h"
#include "csr.h"
#include "mem.h"

/* Connection options -------------------------------------------------------- */

static struct m2sdr_cli_device g_cli_dev;
static sig_atomic_t keep_running = 1;
static int g_sata_lock_fd = -1;

static void int_handler(int dummy)
{
    (void)dummy;
    keep_running = 0;
}

/* Helpers ------------------------------------------------------------------- */

static uint64_t parse_u64(const char *s)
{
    uint64_t v = 0;

    if (m2sdr_cli_parse_u64(s, &v) != 0) {
        m2sdr_cli_error("invalid number '%s'", s ? s : "(null)");
        exit(1);
    }
    return v;
}

static uint64_t parse_size_bytes(const char *label, const char *s)
{
    char *end = NULL;
    long double value;
    long double scale = 1.0L;

    if (!s) {
        m2sdr_cli_error("invalid %s '(null)'", label ? label : "size");
        exit(1);
    }

    errno = 0;
    value = strtold(s, &end);
    if (end == s || errno == ERANGE || value < 0.0L || !isfinite((double)value)) {
        m2sdr_cli_error("invalid %s '%s'", label ? label : "size", s);
        exit(1);
    }

    while (*end && isspace((unsigned char)*end))
        end++;
    if (*end) {
        char suffix[4] = {0, 0, 0, 0};
        unsigned n = 0;

        while (*end && !isspace((unsigned char)*end) && n < sizeof(suffix) - 1u)
            suffix[n++] = (char)tolower((unsigned char)*end++);
        while (*end && isspace((unsigned char)*end))
            end++;
        if (*end) {
            m2sdr_cli_error("invalid %s suffix in '%s'", label ? label : "size", s);
            exit(1);
        }

        if (!strcmp(suffix, "k") || !strcmp(suffix, "kb"))
            scale = 1000.0L;
        else if (!strcmp(suffix, "m") || !strcmp(suffix, "mb"))
            scale = 1000.0L * 1000.0L;
        else if (!strcmp(suffix, "g") || !strcmp(suffix, "gb"))
            scale = 1000.0L * 1000.0L * 1000.0L;
        else if (!strcmp(suffix, "ki") || !strcmp(suffix, "kib"))
            scale = 1024.0L;
        else if (!strcmp(suffix, "mi") || !strcmp(suffix, "mib"))
            scale = 1024.0L * 1024.0L;
        else if (!strcmp(suffix, "gi") || !strcmp(suffix, "gib"))
            scale = 1024.0L * 1024.0L * 1024.0L;
        else {
            m2sdr_cli_error("invalid %s suffix in '%s'", label ? label : "size", s);
            exit(1);
        }
    }

    value *= scale;
    if (value < 0.0L || value > (long double)UINT64_MAX) {
        m2sdr_cli_error("%s out of range: '%s'", label ? label : "size", s);
        exit(1);
    }
    return (uint64_t)ceill(value);
}

static uint32_t parse_u32(const char *s)
{
    uint32_t v = 0;

    if (m2sdr_cli_parse_u32(s, &v) != 0) {
        m2sdr_cli_error("invalid u32 value '%s'", s ? s : "(null)");
        exit(1);
    }
    return v;
}

static uint32_t parse_u32_range_arg(const char *label, const char *s, uint32_t min, uint32_t max)
{
    uint32_t v = 0;

    if (m2sdr_cli_parse_u32(s, &v) != 0 || v < min || v > max) {
        m2sdr_cli_error("invalid %s '%s' (expected %" PRIu32 "..%" PRIu32 ")",
                        label ? label : "value", s ? s : "(null)", min, max);
        exit(1);
    }
    return v;
}

static int parse_timeout_ms(const char *s)
{
    int v = 0;

    if (m2sdr_cli_parse_int_range(s, -1, INT32_MAX, &v) != 0) {
        m2sdr_cli_error("invalid timeout '%s'", s ? s : "(null)");
        exit(1);
    }
    return v;
}

static int parse_bool01(const char *label, const char *s)
{
    unsigned v = 0;

    if (m2sdr_cli_parse_uint_range(s, 0, 1, &v) != 0) {
        m2sdr_cli_invalid_choice(label, s, "0 or 1");
        exit(1);
    }
    return (int)v;
}

static int64_t parse_i64(const char *label, const char *s)
{
    int64_t v = 0;

    if (m2sdr_cli_parse_int64(s, &v) != 0) {
        m2sdr_cli_error("invalid %s '%s'", label ? label : "value", s ? s : "(null)");
        exit(1);
    }
    return v;
}

static double parse_double_arg(const char *label, const char *s)
{
    double v = 0.0;

    if (m2sdr_cli_parse_double(s, &v) != 0) {
        m2sdr_cli_error("invalid %s '%s'", label ? label : "value", s ? s : "(null)");
        exit(1);
    }
    return v;
}

static const char *format_name(enum m2sdr_format format)
{
    return m2sdr_cli_format_name(format);
}

static const char *channel_layout_name(enum m2sdr_channel_layout layout)
{
    switch (layout) {
    case M2SDR_CHANNEL_LAYOUT_1T1R: return "1t1r";
    case M2SDR_CHANNEL_LAYOUT_2T2R: return "2t2r";
    default:                        return "unknown";
    }
}

static enum m2sdr_channel_layout parse_channel_layout(const char *text)
{
    if (text && strcmp(text, "1t1r") == 0)
        return M2SDR_CHANNEL_LAYOUT_1T1R;
    if (text && strcmp(text, "2t2r") == 0)
        return M2SDR_CHANNEL_LAYOUT_2T2R;
    m2sdr_cli_invalid_choice("channel layout", text, "1t1r or 2t2r");
    exit(1);
}

static unsigned channel_layout_count(enum m2sdr_channel_layout layout)
{
    return layout == M2SDR_CHANNEL_LAYOUT_1T1R ? 1u : 2u;
}

static void format_capacity(char *buf, size_t len, uint64_t sectors, uint32_t sector_size)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    long double value = (long double)sectors * (long double)sector_size;
    unsigned unit = 0;

    while (value >= 1024.0L && unit + 1 < sizeof(units)/sizeof(units[0])) {
        value /= 1024.0L;
        unit++;
    }
    if (unit == 0)
        snprintf(buf, len, "%.0Lf %s", value, units[unit]);
    else
        snprintf(buf, len, "%.2Lf %s", value, units[unit]);
}

/* Connection functions ------------------------------------------------------ */

static void m2sdr_sata_unlock(void)
{
    if (g_sata_lock_fd >= 0) {
        flock(g_sata_lock_fd, LOCK_UN);
        close(g_sata_lock_fd);
        g_sata_lock_fd = -1;
    }
}

static int m2sdr_sata_lock(void)
{
    if (g_sata_lock_fd >= 0)
        return 0;

    g_sata_lock_fd = open("/tmp/m2sdr_sata.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (g_sata_lock_fd < 0) {
        perror("/tmp/m2sdr_sata.lock");
        return -1;
    }

    if (flock(g_sata_lock_fd, LOCK_EX) != 0) {
        perror("flock");
        m2sdr_sata_unlock();
        return -1;
    }
    return 0;
}

static struct m2sdr_dev *m2sdr_open_dev(void)
{
    char identifier[M2SDR_IDENT_MAX];
    int rc;
    if (!m2sdr_cli_finalize_device(&g_cli_dev)) {
        exit(1);
    }
    if (m2sdr_sata_lock() != 0)
        exit(1);

    struct m2sdr_dev *dev = NULL;
    rc = m2sdr_open(&dev, m2sdr_cli_device_id(&g_cli_dev));
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr, "Could not open %s\n", m2sdr_cli_device_id(&g_cli_dev));
        m2sdr_sata_unlock();
        exit(1);
    }
    rc = m2sdr_get_identifier(dev, identifier, sizeof(identifier));
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr,
            "Failed to read SoC identifier. If this is PCIe after loading a "
            "new bitstream, rescan the PCIe bus/driver before using SATA.\n");
        m2sdr_close(dev);
        m2sdr_sata_unlock();
        exit(1);
    }
    return dev;
}

static void m2sdr_close_dev(struct m2sdr_dev *dev)
{
    if (dev) {
        m2sdr_close(dev);
    }
    m2sdr_sata_unlock();
}

#ifdef CSR_SATA_PHY_BASE

struct sata_operation {
    struct m2sdr_dev *conn;
    struct sata_route_state saved_route;
};

/* SATA control (guarded) ---------------------------------------------------- */

static struct sata_operation sata_operation_begin(void)
{
    struct sata_operation op;
    op.conn = m2sdr_open_dev();
    op.saved_route = sata_route_state_capture(op.conn);
    sata_require_csrs();
    return op;
}

static void sata_operation_finish(struct sata_operation *op)
{
    sata_route_state_restore(op->conn, &op->saved_route);
    m2sdr_close_dev(op->conn);
}

static enum sata_wait_result wait_done_report(const char *name,
                                              uint32_t (*done_fn)(void *),
                                              uint32_t (*err_fn)(void *),
                                              void *conn,
                                              int timeout_ms,
                                              uint64_t nsectors,
                                              bool report)
{
    int64_t start_us = m2sdr_sata_get_time_us();
    int64_t last_report_us = start_us;

    for (;;) {
        int64_t now_us;
        int64_t elapsed_us;

        if (!keep_running) {
            fprintf(stderr, "%s: interrupted\n", name);
            return SATA_WAIT_INTERRUPTED;
        }

        uint32_t done = done_fn(conn);
        if (done) {
            uint32_t err = err_fn(conn);
            if (report) {
                if (err) printf("%s: done (error=1)\n", name);
                else     printf("%s: done\n", name);
            }
            return SATA_WAIT_OK;
        }

        now_us = m2sdr_sata_get_time_us();
        elapsed_us = now_us - start_us;
        if (timeout_ms >= 0 && elapsed_us >= (int64_t)timeout_ms * 1000) {
            uint32_t err = err_fn(conn);
            if (err)
                fprintf(stderr, "%s: timeout (error=1)\n", name);
            else
                fprintf(stderr, "%s: timeout\n", name);
            return SATA_WAIT_TIMEOUT;
        }

        if (report && now_us - last_report_us >= 500000) {
            double mb = (double)nsectors * 512.0 / (1024.0 * 1024.0);
            double s  = (double)elapsed_us / 1000000.0;
            double mbps = (s > 0.0) ? (mb / s) : 0.0;
            fprintf(stderr, "%s: in progress (%.1f MB, %.2f MB/s)\n", name, mb, mbps);
            last_report_us = now_us;
        }

        sata_wait_sleep(elapsed_us);
    }
}

static enum sata_wait_result wait_done(const char *name,
                                       uint32_t (*done_fn)(void *),
                                       uint32_t (*err_fn)(void *),
                                       void *conn,
                                       int timeout_ms,
                                       uint64_t nsectors)
{
    return wait_done_report(name, done_fn, err_fn, conn, timeout_ms, nsectors, true);
}

static enum sata_wait_result wait_done_quiet(const char *name,
                                             uint32_t (*done_fn)(void *),
                                             uint32_t (*err_fn)(void *),
                                             void *conn,
                                             int timeout_ms)
{
    return wait_done_report(name, done_fn, err_fn, conn, timeout_ms, 1, false);
}

static void print_planned_transfer(const char *name,
                                   uint64_t src_sector,
                                   uint64_t dst_sector,
                                   uint32_t nsectors,
                                   int txsrc,
                                   int rxdst,
                                   int loopback,
                                   int timeout_ms)
{
    printf("%s dry-run:\n", name);
    if (src_sector != UINT64_MAX)
        printf("  Source sector      0x%016" PRIx64 "\n", src_sector);
    if (dst_sector != UINT64_MAX)
        printf("  Destination sector 0x%016" PRIx64 "\n", dst_sector);
    printf("  Sector count       %" PRIu32 "\n", nsectors);
    printf("  TX source          %s\n", txsrc_name(txsrc));
    printf("  RX destination     %s\n", rxdst_name(rxdst));
    printf("  Loopback           %s\n", loopback ? "enabled" : "disabled");
    printf("  Timeout            %d ms\n", timeout_ms);
}

static bool sata_try_pcie_dma_default(struct m2sdr_dev *conn);
static void sata_print_pcie_dma_error(const char *op, int rc);

#ifdef SATA_HOST_IO_AVAILABLE

static int do_etherbone_bench(int argc, char **argv, int argi)
{
    static const uint32_t candidates[] = {1, 2, 4, 8, 16, 32, 64, M2SDR_SATA_ETHERBONE_BULK_WORDS};
    uint32_t iterations = 1024;
    struct m2sdr_dev *conn;
    int status = 0;

    while (argi < argc) {
        if (!strcmp(argv[argi], "--iterations")) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "Missing value for --iterations\n");
                return 1;
            }
            iterations = parse_u32_range_arg("iterations", argv[++argi], 1, 1000000);
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", argv[argi]);
            return 1;
        }
        argi++;
    }

    conn = m2sdr_open_dev();
    printf("Etherbone burst benchmark: iterations=%" PRIu32 "\n", iterations);
    printf("%8s %12s %12s %12s %s\n", "words", "bytes", "write MiB/s", "read MiB/s", "status");

    for (size_t ci = 0; ci < sizeof(candidates) / sizeof(candidates[0]) && keep_running; ci++) {
        uint32_t n = candidates[ci];
        uint32_t tx[M2SDR_SATA_ETHERBONE_BULK_WORDS];
        uint32_t rx[M2SDR_SATA_ETHERBONE_BULK_WORDS];
        uint64_t bytes = (uint64_t)iterations * n * sizeof(uint32_t);
        int64_t start_us;
        int64_t write_us;
        int64_t read_us;
        bool ok = true;

        etherbone_fill_test_words(tx, n);
        memset(rx, 0, n * sizeof(uint32_t));

        if (!etherbone_probe_burst_words(conn, n, true)) {
            ok = false;
        } else {
            start_us = m2sdr_sata_get_time_us();
            for (uint32_t i = 0; i < iterations; i++) {
                if (m2sdr_reg_write_bulk(conn, SATA_HOST_BUFFER_BASE, tx, n) != M2SDR_ERR_OK) {
                    ok = false;
                    break;
                }
            }
            write_us = m2sdr_sata_get_time_us() - start_us;

            if (ok) {
                start_us = m2sdr_sata_get_time_us();
                for (uint32_t i = 0; i < iterations; i++) {
                    if (m2sdr_reg_read_bulk(conn, SATA_HOST_BUFFER_BASE, rx, n) != M2SDR_ERR_OK) {
                        ok = false;
                        break;
                    }
                }
                read_us = m2sdr_sata_get_time_us() - start_us;
            } else {
                read_us = 0;
            }

            if (ok && memcmp(tx, rx, n * sizeof(uint32_t)) != 0)
                ok = false;
        }

        if (ok) {
            double mib = (double)bytes / (1024.0 * 1024.0);
            double write_mibs = write_us > 0 ? mib * 1000000.0 / (double)write_us : 0.0;
            double read_mibs  = read_us  > 0 ? mib * 1000000.0 / (double)read_us  : 0.0;

            printf("%8" PRIu32 " %12" PRIu64 " %12.3f %12.3f ok\n",
                   n, bytes, write_mibs, read_mibs);
        } else {
            status = 1;
            printf("%8" PRIu32 " %12" PRIu64 " %12s %12s fail\n",
                   n, bytes, "-", "-");
            break;
        }
        fflush(stdout);
    }

    m2sdr_close_dev(conn);
    return status;
}

static int do_pcie_dma_bench(int argc, char **argv, int argi, int timeout_ms)
{
    struct m2sdr_dev *conn;
    uint64_t sector;
    uint32_t nsectors;
    uint32_t max_sectors = M2SDR_SATA_PCIE_DMA_MAX_SECTORS;
    uint8_t *buf;
    uint32_t done = 0;
    int64_t write_us = 0;
    int64_t read_us = 0;
    int status = 1;

    if (argc - argi != 2) {
        fprintf(stderr, "usage: pcie-dma-bench SECTOR NSECTORS\n");
        return 1;
    }

    sector = parse_u64(argv[argi++]);
    nsectors = parse_u32(argv[argi++]);
    if (nsectors == 0) {
        m2sdr_cli_error("nsectors must be greater than zero");
        return 1;
    }

    buf = malloc((size_t)max_sectors * SATA_SECTOR_BYTES);
    if (!buf) {
        fprintf(stderr, "Failed to allocate PCIe DMA benchmark buffer.\n");
        return 1;
    }

    conn = m2sdr_open_dev();
    if (!sata_try_pcie_dma_default(conn)) {
        fprintf(stderr, "pcie-dma-bench requires a PCIe device.\n");
        goto out_close;
    }

    while (done < nsectors) {
        uint32_t chunk = nsectors - done;
        size_t bytes;
        int64_t start_us;
        int rc;

        if (chunk > max_sectors)
            chunk = max_sectors;
        bytes = (size_t)chunk * SATA_SECTOR_BYTES;
        fill_pattern(buf, bytes, (sector + done) * SATA_SECTOR_BYTES, SATA_PATTERN_COUNTER);
        start_us = m2sdr_sata_get_time_us();
        rc = m2sdr_sata_pcie_dma_copy(conn, M2SDR_SATA_DMA_HOST_TO_DEVICE,
            sector + done, chunk, buf, timeout_ms, NULL);
        write_us += m2sdr_sata_get_time_us() - start_us;
        if (rc != M2SDR_ERR_OK) {
            sata_print_pcie_dma_error("PCIe DMA benchmark write", rc);
            goto out_close;
        }
        done += chunk;
    }

    done = 0;
    while (done < nsectors) {
        uint32_t chunk = nsectors - done;
        size_t bytes;
        uint64_t bad_offset = 0;
        uint32_t expected = 0;
        uint32_t actual = 0;
        int64_t start_us;
        int rc;

        if (chunk > max_sectors)
            chunk = max_sectors;
        bytes = (size_t)chunk * SATA_SECTOR_BYTES;
        memset(buf, 0, bytes);
        start_us = m2sdr_sata_get_time_us();
        rc = m2sdr_sata_pcie_dma_copy(conn, M2SDR_SATA_DMA_DEVICE_TO_HOST,
            sector + done, chunk, buf, timeout_ms, NULL);
        read_us += m2sdr_sata_get_time_us() - start_us;
        if (rc != M2SDR_ERR_OK) {
            sata_print_pcie_dma_error("PCIe DMA benchmark read", rc);
            goto out_close;
        }
        if (!check_pattern(buf, bytes, (sector + done) * SATA_SECTOR_BYTES,
                           SATA_PATTERN_COUNTER, &bad_offset, &expected, &actual)) {
            fprintf(stderr,
                "Pattern mismatch at byte 0x%016" PRIx64 ": expected=0x%08" PRIx32
                " actual=0x%08" PRIx32 "\n",
                bad_offset, expected, actual);
            goto out_close;
        }
        done += chunk;
    }

    {
        double mib = ((double)nsectors * SATA_SECTOR_BYTES) / (1024.0 * 1024.0);
        double write_mibs = write_us > 0 ? mib * 1000000.0 / (double)write_us : 0.0;
        double read_mibs = read_us > 0 ? mib * 1000000.0 / (double)read_us : 0.0;

        printf("PCIe SATA DMA benchmark: sector=0x%016" PRIx64 " nsectors=%" PRIu32 "\n",
            sector, nsectors);
        printf("  write %.3f MiB/s (%0.3f s)\n", write_mibs, (double)write_us / 1000000.0);
        printf("  read  %.3f MiB/s (%0.3f s)\n", read_mibs, (double)read_us / 1000000.0);
        printf("  verify ok\n");
    }
    status = 0;

out_close:
    m2sdr_close_dev(conn);
    free(buf);
    return status;
}

static int sata_read_to_host_buffer(struct m2sdr_dev *conn, uint64_t sector, uint32_t nsectors, int timeout_ms)
{
    (void)sata_host_buffer_bulk_words(conn);
    sata_sector2mem_program(conn, sector, nsectors, SATA_HOST_BUFFER_BASE);
    m2sdr_write32(conn, CSR_SATA_SECTOR2MEM_START_ADDR, 1);
    return wait_done_quiet("SATA_SECTOR2MEM(diag read)",
        sata_sector2mem_done, sata_sector2mem_error, conn, timeout_ms) == SATA_WAIT_OK ? 0 : 1;
}

static int sata_write_from_host_buffer(void *conn, uint64_t sector, uint32_t nsectors, int timeout_ms)
{
    sata_mem2sector_program(conn, sector, nsectors, SATA_HOST_BUFFER_BASE);
    m2sdr_write32(conn, CSR_SATA_MEM2SECTOR_START_ADDR, 1);
    return wait_done_quiet("SATA_MEM2SECTOR(diag write)",
        sata_mem2sector_done, sata_mem2sector_error, conn, timeout_ms) == SATA_WAIT_OK ? 0 : 1;
}

static FILE *open_stdio_or_file(const char *path, const char *mode, bool *need_close)
{
    FILE *f;

    *need_close = false;
    if (!strcmp(path, "-"))
        return (mode[0] == 'r') ? stdin : stdout;

    f = fopen(path, mode);
    if (!f)
        perror(path);
    else
        *need_close = true;
    return f;
}

static bool sata_try_pcie_dma_default(struct m2sdr_dev *conn)
{
    enum m2sdr_transport_kind transport = M2SDR_TRANSPORT_KIND_UNKNOWN;

    if (m2sdr_get_transport(conn, &transport) != M2SDR_ERR_OK)
        return false;
    return transport == M2SDR_TRANSPORT_KIND_LITEPCIE;
}

static const char *sata_transport_name(struct m2sdr_dev *conn)
{
    enum m2sdr_transport_kind transport = M2SDR_TRANSPORT_KIND_UNKNOWN;

    if (m2sdr_get_transport(conn, &transport) != M2SDR_ERR_OK)
        return NULL;
    switch (transport) {
    case M2SDR_TRANSPORT_KIND_LITEPCIE: return "pcie";
    case M2SDR_TRANSPORT_KIND_LITEETH:  return "ethernet";
    default:                            return NULL;
    }
}

static const char *sata_default_host_destination(void)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    enum m2sdr_transport_kind transport = M2SDR_TRANSPORT_KIND_UNKNOWN;
    const char *dst = NULL;

    if (m2sdr_get_transport(conn, &transport) == M2SDR_ERR_OK) {
        if (transport == M2SDR_TRANSPORT_KIND_LITEPCIE)
            dst = "pcie";
        else if (transport == M2SDR_TRANSPORT_KIND_LITEETH)
            dst = "eth";
    }
    m2sdr_close_dev(conn);
    return dst;
}

static uint32_t sata_file_chunk_sectors(bool try_pcie_dma)
{
    return try_pcie_dma ? M2SDR_SATA_PCIE_DMA_MAX_SECTORS : sata_host_buffer_max_sectors();
}

static void sata_print_pcie_dma_error(const char *op, int rc)
{
    fprintf(stderr, "%s PCIe DMA failed: %s\n", op, m2sdr_strerror(rc));
}

static int sata_read_sectors_to_buffer(struct m2sdr_dev *conn,
                                       uint64_t sector,
                                       uint32_t nsectors,
                                       uint8_t *buf,
                                       int timeout_ms,
                                       bool *try_pcie_dma)
{
    if (try_pcie_dma && *try_pcie_dma) {
        int rc = m2sdr_sata_pcie_dma_copy(conn, M2SDR_SATA_DMA_DEVICE_TO_HOST,
            sector, nsectors, buf, timeout_ms, NULL);
        if (rc == M2SDR_ERR_OK)
            return 0;
        if (rc != M2SDR_ERR_UNSUPPORTED) {
            sata_print_pcie_dma_error("SATA read", rc);
            return 1;
        }
        *try_pcie_dma = false;
    }

    for (uint32_t done = 0; done < nsectors;) {
        uint32_t chunk = nsectors - done;
        size_t bytes;

        if (chunk > sata_host_buffer_max_sectors())
            chunk = sata_host_buffer_max_sectors();
        bytes = (size_t)chunk * SATA_SECTOR_BYTES;
        if (sata_read_to_host_buffer(conn, sector + done, chunk, timeout_ms) != 0)
            return 1;
        sata_host_buffer_read(conn, buf + (size_t)done * SATA_SECTOR_BYTES, bytes);
        done += chunk;
    }
    return 0;
}

static int sata_write_sectors_from_buffer(struct m2sdr_dev *conn,
                                          uint64_t sector,
                                          uint32_t nsectors,
                                          uint8_t *buf,
                                          int timeout_ms,
                                          bool *try_pcie_dma)
{
    if (try_pcie_dma && *try_pcie_dma) {
        int rc = m2sdr_sata_pcie_dma_copy(conn, M2SDR_SATA_DMA_HOST_TO_DEVICE,
            sector, nsectors, buf, timeout_ms, NULL);
        if (rc == M2SDR_ERR_OK)
            return 0;
        if (rc != M2SDR_ERR_UNSUPPORTED) {
            sata_print_pcie_dma_error("SATA write", rc);
            return 1;
        }
        *try_pcie_dma = false;
    }

    for (uint32_t done = 0; done < nsectors;) {
        uint32_t chunk = nsectors - done;
        size_t bytes;

        if (chunk > sata_host_buffer_max_sectors())
            chunk = sata_host_buffer_max_sectors();
        bytes = (size_t)chunk * SATA_SECTOR_BYTES;
        sata_host_buffer_write(conn, buf + (size_t)done * SATA_SECTOR_BYTES, bytes);
        if (sata_write_from_host_buffer(conn, sector + done, chunk, timeout_ms) != 0)
            return 1;
        done += chunk;
    }
    return 0;
}

static int do_read_file(uint64_t src_sector, uint32_t nsectors, const char *path, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    bool try_pcie_dma = sata_try_pcie_dma_default(conn);
    uint32_t max_sectors = sata_file_chunk_sectors(try_pcie_dma);
    uint8_t *buf = malloc((size_t)max_sectors * SATA_SECTOR_BYTES);
    uint32_t done = 0;
    bool close_out = false;
    FILE *out;
    int rc = 1;

    sata_require_csrs();
    if (!buf) {
        fprintf(stderr, "Failed to allocate host buffer.\n");
        goto out_close_dev;
    }

    out = open_stdio_or_file(path, "wb", &close_out);
    if (!out)
        goto out_free;

    while (done < nsectors) {
        uint32_t chunk = nsectors - done;
        size_t bytes;
        if (chunk > max_sectors)
            chunk = max_sectors;
        bytes = (size_t)chunk * SATA_SECTOR_BYTES;

        if (sata_read_sectors_to_buffer(conn, src_sector + done, chunk, buf,
                                        timeout_ms, &try_pcie_dma) != 0)
            goto out_file;
        if (fwrite(buf, 1, bytes, out) != bytes) {
            perror(path);
            goto out_file;
        }
        done += chunk;
    }

    rc = 0;
    printf("Read %" PRIu32 " sectors from 0x%016" PRIx64 " to %s\n",
        nsectors, src_sector, path);

out_file:
    if (close_out)
        fclose(out);
out_free:
    free(buf);
out_close_dev:
    m2sdr_close_dev(conn);
    return rc;
}

static int write_file_to_conn(struct m2sdr_dev *conn, const char *path,
                              uint64_t dst_sector, uint32_t nsectors,
                              bool limit_sectors, int timeout_ms)
{
    bool try_pcie_dma = sata_try_pcie_dma_default(conn);
    uint32_t max_sectors = sata_file_chunk_sectors(try_pcie_dma);
    uint8_t *buf = malloc((size_t)max_sectors * SATA_SECTOR_BYTES);
    uint32_t done = 0;
    bool close_in = false;
    FILE *in;
    int rc = 1;

    sata_require_csrs();
    if (!buf) {
        fprintf(stderr, "Failed to allocate host buffer.\n");
        return 1;
    }

    in = open_stdio_or_file(path, "rb", &close_in);
    if (!in)
        goto out_free;

    for (;;) {
        uint32_t chunk = max_sectors;
        size_t bytes;
        size_t got;

        if (limit_sectors) {
            if (done >= nsectors)
                break;
            if (chunk > nsectors - done)
                chunk = nsectors - done;
        }

        bytes = (size_t)chunk * SATA_SECTOR_BYTES;
        memset(buf, 0, bytes);
        got = fread(buf, 1, bytes, in);
        if (got == 0 && !limit_sectors) {
            if (ferror(in))
                perror(path);
            else
                rc = 0;
            break;
        }
        if (ferror(in)) {
            perror(path);
            goto out_file;
        }
        if (!limit_sectors) {
            chunk = (uint32_t)((got + SATA_SECTOR_BYTES - 1u) / SATA_SECTOR_BYTES);
            bytes = (size_t)chunk * SATA_SECTOR_BYTES;
        }

        if (sata_write_sectors_from_buffer(conn, dst_sector + done, chunk, buf,
                                           timeout_ms, &try_pcie_dma) != 0)
            goto out_file;
        done += chunk;

        if (!limit_sectors && got < (size_t)max_sectors * SATA_SECTOR_BYTES) {
            rc = 0;
            break;
        }
    }

    if (limit_sectors)
        rc = 0;
    if (rc == 0)
        printf("Wrote %" PRIu32 " sectors to 0x%016" PRIx64 " from %s\n",
            done, dst_sector, path);

out_file:
    if (close_in)
        fclose(in);
out_free:
    free(buf);
    return rc;
}

static int do_write_file(const char *path, uint64_t dst_sector, uint32_t nsectors,
                         bool limit_sectors, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    int rc;

    rc = write_file_to_conn(conn, path, dst_sector, nsectors, limit_sectors, timeout_ms);
    m2sdr_close_dev(conn);
    return rc;
}

static int do_write_pattern(uint64_t dst_sector, uint32_t nsectors,
                            enum sata_pattern_kind pattern, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    bool try_pcie_dma = sata_try_pcie_dma_default(conn);
    uint32_t max_sectors = sata_file_chunk_sectors(try_pcie_dma);
    uint8_t *buf = malloc((size_t)max_sectors * SATA_SECTOR_BYTES);
    uint32_t done = 0;
    int rc = 1;

    sata_require_csrs();
    if (!buf) {
        fprintf(stderr, "Failed to allocate host buffer.\n");
        goto out_close_dev;
    }

    while (done < nsectors) {
        uint32_t chunk = nsectors - done;
        size_t bytes;
        if (chunk > max_sectors)
            chunk = max_sectors;
        bytes = (size_t)chunk * SATA_SECTOR_BYTES;

        fill_pattern(buf, bytes, (dst_sector + done) * SATA_SECTOR_BYTES, pattern);
        if (sata_write_sectors_from_buffer(conn, dst_sector + done, chunk, buf,
                                           timeout_ms, &try_pcie_dma) != 0)
            goto out_free;
        done += chunk;
    }

    rc = 0;
    printf("Wrote pattern to %" PRIu32 " sectors at 0x%016" PRIx64 "\n",
        nsectors, dst_sector);

out_free:
    free(buf);
out_close_dev:
    m2sdr_close_dev(conn);
    return rc;
}

static int do_verify_pattern(uint64_t src_sector, uint32_t nsectors,
                             enum sata_pattern_kind pattern, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    bool try_pcie_dma = sata_try_pcie_dma_default(conn);
    uint32_t max_sectors = sata_file_chunk_sectors(try_pcie_dma);
    uint8_t *buf = malloc((size_t)max_sectors * SATA_SECTOR_BYTES);
    uint32_t done = 0;
    int rc = 1;

    sata_require_csrs();
    if (!buf) {
        fprintf(stderr, "Failed to allocate host buffer.\n");
        goto out_close_dev;
    }

    while (done < nsectors) {
        uint32_t chunk = nsectors - done;
        size_t bytes;
        uint64_t bad_offset = 0;
        uint32_t expected = 0;
        uint32_t actual = 0;

        if (chunk > max_sectors)
            chunk = max_sectors;
        bytes = (size_t)chunk * SATA_SECTOR_BYTES;

        if (sata_read_sectors_to_buffer(conn, src_sector + done, chunk, buf,
                                        timeout_ms, &try_pcie_dma) != 0)
            goto out_free;
        if (!check_pattern(buf, bytes, (src_sector + done) * SATA_SECTOR_BYTES,
                           pattern, &bad_offset, &expected, &actual)) {
            fprintf(stderr,
                "Pattern mismatch at byte 0x%016" PRIx64 ": expected=0x%08" PRIx32
                " actual=0x%08" PRIx32 "\n",
                bad_offset, expected, actual);
            goto out_free;
        }
        done += chunk;
    }

    rc = 0;
    printf("Verified pattern over %" PRIu32 " sectors at 0x%016" PRIx64 "\n",
        nsectors, src_sector);

out_free:
    free(buf);
out_close_dev:
    m2sdr_close_dev(conn);
    return rc;
}

/* Named capture catalog storage -------------------------------------------- */

static int sata_read_to_host_buffer_quiet(struct m2sdr_dev *conn, uint64_t sector, uint32_t nsectors, int timeout_ms)
{
    (void)sata_host_buffer_bulk_words(conn);
    sata_sector2mem_program(conn, sector, nsectors, SATA_HOST_BUFFER_BASE);
    m2sdr_write32(conn, CSR_SATA_SECTOR2MEM_START_ADDR, 1);
    return wait_done_quiet("SATA_SECTOR2MEM(catalog)",
        sata_sector2mem_done, sata_sector2mem_error, conn, timeout_ms) == SATA_WAIT_OK ? 0 : 1;
}

static int sata_write_from_host_buffer_quiet(void *conn, uint64_t sector, uint32_t nsectors, int timeout_ms)
{
    sata_mem2sector_program(conn, sector, nsectors, SATA_HOST_BUFFER_BASE);
    m2sdr_write32(conn, CSR_SATA_MEM2SECTOR_START_ADDR, 1);
    return wait_done_quiet("SATA_MEM2SECTOR(catalog)",
        sata_mem2sector_done, sata_mem2sector_error, conn, timeout_ms) == SATA_WAIT_OK ? 0 : 1;
}

static int catalog_buffer_check(void)
{
    if (SATA_CATALOG_SECTORS > sata_host_buffer_max_sectors()) {
        fprintf(stderr, "Catalog region is larger than SATA host buffer.\n");
        return 1;
    }
    return 0;
}

static int catalog_load_from_conn(void *conn, struct sata_catalog *cat, int timeout_ms)
{
    uint32_t bytes = SATA_CATALOG_SECTORS * SATA_SECTOR_BYTES;
    uint8_t *buf;
    char *text;
    int rc = 1;

    if (catalog_buffer_check() != 0)
        return 1;

    buf = calloc(1, bytes + 1u);
    if (!buf) {
        fprintf(stderr, "Failed to allocate catalog buffer.\n");
        return 1;
    }
    if (sata_read_to_host_buffer_quiet(conn, SATA_CATALOG_SECTOR, SATA_CATALOG_SECTORS, timeout_ms) != 0)
        goto out;
    sata_host_buffer_read(conn, buf, bytes);
    text = (char *)buf;
    text[bytes] = '\0';

    if (catalog_parse_text(cat, text) != 0) {
        fprintf(stderr, "Invalid SATA catalog entry.\n");
        goto out;
    }
    rc = 0;

out:
    free(buf);
    return rc;
}

static int catalog_save_to_conn(void *conn, const struct sata_catalog *cat, int timeout_ms)
{
    uint32_t bytes = SATA_CATALOG_SECTORS * SATA_SECTOR_BYTES;
    uint8_t *buf;
    int rc = 1;

    if (catalog_buffer_check() != 0)
        return 1;
    buf = calloc(1, bytes);
    if (!buf) {
        fprintf(stderr, "Failed to allocate catalog buffer.\n");
        return 1;
    }

    if (catalog_format_text(cat, (char *)buf, bytes) != 0) {
        fprintf(stderr, "SATA catalog is full.\n");
        goto out;
    }

    sata_host_buffer_write(conn, buf, bytes);
    if (sata_write_from_host_buffer_quiet(conn, SATA_CATALOG_SECTOR, SATA_CATALOG_SECTORS, timeout_ms) != 0)
        goto out;
    rc = 0;

out:
    free(buf);
    return rc;
}

static int catalog_load(struct sata_catalog *cat, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    int rc;

    sata_require_csrs();
    rc = catalog_load_from_conn(conn, cat, timeout_ms);
    m2sdr_close_dev(conn);
    return rc;
}

static int catalog_save(const struct sata_catalog *cat, int timeout_ms)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    int rc;

    sata_require_csrs();
    rc = catalog_save_to_conn(conn, cat, timeout_ms);
    m2sdr_close_dev(conn);
    return rc;
}

static int catalog_require(struct sata_catalog *cat, int timeout_ms)
{
    if (catalog_load(cat, timeout_ms) != 0)
        return 1;
    if (!cat->initialized) {
        fprintf(stderr, "SATA catalog is not initialized. Run: m2sdr_sata init\n");
        return 1;
    }
    return 0;
}

static int do_catalog_init(int timeout_ms)
{
    struct sata_catalog cat;
    struct m2sdr_dev *conn;
    int rc;

    catalog_clear(&cat);
    conn = m2sdr_open_dev();
    sata_require_csrs();
    rc = catalog_save_to_conn(conn, &cat, timeout_ms);
    m2sdr_close_dev(conn);
    if (rc != 0)
        return 1;
    printf("Initialized SATA catalog at sector 0x%016" PRIx64 " (%u sectors).\n",
        (uint64_t)SATA_CATALOG_SECTOR, SATA_CATALOG_SECTORS);
    return 0;
}

static int do_catalog_list(int timeout_ms)
{
    struct sata_catalog cat;
    int count = 0;

    if (catalog_require(&cat, timeout_ms) != 0)
        return 1;

    printf("%-24s %-18s %-10s %-12s %-8s %-8s\n",
        "NAME", "SECTOR", "SECTORS", "RATE", "FORMAT", "CHANS");
    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        const struct sata_capture_entry *e = &cat.entries[i];
        if (!e->used)
            continue;
        printf("%-24s 0x%016" PRIx64 " %-10" PRIu32 " %-12" PRId64 " %-8s %-8s\n",
            e->name, e->sector, e->nsectors, e->sample_rate, e->format, e->channel_layout);
        count++;
    }
    if (count == 0)
        printf("(empty)\n");
    return 0;
}

static int do_catalog_show(const char *name, int timeout_ms)
{
    struct sata_catalog cat;
    struct sata_capture_entry *e;

    if (catalog_require(&cat, timeout_ms) != 0)
        return 1;
    e = catalog_find(&cat, name);
    if (!e) {
        fprintf(stderr, "Capture '%s' not found.\n", name);
        return 1;
    }
    catalog_entry_print(e);
    return 0;
}

static int do_catalog_delete(const char *name, int timeout_ms)
{
    struct sata_catalog cat;
    struct sata_capture_entry *e;
    struct m2sdr_dev *conn;
    int rc;

    conn = m2sdr_open_dev();
    sata_require_csrs();
    rc = catalog_load_from_conn(conn, &cat, timeout_ms);
    if (rc != 0) {
        m2sdr_close_dev(conn);
        return 1;
    }
    if (!cat.initialized) {
        fprintf(stderr, "SATA catalog is not initialized. Run: m2sdr_sata init\n");
        m2sdr_close_dev(conn);
        return 1;
    }
    e = catalog_find(&cat, name);
    if (!e) {
        fprintf(stderr, "Capture '%s' not found.\n", name);
        m2sdr_close_dev(conn);
        return 1;
    }
    memset(e, 0, sizeof(*e));
    rc = catalog_save_to_conn(conn, &cat, timeout_ms);
    m2sdr_close_dev(conn);
    if (rc != 0)
        return 1;
    printf("Deleted capture '%s' from catalog. Data sectors were not erased.\n", name);
    return 0;
}

static int do_catalog_fsck(int timeout_ms)
{
    struct sata_catalog cat;
    int errors = 0;

    if (catalog_require(&cat, timeout_ms) != 0)
        return 1;
    for (int i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++) {
        const struct sata_capture_entry *a = &cat.entries[i];
        if (!a->used)
            continue;
        if (!catalog_name_valid(a->name) || a->nsectors == 0 || a->sector < SATA_DATA_START) {
            fprintf(stderr, "Invalid entry: %s\n", a->name);
            errors++;
        }
        if (a->meta_nsectors != 0 &&
            (a->meta_sector < SATA_DATA_START ||
             a->meta_bytes > (uint64_t)a->meta_nsectors * SATA_SECTOR_BYTES)) {
            fprintf(stderr, "Invalid SigMF metadata region: %s\n", a->name);
            errors++;
        }
        if (a->meta_nsectors != 0 &&
            catalog_regions_overlap(a->sector, a->nsectors, a->meta_sector, a->meta_nsectors)) {
            fprintf(stderr, "Self-overlap between data and metadata: %s\n", a->name);
            errors++;
        }
        for (int j = i + 1; j < SATA_CATALOG_MAX_ENTRIES; j++) {
            const struct sata_capture_entry *b = &cat.entries[j];
            if (!b->used)
                continue;
            if (strcmp(a->name, b->name) == 0) {
                fprintf(stderr, "Duplicate name: %s\n", a->name);
                errors++;
            }
            if (catalog_regions_overlap(a->sector, a->nsectors, b->sector, b->nsectors)) {
                fprintf(stderr, "Overlap: %s and %s\n", a->name, b->name);
                errors++;
            }
            if (a->meta_nsectors != 0 &&
                catalog_regions_overlap(a->meta_sector, a->meta_nsectors, b->sector, b->nsectors)) {
                fprintf(stderr, "Metadata/data overlap: %s and %s\n", a->name, b->name);
                errors++;
            }
            if (b->meta_nsectors != 0 &&
                catalog_regions_overlap(a->sector, a->nsectors, b->meta_sector, b->meta_nsectors)) {
                fprintf(stderr, "Data/metadata overlap: %s and %s\n", a->name, b->name);
                errors++;
            }
            if (a->meta_nsectors != 0 && b->meta_nsectors != 0 &&
                catalog_regions_overlap(a->meta_sector, a->meta_nsectors,
                                        b->meta_sector, b->meta_nsectors)) {
                fprintf(stderr, "Metadata overlap: %s and %s\n", a->name, b->name);
                errors++;
            }
        }
    }
    if (errors == 0) {
        printf("SATA catalog OK.\n");
        return 0;
    }
    fprintf(stderr, "SATA catalog has %d error(s).\n", errors);
    return 1;
}

static void catalog_entry_get_sigmf_metadata(const struct sata_capture_entry *entry,
                                             struct m2sdr_sigmf_meta *meta,
                                             int timeout_ms);

static int export_entry_data(const struct sata_capture_entry *e, const char *path, int timeout_ms)
{
    struct m2sdr_dev *conn;
    uint32_t max_sectors;
    uint8_t *buf;
    uint32_t done = 0;
    uint64_t remaining;
    bool try_pcie_dma;
    bool close_out = false;
    FILE *out = NULL;
    int rc = 1;

    if (!e)
        return 1;

    remaining = e->bytes ? e->bytes : (uint64_t)e->nsectors * SATA_SECTOR_BYTES;
    conn = m2sdr_open_dev();
    sata_require_csrs();
    try_pcie_dma = sata_try_pcie_dma_default(conn);
    max_sectors = sata_file_chunk_sectors(try_pcie_dma);
    buf = malloc((size_t)max_sectors * SATA_SECTOR_BYTES);
    if (!buf) {
        fprintf(stderr, "Failed to allocate export buffer.\n");
        goto out_close_dev;
    }
    out = open_stdio_or_file(path, "wb", &close_out);
    if (!out)
        goto out_close_dev;

    while (done < e->nsectors && remaining > 0) {
        uint32_t chunk = e->nsectors - done;
        size_t chunk_bytes;
        size_t write_bytes;

        if (chunk > max_sectors)
            chunk = max_sectors;
        chunk_bytes = (size_t)chunk * SATA_SECTOR_BYTES;
        if (sata_read_sectors_to_buffer(conn, e->sector + done, chunk, buf,
                                        timeout_ms, &try_pcie_dma) != 0)
            goto out_close_dev;
        write_bytes = remaining < chunk_bytes ? (size_t)remaining : chunk_bytes;
        if (fwrite(buf, 1, write_bytes, out) != write_bytes) {
            perror(path);
            goto out_close_dev;
        }
        remaining -= write_bytes;
        done += chunk;
    }
    rc = 0;

out_close_dev:
    m2sdr_close_dev(conn);
    if (close_out)
        fclose(out);
    free(buf);
    return rc;
}

static int do_export_capture(const char *name, const char *path, int timeout_ms)
{
    struct sata_catalog cat;
    struct sata_capture_entry *e;

    if (catalog_require(&cat, timeout_ms) != 0)
        return 1;
    e = catalog_find(&cat, name);
    if (!e) {
        fprintf(stderr, "Capture '%s' not found.\n", name);
        return 1;
    }
    if (export_entry_data(e, path, timeout_ms) != 0)
        return 1;
    printf("Exported '%s' to %s (%" PRIu64 " bytes).\n",
        name, path, e->bytes ? e->bytes : (uint64_t)e->nsectors * SATA_SECTOR_BYTES);
    return 0;
}

static int do_export_sigmf_capture(const char *name, const char *path, int timeout_ms)
{
    struct sata_catalog cat;
    struct sata_capture_entry *e;
    struct m2sdr_sigmf_meta meta;
    char data_path[1024];
    char meta_path[1024];
    char transport[32];

    if (catalog_require(&cat, timeout_ms) != 0)
        return 1;
    e = catalog_find(&cat, name);
    if (!e) {
        fprintf(stderr, "Capture '%s' not found.\n", name);
        return 1;
    }
    if (m2sdr_sigmf_derive_paths(path, data_path, sizeof(data_path),
                                 meta_path, sizeof(meta_path)) != 0) {
        fprintf(stderr, "Could not derive SigMF paths from %s\n", path);
        return 1;
    }

    if (export_entry_data(e, data_path, timeout_ms) != 0)
        return 1;

    catalog_entry_get_sigmf_metadata(e, &meta, timeout_ms);
    snprintf(transport, sizeof(transport), "%s",
        meta.m2sdr_transport[0] ? meta.m2sdr_transport : "sata");
    snprintf(meta.data_path, sizeof(meta.data_path), "%s", data_path);
    snprintf(meta.meta_path, sizeof(meta.meta_path), "%s", meta_path);
    m2sdr_sata_sigmf_set_storage(&meta,
        e->sector, e->nsectors, e->bytes,
        e->meta_sector, e->meta_nsectors, e->meta_bytes,
        transport);
    if (m2sdr_sigmf_write(&meta) != 0) {
        fprintf(stderr, "Failed to write SigMF metadata: %s\n", meta_path);
        return 1;
    }

    printf("Exported SigMF '%s' to %s + %s (%" PRIu64 " bytes).\n",
        name, meta_path, data_path,
        e->bytes ? e->bytes : (uint64_t)e->nsectors * SATA_SECTOR_BYTES);
    return 0;
}

static uint64_t file_size_bytes(const char *path)
{
    struct stat st;

    if (!path || !strcmp(path, "-")) {
        fprintf(stderr, "Named import requires a regular file path, not '-'.\n");
        exit(1);
    }
    if (stat(path, &st) != 0) {
        perror(path);
        exit(1);
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "%s is not a regular file.\n", path);
        exit(1);
    }
    return (uint64_t)st.st_size;
}

struct named_transfer_options {
    bool have_sector;
    uint64_t sector;
    int64_t sample_rate;
    enum m2sdr_format format;
    enum m2sdr_channel_layout channel_layout;
    uint64_t rx_freq;
    uint64_t tx_freq;
    uint64_t bandwidth;
    int64_t rx_gain;
    int64_t tx_att;
    char notes[SATA_CAPTURE_NOTES_MAX];
};

static void named_transfer_options_init(struct named_transfer_options *opts)
{
    struct m2sdr_config cfg;

    memset(opts, 0, sizeof(*opts));
    m2sdr_config_init(&cfg);
    opts->sample_rate = cfg.sample_rate;
    opts->format = M2SDR_FORMAT_SC16_Q11;
    opts->channel_layout = cfg.channel_layout;
    opts->rx_freq = (uint64_t)cfg.rx_freq;
    opts->tx_freq = (uint64_t)cfg.tx_freq;
    opts->bandwidth = (uint64_t)cfg.bandwidth;
    opts->rx_gain = cfg.rx_gain1;
    opts->tx_att = cfg.tx_att;
}

static int parse_named_option(struct named_transfer_options *opts, int argc, char **argv, int *index)
{
    const char *opt = argv[*index];
    const char *value = NULL;

    if (strncmp(opt, "--", 2) != 0)
        return 0;
    if (*index + 1 >= argc) {
        fprintf(stderr, "Missing value for %s\n", opt);
        exit(1);
    }
    value = argv[++(*index)];

    if (!strcmp(opt, "--sector")) {
        opts->sector = parse_u64(value);
        opts->have_sector = true;
    } else if (!strcmp(opt, "--sample-rate") || !strcmp(opt, "--samplerate")) {
        opts->sample_rate = parse_i64("sample rate", value);
    } else if (!strcmp(opt, "--format")) {
        if (m2sdr_cli_parse_format(value, &opts->format) != 0) {
            m2sdr_cli_invalid_choice("format", value, "sc16 or sc8");
            exit(1);
        }
    } else if (!strcmp(opt, "--channel-layout") || !strcmp(opt, "--chan")) {
        opts->channel_layout = parse_channel_layout(value);
    } else if (!strcmp(opt, "--rx-freq") || !strcmp(opt, "--rx_freq")) {
        opts->rx_freq = (uint64_t)parse_i64("RX frequency", value);
    } else if (!strcmp(opt, "--tx-freq") || !strcmp(opt, "--tx_freq")) {
        opts->tx_freq = (uint64_t)parse_i64("TX frequency", value);
    } else if (!strcmp(opt, "--bandwidth")) {
        opts->bandwidth = (uint64_t)parse_i64("bandwidth", value);
    } else if (!strcmp(opt, "--rx-gain") || !strcmp(opt, "--rx_gain")) {
        opts->rx_gain = parse_i64("RX gain", value);
    } else if (!strcmp(opt, "--tx-att") || !strcmp(opt, "--tx_att")) {
        opts->tx_att = parse_i64("TX attenuation", value);
    } else if (!strcmp(opt, "--notes")) {
        if (!catalog_text_valid(value, sizeof(opts->notes))) {
            fprintf(stderr, "Notes are too long or contain '|'/newline.\n");
            exit(1);
        }
        catalog_copy(opts->notes, sizeof(opts->notes), value);
    } else {
        fprintf(stderr, "Unknown option: %s\n", opt);
        exit(1);
    }
    return 1;
}

static void catalog_entry_from_options(struct sata_capture_entry *e,
                                       const char *name,
                                       const struct named_transfer_options *opts,
                                       uint64_t sector,
                                       uint32_t nsectors,
                                       uint64_t bytes,
                                       uint64_t meta_sector,
                                       uint32_t meta_nsectors,
                                       uint64_t meta_bytes)
{
    memset(e, 0, sizeof(*e));
    e->used = true;
    catalog_copy(e->name, sizeof(e->name), name);
    e->sector = sector;
    e->nsectors = nsectors;
    e->bytes = bytes;
    e->meta_sector = meta_sector;
    e->meta_nsectors = meta_nsectors;
    e->meta_bytes = meta_bytes;
    e->sample_rate = opts->sample_rate;
    catalog_copy(e->format, sizeof(e->format), format_name(opts->format));
    catalog_copy(e->channel_layout, sizeof(e->channel_layout), channel_layout_name(opts->channel_layout));
    e->rx_freq = opts->rx_freq;
    e->tx_freq = opts->tx_freq;
    e->bandwidth = opts->bandwidth;
    e->rx_gain = opts->rx_gain;
    e->tx_att = opts->tx_att;
    e->created = (uint64_t)time(NULL);
    catalog_copy(e->notes, sizeof(e->notes), opts->notes);
}

static int catalog_assign_new_storage(struct sata_catalog *cat,
                                      const char *name,
                                      bool have_sector,
                                      uint64_t requested_sector,
                                      uint32_t data_nsectors,
                                      uint64_t *data_sector,
                                      uint64_t *meta_sector)
{
    if (data_nsectors > UINT32_MAX - M2SDR_SATA_SIGMF_META_SECTORS) {
        fprintf(stderr, "Capture is too large to reserve SigMF metadata sectors.\n");
        return 1;
    }

    *data_sector = have_sector ? requested_sector :
        catalog_alloc_sector(cat, data_nsectors + M2SDR_SATA_SIGMF_META_SECTORS);
    *meta_sector = *data_sector + data_nsectors;
    return catalog_validate_new_storage(cat, name,
        *data_sector, data_nsectors,
        *meta_sector, M2SDR_SATA_SIGMF_META_SECTORS);
}

static int sata_write_sigmf_metadata_to_conn(struct m2sdr_dev *conn,
                                             struct m2sdr_sigmf_meta *meta,
                                             uint64_t data_sector,
                                             uint32_t data_nsectors,
                                             uint64_t data_bytes,
                                             uint64_t meta_sector,
                                             uint32_t meta_nsectors,
                                             int timeout_ms,
                                             uint64_t *meta_bytes)
{
    uint64_t capacity = (uint64_t)meta_nsectors * SATA_SECTOR_BYTES;
    uint64_t last_bytes = UINT64_MAX;
    uint8_t *buf;
    bool try_pcie_dma;
    int rc = 1;

    if (capacity == 0 || capacity > SIZE_MAX) {
        fprintf(stderr, "Invalid SigMF metadata region size.\n");
        return 1;
    }

    buf = calloc(1, (size_t)capacity);
    if (!buf) {
        fprintf(stderr, "Failed to allocate SigMF metadata buffer.\n");
        return 1;
    }

    if (meta_bytes)
        *meta_bytes = 0;
    for (unsigned pass = 0; pass < 4; pass++) {
        uint64_t current_bytes;

        memset(buf, 0, (size_t)capacity);
        m2sdr_sata_sigmf_set_storage(meta,
            data_sector, data_nsectors, data_bytes,
            meta_sector, meta_nsectors,
            meta_bytes ? *meta_bytes : 0,
            sata_transport_name(conn));
        if (m2sdr_sigmf_write_text(meta, (char *)buf, (size_t)capacity) != 0) {
            fprintf(stderr, "SigMF metadata does not fit in %" PRIu32 " SATA sectors.\n",
                meta_nsectors);
            goto out;
        }
        current_bytes = strlen((char *)buf);
        if (meta_bytes)
            *meta_bytes = current_bytes;
        if (current_bytes == last_bytes)
            break;
        last_bytes = current_bytes;
    }

    try_pcie_dma = sata_try_pcie_dma_default(conn);
    if (sata_write_sectors_from_buffer(conn, meta_sector, meta_nsectors, buf,
                                       timeout_ms, &try_pcie_dma) != 0)
        goto out;
    rc = 0;

out:
    free(buf);
    return rc;
}

static int catalog_entry_write_sigmf_metadata_to_conn(struct m2sdr_dev *conn,
                                                      struct sata_capture_entry *entry,
                                                      const char *name,
                                                      int timeout_ms)
{
    struct m2sdr_sigmf_meta meta;

    if (!entry || entry->meta_nsectors == 0)
        return 0;

    m2sdr_sata_sigmf_from_entry(&meta, name, entry);
    return sata_write_sigmf_metadata_to_conn(conn, &meta,
        entry->sector, entry->nsectors, entry->bytes,
        entry->meta_sector, entry->meta_nsectors,
        timeout_ms, &entry->meta_bytes);
}

static int catalog_entry_write_sigmf_metadata(struct sata_capture_entry *entry,
                                              const char *name,
                                              int timeout_ms)
{
    struct m2sdr_dev *conn;
    int rc;

    if (!entry || entry->meta_nsectors == 0)
        return 0;

    conn = m2sdr_open_dev();
    sata_require_csrs();
    rc = catalog_entry_write_sigmf_metadata_to_conn(conn, entry, name, timeout_ms);
    m2sdr_close_dev(conn);
    return rc;
}

static int catalog_entry_read_sigmf_metadata_from_conn(struct m2sdr_dev *conn,
                                                       const struct sata_capture_entry *entry,
                                                       struct m2sdr_sigmf_meta *meta,
                                                       int timeout_ms)
{
    uint64_t capacity;
    uint8_t *buf;
    bool try_pcie_dma;
    size_t text_len;
    char hint[256];
    int rc = 1;

    if (!entry || !meta || entry->meta_nsectors == 0)
        return -1;

    capacity = (uint64_t)entry->meta_nsectors * SATA_SECTOR_BYTES;
    if (capacity == 0 || capacity > SIZE_MAX - 1u)
        return -1;

    buf = calloc(1, (size_t)capacity + 1u);
    if (!buf)
        return -1;
    try_pcie_dma = sata_try_pcie_dma_default(conn);
    if (sata_read_sectors_to_buffer(conn, entry->meta_sector, entry->meta_nsectors,
                                    buf, timeout_ms, &try_pcie_dma) != 0)
        goto out;

    if (entry->meta_bytes != 0 && entry->meta_bytes <= capacity)
        text_len = (size_t)entry->meta_bytes;
    else
        text_len = strnlen((char *)buf, (size_t)capacity);
    if (text_len == 0)
        goto out;

    snprintf(hint, sizeof(hint), "%s.sigmf-meta", entry->name);
    if (m2sdr_sigmf_read_text((char *)buf, text_len, hint, meta) != 0)
        goto out;
    rc = 0;

out:
    free(buf);
    return rc;
}

static int catalog_entry_read_sigmf_metadata(const struct sata_capture_entry *entry,
                                             struct m2sdr_sigmf_meta *meta,
                                             int timeout_ms)
{
    struct m2sdr_dev *conn;
    int rc;

    if (!entry || !meta || entry->meta_nsectors == 0)
        return -1;

    conn = m2sdr_open_dev();
    sata_require_csrs();
    rc = catalog_entry_read_sigmf_metadata_from_conn(conn, entry, meta, timeout_ms);
    m2sdr_close_dev(conn);
    return rc;
}

static void catalog_entry_get_sigmf_metadata(const struct sata_capture_entry *entry,
                                             struct m2sdr_sigmf_meta *meta,
                                             int timeout_ms)
{
    if (catalog_entry_read_sigmf_metadata(entry, meta, timeout_ms) != 0)
        m2sdr_sata_sigmf_from_entry(meta, entry ? entry->name : NULL, entry);
}

static int do_import_capture(const char *name, const char *path, int argc, char **argv,
                             int argi, int timeout_ms, bool dry_run)
{
    struct sata_catalog cat;
    struct named_transfer_options opts;
    struct sata_capture_entry entry;
    uint64_t bytes = file_size_bytes(path);
    uint32_t nsectors;
    uint64_t sector;
    uint64_t meta_sector;
    struct m2sdr_dev *conn = NULL;
    int rc = 1;

    if (bytes == 0) {
        fprintf(stderr, "%s is empty.\n", path);
        return 1;
    }
    nsectors = (uint32_t)((bytes + SATA_SECTOR_BYTES - 1u) / SATA_SECTOR_BYTES);
    if ((uint64_t)nsectors * SATA_SECTOR_BYTES < bytes) {
        fprintf(stderr, "Import file is too large.\n");
        return 1;
    }

    named_transfer_options_init(&opts);
    while (argi < argc) {
        if (!parse_named_option(&opts, argc, argv, &argi)) {
            fprintf(stderr, "Unexpected argument: %s\n", argv[argi]);
            return 1;
        }
        argi++;
    }

    conn = m2sdr_open_dev();
    sata_require_csrs();
    if (catalog_load_from_conn(conn, &cat, timeout_ms) != 0)
        goto out_close_dev;
    if (!cat.initialized)
        catalog_clear(&cat);
    if (catalog_assign_new_storage(&cat, name, opts.have_sector, opts.sector,
                                   nsectors, &sector, &meta_sector) != 0)
        goto out_close_dev;
    if (dry_run) {
        printf("import dry-run: name=%s path=%s sector=0x%016" PRIx64
               " nsectors=%" PRIu32 " bytes=%" PRIu64
               " sigmf_sector=0x%016" PRIx64 " sigmf_nsectors=%u\n",
            name, path, sector, nsectors, bytes,
            meta_sector, M2SDR_SATA_SIGMF_META_SECTORS);
        rc = 0;
        goto out_close_dev;
    }

    if (write_file_to_conn(conn, path, sector, nsectors, true, timeout_ms) != 0)
        goto out_close_dev;
    catalog_entry_from_options(&entry, name, &opts, sector, nsectors, bytes,
        meta_sector, M2SDR_SATA_SIGMF_META_SECTORS, 0);
    if (catalog_entry_write_sigmf_metadata_to_conn(conn, &entry, name, timeout_ms) != 0)
        goto out_close_dev;
    if (catalog_add_entry(&cat, &entry) != 0)
        goto out_close_dev;
    if (catalog_save_to_conn(conn, &cat, timeout_ms) != 0)
        goto out_close_dev;
    printf("Imported '%s' at sector 0x%016" PRIx64 " (%" PRIu32 " sectors).\n",
        name, sector, nsectors);
    rc = 0;

out_close_dev:
    if (conn)
        m2sdr_close_dev(conn);
    return rc;
}

static int do_import_sigmf_capture(const char *name, const char *input_path,
                                   int argc, char **argv, int argi,
                                   int timeout_ms, bool dry_run)
{
    struct sata_catalog cat;
    struct m2sdr_sigmf_meta meta;
    struct sata_capture_entry entry;
    char source_data_path[1024];
    uint64_t bytes;
    uint64_t meta_bytes = 0;
    uint32_t nsectors;
    uint64_t sector;
    uint64_t meta_sector;
    bool have_sector = false;
    uint64_t requested_sector = 0;
    struct m2sdr_dev *conn = NULL;
    int rc = 1;

    while (argi < argc) {
        if (!strcmp(argv[argi], "--sector")) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "Missing value for --sector\n");
                return 1;
            }
            requested_sector = parse_u64(argv[++argi]);
            have_sector = true;
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", argv[argi]);
            return 1;
        }
        argi++;
    }

    if (m2sdr_sigmf_read(input_path, &meta) != 0) {
        fprintf(stderr, "Could not read SigMF metadata from %s\n", input_path);
        return 1;
    }
    if (m2sdr_sigmf_format_from_datatype(meta.datatype) == (enum m2sdr_format)-1) {
        fprintf(stderr, "Unsupported SigMF datatype '%s'.\n", meta.datatype);
        return 1;
    }
    snprintf(source_data_path, sizeof(source_data_path), "%s", meta.data_path);
    bytes = file_size_bytes(source_data_path);
    if (bytes == 0) {
        fprintf(stderr, "%s is empty.\n", source_data_path);
        return 1;
    }
    nsectors = m2sdr_sata_bytes_to_sectors(bytes);
    if ((uint64_t)nsectors * SATA_SECTOR_BYTES < bytes) {
        fprintf(stderr, "SigMF dataset is too large.\n");
        return 1;
    }

    conn = m2sdr_open_dev();
    sata_require_csrs();
    if (catalog_load_from_conn(conn, &cat, timeout_ms) != 0)
        goto out_close_dev;
    if (!cat.initialized)
        catalog_clear(&cat);
    if (catalog_assign_new_storage(&cat, name, have_sector, requested_sector,
                                   nsectors, &sector, &meta_sector) != 0)
        goto out_close_dev;
    if (dry_run) {
        printf("import dry-run: name=%s meta=%s data=%s sector=0x%016" PRIx64
               " nsectors=%" PRIu32 " bytes=%" PRIu64
               " sigmf_sector=0x%016" PRIx64 " sigmf_nsectors=%u\n",
            name, input_path, source_data_path, sector, nsectors, bytes,
            meta_sector, M2SDR_SATA_SIGMF_META_SECTORS);
        rc = 0;
        goto out_close_dev;
    }

    if (write_file_to_conn(conn, source_data_path, sector, nsectors, true, timeout_ms) != 0)
        goto out_close_dev;

    snprintf(meta.data_path, sizeof(meta.data_path), "%s.sigmf-data", name);
    snprintf(meta.meta_path, sizeof(meta.meta_path), "%s.sigmf-meta", name);
    if (sata_write_sigmf_metadata_to_conn(conn, &meta,
        sector, nsectors, bytes,
        meta_sector, M2SDR_SATA_SIGMF_META_SECTORS,
        timeout_ms, &meta_bytes) != 0)
        goto out_close_dev;

    if (m2sdr_sata_sigmf_entry_from_meta(&entry, name, &meta,
            sector, nsectors, bytes,
            meta_sector, M2SDR_SATA_SIGMF_META_SECTORS,
            meta_bytes) != 0) {
        fprintf(stderr, "Could not convert SigMF metadata to SATA catalog entry.\n");
        goto out_close_dev;
    }
    if (catalog_add_entry(&cat, &entry) != 0)
        goto out_close_dev;
    if (catalog_save_to_conn(conn, &cat, timeout_ms) != 0)
        goto out_close_dev;
    printf("Imported SigMF '%s' at sector 0x%016" PRIx64 " (%" PRIu32 " sectors).\n",
        name, sector, nsectors);
    rc = 0;

out_close_dev:
    if (conn)
        m2sdr_close_dev(conn);
    return rc;
}

static int do_import_auto(const char *name, const char *input_path,
                          int argc, char **argv, int argi,
                          int timeout_ms, bool dry_run)
{
    struct m2sdr_sigmf_meta meta;

    if (m2sdr_sigmf_read(input_path, &meta) == 0)
        return do_import_sigmf_capture(name, input_path, argc, argv, argi, timeout_ms, dry_run);
    return do_import_capture(name, input_path, argc, argv, argi, timeout_ms, dry_run);
}


#else

static int do_read_file(uint64_t src_sector, uint32_t nsectors, const char *path, int timeout_ms)
{
    (void)src_sector; (void)nsectors; (void)path; (void)timeout_ms;
    fprintf(stderr, "SATA host sector I/O requires a gateware build with SATA_HOST_BUFFER.\n");
    return 1;
}

static int do_write_file(const char *path, uint64_t dst_sector, uint32_t nsectors,
                         bool limit_sectors, int timeout_ms)
{
    (void)path; (void)dst_sector; (void)nsectors; (void)limit_sectors; (void)timeout_ms;
    fprintf(stderr, "SATA host sector I/O requires a gateware build with SATA_HOST_BUFFER.\n");
    return 1;
}

static int do_write_pattern(uint64_t dst_sector, uint32_t nsectors,
                            enum sata_pattern_kind pattern, int timeout_ms)
{
    (void)dst_sector; (void)nsectors; (void)pattern; (void)timeout_ms;
    fprintf(stderr, "SATA host sector I/O requires a gateware build with SATA_HOST_BUFFER.\n");
    return 1;
}

static int do_verify_pattern(uint64_t src_sector, uint32_t nsectors,
                             enum sata_pattern_kind pattern, int timeout_ms)
{
    (void)src_sector; (void)nsectors; (void)pattern; (void)timeout_ms;
    fprintf(stderr, "SATA host sector I/O requires a gateware build with SATA_HOST_BUFFER.\n");
    return 1;
}

#endif

static int do_record(uint64_t dst_sector, uint32_t nsectors, int timeout_ms, bool dry_run)
{
    struct sata_operation op = sata_operation_begin();
    struct m2sdr_dev *conn = op.conn;
    int txsrc = (int)m2sdr_read32(conn, CSR_CROSSBAR_MUX_SEL_ADDR);

    /* Normal path: RX -> demux -> SATA_RX_STREAMER. */
    txrx_loopback_set(conn, 0);
    crossbar_set(conn,
        txsrc,
        RXDST_SATA
    );

    sata_rx_program(conn, dst_sector, nsectors);
    if (dry_run) {
        print_planned_transfer("record", UINT64_MAX, dst_sector, nsectors, txsrc, RXDST_SATA, 0, timeout_ms);
        sata_operation_finish(&op);
        return 0;
    }
    sata_rx_start(conn);

    enum sata_wait_result rc =
        wait_done("SATA_RX(record)", sata_rx_done, sata_rx_error, conn, timeout_ms, nsectors);
    sata_operation_finish(&op);
    return rc == SATA_WAIT_OK ? 0 : 1;
}

static int do_record_start(uint64_t dst_sector, uint32_t nsectors, int timeout_ms, bool dry_run)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    int txsrc = (int)m2sdr_read32(conn, CSR_CROSSBAR_MUX_SEL_ADDR);

    sata_require_csrs();
    txrx_loopback_set(conn, 0);
    crossbar_set(conn, txsrc, RXDST_SATA);
    sata_rx_program(conn, dst_sector, nsectors);
    if (dry_run) {
        print_planned_transfer("record-start", UINT64_MAX, dst_sector, nsectors, txsrc, RXDST_SATA, 0, timeout_ms);
        m2sdr_close_dev(conn);
        return 0;
    }
    sata_rx_start(conn);
    printf("SATA_RX(record): started sector=0x%016" PRIx64 " nsectors=%" PRIu32 "\n",
        dst_sector, nsectors);
    m2sdr_close_dev(conn);
    return 0;
}

static int do_play(uint64_t src_sector, uint32_t nsectors, int timeout_ms, bool dry_run)
{
    struct sata_operation op = sata_operation_begin();
    struct m2sdr_dev *conn = op.conn;
    int rxdst = (int)m2sdr_read32(conn, CSR_CROSSBAR_DEMUX_SEL_ADDR);

    /* Normal path: SATA_TX_STREAMER -> mux -> TX. */
    txrx_loopback_set(conn, 0);
    crossbar_set(conn,
        TXSRC_SATA,
        rxdst
    );

    sata_tx_program(conn, src_sector, nsectors);
    if (dry_run) {
        print_planned_transfer("play", src_sector, UINT64_MAX, nsectors, TXSRC_SATA, rxdst, 0, timeout_ms);
        sata_operation_finish(&op);
        return 0;
    }
    sata_tx_start(conn);

    enum sata_wait_result rc =
        wait_done("SATA_TX(play)", sata_tx_done, sata_tx_error, conn, timeout_ms, nsectors);
    sata_operation_finish(&op);
    return rc == SATA_WAIT_OK ? 0 : 1;
}

static int do_play_start(uint64_t src_sector, uint32_t nsectors, int timeout_ms, bool dry_run)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    int rxdst = (int)m2sdr_read32(conn, CSR_CROSSBAR_DEMUX_SEL_ADDR);

    sata_require_csrs();
    txrx_loopback_set(conn, 0);
    crossbar_set(conn, TXSRC_SATA, rxdst);
    sata_tx_program(conn, src_sector, nsectors);
    if (dry_run) {
        print_planned_transfer("play-start", src_sector, UINT64_MAX, nsectors, TXSRC_SATA, rxdst, 0, timeout_ms);
        m2sdr_close_dev(conn);
        return 0;
    }
    sata_tx_start(conn);
    printf("SATA_TX(play): started sector=0x%016" PRIx64 " nsectors=%" PRIu32 "\n",
        src_sector, nsectors);
    m2sdr_close_dev(conn);
    return 0;
}

static int do_replay(uint64_t src_sector, uint32_t nsectors, const char *dst_s, int timeout_ms, bool dry_run)
{
    struct sata_operation op = sata_operation_begin();
    struct m2sdr_dev *conn = op.conn;

    int rxdst = parse_rxdst(dst_s);

    /* SATA -> TX -> loopback -> RX destination. */
    crossbar_set(conn, TXSRC_SATA, rxdst);
    txrx_loopback_set(conn, 1);

    sata_tx_program(conn, src_sector, nsectors);
    if (dry_run) {
        print_planned_transfer("replay", src_sector, UINT64_MAX, nsectors, TXSRC_SATA, rxdst, 1, timeout_ms);
        sata_operation_finish(&op);
        return 0;
    }
    sata_tx_start(conn);

    enum sata_wait_result rc =
        wait_done("SATA_TX(replay)", sata_tx_done, sata_tx_error, conn, timeout_ms, nsectors);
    sata_operation_finish(&op);
    return rc == SATA_WAIT_OK ? 0 : 1;
}

static int do_copy(uint64_t src_sector, uint64_t dst_sector, uint32_t nsectors, int timeout_ms, bool dry_run)
{
    struct sata_operation op = sata_operation_begin();
    struct m2sdr_dev *conn = op.conn;
    enum sata_wait_result tx_rc = SATA_WAIT_OK;
    enum sata_wait_result rx_rc = SATA_WAIT_OK;
    int rc = 0;

    /* SSD -> SSD:
     * SATA_TX_STREAMER -> TX -> loopback -> RX -> SATA_RX_STREAMER.
     *
     * Sequence one sector at a time. The RX streamer stages one full sector
     * before writing it, and issuing the next SATA read before that write has
     * completed can deadlock the shared SATA core during disk-to-disk copies.
     */
    crossbar_set(conn, TXSRC_SATA, RXDST_SATA);
    txrx_loopback_set(conn, 1);

    if (dry_run) {
        print_planned_transfer("copy", src_sector, dst_sector, nsectors, TXSRC_SATA, RXDST_SATA, 1, timeout_ms);
        sata_operation_finish(&op);
        return 0;
    }

    for (uint32_t i = 0; i < nsectors; i++) {
        sata_rx_program(conn, dst_sector + i, 1);
        sata_tx_program(conn, src_sector + i, 1);

        /* Start RX first so the looped-back stream always has a sink. */
        sata_rx_start(conn);
        usleep(5 * 1000);
        sata_tx_start(conn);

        tx_rc = wait_done_quiet("SATA_TX(copy-src)", sata_tx_done, sata_tx_error, conn, timeout_ms);
        if (tx_rc == SATA_WAIT_OK)
            rx_rc = wait_done_quiet("SATA_RX(copy-dst)", sata_rx_done, sata_rx_error, conn, timeout_ms);

        if (tx_rc != SATA_WAIT_OK || rx_rc != SATA_WAIT_OK) {
            sata_streamers_reset(conn, true, true);
            rc = 1;
            break;
        }

        if (nsectors > 1 && (((i + 1) % 64) == 0 || (i + 1) == nsectors))
            printf("SATA_COPY(copy): copied %" PRIu32 "/%" PRIu32 " sectors\n", i + 1, nsectors);
    }

    if (rc == 0)
        printf("SATA_COPY(copy): done\n");

    sata_operation_finish(&op);
    return rc;
}

static int parse_stream_selector(const char *label, const char *text, bool *rx, bool *tx)
{
    if (!text || !rx || !tx)
        return -1;

    *rx = false;
    *tx = false;
    if (!strcmp(text, "rx")) {
        *rx = true;
        return 0;
    }
    if (!strcmp(text, "tx")) {
        *tx = true;
        return 0;
    }
    if (!strcmp(text, "both")) {
        *rx = true;
        *tx = true;
        return 0;
    }

    m2sdr_cli_invalid_choice(label, text, "rx, tx, or both");
    return -1;
}

static int do_stream_stop(const char *which_s)
{
    bool rx = false;
    bool tx = false;
    struct m2sdr_dev *conn;

    if (parse_stream_selector("stream", which_s, &rx, &tx) != 0)
        return 1;

    conn = m2sdr_open_dev();
    sata_require_csrs();

#ifdef M2SDR_CSR_SATA_STREAMER_CONTROL_ADDR
    sata_streamers_reset(conn, rx, tx);
    printf("SATA streamer reset: %s\n", which_s);
    m2sdr_close_dev(conn);
    return 0;
#else
    fprintf(stderr,
        "SATA streamer stop is not supported by this gateware/software header. "
        "Rebuild and load gateware with CSR_SATA_STREAMER_CONTROL.\n");
    m2sdr_close_dev(conn);
    return 1;
#endif
}

#ifdef SATA_HOST_IO_AVAILABLE

struct capture_options {
    struct named_transfer_options named;
    bool have_seconds;
    double seconds;
    bool have_size;
    uint64_t size_bytes;
};

static void capture_options_init(struct capture_options *opts)
{
    memset(opts, 0, sizeof(*opts));
    named_transfer_options_init(&opts->named);
}

static int parse_capture_option(struct capture_options *opts, int argc, char **argv, int *index)
{
    const char *opt = argv[*index];
    const char *value = NULL;

    if (strcmp(opt, "--seconds") == 0 || strcmp(opt, "--secs") == 0) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", opt);
            exit(1);
        }
        value = argv[++(*index)];
        opts->seconds = parse_double_arg("seconds", value);
        if (opts->seconds <= 0.0) {
            fprintf(stderr, "seconds must be greater than zero.\n");
            exit(1);
        }
        opts->have_seconds = true;
        return 1;
    }
    if (strcmp(opt, "--size") == 0 || strcmp(opt, "--bytes") == 0) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", opt);
            exit(1);
        }
        value = argv[++(*index)];
        opts->size_bytes = parse_size_bytes("capture size", value);
        if (opts->size_bytes == 0) {
            fprintf(stderr, "capture size must be greater than zero.\n");
            exit(1);
        }
        opts->have_size = true;
        return 1;
    }
    return parse_named_option(&opts->named, argc, argv, index);
}

static int capture_compute_size(const struct capture_options *opts,
                                uint32_t *nsectors,
                                uint64_t *bytes)
{
    if (opts->have_seconds && opts->have_size) {
        fprintf(stderr, "Use either --seconds or --size, not both.\n");
        return 1;
    }
    if (!opts->have_seconds && !opts->have_size) {
        fprintf(stderr, "capture requires --seconds SEC or --size BYTES.\n");
        return 1;
    }
    if (opts->have_size) {
        *bytes = opts->size_bytes;
        *nsectors = m2sdr_sata_bytes_to_sectors(*bytes);
        if (*nsectors == 0 || (uint64_t)*nsectors * SATA_SECTOR_BYTES < *bytes) {
            fprintf(stderr, "Capture size is out of range.\n");
            return 1;
        }
        return 0;
    }
    {
        unsigned channels = channel_layout_count(opts->named.channel_layout);
        size_t format_bytes = m2sdr_format_size(opts->named.format);
        long double exact_bytes;

        if (opts->named.sample_rate <= 0 || format_bytes == 0) {
            fprintf(stderr, "Invalid capture sample rate or format.\n");
            return 1;
        }
        exact_bytes = (long double)opts->seconds *
                      (long double)opts->named.sample_rate *
                      (long double)channels *
                      (long double)format_bytes;
        if (exact_bytes <= 0.0L || exact_bytes > (long double)UINT64_MAX) {
            fprintf(stderr, "Capture size is out of range.\n");
            return 1;
        }
        {
            long double whole = floorl(exact_bytes);
            long double frac = exact_bytes - whole;
            *bytes = (uint64_t)((frac < 1e-6L) ? whole : ceill(exact_bytes));
        }
        *nsectors = (uint32_t)((*bytes + SATA_SECTOR_BYTES - 1u) / SATA_SECTOR_BYTES);
        if (*nsectors == 0 || (uint64_t)*nsectors * SATA_SECTOR_BYTES < *bytes) {
            fprintf(stderr, "Capture sector count is out of range.\n");
            return 1;
        }
    }
    return 0;
}

static int apply_rf_config_from_options(const struct named_transfer_options *opts)
{
    struct m2sdr_config cfg;
    struct m2sdr_dev *dev;
    int rc;

    m2sdr_config_init(&cfg);
    cfg.sample_rate = opts->sample_rate;
    cfg.bandwidth = (int64_t)opts->bandwidth;
    cfg.rx_freq = (int64_t)opts->rx_freq;
    cfg.tx_freq = (int64_t)opts->tx_freq;
    cfg.rx_gain1 = opts->rx_gain;
    cfg.rx_gain2 = opts->rx_gain;
    cfg.tx_att = opts->tx_att;
    cfg.program_rx_gains = true;
    cfg.enable_8bit_mode = (opts->format == M2SDR_FORMAT_SC8_Q7);
    cfg.channel_layout = opts->channel_layout;
    cfg.chan_mode = channel_layout_name(opts->channel_layout);

    dev = m2sdr_open_dev();
    rc = m2sdr_apply_config(dev, &cfg);
    if (rc != M2SDR_ERR_OK)
        fprintf(stderr, "m2sdr_apply_config failed: %s\n", m2sdr_strerror(rc));
    m2sdr_close_dev(dev);
    return rc == M2SDR_ERR_OK ? 0 : 1;
}

static int catalog_entry_to_options(const struct sata_capture_entry *e,
                                    struct named_transfer_options *opts)
{
    named_transfer_options_init(opts);
    opts->sample_rate = e->sample_rate;
    if (m2sdr_cli_parse_format(e->format, &opts->format) != 0) {
        fprintf(stderr, "Capture '%s' has invalid format '%s'.\n", e->name, e->format);
        return 1;
    }
    opts->channel_layout = parse_channel_layout(e->channel_layout);
    opts->rx_freq = e->rx_freq;
    opts->tx_freq = e->tx_freq;
    opts->bandwidth = e->bandwidth;
    opts->rx_gain = e->rx_gain;
    opts->tx_att = e->tx_att;
    catalog_copy(opts->notes, sizeof(opts->notes), e->notes);
    return 0;
}

static int sigmf_meta_to_options(const char *name,
                                 const struct m2sdr_sigmf_meta *meta,
                                 struct named_transfer_options *opts)
{
    enum m2sdr_format format;

    named_transfer_options_init(opts);
    format = m2sdr_sigmf_format_from_datatype(meta->datatype);
    if (format == (enum m2sdr_format)-1) {
        fprintf(stderr, "Capture '%s' has unsupported SigMF datatype '%s'.\n",
            name, meta->datatype);
        return 1;
    }
    opts->format = format;
    if (meta->has_sample_rate)
        opts->sample_rate = (int64_t)meta->sample_rate;
    if (meta->has_num_channels)
        opts->channel_layout = meta->num_channels <= 1 ?
            M2SDR_CHANNEL_LAYOUT_1T1R : M2SDR_CHANNEL_LAYOUT_2T2R;
    if (meta->has_center_freq) {
        opts->rx_freq = (uint64_t)meta->center_freq;
        opts->tx_freq = (uint64_t)meta->center_freq;
    }
    if (meta->description[0])
        catalog_copy(opts->notes, sizeof(opts->notes), meta->description);
    return 0;
}

static void parse_replay_rf_overrides(struct named_transfer_options *opts,
                                      int argc, char **argv, int argi)
{
    while (argi < argc) {
        if (!parse_named_option(opts, argc, argv, &argi)) {
            fprintf(stderr, "Unexpected argument: %s\n", argv[argi]);
            exit(1);
        }
        argi++;
    }
}

static int do_capture_named(const char *name, int argc, char **argv, int argi,
                            int timeout_ms, bool start_only, bool dry_run)
{
    struct sata_catalog cat;
    struct capture_options opts;
    struct sata_capture_entry entry;
    uint64_t bytes = 0;
    uint32_t nsectors = 0;
    uint64_t sector;
    uint64_t meta_sector;

    capture_options_init(&opts);
    while (argi < argc) {
        if (!parse_capture_option(&opts, argc, argv, &argi)) {
            fprintf(stderr, "Unexpected argument: %s\n", argv[argi]);
            return 1;
        }
        argi++;
    }
    if (capture_compute_size(&opts, &nsectors, &bytes) != 0)
        return 1;
    if (catalog_load(&cat, timeout_ms) != 0)
        return 1;
    if (!cat.initialized)
        catalog_clear(&cat);
    if (catalog_assign_new_storage(&cat, name, opts.named.have_sector, opts.named.sector,
                                   nsectors, &sector, &meta_sector) != 0)
        return 1;
    if (dry_run) {
        printf("%s dry-run: name=%s sector=0x%016" PRIx64 " nsectors=%" PRIu32
               " bytes=%" PRIu64 " sigmf_sector=0x%016" PRIx64 " sigmf_nsectors=%u"
               " sample_rate=%" PRId64 " format=%s channels=%s\n",
            start_only ? "capture-start" : "capture",
            name, sector, nsectors, bytes,
            meta_sector, M2SDR_SATA_SIGMF_META_SECTORS,
            opts.named.sample_rate,
            format_name(opts.named.format), channel_layout_name(opts.named.channel_layout));
        return 0;
    }

    if (apply_rf_config_from_options(&opts.named) != 0)
        return 1;
    if (start_only) {
        catalog_entry_from_options(&entry, name, &opts.named, sector, nsectors, bytes,
            meta_sector, M2SDR_SATA_SIGMF_META_SECTORS, 0);
        if (catalog_entry_write_sigmf_metadata(&entry, name, timeout_ms) != 0)
            return 1;
        if (catalog_add_entry(&cat, &entry) != 0)
            return 1;
        if (catalog_save(&cat, timeout_ms) != 0)
            return 1;
        if (do_record_start(sector, nsectors, timeout_ms, false) != 0)
            return 1;
        printf("Started capture '%s' at sector 0x%016" PRIx64 " (%" PRIu32 " sectors).\n",
            name, sector, nsectors);
        return 0;
    } else {
        if (do_record(sector, nsectors, timeout_ms, false) != 0)
            return 1;
    }

    catalog_entry_from_options(&entry, name, &opts.named, sector, nsectors, bytes,
        meta_sector, M2SDR_SATA_SIGMF_META_SECTORS, 0);
    if (catalog_entry_write_sigmf_metadata(&entry, name, timeout_ms) != 0)
        return 1;
    if (catalog_add_entry(&cat, &entry) != 0)
        return 1;
    if (catalog_save(&cat, timeout_ms) != 0)
        return 1;
    printf("Recorded capture '%s' at sector 0x%016" PRIx64 " (%" PRIu32 " sectors).\n",
        name, sector, nsectors);
    return 0;
}

static int do_replay_host_named(const char *name, int argc, char **argv, int argi,
                                int timeout_ms, bool dry_run)
{
    struct sata_catalog cat;
    struct sata_capture_entry *e;
    const char *dst = NULL;

    while (argi < argc) {
        if (!strcmp(argv[argi], "--dst")) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "Missing value for --dst\n");
                return 1;
            }
            dst = argv[++argi];
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", argv[argi]);
            return 1;
        }
        argi++;
    }
    if (!dst)
        dst = sata_default_host_destination();
    if (!dst) {
        fprintf(stderr, "serve could not infer host destination; use --dst pcie|eth.\n");
        return 1;
    }
    if (strcmp(dst, "pcie") != 0 && strcmp(dst, "eth") != 0) {
        m2sdr_cli_invalid_choice("serve destination", dst, "pcie or eth");
        return 1;
    }
    if (catalog_require(&cat, timeout_ms) != 0)
        return 1;
    e = catalog_find(&cat, name);
    if (!e) {
        fprintf(stderr, "Capture '%s' not found.\n", name);
        return 1;
    }
    return do_replay(e->sector, e->nsectors, dst, timeout_ms, dry_run);
}

static int do_replay_rf_named(const char *name, int argc, char **argv, int argi,
                              int timeout_ms, bool dry_run)
{
    struct sata_catalog cat;
    struct sata_capture_entry *e;
    struct m2sdr_sigmf_meta meta;
    struct named_transfer_options opts;
    bool have_sigmf_options = false;

    if (catalog_require(&cat, timeout_ms) != 0)
        return 1;
    e = catalog_find(&cat, name);
    if (!e) {
        fprintf(stderr, "Capture '%s' not found.\n", name);
        return 1;
    }
    if (catalog_entry_read_sigmf_metadata(e, &meta, timeout_ms) == 0) {
        if (sigmf_meta_to_options(name, &meta, &opts) != 0)
            return 1;
        have_sigmf_options = true;
    }
    if (!have_sigmf_options && catalog_entry_to_options(e, &opts) != 0)
        return 1;
    parse_replay_rf_overrides(&opts, argc, argv, argi);
    if (dry_run)
        return do_play(e->sector, e->nsectors, timeout_ms, true);
    if (apply_rf_config_from_options(&opts) != 0)
        return 1;
    return do_play(e->sector, e->nsectors, timeout_ms, false);
}

#endif /* SATA_HOST_IO_AVAILABLE */

#endif /* CSR_SATA_PHY_BASE */

/* Status ------------------------------------------------------------------- */

static void status(void)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();
    enum m2sdr_transport_kind transport = M2SDR_TRANSPORT_KIND_UNKNOWN;

    printf("Device:\n");
    if (m2sdr_get_transport(conn, &transport) == M2SDR_ERR_OK) {
        printf("  Transport          %s\n",
            transport == M2SDR_TRANSPORT_KIND_LITEPCIE ? "pcie" :
            transport == M2SDR_TRANSPORT_KIND_LITEETH  ? "ethernet" : "unknown");
    } else {
        printf("  Transport          unknown\n");
    }

#ifdef CSR_CROSSBAR_BASE
    uint32_t txsel = m2sdr_read32(conn, CSR_CROSSBAR_MUX_SEL_ADDR);
    uint32_t rxsel = m2sdr_read32(conn, CSR_CROSSBAR_DEMUX_SEL_ADDR);
    printf("Route:\n");
    printf("  TX source          %s (%" PRIu32 ")\n", txsrc_name((int)txsel), txsel);
    printf("  RX destination     %s (%" PRIu32 ")\n", rxdst_name((int)rxsel), rxsel);
#else
    printf("Route:\n");
    printf("  Crossbar           not present\n");
#endif

#ifdef CSR_SATA_PHY_BASE
    {
        struct m2sdr_sata_info sata_info;
        int sata_rc = m2sdr_get_sata_info(conn, &sata_info, 1000);

        printf("SATA:\n");
        if (sata_rc == M2SDR_ERR_OK) {
            printf("  PHY enable         %s\n", sata_info.phy_enabled ? "yes" : "no");
            printf("  PHY status         0x%08" PRIx32 "\n", sata_info.phy_status);
            printf("  PHY ready          %s\n", sata_info.phy_ready ? "yes" : "no");
            printf("  TX ready           %s\n", sata_info.tx_ready ? "yes" : "no");
            printf("  RX ready           %s\n", sata_info.rx_ready ? "yes" : "no");
            printf("  CTRL ready         %s\n", sata_info.ctrl_ready ? "yes" : "no");
            printf("  Drive              %s\n", sata_info.drive_present ? "present" : "not present");
            if (sata_info.drive_present) {
                char capacity[32];
                uint32_t sector_size = sata_info.logical_sector_size ? sata_info.logical_sector_size : SATA_SECTOR_BYTES;

                format_capacity(capacity, sizeof(capacity), sata_info.sector_count, sector_size);
                printf("  Model              %s\n", sata_info.model[0] ? sata_info.model : "unknown");
                printf("  Serial             %s\n", sata_info.serial[0] ? sata_info.serial : "unknown");
                printf("  Firmware           %s\n", sata_info.firmware[0] ? sata_info.firmware : "unknown");
                printf("  Sectors            %" PRIu64 "\n", sata_info.sector_count);
                printf("  Sector size        %" PRIu32 " bytes\n", sector_size);
                printf("  Capacity           %s\n", capacity);
            }
        } else if (sata_rc == M2SDR_ERR_UNSUPPORTED) {
            printf("  Identify           not supported by this gateware/software header\n");
        } else {
            printf("  Identify           failed (%s)\n", m2sdr_strerror(sata_rc));
        }
    }

#ifdef CSR_TXRX_LOOPBACK_BASE
    {
        uint32_t v = m2sdr_read32(conn, CSR_TXRX_LOOPBACK_CONTROL_ADDR);
        uint32_t loopback =
            (v >> CSR_TXRX_LOOPBACK_CONTROL_ENABLE_OFFSET) &
            ((1u << CSR_TXRX_LOOPBACK_CONTROL_ENABLE_SIZE) - 1);
        printf("  Loopback           %s\n", loopback ? "enabled" : "disabled");
    }
#endif

#ifdef CSR_SATA_TX_STREAMER_BASE
    printf("  TX streamer        sector=0x%016" PRIx64 " nsectors=%" PRIu32 " done=%" PRIu32 " error=%" PRIu32 "\n",
        csr_read64(conn, CSR_SATA_TX_STREAMER_SECTOR_ADDR),
        m2sdr_read32(conn, CSR_SATA_TX_STREAMER_NSECTORS_ADDR),
        m2sdr_read32(conn, CSR_SATA_TX_STREAMER_DONE_ADDR),
        m2sdr_read32(conn, CSR_SATA_TX_STREAMER_ERROR_ADDR));
#endif
#ifdef CSR_SATA_RX_STREAMER_BASE
    printf("  RX streamer        sector=0x%016" PRIx64 " nsectors=%" PRIu32 " done=%" PRIu32 " error=%" PRIu32 "\n",
        csr_read64(conn, CSR_SATA_RX_STREAMER_SECTOR_ADDR),
        m2sdr_read32(conn, CSR_SATA_RX_STREAMER_NSECTORS_ADDR),
        m2sdr_read32(conn, CSR_SATA_RX_STREAMER_DONE_ADDR),
        m2sdr_read32(conn, CSR_SATA_RX_STREAMER_ERROR_ADDR));
#endif

#ifdef SATA_HOST_IO_AVAILABLE
    printf("Host I/O:\n");
    if (transport == M2SDR_TRANSPORT_KIND_LITEPCIE) {
        printf("  Path               PCIe userspace DMA\n");
        printf("  Chunk sectors      %u\n", M2SDR_SATA_PCIE_DMA_MAX_SECTORS);
    } else {
        printf("  Path               Etherbone host staging buffer\n");
        printf("  Host buffer        %u bytes (%u sectors)\n",
            SATA_HOST_BUFFER_SIZE, sata_host_buffer_max_sectors());
        if (transport == M2SDR_TRANSPORT_KIND_LITEETH)
            printf("  Etherbone burst    %" PRIu32 " words\n", sata_host_buffer_bulk_words(conn));
    }
    {
        struct sata_catalog cat;
        if (catalog_load_from_conn(conn, &cat, 1000) == 0) {
            unsigned used = 0;
            for (unsigned i = 0; i < SATA_CATALOG_MAX_ENTRIES; i++)
                if (cat.entries[i].used)
                    used++;
            printf("Catalog:\n");
            printf("  State              %s\n", cat.initialized ? "initialized" : "not initialized");
            printf("  Sector             0x%016" PRIx64 "\n", (uint64_t)SATA_CATALOG_SECTOR);
            printf("  Entries            %u/%u\n", used, SATA_CATALOG_MAX_ENTRIES);
        } else {
            printf("Catalog:\n");
            printf("  State              unreadable or not initialized\n");
        }
    }
#endif

#else
    printf("SATA:\n");
    printf("  not present\n");
#endif

    m2sdr_close_dev(conn);
}

/* Route (always available if crossbar exists) ------------------------------ */

static void do_route(const char *txsrc_s, const char *rxdst_s, int loopback_en)
{
    struct m2sdr_dev *conn = m2sdr_open_dev();

    int txsrc = parse_txsrc(txsrc_s);
    int rxdst = parse_rxdst(rxdst_s);

    crossbar_set(conn, txsrc, rxdst);

#ifdef CSR_SATA_PHY_BASE
    if (loopback_en >= 0) {
        /* Only meaningful if loopback CSR is present. */
#ifdef CSR_TXRX_LOOPBACK_BASE
        txrx_loopback_set(conn, loopback_en);
#else
        fprintf(stderr, "TXRX loopback CSR not present.\n");
        exit(1);
#endif
    }
#else
    if (loopback_en >= 0) {
        fprintf(stderr, "SATA/loopback not present in this gateware.\n");
        exit(1);
    }
#endif

    m2sdr_close_dev(conn);
}

/* Help --------------------------------------------------------------------- */

static void help(void)
{
    printf("M2SDR SATA Utility\n"
           "usage: m2sdr_sata [options] cmd [args...]\n"
           "\n"
           "options:\n"
           "  -h, --help                     Show this help message.\n"
           "  -d, --device DEV               Use explicit device id.\n"
           "  -c, --device-num N             Select the device (default: 0).\n"
           "  -i, --ip ADDR                  Target IP address for Etherbone.\n"
           "  -p, --port PORT                Port number (default: 1234).\n"
           "      --timeout-ms MS            Timeout for streamer operations (default: 30000, -1=infinite).\n"
           "      --dry-run                  Show planned operation without starting the streamer.\n"
           "      --pattern NAME             Diagnostic pattern: zero|counter|prbs.\n"
           "      --no-bulk-etherbone        Disable multi-word Etherbone host-buffer access.\n"
           "\n"
           "workflow commands:\n"
           "info\n"
           "    Show transport, SATA link, drive, catalog and host I/O status.\n"
           "\n"
           "init\n"
           "    Initialize/reset the named SATA catalog.\n"
           "\n"
           "list\n"
           "    List named SATA entries.\n"
           "\n"
           "show NAME\n"
           "    Show catalog and SigMF metadata for one entry.\n"
           "\n"
           "delete NAME\n"
           "    Remove one entry from the catalog without erasing sectors.\n"
           "\n"
           "check\n"
           "    Check catalog names, sector ranges and overlaps.\n"
           "\n"
#ifdef CSR_SATA_PHY_BASE
           "capture NAME --seconds SEC|--size BYTES [RF options]\n"
           "    Tune RF, record RX stream to SATA, and store SigMF metadata.\n"
           "\n"
           "capture-start NAME --seconds SEC|--size BYTES [RF options]\n"
           "    Start a named RX-to-SATA capture and return immediately.\n"
           "\n"
           "import NAME FILE|SIGMF [metadata options]\n"
           "    Import a raw file or SigMF dataset to SATA and catalog it.\n"
           "\n"
           "export NAME PATH [--raw]\n"
           "    Export as SigMF metadata+data by default; --raw writes payload only.\n"
           "\n"
           "play NAME [RF overrides]\n"
           "    Replay SATA content to the RF TX path.\n"
           "\n"
           "serve NAME [--dst pcie|eth]\n"
           "    Replay SATA content into the normal host RX path for Soapy/GQRX.\n"
           "\n"
           "stop RX|TX|BOTH\n"
           "    Reset the selected SATA streamer(s).\n"
           "\n"
           "RF/metadata options: --sector, --sample-rate, --format sc16|sc8,\n"
           "    --channel-layout 1t1r|2t2r, --rx-freq, --tx-freq, --bandwidth,\n"
           "    --rx-gain, --tx-att, --notes.\n"
           "\n"
           "diagnostics:\n"
           "diag route TXSRC RXDST [LOOPBACK]\n"
           "    Set raw crossbar routing; txsrc/rxdst: pcie|eth|sata.\n"
           "\n"
           "diag record DST_SECTOR NSECTORS\n"
           "diag record-start DST_SECTOR NSECTORS\n"
           "diag play SRC_SECTOR NSECTORS\n"
           "diag play-start SRC_SECTOR NSECTORS\n"
           "diag replay SRC_SECTOR NSECTORS pcie|eth|sata\n"
           "diag copy SRC_SECTOR DST_SECTOR NSECTORS\n"
           "    Raw sector streamer diagnostics.\n"
           "\n"
           "diag read SECTOR NSECTORS FILE|-\n"
           "diag write FILE|- SECTOR [NSECTORS]\n"
           "    Raw host file <-> SATA sector diagnostics.\n"
           "\n"
           "diag pattern-write SECTOR NSECTORS\n"
           "diag pattern-check SECTOR NSECTORS\n"
           "    Fill/check sectors with --pattern.\n"
           "\n"
#ifdef SATA_HOST_IO_AVAILABLE
           "diag etherbone-bench [--iterations N]\n"
           "diag pcie-dma-bench SECTOR NSECTORS\n"
           "    Host I/O performance diagnostics.\n"
           "\n"
#endif
#else
           "SATA workflow commands are not available with this software header.\n"
           "\n"
#endif
           "diag header TX|RX|BOTH ENABLE HEADER_ENABLE\n"
           "    Raw header control bits (writes CSR_HEADER_*_CONTROL).\n"
           "\n");
}

/* Main --------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *cmd;
    int c;
    int option_index = 0;

    int timeout_ms = 30000;
    bool dry_run = false;
    const char *pattern_name = "counter";
    m2sdr_cli_device_init(&g_cli_dev);
    static struct option options[] = {
        { "help", no_argument, NULL, 'h' },
        { "device", required_argument, NULL, 'd' },
        { "device-num", required_argument, NULL, 'c' },
        { "ip", required_argument, NULL, 'i' },
        { "port", required_argument, NULL, 'p' },
        { "timeout-ms", required_argument, NULL, 'T' },
        { "dry-run", no_argument, NULL, 'n' },
        { "pattern", required_argument, NULL, 'P' },
        { "no-bulk-etherbone", no_argument, NULL, 1000 },
        { NULL, 0, NULL, 0 }
    };

    signal(SIGINT, int_handler);
    for (;;) {
        c = getopt_long(argc, argv, "+hd:c:i:p:T:nP:", options, &option_index);
        if (c == -1)
            break;
        switch (c) {
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
        case 'T':
            timeout_ms = parse_timeout_ms(optarg);
            break;
        case 'n':
            dry_run = true;
            break;
        case 'P':
            pattern_name = optarg;
            break;
        case 1000:
            m2sdr_sata_set_no_bulk_etherbone(true);
            break;
        default:
            exit(1);
        }
    }

    if (optind >= argc) {
        help();
        return 1;
    }

    cmd = argv[optind++];

    if (!strcmp(cmd, "info")) {
        status();
        return 0;
    }

#ifdef CSR_SATA_PHY_BASE
#ifdef SATA_HOST_IO_AVAILABLE
    if (!strcmp(cmd, "init")) {
        return do_catalog_init(timeout_ms);
    }

    if (!strcmp(cmd, "list")) {
        return do_catalog_list(timeout_ms);
    }

    if (!strcmp(cmd, "show")) {
        if (argc - optind < 1) {
            help();
            return 1;
        }
        return do_catalog_show(argv[optind++], timeout_ms);
    }

    if (!strcmp(cmd, "delete")) {
        if (argc - optind < 1) {
            help();
            return 1;
        }
        return do_catalog_delete(argv[optind++], timeout_ms);
    }

    if (!strcmp(cmd, "check")) {
        return do_catalog_fsck(timeout_ms);
    }

    if (!strcmp(cmd, "capture")) {
        if (argc - optind < 1) {
            help();
            return 1;
        }
        const char *name = argv[optind++];
        return do_capture_named(name, argc, argv, optind, timeout_ms, false, dry_run);
    }

    if (!strcmp(cmd, "capture-start")) {
        if (argc - optind < 1) {
            help();
            return 1;
        }
        const char *name = argv[optind++];
        return do_capture_named(name, argc, argv, optind, timeout_ms, true, dry_run);
    }

    if (!strcmp(cmd, "import")) {
        if (argc - optind < 2) {
            help();
            return 1;
        }
        const char *name = argv[optind++];
        const char *path = argv[optind++];
        return do_import_auto(name, path, argc, argv, optind, timeout_ms, dry_run);
    }

    if (!strcmp(cmd, "export")) {
        bool raw = false;
        if (argc - optind < 2) {
            help();
            return 1;
        }
        const char *name = argv[optind++];
        const char *path = argv[optind++];
        while (optind < argc) {
            if (!strcmp(argv[optind], "--raw"))
                raw = true;
            else {
                fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
                return 1;
            }
            optind++;
        }
        return raw ? do_export_capture(name, path, timeout_ms) :
                     do_export_sigmf_capture(name, path, timeout_ms);
    }

    if (!strcmp(cmd, "play")) {
        if (argc - optind < 1) {
            help();
            return 1;
        }
        const char *name = argv[optind++];
        return do_replay_rf_named(name, argc, argv, optind, timeout_ms, dry_run);
    }

    if (!strcmp(cmd, "serve")) {
        if (argc - optind < 1) {
            help();
            return 1;
        }
        const char *name = argv[optind++];
        return do_replay_host_named(name, argc, argv, optind, timeout_ms, dry_run);
    }
#endif

    if (!strcmp(cmd, "stop")) {
        if (argc - optind < 1) {
            help();
            return 1;
        }
        return do_stream_stop(argv[optind++]);
    }

#else
    if (!strcmp(cmd, "init") || !strcmp(cmd, "list") || !strcmp(cmd, "show") ||
        !strcmp(cmd, "delete") || !strcmp(cmd, "check") || !strcmp(cmd, "capture") ||
        !strcmp(cmd, "capture-start") || !strcmp(cmd, "import") || !strcmp(cmd, "export") ||
        !strcmp(cmd, "play") || !strcmp(cmd, "serve") || !strcmp(cmd, "stop")) {
        fprintf(stderr, "Command '%s' not available: SATA not present in this gateware.\n", cmd);
        return 1;
    }
#endif

    if (!strcmp(cmd, "diag")) {
        const char *diag_cmd;

        if (optind >= argc) {
            help();
            return 1;
        }
        diag_cmd = argv[optind++];

        if (!strcmp(diag_cmd, "header")) {
            if (argc - optind < 3) {
                help();
                return 1;
            }
            const char *which_s = argv[optind++];
            int which = 0;
            if (!strcmp(which_s, "tx")) which = 0;
            else if (!strcmp(which_s, "rx")) which = 1;
            else if (!strcmp(which_s, "both")) which = 2;
            else {
                m2sdr_cli_invalid_choice("header target", which_s, "tx, rx, or both");
                return 1;
            }
            int enable        = parse_bool01("header enable", argv[optind++]);
            int header_enable = parse_bool01("header header_enable", argv[optind++]);

            struct m2sdr_dev *conn = m2sdr_open_dev();
            header_set_raw(conn, which, enable, header_enable);
            m2sdr_close_dev(conn);
            return 0;
        }

#ifdef CSR_SATA_PHY_BASE
        if (!strcmp(diag_cmd, "route")) {
            if (argc - optind < 2) {
                help();
                return 1;
            }
            const char *txsrc = argv[optind++];
            const char *rxdst = argv[optind++];
            int loopback = -1;
            if (optind < argc)
                loopback = parse_bool01("loopback", argv[optind++]);
            do_route(txsrc, rxdst, loopback);
            return 0;
        }

        if (!strcmp(diag_cmd, "record") || !strcmp(diag_cmd, "record-start")) {
            if (argc - optind < 2) {
                help();
                return 1;
            }
            uint64_t dst_sector = parse_u64(argv[optind++]);
            uint32_t nsectors   = parse_u32(argv[optind++]);
            if (nsectors == 0) {
                m2sdr_cli_error("nsectors must be greater than zero");
                return 1;
            }
            return !strcmp(diag_cmd, "record") ?
                do_record(dst_sector, nsectors, timeout_ms, dry_run) :
                do_record_start(dst_sector, nsectors, timeout_ms, dry_run);
        }

        if (!strcmp(diag_cmd, "play") || !strcmp(diag_cmd, "play-start")) {
            if (argc - optind < 2) {
                help();
                return 1;
            }
            uint64_t src_sector = parse_u64(argv[optind++]);
            uint32_t nsectors   = parse_u32(argv[optind++]);
            if (nsectors == 0) {
                m2sdr_cli_error("nsectors must be greater than zero");
                return 1;
            }
            return !strcmp(diag_cmd, "play") ?
                do_play(src_sector, nsectors, timeout_ms, dry_run) :
                do_play_start(src_sector, nsectors, timeout_ms, dry_run);
        }

        if (!strcmp(diag_cmd, "replay")) {
            if (argc - optind < 3) {
                help();
                return 1;
            }
            uint64_t src_sector = parse_u64(argv[optind++]);
            uint32_t nsectors   = parse_u32(argv[optind++]);
            const char *dst     = argv[optind++];
            if (nsectors == 0) {
                m2sdr_cli_error("nsectors must be greater than zero");
                return 1;
            }
            return do_replay(src_sector, nsectors, dst, timeout_ms, dry_run);
        }

        if (!strcmp(diag_cmd, "copy")) {
            if (argc - optind < 3) {
                help();
                return 1;
            }
            uint64_t src_sector = parse_u64(argv[optind++]);
            uint64_t dst_sector = parse_u64(argv[optind++]);
            uint32_t nsectors   = parse_u32(argv[optind++]);
            if (nsectors == 0) {
                m2sdr_cli_error("nsectors must be greater than zero");
                return 1;
            }
            return do_copy(src_sector, dst_sector, nsectors, timeout_ms, dry_run);
        }

#ifdef SATA_HOST_IO_AVAILABLE
        if (!strcmp(diag_cmd, "read")) {
            if (argc - optind < 3) {
                help();
                return 1;
            }
            uint64_t src_sector = parse_u64(argv[optind++]);
            uint32_t nsectors   = parse_u32(argv[optind++]);
            const char *path    = argv[optind++];
            if (nsectors == 0) {
                m2sdr_cli_error("nsectors must be greater than zero");
                return 1;
            }
            if (dry_run) {
                printf("diag read dry-run: sector=0x%016" PRIx64 " nsectors=%" PRIu32 " path=%s\n",
                    src_sector, nsectors, path);
                return 0;
            }
            return do_read_file(src_sector, nsectors, path, timeout_ms);
        }

        if (!strcmp(diag_cmd, "write")) {
            if (argc - optind < 2) {
                help();
                return 1;
            }
            const char *path      = argv[optind++];
            uint64_t dst_sector   = parse_u64(argv[optind++]);
            uint32_t nsectors     = 0;
            bool limit_sectors    = false;
            if (optind < argc) {
                nsectors = parse_u32(argv[optind++]);
                if (nsectors == 0) {
                    m2sdr_cli_error("nsectors must be greater than zero");
                    return 1;
                }
                limit_sectors = true;
            }
            if (dry_run) {
                printf("diag write dry-run: path=%s sector=0x%016" PRIx64,
                    path, dst_sector);
                if (limit_sectors)
                    printf(" nsectors=%" PRIu32, nsectors);
                printf("\n");
                return 0;
            }
            return do_write_file(path, dst_sector, nsectors, limit_sectors, timeout_ms);
        }

        if (!strcmp(diag_cmd, "pattern-write") || !strcmp(diag_cmd, "pattern-check")) {
            if (argc - optind < 2) {
                help();
                return 1;
            }
            uint64_t sector = parse_u64(argv[optind++]);
            uint32_t nsectors = parse_u32(argv[optind++]);
            if (nsectors == 0) {
                m2sdr_cli_error("nsectors must be greater than zero");
                return 1;
            }
            if (dry_run) {
                printf("diag %s dry-run: sector=0x%016" PRIx64 " nsectors=%" PRIu32 " pattern=%s\n",
                    diag_cmd, sector, nsectors, pattern_name);
                return 0;
            }
            return !strcmp(diag_cmd, "pattern-write") ?
                do_write_pattern(sector, nsectors, parse_pattern(pattern_name), timeout_ms) :
                do_verify_pattern(sector, nsectors, parse_pattern(pattern_name), timeout_ms);
        }

        if (!strcmp(diag_cmd, "etherbone-bench"))
            return do_etherbone_bench(argc, argv, optind);

        if (!strcmp(diag_cmd, "pcie-dma-bench"))
            return do_pcie_dma_bench(argc, argv, optind, timeout_ms);
#endif
#endif

        fprintf(stderr, "Unknown diagnostic command: %s\n", diag_cmd);
        return 1;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    help();
    return 1;
}
