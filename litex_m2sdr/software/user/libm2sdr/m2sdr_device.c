/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR library
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

/* Includes */
/*----------*/

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csr.h"
#include "m2sdr_internal.h"

/* Defines */
/*---------*/

#ifdef USE_LITEETH
#define M2SDR_LITEETH_CONTROL_TIMEOUT_MS 500
#define M2SDR_LITEETH_OPEN_PROBE_ATTEMPTS 3
#define M2SDR_LITEETH_OPEN_PROBE_DELAY_US 1000
#endif

/* Helpers */
/*---------*/

/* Convert an error code to the short public string form used by
 * m2sdr_strerror(). */
static const char *m2sdr_err_str(int err)
{
    switch (err) {
    case M2SDR_ERR_OK:          return "ok";
    case M2SDR_ERR_UNEXPECTED:  return "unexpected";
    case M2SDR_ERR_INVAL:       return "invalid";
    case M2SDR_ERR_IO:          return "io";
    case M2SDR_ERR_TIMEOUT:     return "timeout";
    case M2SDR_ERR_NO_MEM:      return "no_mem";
    case M2SDR_ERR_UNSUPPORTED: return "unsupported";
    case M2SDR_ERR_PARSE:       return "parse";
    case M2SDR_ERR_RANGE:       return "range";
    case M2SDR_ERR_STATE:       return "state";
    default:                    return "unknown";
    }
}

/* Remove trailing newlines from firmware-provided text strings. */
static void m2sdr_trim_nl(char *s)
{
    size_t n;

    if (!s)
        return;

    n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

/* Return the backend-specific default identifier used when the caller does
 * not specify a target. */
static void m2sdr_default_device(char *out, size_t out_len)
{
#ifdef USE_LITEPCIE
    snprintf(out, out_len, "/dev/m2sdr0");
#elif defined(USE_LITEETH)
    snprintf(out, out_len, "192.168.1.50:1234");
#else
    if (out_len)
        out[0] = '\0';
#endif
}

/* Accept either explicit transport identifiers ("pcie:", "eth:") or the
 * historical shorthand forms (/dev/m2sdrN and ip[:port]). */
static int m2sdr_parse_identifier(const char *id,
                                  char *path_out, size_t path_len,
                                  char *ip_out, size_t ip_len,
                                  uint16_t *port_out)
{
    char tmp[M2SDR_DEVICE_STR_MAX];
    char *colon;
    char *endptr = NULL;
    unsigned long port_ul = 0;

    if (!id || !id[0]) {
        m2sdr_default_device(tmp, sizeof(tmp));
        id = tmp;
    }

    if (!strncmp(id, "pcie:", 5)) {
        /* Explicit transport prefix: everything after it is treated as a
         * literal device path and bypasses the IP parsing path below. */
        if (strnlen(id + 5, path_len) >= path_len)
            return -1;
        snprintf(path_out, path_len, "%s", id + 5);
        return 0;
    }

    if (!strncmp(id, "eth:", 4))
        id += 4;

    if (id[0] == '/') {
        /* Preserve the historical "/dev/m2sdrN" shorthand accepted by the
         * original utilities. */
        if (strnlen(id, path_len) >= path_len)
            return -1;
        snprintf(path_out, path_len, "%s", id);
        return 0;
    }

    /* Anything else is interpreted as LiteEth shorthand: "ip[:port]". */
    if (strnlen(id, ip_len) >= ip_len)
        return -1;

    snprintf(ip_out, ip_len, "%s", id);
    colon = strchr(ip_out, ':');
    if (colon) {
        if (colon == ip_out || colon[1] == '\0')
            return -1;
        *colon = '\0';
        errno = 0;
        port_ul = strtoul(colon + 1, &endptr, 10);
        if (errno != 0 || endptr == (colon + 1) || *endptr != '\0' || port_ul == 0 || port_ul > 65535)
            return -1;
        *port_out = (uint16_t)port_ul;
    }

    return 0;
}

int m2sdr_test_parse_identifier(const char *id, uint16_t *port_out)
{
    char path[M2SDR_DEVICE_STR_MAX] = {0};
    char ip[M2SDR_DEVICE_STR_MAX] = {0};
    uint16_t port = 1234;
    int rc;

    rc = m2sdr_parse_identifier(id, path, sizeof(path), ip, sizeof(ip), &port);
    if (port_out)
        *port_out = port;
    return rc;
}

#ifdef USE_LITEETH
static int m2sdr_liteeth_from_eb_error(int err)
{
    switch (err) {
    case EB_ERR_OK:
        return M2SDR_ERR_OK;
    case EB_ERR_TIMEOUT:
    case EB_ERR_INTERRUPTED:
        return M2SDR_ERR_TIMEOUT;
    default:
        return M2SDR_ERR_IO;
    }
}

static int m2sdr_liteeth_probe_control(struct m2sdr_dev *dev)
{
#ifdef CSR_IDENTIFIER_MEM_BASE
    int rc = M2SDR_ERR_IO;

    if (!dev || !dev->eb)
        return M2SDR_ERR_INVAL;

    for (int attempt = 0; attempt < M2SDR_LITEETH_OPEN_PROBE_ATTEMPTS; attempt++) {
        uint32_t value = 0;
        int err = eb_read32_checked(dev->eb, CSR_IDENTIFIER_MEM_BASE, &value);

        rc = m2sdr_liteeth_from_eb_error(err);
        if (rc == M2SDR_ERR_OK && (value & 0xffu) == 'L')
            return M2SDR_ERR_OK;
        if (rc == M2SDR_ERR_TIMEOUT)
            usleep(M2SDR_LITEETH_OPEN_PROBE_DELAY_US);
    }

    return rc == M2SDR_ERR_OK ? M2SDR_ERR_IO : rc;
#else
    (void)dev;
    return M2SDR_ERR_OK;
#endif
}
#endif

/* Read one logical 64-bit CSR value stored as hi/lo 32-bit words. */
static int m2sdr_read_reg_u64(struct m2sdr_dev *dev, uint32_t addr, uint64_t *value)
{
    uint32_t high = 0;
    uint32_t low  = 0;

    if (!dev || !value)
        return M2SDR_ERR_INVAL;

    if (m2sdr_reg_read(dev, addr + 0, &high) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_read(dev, addr + 4, &low) != 0)
        return M2SDR_ERR_IO;

    *value = ((uint64_t)high << 32) | (uint64_t)low;
    return M2SDR_ERR_OK;
}

static int m2sdr_has_eth_ptp(struct m2sdr_dev *dev, bool *present)
{
    struct m2sdr_capabilities caps;

    if (!dev || !present)
        return M2SDR_ERR_INVAL;

    *present = false;
    if (m2sdr_get_capabilities(dev, &caps) != 0)
        return M2SDR_ERR_IO;

    *present = (caps.features & M2SDR_FEATURE_ETH_PTP_MASK) != 0;
    return M2SDR_ERR_OK;
}

static int m2sdr_time_owned_by_ptp(struct m2sdr_dev *dev, bool *owned)
{
    bool has_eth_ptp = false;
    int ret = 0;

    if (!owned)
        return M2SDR_ERR_INVAL;

    if (!dev)
        return M2SDR_ERR_INVAL;

    ret = m2sdr_has_eth_ptp(dev, &has_eth_ptp);
    if (ret != M2SDR_ERR_OK)
        return ret;
    if (!has_eth_ptp) {
        *owned = false;
        return M2SDR_ERR_OK;
    }

#if defined(CSR_PTP_DISCIPLINE_STATUS_ADDR) && \
    defined(CSR_PTP_DISCIPLINE_STATUS_ACTIVE_OFFSET) && \
    defined(CSR_PTP_DISCIPLINE_STATUS_ACTIVE_SIZE)
    {
        uint32_t status = 0;

        if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_STATUS_ADDR, &status) != 0)
            return M2SDR_ERR_IO;

        *owned = ((status >> CSR_PTP_DISCIPLINE_STATUS_ACTIVE_OFFSET) &
            ((1u << CSR_PTP_DISCIPLINE_STATUS_ACTIVE_SIZE) - 1u)) != 0;
        return M2SDR_ERR_OK;
    }
