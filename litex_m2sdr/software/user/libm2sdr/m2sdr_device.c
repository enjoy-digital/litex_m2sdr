/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR device API
 */

#include "m2sdr_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

#include "csr.h"

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
    default:                    return "unknown";
    }
}

const char *m2sdr_strerror(int err)
{
    return m2sdr_err_str(err);
}

static void m2sdr_trim_nl(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    if (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r'))
        s[n-1] = '\0';
}

static void m2sdr_default_device(char *out, size_t out_len)
{
#ifdef USE_LITEPCIE
    snprintf(out, out_len, "/dev/m2sdr0");
#elif defined(USE_LITEETH)
    snprintf(out, out_len, "192.168.1.50:1234");
#else
    if (out_len) out[0] = '\0';
#endif
}

static int m2sdr_parse_identifier(const char *id, char *path_out, size_t path_len,
                                  char *ip_out, size_t ip_len, uint16_t *port_out)
{
    char tmp[M2SDR_DEVICE_STR_MAX];
    if (!id || !id[0]) {
        m2sdr_default_device(tmp, sizeof(tmp));
        id = tmp;
    }

    if (!strncmp(id, "pcie:", 5)) {
        if (strnlen(id + 5, path_len) >= path_len)
            return -1;
        snprintf(path_out, path_len, "%s", id + 5);
        return 0;
    }
    if (!strncmp(id, "eth:", 4)) {
        id += 4;
    }

    if (id[0] == '/') {
        if (strnlen(id, path_len) >= path_len)
            return -1;
        snprintf(path_out, path_len, "%s", id);
        return 0;
    }

    /* treat as ip[:port] */
    if (strnlen(id, ip_len) >= ip_len)
        return -1;
    snprintf(ip_out, ip_len, "%s", id);
    char *colon = strchr(ip_out, ':');
    if (colon) {
        *colon = '\0';
        *port_out = (uint16_t)strtoul(colon + 1, NULL, 0);
    }
    return 0;
}

int m2sdr_open(struct m2sdr_dev **dev_out, const char *device_identifier)
{
    if (!dev_out)
        return M2SDR_ERR_INVAL;

    struct m2sdr_dev *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return M2SDR_ERR_NO_MEM;

#ifdef USE_LITEPCIE
    dev->fd = -1;
    dev->transport = M2SDR_TRANSPORT_LITEPCIE;
    char path[M2SDR_DEVICE_STR_MAX] = {0};
    char ip_dummy[64] = {0};
    uint16_t port_dummy = 1234;
    if (m2sdr_parse_identifier(device_identifier, path, sizeof(path), ip_dummy, sizeof(ip_dummy), &port_dummy) != 0) {
        free(dev);
        return M2SDR_ERR_INVAL;
    }

    if (path[0] == '\0')
        m2sdr_default_device(path, sizeof(path));

    dev->fd = open(path, O_RDWR | O_CLOEXEC);
    if (dev->fd < 0) {
        free(dev);
        return M2SDR_ERR_IO;
    }
    snprintf(dev->device_path, sizeof(dev->device_path), "%s", path);

#elif defined(USE_LITEETH)
    dev->transport = M2SDR_TRANSPORT_LITEETH;
    char path_dummy[M2SDR_DEVICE_STR_MAX] = {0};
    char ip[64] = {0};
    uint16_t port = 1234;
    if (m2sdr_parse_identifier(device_identifier, path_dummy, sizeof(path_dummy), ip, sizeof(ip), &port) != 0) {
        free(dev);
        return M2SDR_ERR_INVAL;
    }
    if (ip[0] == '\0')
        snprintf(ip, sizeof(ip), "192.168.1.50");

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    dev->eb = eb_connect(ip, port_str, 1);
    if (!dev->eb) {
        free(dev);
        return M2SDR_ERR_IO;
    }
    snprintf(dev->eth_ip, sizeof(dev->eth_ip), "%s", ip);
    dev->eth_port = port;
#else
    free(dev);
    return M2SDR_ERR_UNSUPPORTED;
#endif

    *dev_out = dev;
    return M2SDR_ERR_OK;
}

void m2sdr_close(struct m2sdr_dev *dev)
{
    if (!dev)
        return;

#ifdef USE_LITEPCIE
    if (dev->fd >= 0)
        close(dev->fd);
    dev->fd = -1;
#endif
#ifdef USE_LITEETH
    if (dev->udp_inited) {
        liteeth_udp_cleanup(&dev->udp);
        dev->udp_inited = 0;
    }
    if (dev->eb) {
        eb_disconnect(&dev->eb);
        dev->eb = NULL;
    }
#endif
    free(dev);
}

int m2sdr_reg_read(struct m2sdr_dev *dev, uint32_t addr, uint32_t *val)
{
    return m2sdr_hal_readl(dev, addr, val);
}

int m2sdr_reg_write(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    return m2sdr_hal_writel(dev, addr, val);
}

int m2sdr_get_fd(struct m2sdr_dev *dev)
{
#ifdef USE_LITEPCIE
    if (!dev) return -1;
    return dev->fd;
#else
    (void)dev;
    return -1;
#endif
}

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

static int m2sdr_read_ident(struct m2sdr_dev *dev, char *buf, size_t len)
{
    if (!buf || len == 0)
        return M2SDR_ERR_INVAL;

    size_t max = len - 1;
    if (max > M2SDR_IDENT_MAX - 1)
        max = M2SDR_IDENT_MAX - 1;

    for (size_t i = 0; i < max; i++) {
        uint32_t v;
        if (m2sdr_reg_read(dev, CSR_IDENTIFIER_MEM_BASE + 4 * i, &v) != 0)
            return M2SDR_ERR_IO;
        buf[i] = (char)(v & 0xff);
        if (buf[i] == '\0')
            break;
    }
    buf[max] = '\0';
    m2sdr_trim_nl(buf);
    return M2SDR_ERR_OK;
}

int m2sdr_get_device_info(struct m2sdr_dev *dev, struct m2sdr_devinfo *info)
{
    if (!dev || !info)
        return M2SDR_ERR_INVAL;

    memset(info, 0, sizeof(*info));

#ifdef USE_LITEPCIE
    snprintf(info->transport, sizeof(info->transport), "litepcie");
    snprintf(info->path, sizeof(info->path), "%s", dev->device_path);
#elif defined(USE_LITEETH)
    snprintf(info->transport, sizeof(info->transport), "liteeth");
    snprintf(info->path, sizeof(info->path), "%s:%u", dev->eth_ip, (unsigned)dev->eth_port);
#endif

    /* Serial from DNA */
    uint32_t high = 0, low = 0;
    if (m2sdr_reg_read(dev, CSR_DNA_ID_ADDR + 0, &high) == 0 &&
        m2sdr_reg_read(dev, CSR_DNA_ID_ADDR + 4, &low) == 0) {
        snprintf(info->serial, sizeof(info->serial), "%x%08x", high, low);
    }

    /* Identification string */
    m2sdr_read_ident(dev, info->identification, sizeof(info->identification));

    return M2SDR_ERR_OK;
}

int m2sdr_get_device_list(struct m2sdr_devinfo *list, size_t max, size_t *count)
{
    if (!list || !count)
        return M2SDR_ERR_INVAL;

    size_t found = 0;

#ifdef USE_LITEPCIE
    for (int i = 0; i < 8 && found < max; i++) {
        char dev_id[64];
        snprintf(dev_id, sizeof(dev_id), "pcie:/dev/m2sdr%d", i);
        struct m2sdr_dev *dev = NULL;
        if (m2sdr_open(&dev, dev_id) != 0)
            continue;
        if (m2sdr_get_device_info(dev, &list[found]) == 0)
            found++;
        m2sdr_close(dev);
    }
#elif defined(USE_LITEETH)
    if (found < max) {
        char dev_id[64];
        snprintf(dev_id, sizeof(dev_id), "eth:192.168.1.50:1234");
        struct m2sdr_dev *dev = NULL;
        if (m2sdr_open(&dev, dev_id) == 0) {
            if (m2sdr_get_device_info(dev, &list[found]) == 0)
                found++;
            m2sdr_close(dev);
        }
    }
#endif

    *count = found;
    return M2SDR_ERR_OK;
}

int m2sdr_get_capabilities(struct m2sdr_dev *dev, struct m2sdr_capabilities *caps)
{
    if (!dev || !caps)
        return M2SDR_ERR_INVAL;

    uint32_t v = 0;
    if (m2sdr_reg_read(dev, CSR_CAPABILITY_API_VERSION_ADDR, &v) != 0)
        return M2SDR_ERR_IO;
    caps->api_version = v;

    if (m2sdr_reg_read(dev, CSR_CAPABILITY_FEATURES_ADDR, &v) != 0)
        return M2SDR_ERR_IO;
    caps->features = v;

    if (m2sdr_reg_read(dev, CSR_CAPABILITY_BOARD_INFO_ADDR, &v) != 0)
        return M2SDR_ERR_IO;
    caps->board_info = v;

    if (m2sdr_reg_read(dev, CSR_CAPABILITY_PCIE_CONFIG_ADDR, &v) != 0)
        return M2SDR_ERR_IO;
    caps->pcie_config = v;

    if (m2sdr_reg_read(dev, CSR_CAPABILITY_ETH_CONFIG_ADDR, &v) != 0)
        return M2SDR_ERR_IO;
    caps->eth_config = v;

    if (m2sdr_reg_read(dev, CSR_CAPABILITY_SATA_CONFIG_ADDR, &v) != 0)
        return M2SDR_ERR_IO;
    caps->sata_config = v;

    return M2SDR_ERR_OK;
}

int m2sdr_get_time(struct m2sdr_dev *dev, uint64_t *time_ns)
{
    if (!dev || !time_ns)
        return M2SDR_ERR_INVAL;

    uint32_t ctrl;
    if (m2sdr_reg_read(dev, CSR_TIME_GEN_CONTROL_ADDR, &ctrl) != 0)
        return M2SDR_ERR_IO;

    m2sdr_reg_write(dev, CSR_TIME_GEN_CONTROL_ADDR, ctrl | 0x2);
    m2sdr_reg_write(dev, CSR_TIME_GEN_CONTROL_ADDR, ctrl & ~0x2);

    uint32_t hi, lo;
    if (m2sdr_reg_read(dev, CSR_TIME_GEN_READ_TIME_ADDR + 0, &hi) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_read(dev, CSR_TIME_GEN_READ_TIME_ADDR + 4, &lo) != 0)
        return M2SDR_ERR_IO;

    *time_ns = ((uint64_t)hi << 32) | lo;
    return M2SDR_ERR_OK;
}

int m2sdr_set_time(struct m2sdr_dev *dev, uint64_t time_ns)
{
    if (!dev)
        return M2SDR_ERR_INVAL;

    m2sdr_reg_write(dev, CSR_TIME_GEN_WRITE_TIME_ADDR + 0, (uint32_t)(time_ns >> 32));
    m2sdr_reg_write(dev, CSR_TIME_GEN_WRITE_TIME_ADDR + 4, (uint32_t)(time_ns & 0xffffffffu));
    m2sdr_reg_write(dev, CSR_TIME_GEN_CONTROL_ADDR, (1 << CSR_TIME_GEN_CONTROL_WRITE_OFFSET));
    m2sdr_reg_write(dev, CSR_TIME_GEN_CONTROL_ADDR, 0);
    return M2SDR_ERR_OK;
}

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

int m2sdr_get_fpga_dna(struct m2sdr_dev *dev, uint64_t *dna)
{
    if (!dev || !dna)
        return M2SDR_ERR_INVAL;
#ifdef CSR_DNA_BASE
    uint32_t high = 0;
    uint32_t low = 0;
    if (m2sdr_reg_read(dev, CSR_DNA_ID_ADDR + 0, &high) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_read(dev, CSR_DNA_ID_ADDR + 4, &low) != 0)
        return M2SDR_ERR_IO;
    *dna = ((uint64_t)high << 32) | (uint64_t)low;
    return M2SDR_ERR_OK;
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

int m2sdr_get_fpga_sensors(struct m2sdr_dev *dev, struct m2sdr_fpga_sensors *sensors)
{
    if (!dev || !sensors)
        return M2SDR_ERR_INVAL;
#ifdef CSR_XADC_BASE
    uint32_t temp = 0;
    uint32_t vccint = 0;
    uint32_t vccaux = 0;
    uint32_t vccbram = 0;
    if (m2sdr_reg_read(dev, CSR_XADC_TEMPERATURE_ADDR, &temp) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_read(dev, CSR_XADC_VCCINT_ADDR, &vccint) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_read(dev, CSR_XADC_VCCAUX_ADDR, &vccaux) != 0)
        return M2SDR_ERR_IO;
    if (m2sdr_reg_read(dev, CSR_XADC_VCCBRAM_ADDR, &vccbram) != 0)
        return M2SDR_ERR_IO;
    sensors->temperature_c = (double)temp * 503.975 / 4096.0 - 273.15;
    sensors->vccint_v = (double)vccint / 4096.0 * 3.0;
    sensors->vccaux_v = (double)vccaux / 4096.0 * 3.0;
    sensors->vccbram_v = (double)vccbram / 4096.0 * 3.0;
    return M2SDR_ERR_OK;
#else
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

int m2sdr_set_rx_header(struct m2sdr_dev *dev, bool enable, bool strip_header)
{
    if (!dev)
        return M2SDR_ERR_INVAL;
    dev->rx_header_enable = enable ? 1 : 0;
    dev->rx_strip_header = strip_header ? 1 : 0;

    m2sdr_reg_write(dev, CSR_HEADER_RX_CONTROL_ADDR,
        (1 << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
        ((enable ? 1 : 0) << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET));
    return M2SDR_ERR_OK;
}

int m2sdr_set_tx_header(struct m2sdr_dev *dev, bool enable)
{
    if (!dev)
        return M2SDR_ERR_INVAL;
    dev->tx_header_enable = enable ? 1 : 0;

    m2sdr_reg_write(dev, CSR_HEADER_TX_CONTROL_ADDR,
        (1 << CSR_HEADER_TX_CONTROL_ENABLE_OFFSET) |
        ((enable ? 1 : 0) << CSR_HEADER_TX_CONTROL_HEADER_ENABLE_OFFSET));
    return M2SDR_ERR_OK;
}

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
    m2sdr_reg_write(dev, CSR_GPIO_CONTROL_ADDR, control);
    return M2SDR_ERR_OK;
#else
    (void)enable;
    (void)loopback;
    (void)source_csr;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

int m2sdr_gpio_write(struct m2sdr_dev *dev, uint8_t value, uint8_t oe)
{
    if (!dev)
        return M2SDR_ERR_INVAL;
#ifdef CSR_GPIO_BASE
    m2sdr_reg_write(dev, CSR_GPIO__O_ADDR, value & 0xF);
    m2sdr_reg_write(dev, CSR_GPIO_OE_ADDR, oe & 0xF);
    return M2SDR_ERR_OK;
#else
    (void)value;
    (void)oe;
    return M2SDR_ERR_UNSUPPORTED;
#endif
}

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