#else
    *owned = false;
    return M2SDR_ERR_OK;
#endif
}

static __attribute__((unused)) int m2sdr_get_ptp_discipline_control(struct m2sdr_dev *dev, uint32_t *control)
{
#if defined(CSR_PTP_DISCIPLINE_CONTROL_ADDR)
    if (!dev || !control)
        return M2SDR_ERR_INVAL;

    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_CONTROL_ADDR, control) != 0)
        return M2SDR_ERR_IO;

    return M2SDR_ERR_OK;
#else
    (void)dev;
    (void)control;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

static __attribute__((unused)) int m2sdr_write_ptp_discipline_control(struct m2sdr_dev *dev, bool enable, bool holdover)
{
#if defined(CSR_PTP_DISCIPLINE_CONTROL_ADDR) && \
    defined(CSR_PTP_DISCIPLINE_CONTROL_ENABLE_OFFSET) && \
    defined(CSR_PTP_DISCIPLINE_CONTROL_ENABLE_SIZE) && \
    defined(CSR_PTP_DISCIPLINE_CONTROL_HOLDOVER_OFFSET) && \
    defined(CSR_PTP_DISCIPLINE_CONTROL_HOLDOVER_SIZE)
    uint32_t control = 0;

    if (!dev)
        return M2SDR_ERR_INVAL;

    control |= ((enable ? 1u : 0u) & ((1u << CSR_PTP_DISCIPLINE_CONTROL_ENABLE_SIZE) - 1u))
        << CSR_PTP_DISCIPLINE_CONTROL_ENABLE_OFFSET;
    control |= ((holdover ? 1u : 0u) & ((1u << CSR_PTP_DISCIPLINE_CONTROL_HOLDOVER_SIZE) - 1u))
        << CSR_PTP_DISCIPLINE_CONTROL_HOLDOVER_OFFSET;

    if (m2sdr_reg_write(dev, CSR_PTP_DISCIPLINE_CONTROL_ADDR, control) != 0)
        return M2SDR_ERR_IO;

    return M2SDR_ERR_OK;
#else
    (void)dev;
    (void)enable;
    (void)holdover;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Read the board identifier memory into a caller-provided string buffer. */
static int m2sdr_read_identifier_mem(struct m2sdr_dev *dev, char *buf, size_t len)
{
    size_t i;
    size_t max;

    if (!dev || !buf || len == 0)
        return M2SDR_ERR_INVAL;

    max = len - 1;
    if (max > M2SDR_IDENT_MAX - 1)
        max = M2SDR_IDENT_MAX - 1;

    for (i = 0; i < max; i++) {
        uint32_t value = 0;

        if (m2sdr_reg_read(dev, CSR_IDENTIFIER_MEM_BASE + 4 * i, &value) != 0)
            return M2SDR_ERR_IO;

        buf[i] = (char)(value & 0xff);
        if (buf[i] == '\0')
            break;
    }

    buf[max] = '\0';
    m2sdr_trim_nl(buf);
    return M2SDR_ERR_OK;
}

/* Fill the public transport/path description from the active backend state. */
static void m2sdr_fill_transport_info(struct m2sdr_dev *dev, struct m2sdr_devinfo *info)
{
#ifdef USE_LITEPCIE
    snprintf(info->transport, sizeof(info->transport), "litepcie");
    snprintf(info->path, sizeof(info->path), "%s", dev->device_path);
#elif defined(USE_LITEETH)
    snprintf(info->transport, sizeof(info->transport), "liteeth");
    snprintf(info->path, sizeof(info->path), "%s:%u", dev->eth_ip, (unsigned)dev->eth_port);
#else
    (void)dev;
    (void)info;
#endif
}

/* Probe one candidate identifier and return its public metadata. */
static int m2sdr_probe_one(const char *device_id, struct m2sdr_devinfo *info)
{
    struct m2sdr_dev *dev = NULL;
    int rc;

    rc = m2sdr_open(&dev, device_id);
    if (rc != M2SDR_ERR_OK)
        return rc;

    rc = m2sdr_get_device_info(dev, info);
    m2sdr_close(dev);
    return rc;
}

/* Version / Lifetime */
/*--------------------*/

/* Return a short static string for a libm2sdr status code. */
const char *m2sdr_strerror(int err)
{
    return m2sdr_err_str(err);
}

/* Report the library API/ABI/version tuple compiled into this build. */
void m2sdr_get_version(struct m2sdr_version *ver)
{
    if (!ver)
        return;

    ver->api         = M2SDR_API_VERSION;
    ver->abi         = M2SDR_ABI_VERSION;
    ver->version_str = M2SDR_VERSION_STRING;
}

/* Open a device using either the PCIe or LiteEth backend and return the
 * opaque per-device library state. */
int m2sdr_open(struct m2sdr_dev **dev_out, const char *device_identifier)
{
    struct m2sdr_dev *dev;

    if (!dev_out)
        return M2SDR_ERR_INVAL;

    dev = calloc(1, sizeof(*dev));
    if (!dev)
        return M2SDR_ERR_NO_MEM;

#ifdef USE_LITEPCIE
    {
        char path[M2SDR_DEVICE_STR_MAX] = {0};
        char ip_dummy[64] = {0};
        uint16_t port_dummy = 1234;

        dev->fd = -1;
        dev->transport = M2SDR_TRANSPORT_LITEPCIE;

        if (m2sdr_parse_identifier(device_identifier, path, sizeof(path),
                                   ip_dummy, sizeof(ip_dummy), &port_dummy) != 0) {
            free(dev);
            return M2SDR_ERR_PARSE;
        }

        if (path[0] == '\0')
            m2sdr_default_device(path, sizeof(path));

        /* Keep the public API simple: open once here, then let the HAL and
         * higher layers operate on the stored descriptor. */
        dev->fd = open(path, O_RDWR | O_CLOEXEC);
        if (dev->fd < 0) {
            free(dev);
            return M2SDR_ERR_IO;
        }

        snprintf(dev->device_path, sizeof(dev->device_path), "%s", path);
    }
#elif defined(USE_LITEETH)
    {
        char path_dummy[M2SDR_DEVICE_STR_MAX] = {0};
        char ip[64] = {0};
        char port_str[16];
        uint16_t port = 1234;
        int rc;

        dev->transport = M2SDR_TRANSPORT_LITEETH;

        if (m2sdr_parse_identifier(device_identifier, path_dummy, sizeof(path_dummy),
                                   ip, sizeof(ip), &port) != 0) {
            free(dev);
            return M2SDR_ERR_PARSE;
        }

        if (ip[0] == '\0')
            snprintf(ip, sizeof(ip), "192.168.1.50");

        snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
        /* Etherbone owns the actual network connection; libm2sdr keeps only
         * the resolved address tuple plus the opaque handle. */
        dev->eb = eb_connect(ip, port_str, 1);
        if (!dev->eb) {
            free(dev);
            return M2SDR_ERR_IO;
        }
        if (eb_set_timeout(dev->eb, M2SDR_LITEETH_CONTROL_TIMEOUT_MS) != EB_ERR_OK) {
            eb_disconnect(&dev->eb);
            free(dev);
            return M2SDR_ERR_IO;
        }
        rc = m2sdr_liteeth_probe_control(dev);
        if (rc != M2SDR_ERR_OK) {
            eb_disconnect(&dev->eb);
            free(dev);
            return rc;
        }

        snprintf(dev->eth_ip, sizeof(dev->eth_ip), "%s", ip);
        dev->eth_port = port;
    }
#else
    free(dev);
    return M2SDR_ERR_UNSUPPORTED;
#endif

    *dev_out = dev;
    return M2SDR_ERR_OK;
}

/* Close a device and release all transport-specific resources it owns. */
void m2sdr_close(struct m2sdr_dev *dev)
{
    if (!dev)
        return;

#ifdef USE_LITEPCIE
    m2sdr_stream_cleanup(dev);
    if (dev->fd >= 0)
        close(dev->fd);
    dev->fd = -1;
#endif

#ifdef USE_LITEETH
    m2sdr_stream_cleanup(dev);
    if (dev->udp_inited) {
        liteeth_udp_cleanup(&dev->udp);
        dev->udp_inited = 0;
    }
    if (dev->eb) {
        eb_disconnect(&dev->eb);
        dev->eb = NULL;
    }
#endif

    dev->ad9361_phy = NULL;
    free(dev);
}

/* Basic accessors */
/*-----------------*/

/* Read a CSR through the currently-selected backend HAL. */
int m2sdr_reg_read(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val)
{
    return m2sdr_hal_readl(dev, addr, val);
}

/* Write a CSR through the currently-selected backend HAL. */
int m2sdr_reg_write(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    return m2sdr_hal_writel(dev, addr, val);
}

/* Legacy escape hatches kept for Soapy and advanced utilities. */
/* Return the underlying PCIe file descriptor when available. */
int m2sdr_get_fd(struct m2sdr_dev *dev)
{
#ifdef USE_LITEPCIE
    if (!dev)
        return -1;
    return dev->fd;
#else
    (void)dev;
    return -1;
#endif
}

/* Return the raw backend handle used by advanced integrations. */
void *m2sdr_get_eb_handle(struct m2sdr_dev *dev)
{
    if (!dev)
        return NULL;

#ifdef USE_LITEETH
    return dev->eb;
#else
    return NULL;
#endif
}

int m2sdr_get_transport(struct m2sdr_dev *dev, enum m2sdr_transport_kind *transport)
{
    if (!dev || !transport)
        return M2SDR_ERR_INVAL;

    switch (dev->transport) {
    case M2SDR_TRANSPORT_LITEPCIE:
        *transport = M2SDR_TRANSPORT_KIND_LITEPCIE;
        return M2SDR_ERR_OK;
    case M2SDR_TRANSPORT_LITEETH:
        *transport = M2SDR_TRANSPORT_KIND_LITEETH;
        return M2SDR_ERR_OK;
    default:
        *transport = M2SDR_TRANSPORT_KIND_UNKNOWN;
        return M2SDR_ERR_STATE;
    }
}

/* Return the raw backend handle used by advanced integrations.
 * This is kept for backward compatibility; prefer m2sdr_get_fd() and
 * m2sdr_get_eb_handle() in new code. */
void *m2sdr_get_handle(struct m2sdr_dev *dev)
{
    if (!dev)
        return NULL;

#ifdef USE_LITEPCIE
    return (void *)(intptr_t)dev->fd;
#elif defined(USE_LITEETH)
    return dev->eb;
#else
    return NULL;
#endif
}

/* Discovery / Identification */
/*----------------------------*/

/* Read stable board identity fields from an already-open device. */
int m2sdr_get_device_info(struct m2sdr_dev *dev, struct m2sdr_devinfo *info)
{
    uint64_t dna = 0;

    if (!dev || !info)
        return M2SDR_ERR_INVAL;

    memset(info, 0, sizeof(*info));
    m2sdr_fill_transport_info(dev, info);

    /* The DNA-derived serial is the most stable identity exposed by current
     * gateware, so use it when available. */
    if (m2sdr_get_fpga_dna(dev, &dna) == M2SDR_ERR_OK)
        snprintf(info->serial, sizeof(info->serial), "%llx", (unsigned long long)dna);

#ifdef CSR_IDENTIFIER_MEM_BASE
    m2sdr_read_identifier_mem(dev, info->identification, sizeof(info->identification));
#endif

    return M2SDR_ERR_OK;
}

/* Enumerate the set of reachable devices for the active backend. */
int m2sdr_get_device_list(struct m2sdr_devinfo *list, size_t max, size_t *count)
{
    size_t found = 0;

    if (!list || !count)
        return M2SDR_ERR_INVAL;

#ifdef USE_LITEPCIE
    for (int i = 0; i < 8 && found < max; i++) {
        char dev_id[64];

        /* Match the historical expectation that boards enumerate as a small,
         * fixed /dev/m2sdrN set instead of walking sysfs. */
        snprintf(dev_id, sizeof(dev_id), "pcie:/dev/m2sdr%d", i);
        if (m2sdr_probe_one(dev_id, &list[found]) == M2SDR_ERR_OK)
            found++;
    }
#elif defined(USE_LITEETH)
    if (found < max) {
        char dev_id[64];

        /* Etherbone discovery is still a single default target probe rather
         * than a subnet scan, which keeps this helper deterministic. */
        snprintf(dev_id, sizeof(dev_id), "eth:192.168.1.50:1234");
        if (m2sdr_probe_one(dev_id, &list[found]) == M2SDR_ERR_OK)
            found++;
    }
#endif

    *count = found;
    return M2SDR_ERR_OK;
}

/* Read the bitstream capability registers into a structured view. */
int m2sdr_get_capabilities(struct m2sdr_dev *dev, struct m2sdr_capabilities *caps)
{
    uint32_t value = 0;

    if (!dev || !caps)
        return M2SDR_ERR_INVAL;

    if (m2sdr_reg_read(dev, CSR_CAPABILITY_API_VERSION_ADDR, &value) != 0)
        return M2SDR_ERR_IO;
    caps->api_version = value;

    if (m2sdr_reg_read(dev, CSR_CAPABILITY_FEATURES_ADDR, &value) != 0)
        return M2SDR_ERR_IO;
    caps->features = value;

    if (m2sdr_reg_read(dev, CSR_CAPABILITY_BOARD_INFO_ADDR, &value) != 0)
        return M2SDR_ERR_IO;
    caps->board_info = value;

    if (m2sdr_reg_read(dev, CSR_CAPABILITY_PCIE_CONFIG_ADDR, &value) != 0)
        return M2SDR_ERR_IO;
    caps->pcie_config = value;

    if (m2sdr_reg_read(dev, CSR_CAPABILITY_ETH_CONFIG_ADDR, &value) != 0)
        return M2SDR_ERR_IO;
    caps->eth_config = value;

    if (m2sdr_reg_read(dev, CSR_CAPABILITY_SATA_CONFIG_ADDR, &value) != 0)
        return M2SDR_ERR_IO;
    caps->sata_config = value;

    return M2SDR_ERR_OK;
}

/* Return the FPGA identifier string directly from the identifier memory. */
int m2sdr_get_identifier(struct m2sdr_dev *dev, char *buf, size_t len)
{
    if (!dev || !buf || len == 0)
        return M2SDR_ERR_INVAL;

#ifdef CSR_IDENTIFIER_MEM_BASE
    return m2sdr_read_identifier_mem(dev, buf, len);
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Board information */
/*-------------------*/

/* Read the FPGA git hash when the current gateware exposes it. */
int m2sdr_get_fpga_git_hash(struct m2sdr_dev *dev, uint32_t *hash)
{
    if (!dev || !hash)
        return M2SDR_ERR_INVAL;

#ifdef CSR_GIT_BASE
    if (m2sdr_reg_read(dev, CSR_GIT_BASE, hash) != 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Reserved placeholder for future clock metadata reporting. */
int m2sdr_get_clock_info(struct m2sdr_dev *dev, struct m2sdr_clock_info *info)
{
    if (!dev || !info)
        return M2SDR_ERR_INVAL;

    return M2SDR_ERR_UNSUPPORTED;
}

int m2sdr_get_ptp_discipline_config(struct m2sdr_dev *dev, struct m2sdr_ptp_discipline_config *cfg)
{
#if defined(CSR_PTP_DISCIPLINE_CONTROL_ADDR)
    bool has_eth_ptp = false;
    uint32_t control = 0;
    uint32_t value = 0;
    int ret = 0;

    if (!dev || !cfg)
        return M2SDR_ERR_INVAL;

    memset(cfg, 0, sizeof(*cfg));

    ret = m2sdr_has_eth_ptp(dev, &has_eth_ptp);
    if (ret != M2SDR_ERR_OK)
        return ret;
    if (!has_eth_ptp)
        return M2SDR_ERR_UNSUPPORTED;

    ret = m2sdr_get_ptp_discipline_control(dev, &control);
    if (ret != M2SDR_ERR_OK)
        return ret;

#if defined(CSR_PTP_DISCIPLINE_CONTROL_ENABLE_OFFSET) && defined(CSR_PTP_DISCIPLINE_CONTROL_ENABLE_SIZE)
    cfg->enable = ((control >> CSR_PTP_DISCIPLINE_CONTROL_ENABLE_OFFSET) &
        ((1u << CSR_PTP_DISCIPLINE_CONTROL_ENABLE_SIZE) - 1u)) != 0;
#endif
#if defined(CSR_PTP_DISCIPLINE_CONTROL_HOLDOVER_OFFSET) && defined(CSR_PTP_DISCIPLINE_CONTROL_HOLDOVER_SIZE)
    cfg->holdover = ((control >> CSR_PTP_DISCIPLINE_CONTROL_HOLDOVER_OFFSET) &
        ((1u << CSR_PTP_DISCIPLINE_CONTROL_HOLDOVER_SIZE) - 1u)) != 0;
#endif

#ifdef CSR_PTP_DISCIPLINE_UPDATE_CYCLES_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_UPDATE_CYCLES_ADDR, &cfg->update_cycles) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_COARSE_THRESHOLD_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_COARSE_THRESHOLD_ADDR, &cfg->coarse_threshold_ns) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_PHASE_THRESHOLD_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_PHASE_THRESHOLD_ADDR, &cfg->phase_threshold_ns) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_LOCK_WINDOW_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_LOCK_WINDOW_ADDR, &cfg->lock_window_ns) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_PHASE_STEP_SHIFT_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_PHASE_STEP_SHIFT_ADDR, &value) != 0)
        return M2SDR_ERR_IO;
    cfg->phase_step_shift = (uint8_t)value;
#endif
#ifdef CSR_PTP_DISCIPLINE_PHASE_STEP_MAX_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_PHASE_STEP_MAX_ADDR, &cfg->phase_step_max_ns) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_TRIM_LIMIT_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_TRIM_LIMIT_ADDR, &cfg->trim_limit) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_P_GAIN_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_P_GAIN_ADDR, &value) != 0)
        return M2SDR_ERR_IO;
    cfg->p_gain = (uint16_t)value;
#endif

    return M2SDR_ERR_OK;
#else
    (void)dev;
    (void)cfg;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

int m2sdr_set_ptp_discipline_config(struct m2sdr_dev *dev, const struct m2sdr_ptp_discipline_config *cfg)
{
#if defined(CSR_PTP_DISCIPLINE_CONTROL_ADDR)
    bool has_eth_ptp = false;
    int ret = 0;

    if (!dev || !cfg)
        return M2SDR_ERR_INVAL;

    ret = m2sdr_has_eth_ptp(dev, &has_eth_ptp);
    if (ret != M2SDR_ERR_OK)
        return ret;
    if (!has_eth_ptp)
        return M2SDR_ERR_UNSUPPORTED;

    if (cfg->phase_step_shift > 63u)
        return M2SDR_ERR_RANGE;

#ifdef CSR_PTP_DISCIPLINE_UPDATE_CYCLES_ADDR
    if (m2sdr_reg_write(dev, CSR_PTP_DISCIPLINE_UPDATE_CYCLES_ADDR, cfg->update_cycles) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_COARSE_THRESHOLD_ADDR
    if (m2sdr_reg_write(dev, CSR_PTP_DISCIPLINE_COARSE_THRESHOLD_ADDR, cfg->coarse_threshold_ns) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_PHASE_THRESHOLD_ADDR
    if (m2sdr_reg_write(dev, CSR_PTP_DISCIPLINE_PHASE_THRESHOLD_ADDR, cfg->phase_threshold_ns) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_LOCK_WINDOW_ADDR
    if (m2sdr_reg_write(dev, CSR_PTP_DISCIPLINE_LOCK_WINDOW_ADDR, cfg->lock_window_ns) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_PHASE_STEP_SHIFT_ADDR
    if (m2sdr_reg_write(dev, CSR_PTP_DISCIPLINE_PHASE_STEP_SHIFT_ADDR, cfg->phase_step_shift) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_PHASE_STEP_MAX_ADDR
    if (m2sdr_reg_write(dev, CSR_PTP_DISCIPLINE_PHASE_STEP_MAX_ADDR, cfg->phase_step_max_ns) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_TRIM_LIMIT_ADDR
    if (m2sdr_reg_write(dev, CSR_PTP_DISCIPLINE_TRIM_LIMIT_ADDR, cfg->trim_limit) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_P_GAIN_ADDR
    if (m2sdr_reg_write(dev, CSR_PTP_DISCIPLINE_P_GAIN_ADDR, cfg->p_gain) != 0)
        return M2SDR_ERR_IO;
#endif

    return m2sdr_write_ptp_discipline_control(dev, cfg->enable, cfg->holdover);
#else
    (void)dev;
    (void)cfg;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

int m2sdr_clear_ptp_counters(struct m2sdr_dev *dev)
{
#if defined(CSR_PTP_DISCIPLINE_CLEAR_COUNTERS_ADDR)
    bool has_eth_ptp = false;
    int ret = 0;

    if (!dev)
        return M2SDR_ERR_INVAL;

    ret = m2sdr_has_eth_ptp(dev, &has_eth_ptp);
    if (ret != M2SDR_ERR_OK)
        return ret;
    if (!has_eth_ptp)
        return M2SDR_ERR_UNSUPPORTED;

    if (m2sdr_reg_write(dev, CSR_PTP_DISCIPLINE_CLEAR_COUNTERS_ADDR, 1) != 0)
        return M2SDR_ERR_IO;

    return M2SDR_ERR_OK;
#else
    (void)dev;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Read the current Ethernet PTP and time-discipline status when available. */
int m2sdr_get_ptp_status(struct m2sdr_dev *dev, struct m2sdr_ptp_status *status)
{
#if defined(CSR_ETH_PTP_MASTER_IP_ADDR) && defined(CSR_PTP_DISCIPLINE_STATUS_ADDR)
    bool has_eth_ptp = false;
    uint32_t reg = 0;
    uint32_t value = 0;
    uint64_t u64 = 0;
    int ret = 0;

    if (!dev || !status)
        return M2SDR_ERR_INVAL;

    memset(status, 0, sizeof(*status));

    ret = m2sdr_has_eth_ptp(dev, &has_eth_ptp);
    if (ret != M2SDR_ERR_OK)
        return ret;
    if (!has_eth_ptp)
        return M2SDR_ERR_UNSUPPORTED;

    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_STATUS_ADDR, &reg) != 0)
        return M2SDR_ERR_IO;
    status->enabled = ((reg >> CSR_PTP_DISCIPLINE_STATUS_ENABLE_OFFSET) &
        ((1u << CSR_PTP_DISCIPLINE_STATUS_ENABLE_SIZE) - 1u)) != 0;
    status->active = ((reg >> CSR_PTP_DISCIPLINE_STATUS_ACTIVE_OFFSET) &
        ((1u << CSR_PTP_DISCIPLINE_STATUS_ACTIVE_SIZE) - 1u)) != 0;
    status->ptp_locked = ((reg >> CSR_PTP_DISCIPLINE_STATUS_PTP_LOCKED_OFFSET) &
        ((1u << CSR_PTP_DISCIPLINE_STATUS_PTP_LOCKED_SIZE) - 1u)) != 0;
    status->time_locked = ((reg >> CSR_PTP_DISCIPLINE_STATUS_TIME_LOCKED_OFFSET) &
        ((1u << CSR_PTP_DISCIPLINE_STATUS_TIME_LOCKED_SIZE) - 1u)) != 0;
    status->holdover = ((reg >> CSR_PTP_DISCIPLINE_STATUS_HOLDOVER_OFFSET) &
        ((1u << CSR_PTP_DISCIPLINE_STATUS_HOLDOVER_SIZE) - 1u)) != 0;
    status->state = (uint8_t)((reg >> CSR_PTP_DISCIPLINE_STATUS_STATE_OFFSET) &
        ((1u << CSR_PTP_DISCIPLINE_STATUS_STATE_SIZE) - 1u));

    if (m2sdr_reg_read(dev, CSR_ETH_PTP_MASTER_IP_ADDR, &status->master_ip) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_TIME_INC_ADDR, &status->time_inc) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_read_reg_u64(dev, CSR_PTP_DISCIPLINE_LAST_ERROR_ADDR, &u64) != 0)
        return M2SDR_ERR_IO;
    status->last_error_ns = (int64_t)u64;

#ifdef CSR_PTP_DISCIPLINE_LAST_PTP_TIME_ADDR
    if (m2sdr_read_reg_u64(dev, CSR_PTP_DISCIPLINE_LAST_PTP_TIME_ADDR, &status->last_ptp_time_ns) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_LAST_LOCAL_TIME_ADDR
    if (m2sdr_read_reg_u64(dev, CSR_PTP_DISCIPLINE_LAST_LOCAL_TIME_ADDR, &status->last_local_time_ns) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_IDENTITY_LOCAL_CLOCK_ID_ADDR
    if (m2sdr_read_reg_u64(dev, CSR_PTP_IDENTITY_LOCAL_CLOCK_ID_ADDR, &status->local_port.clock_id) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_IDENTITY_LOCAL_PORT_NUMBER_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_IDENTITY_LOCAL_PORT_NUMBER_ADDR, &value) != 0)
        return M2SDR_ERR_IO;
    status->local_port.port_number = (uint16_t)value;
#endif
#ifdef CSR_PTP_IDENTITY_MASTER_CLOCK_ID_ADDR
    if (m2sdr_read_reg_u64(dev, CSR_PTP_IDENTITY_MASTER_CLOCK_ID_ADDR, &status->master_port.clock_id) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_IDENTITY_MASTER_PORT_NUMBER_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_IDENTITY_MASTER_PORT_NUMBER_ADDR, &value) != 0)
        return M2SDR_ERR_IO;
    status->master_port.port_number = (uint16_t)value;
#endif
#ifdef CSR_PTP_IDENTITY_UPDATE_COUNT_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_IDENTITY_UPDATE_COUNT_ADDR, &status->identity_updates) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_COARSE_STEPS_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_COARSE_STEPS_ADDR, &status->coarse_steps) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_PHASE_STEPS_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_PHASE_STEPS_ADDR, &status->phase_steps) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_RATE_UPDATES_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_RATE_UPDATES_ADDR, &status->rate_updates) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_PTP_LOCK_LOSSES_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_PTP_LOCK_LOSSES_ADDR, &status->ptp_lock_losses) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_PTP_DISCIPLINE_TIME_LOCK_LOSSES_ADDR
    if (m2sdr_reg_read(dev, CSR_PTP_DISCIPLINE_TIME_LOCK_LOSSES_ADDR, &status->time_lock_losses) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_ETH_PTP_INVALID_HEADER_COUNT_ADDR
    if (m2sdr_reg_read(dev, CSR_ETH_PTP_INVALID_HEADER_COUNT_ADDR, &status->invalid_header_count) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_ETH_PTP_WRONG_PEER_COUNT_ADDR
    if (m2sdr_reg_read(dev, CSR_ETH_PTP_WRONG_PEER_COUNT_ADDR, &status->wrong_peer_count) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_ETH_PTP_WRONG_REQUESTER_COUNT_ADDR
    if (m2sdr_reg_read(dev, CSR_ETH_PTP_WRONG_REQUESTER_COUNT_ADDR, &status->wrong_requester_count) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_ETH_PTP_RX_TIMEOUT_COUNT_ADDR
    if (m2sdr_reg_read(dev, CSR_ETH_PTP_RX_TIMEOUT_COUNT_ADDR, &status->rx_timeout_count) != 0)
        return M2SDR_ERR_IO;
#endif
#ifdef CSR_ETH_PTP_ANNOUNCE_EXPIRY_COUNT_ADDR
    if (m2sdr_reg_read(dev, CSR_ETH_PTP_ANNOUNCE_EXPIRY_COUNT_ADDR, &status->announce_expiry_count) != 0)
        return M2SDR_ERR_IO;
#endif

    return M2SDR_ERR_OK;
#else
    (void)dev;
    (void)status;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Latch and read the current hardware time in nanoseconds. */
int m2sdr_get_time(struct m2sdr_dev *dev, uint64_t *time_ns)
{
    uint32_t ctrl = 0;

    if (!dev || !time_ns)
        return M2SDR_ERR_INVAL;

    if (m2sdr_reg_read(dev, CSR_TIME_GEN_CONTROL_ADDR, &ctrl) != 0)
        return M2SDR_ERR_IO;

    /* The time generator snapshots into the read registers on a READ pulse. */
    if (m2sdr_reg_write(dev, CSR_TIME_GEN_CONTROL_ADDR,
        ctrl | (1u << CSR_TIME_GEN_CONTROL_READ_OFFSET)) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_write(dev, CSR_TIME_GEN_CONTROL_ADDR,
        ctrl & ~(1u << CSR_TIME_GEN_CONTROL_READ_OFFSET)) != 0)
        return M2SDR_ERR_IO;

    return m2sdr_read_reg_u64(dev, CSR_TIME_GEN_READ_TIME_ADDR, time_ns);
}

/* Program the hardware time generator with an absolute time value. */
int m2sdr_set_time(struct m2sdr_dev *dev, uint64_t time_ns)
{
    bool ptp_owned = false;
    int ret = 0;

    if (!dev)
        return M2SDR_ERR_INVAL;

    ret = m2sdr_time_owned_by_ptp(dev, &ptp_owned);
    if (ret != M2SDR_ERR_OK)
        return ret;
    if (ptp_owned)
        return M2SDR_ERR_STATE;

    if (m2sdr_reg_write(dev, CSR_TIME_GEN_WRITE_TIME_ADDR + 0, (uint32_t)(time_ns >> 32)) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_write(dev, CSR_TIME_GEN_WRITE_TIME_ADDR + 4, (uint32_t)(time_ns & 0xffffffffu)) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_write(dev, CSR_TIME_GEN_CONTROL_ADDR, (1 << CSR_TIME_GEN_CONTROL_WRITE_OFFSET)) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_write(dev, CSR_TIME_GEN_CONTROL_ADDR, 0) != 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
}

/* Read the 64-bit FPGA DNA value used as the stable board serial source. */
int m2sdr_get_fpga_dna(struct m2sdr_dev *dev, uint64_t *dna)
{
    if (!dev || !dna)
        return M2SDR_ERR_INVAL;

#ifdef CSR_DNA_BASE
    return m2sdr_read_reg_u64(dev, CSR_DNA_ID_ADDR, dna);
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Convert raw XADC measurements into engineering units. */
int m2sdr_get_fpga_sensors(struct m2sdr_dev *dev, struct m2sdr_fpga_sensors *sensors)
{
    uint32_t temp = 0;
    uint32_t vccint = 0;
    uint32_t vccaux = 0;
    uint32_t vccbram = 0;

    if (!dev || !sensors)
        return M2SDR_ERR_INVAL;

#ifdef CSR_XADC_BASE
    if (m2sdr_reg_read(dev, CSR_XADC_TEMPERATURE_ADDR, &temp) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_read(dev, CSR_XADC_VCCINT_ADDR, &vccint) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_read(dev, CSR_XADC_VCCAUX_ADDR, &vccaux) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_read(dev, CSR_XADC_VCCBRAM_ADDR, &vccbram) != 0)
        return M2SDR_ERR_IO;

    sensors->temperature_c = (double)temp * 503.975 / 4096.0 - 273.15;
    sensors->vccint_v      = (double)vccint  / 4096.0 * 3.0;
    sensors->vccaux_v      = (double)vccaux  / 4096.0 * 3.0;
    sensors->vccbram_v     = (double)vccbram / 4096.0 * 3.0;
    return M2SDR_ERR_OK;
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Control helpers */
/*-----------------*/

static void m2sdr_reset_keep_error(int rc, int *status)
{
    if (rc != M2SDR_ERR_OK && rc != M2SDR_ERR_UNSUPPORTED && *status == M2SDR_ERR_OK)
        *status = rc;
}

/* Select 8-bit or 16-bit AD9361 sample packing in the FPGA datapath. */
int m2sdr_set_bitmode(struct m2sdr_dev *dev, bool enable_8bit)
{
    if (!dev)
        return M2SDR_ERR_INVAL;

#ifdef CSR_AD9361_BITMODE_ADDR
    if (m2sdr_reg_write(dev, CSR_AD9361_BITMODE_ADDR, enable_8bit ? 1 : 0) != 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Enable or disable the FPGA DMA loopback path when the backend supports it. */
int m2sdr_set_dma_loopback(struct m2sdr_dev *dev, bool enable)
{
    if (!dev)
        return M2SDR_ERR_INVAL;

#ifdef CSR_PCIE_DMA0_LOOPBACK_ENABLE_ADDR
    if (m2sdr_reg_write(dev, CSR_PCIE_DMA0_LOOPBACK_ENABLE_ADDR, enable ? 1 : 0) != 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Enable or disable the common TX stream to RX stream loopback path. */
int m2sdr_set_txrx_loopback(struct m2sdr_dev *dev, bool enable)
{
    if (!dev)
        return M2SDR_ERR_INVAL;

#if defined(CSR_TXRX_LOOPBACK_CONTROL_ADDR) && \
    defined(CSR_TXRX_LOOPBACK_CONTROL_ENABLE_OFFSET)
    uint32_t control = (enable ? 1u : 0u) << CSR_TXRX_LOOPBACK_CONTROL_ENABLE_OFFSET;
    if (m2sdr_reg_write(dev, CSR_TXRX_LOOPBACK_CONTROL_ADDR, control) != 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
#else
    (void)enable;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Enable or disable the RFIC PHY TX data to RX data loopback path. */
int m2sdr_set_rfic_data_loopback(struct m2sdr_dev *dev, bool enable)
{
    uint32_t control = 0;
    uint32_t mask;

    if (!dev)
        return M2SDR_ERR_INVAL;

#if defined(CSR_AD9361_PHY_CONTROL_ADDR) && \
    defined(CSR_AD9361_PHY_CONTROL_LOOPBACK_OFFSET) && \
    defined(CSR_AD9361_PHY_CONTROL_LOOPBACK_SIZE)
    mask = ((1u << CSR_AD9361_PHY_CONTROL_LOOPBACK_SIZE) - 1u) <<
        CSR_AD9361_PHY_CONTROL_LOOPBACK_OFFSET;
    if (m2sdr_reg_read(dev, CSR_AD9361_PHY_CONTROL_ADDR, &control) != 0)
        return M2SDR_ERR_IO;
    control &= ~mask;
    if (enable)
        control |= 1u << CSR_AD9361_PHY_CONTROL_LOOPBACK_OFFSET;
    if (m2sdr_reg_write(dev, CSR_AD9361_PHY_CONTROL_ADDR, control) != 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
#else
    (void)enable;
    (void)control;
    (void)mask;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Read back the RFIC PHY TX data to RX data loopback state. */
int m2sdr_get_rfic_data_loopback(struct m2sdr_dev *dev, bool *enabled)
{
    uint32_t control = 0;
    uint32_t mask;

    if (!dev || !enabled)
        return M2SDR_ERR_INVAL;

#if defined(CSR_AD9361_PHY_CONTROL_ADDR) && \
    defined(CSR_AD9361_PHY_CONTROL_LOOPBACK_OFFSET) && \
    defined(CSR_AD9361_PHY_CONTROL_LOOPBACK_SIZE)
    mask = ((1u << CSR_AD9361_PHY_CONTROL_LOOPBACK_SIZE) - 1u) <<
        CSR_AD9361_PHY_CONTROL_LOOPBACK_OFFSET;
    if (m2sdr_reg_read(dev, CSR_AD9361_PHY_CONTROL_ADDR, &control) != 0)
        return M2SDR_ERR_IO;
    *enabled = (control & mask) != 0;
    return M2SDR_ERR_OK;
#else
    (void)control;
    (void)mask;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Return the FPGA streaming datapath to the neutral state expected by normal
 * RF use and standalone diagnostics. This intentionally does not touch AD9361
 * RF state; use m2sdr_apply_config() or RF setters for that. */
int m2sdr_reset_datapath(struct m2sdr_dev *dev)
{
    int status = M2SDR_ERR_OK;

    if (!dev)
        return M2SDR_ERR_INVAL;

    m2sdr_reset_keep_error(m2sdr_liteeth_rx_stream_deactivate(dev), &status);
    m2sdr_reset_keep_error(m2sdr_liteeth_tx_stream_deactivate(dev), &status);

#ifdef CSR_CROSSBAR_MUX_SEL_ADDR
    if (m2sdr_reg_write(dev, CSR_CROSSBAR_MUX_SEL_ADDR, 0) != 0)
        m2sdr_reset_keep_error(M2SDR_ERR_IO, &status);
#endif
#ifdef CSR_CROSSBAR_DEMUX_SEL_ADDR
    if (m2sdr_reg_write(dev, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0) != 0)
        m2sdr_reset_keep_error(M2SDR_ERR_IO, &status);
#endif

    m2sdr_reset_keep_error(m2sdr_set_txrx_loopback(dev, false), &status);
    m2sdr_reset_keep_error(m2sdr_set_rfic_data_loopback(dev, false), &status);
    m2sdr_reset_keep_error(m2sdr_set_fpga_prbs_tx(dev, false), &status);
    m2sdr_reset_keep_error(m2sdr_set_rx_header(dev, false, false), &status);
    m2sdr_reset_keep_error(m2sdr_set_tx_header(dev, false), &status);
    m2sdr_reset_keep_error(m2sdr_set_bitmode(dev, false), &status);

    return status;
}

/* Enable or disable RX-side DMA headers and remember whether the sync API
 * should strip them before returning samples to the caller. */
int m2sdr_set_rx_header(struct m2sdr_dev *dev, bool enable, bool strip_header)
{
    if (!dev)
        return M2SDR_ERR_INVAL;

    dev->rx_header_enable = enable ? 1 : 0;
    dev->rx_strip_header  = strip_header ? 1 : 0;

    if (m2sdr_reg_write(dev, CSR_HEADER_RX_CONTROL_ADDR,
        (1 << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
        ((enable ? 1 : 0) << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET)) != 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
}

/* Enable or disable TX-side insertion of the FPGA DMA header. */
int m2sdr_set_tx_header(struct m2sdr_dev *dev, bool enable)
{
    if (!dev)
        return M2SDR_ERR_INVAL;

    dev->tx_header_enable = enable ? 1 : 0;

    if (m2sdr_reg_write(dev, CSR_HEADER_TX_CONTROL_ADDR,
        (1 << CSR_HEADER_TX_CONTROL_ENABLE_OFFSET) |
        ((enable ? 1 : 0) << CSR_HEADER_TX_CONTROL_HEADER_ENABLE_OFFSET)) != 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
}

/* Configure GPIO ownership, loopback mode, and the data source selection. */
int m2sdr_gpio_config(struct m2sdr_dev *dev, bool enable, bool loopback, bool source_csr)
{
    if (!dev)
        return M2SDR_ERR_INVAL;

#ifdef CSR_GPIO_BASE
    uint32_t control = 0;

    if (m2sdr_reg_read(dev, CSR_GPIO_CONTROL_ADDR, &control) != 0)
        return M2SDR_ERR_IO;

    if (enable) {
        control |= (1 << CSR_GPIO_CONTROL_ENABLE_OFFSET);

        /* Loopback and source selection are meaningful only while GPIO control
         * is owned by the CSR path. */
        if (loopback)
            control |= (1 << CSR_GPIO_CONTROL_LOOPBACK_OFFSET);
        else
            control &= ~(1 << CSR_GPIO_CONTROL_LOOPBACK_OFFSET);

        if (source_csr)
            control |= (1 << CSR_GPIO_CONTROL_SOURCE_OFFSET);
        else
            control &= ~(1 << CSR_GPIO_CONTROL_SOURCE_OFFSET);
    } else {
        control &= ~(1 << CSR_GPIO_CONTROL_ENABLE_OFFSET);
    }

    if (m2sdr_reg_write(dev, CSR_GPIO_CONTROL_ADDR, control) != 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
#else
    (void)enable;
    (void)loopback;
    (void)source_csr;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Drive the 4-bit GPIO output and output-enable fields. */
int m2sdr_gpio_write(struct m2sdr_dev *dev, uint8_t value, uint8_t oe)
{
    if (!dev)
        return M2SDR_ERR_INVAL;

#ifdef CSR_GPIO_BASE
    if (m2sdr_reg_write(dev, CSR_GPIO__O_ADDR, value & 0xF) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_write(dev, CSR_GPIO_OE_ADDR, oe & 0xF) != 0)
        return M2SDR_ERR_IO;
    return M2SDR_ERR_OK;
#else
    (void)value;
    (void)oe;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

/* Read the current 4-bit GPIO input value. */
int m2sdr_gpio_read(struct m2sdr_dev *dev, uint8_t *value)
{
    if (!dev || !value)
        return M2SDR_ERR_INVAL;

#ifdef CSR_GPIO_BASE
    uint32_t v = 0;

    if (m2sdr_reg_read(dev, CSR_GPIO__I_ADDR, &v) != 0)
        return M2SDR_ERR_IO;

    *value = v & 0xF;
    return M2SDR_ERR_OK;
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif
}
