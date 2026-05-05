/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2026 Enjoy Digital.
 * SPDX-License-Identifier: Apache-2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <fcntl.h>
#include <unistd.h>
#include <memory>
#include <sys/mman.h>
#include <stdint.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <inttypes.h>
#include <time.h>

#include "ad9361/platform.h"
#include "ad9361/ad9361.h"
#include "ad9361/ad9361_api.h"

#include "libm2sdr/m2sdr_config.h"

#include "liblitepcie.h"
#include "libm2sdr.h"
#include "etherbone.h"

#include "LiteXM2SDRDevice.hpp"

#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Types.hpp>

/***************************************************************************************************
 *                                    AD9361
 **************************************************************************************************/

/* FIXME: Cleanup and try to share common approach/code with m2sdr_rf. */

/* AD9361 SPI */

namespace {
std::mutex spi_map_mutex;
std::unordered_map<uint8_t, litex_m2sdr_device_desc_t> spi_fd_map;
uint8_t spi_next_id = 0;
litex_m2sdr_device_desc_t spi_last_fd =
#if USE_LITEPCIE
    FD_INIT;
#else
    nullptr;
#endif
bool spi_warned_fallback = false;

uint8_t spi_register_fd(litex_m2sdr_device_desc_t fd)
{
    std::lock_guard<std::mutex> lock(spi_map_mutex);
    uint8_t id = spi_next_id;
    for (uint16_t i = 0; i < 256; i++) {
        if (spi_fd_map.find(id) == spi_fd_map.end()) {
            spi_next_id = static_cast<uint8_t>(id + 1);
            spi_fd_map[id] = fd;
            spi_last_fd = fd;
            return id;
        }
        id = static_cast<uint8_t>(id + 1);
    }
    throw std::runtime_error("spi_register_fd(): no free SPI ids available");
}

void spi_unregister_fd(uint8_t id)
{
    std::lock_guard<std::mutex> lock(spi_map_mutex);
    spi_fd_map.erase(id);
    if (spi_fd_map.empty()) {
#if USE_LITEPCIE
        spi_last_fd = FD_INIT;
#else
        spi_last_fd = nullptr;
#endif
    } else {
        spi_last_fd = spi_fd_map.begin()->second;
    }
}

litex_m2sdr_device_desc_t spi_get_fd(const struct spi_device *spi)
{
    std::lock_guard<std::mutex> lock(spi_map_mutex);
    auto it = spi_fd_map.find(spi->id_no);
    if (it == spi_fd_map.end()) {
#if USE_LITEPCIE
        if (spi_last_fd >= 0) {
#else
        if (spi_last_fd) {
#endif
            if (!spi_warned_fallback) {
                spi_warned_fallback = true;
                fprintf(stderr,
                        "spi_write_then_read(): SPI id %u not found, using last registered fd\n",
                        (unsigned)spi->id_no);
            }
            return spi_last_fd;
        }
#if USE_LITEPCIE
        return FD_INIT;
#else
        return nullptr;
#endif
    }
    return it->second;
}

uint8_t parse_agc_mode(const std::string &mode)
{
    if (mode == "slow" || mode == "slowattack")
        return RF_GAIN_SLOWATTACK_AGC;
    if (mode == "fast" || mode == "fastattack")
        return RF_GAIN_FASTATTACK_AGC;
    if (mode == "hybrid")
        return RF_GAIN_HYBRID_AGC;
    if (mode == "manual" || mode == "mgc")
        return RF_GAIN_MGC;
    throw std::runtime_error("Invalid rx_agc_mode: " + mode);
}

bool antenna_allowed(const std::vector<std::string> &ants, const std::string &name)
{
    if (ants.empty())
        return true;
    return std::find(ants.begin(), ants.end(), name) != ants.end();
}

bool has_eth_ptp_support(struct m2sdr_dev *dev)
{
    struct m2sdr_capabilities caps;

    if (!dev)
        return false;
    if (m2sdr_get_capabilities(dev, &caps) != 0)
        return false;

    return (caps.features & M2SDR_FEATURE_ETH_PTP_MASK) != 0;
}

std::string ptp_state_to_string(uint8_t state)
{
    switch (state) {
    case 0: return "manual";
    case 1: return "acquire";
    case 2: return "locked";
    case 3: return "holdover";
    default: return "unknown";
    }
}

std::string ptp_ipv4_to_string(uint32_t ip)
{
    char buf[32];

    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  (ip >> 24) & 0xffu,
                  (ip >> 16) & 0xffu,
                  (ip >>  8) & 0xffu,
                  (ip >>  0) & 0xffu);
    return std::string(buf);
}

std::string ptp_port_identity_to_string(const struct m2sdr_ptp_port_identity &port)
{
    char buf[64];

    if ((port.clock_id == 0) && (port.port_number == 0))
        return "n/a";

    std::snprintf(buf, sizeof(buf), "%016" PRIx64 ":%u",
                  port.clock_id,
                  static_cast<unsigned>(port.port_number));
    return std::string(buf);
}

void fir_set_64_taps(int16_t *dst, const int16_t *src)
{
    std::memset(dst, 0, 128 * sizeof(int16_t));
    std::memcpy(dst, src, 64 * sizeof(int16_t));
}

AD9361_RXFIRConfig make_rx_fir_from_64(const int16_t *taps)
{
    AD9361_RXFIRConfig cfg = rx_fir_config;
    cfg.rx      = 3;
    cfg.rx_gain = -6;
    cfg.rx_dec  = 1;
    fir_set_64_taps(cfg.rx_coef, taps);
    cfg.rx_coef_size = 64;
    return cfg;
}

AD9361_TXFIRConfig make_tx_fir_from_64(const int16_t *taps, int tx_gain_db = -6)
{
    AD9361_TXFIRConfig cfg = tx_fir_config;
    cfg.tx      = 3;
    cfg.tx_gain = tx_gain_db;
    cfg.tx_int  = 1;
    fir_set_64_taps(cfg.tx_coef, taps);
    cfg.tx_coef_size = 64;
    return cfg;
}

AD9361_RXFIRConfig make_rx_fir_bypass()
{
    AD9361_RXFIRConfig cfg = rx_fir_config;
    cfg.rx      = 3;
    cfg.rx_gain = -6;
    cfg.rx_dec  = 1;
    std::memset(cfg.rx_coef, 0, sizeof(cfg.rx_coef));
    cfg.rx_coef[0] = 32767;
    cfg.rx_coef_size = 64;
    return cfg;
}

AD9361_TXFIRConfig make_tx_fir_bypass()
{
    AD9361_TXFIRConfig cfg = tx_fir_config;
    cfg.tx      = 3;
    cfg.tx_gain = 0;
    cfg.tx_int  = 1;
    std::memset(cfg.tx_coef, 0, sizeof(cfg.tx_coef));
    cfg.tx_coef[0] = 32767;
    cfg.tx_coef_size = 64;
    return cfg;
}

/* Experimental 1x FIR candidates for the 122.88 MSPS oversampling mode.
 * These widen the FIR passband versus the legacy BladeRF-derived RX filter.
 * They are symmetric 64-tap LPFs and should be treated as A/B test profiles. */
static const int16_t fir_1x_wide_taps[64] = {
     -65,     15,    109,    -15,   -166,      9,    240,      4,
    -331,    -30,    442,     72,   -577,   -135,    739,    228,
    -934,   -359,   1173,    544,  -1473,   -811,   1867,   1208,
   -2425,  -1847,   3323,   3033,  -5141,  -6052,  11568,  32767,
   32767,  11568,  -6052,  -5141,   3033,   3323,  -1847,  -2425,
    1208,   1867,   -811,  -1473,    544,   1173,   -359,   -934,
     228,    739,   -135,   -577,     72,    442,    -30,   -331,
       4,    240,      9,   -166,    -15,    109,     15,    -65,
};

bool select_ad9361_fir_profile_1x(const std::string &name,
                                  AD9361_RXFIRConfig &rx_cfg,
                                  AD9361_TXFIRConfig &tx_cfg,
                                  std::string &canonical_name)
{
    if (name.empty() || name == "legacy") {
        rx_cfg = rx_fir_config;
        tx_cfg = tx_fir_config;
        canonical_name = "legacy";
        return true;
    }
    if (name == "bypass") {
        rx_cfg = make_rx_fir_bypass();
        tx_cfg = make_tx_fir_bypass();
        canonical_name = "bypass";
        return true;
    }
    if (name == "match") {
        rx_cfg = rx_fir_config;
        tx_cfg = make_tx_fir_from_64(rx_fir_config.rx_coef, -6);
        canonical_name = "match";
        return true;
    }
    if (name == "wide") {
        rx_cfg = make_rx_fir_from_64(fir_1x_wide_taps);
        tx_cfg = make_tx_fir_from_64(fir_1x_wide_taps, -6);
        canonical_name = "wide";
        return true;
    }
    return false;
}

#if USE_LITEETH
constexpr unsigned SOAPY_LITEETH_RF_OP_TIMEOUT_MS = 5000;
thread_local uint64_t soapy_rf_op_deadline_us = 0;
thread_local bool soapy_rf_op_timed_out = false;
thread_local unsigned soapy_rf_op_depth = 0;

uint64_t monotonic_time_us()
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return static_cast<uint64_t>(ts.tv_sec) * 1000000u +
           static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

class LiteEthRfOpTimeout {
public:
    explicit LiteEthRfOpTimeout(
        const char *operation,
        unsigned timeout_ms = SOAPY_LITEETH_RF_OP_TIMEOUT_MS) :
        _operation(operation),
        _timeout_ms(timeout_ms),
        _owner(soapy_rf_op_depth == 0)
    {
        soapy_rf_op_depth++;

        if (_owner) {
            uint64_t now = monotonic_time_us();

            soapy_rf_op_timed_out = false;
            soapy_rf_op_deadline_us = now ?
                now + static_cast<uint64_t>(_timeout_ms) * 1000u : 0;
        }
    }

    ~LiteEthRfOpTimeout()
    {
        if (soapy_rf_op_depth)
            soapy_rf_op_depth--;

        if (_owner)
            soapy_rf_op_deadline_us = 0;
    }

    void throw_if_timed_out() const
    {
        if (soapy_rf_op_timed_out) {
            throw std::runtime_error(std::string(_operation) + " timed out after " +
                std::to_string(_timeout_ms) + " ms");
        }
    }

private:
    const char *_operation;
    unsigned _timeout_ms;
    bool _owner;
};

bool soapy_rf_op_timeout_expired()
{
    uint64_t now;

    if (!soapy_rf_op_deadline_us)
        return false;

    now = monotonic_time_us();
    return now && now >= soapy_rf_op_deadline_us;
}
#endif
} // namespace

//#define AD9361_SPI_WRITE_DEBUG
//#define AD9361_SPI_READ_DEBUG

int spi_write_then_read(struct spi_device *spi,
                        const unsigned char *txbuf, unsigned n_tx,
                        unsigned char *rxbuf, unsigned n_rx)
{
    litex_m2sdr_device_desc_t fd = spi_get_fd(spi);
#if USE_LITEPCIE
    if (fd < 0)
        throw std::runtime_error("spi_write_then_read(): invalid SPI device");
    void *conn = (void *)(intptr_t)fd;
#else
    if (!fd)
        throw std::runtime_error("spi_write_then_read(): invalid SPI device");
    if (soapy_rf_op_timeout_expired()) {
        if (!soapy_rf_op_timed_out) {
            SoapySDR::logf(SOAPY_SDR_ERROR,
                "AD9361 SPI transaction aborted after %u ms RF operation timeout",
                SOAPY_LITEETH_RF_OP_TIMEOUT_MS);
        }
        soapy_rf_op_timed_out = true;
        return -ETIMEDOUT;
    }
    void *conn = fd;
#endif

    /* Single Byte Read. */
    if (n_tx == 2 && n_rx == 1) {
        uint16_t reg = txbuf[0] << 8 | txbuf[1];

        if (!m2sdr_ad9361_spi_read_checked(conn, reg, &rxbuf[0])) {
            fprintf(stderr, "spi_write_then_read(): AD9361 SPI read failed @0x%03x\n", reg);
            return -EIO;
        }

    /* Single Byte Write. */
    } else if (n_tx == 3 && n_rx == 0) {
        uint16_t reg = txbuf[0] << 8 | txbuf[1];

        if (!m2sdr_ad9361_spi_write_checked(conn, reg, txbuf[2])) {
            fprintf(stderr, "spi_write_then_read(): AD9361 SPI write failed @0x%03x\n", reg);
            return -EIO;
        }

    /* Unsupported. */
    } else {
        fprintf(stderr, "Unsupported SPI transfer n_tx=%d n_rx=%d\n",
                n_tx, n_rx);
        exit(1);
    }

    return 0;
}

/* AD9361 Delays */

void udelay(unsigned long usecs)
{
    usleep(usecs);
}

void mdelay(unsigned long msecs)
{
    usleep(msecs * 1000);
}

unsigned long msleep_interruptible(unsigned int msecs)
{
    usleep(msecs * 1000);
    return 0;
}

/* AD9361 GPIO */

#define AD9361_GPIO_RESET_PIN 0

bool gpio_is_valid(int number)
{
    switch(number) {
       case AD9361_GPIO_RESET_PIN:
           return true;
       default:
           return false;
    }
}

void gpio_set_value(unsigned /*gpio*/, int /*value*/){}

std::string getLiteXM2SDRSerial(struct m2sdr_dev *dev);
std::string getLiteXM2SDRIdentification(struct m2sdr_dev *dev);

#if USE_LITEPCIE
void dma_set_loopback(int fd, bool loopback_enable) {
    struct litepcie_ioctl_dma m;
    m.loopback_enable = loopback_enable ? 1 : 0;
    checked_ioctl(fd, LITEPCIE_IOCTL_DMA, &m);
}
#endif

/***************************************************************************************************
 *                                     Constructor
 **************************************************************************************************/

SoapyLiteXM2SDR::SoapyLiteXM2SDR(const SoapySDR::Kwargs &args)
    : _deviceArgs(args), _rx_buf_size(0), _tx_buf_size(0), _rx_buf_count(0), _tx_buf_count(0),
#if USE_LITEETH
      _udp_inited(false),
#endif
      _fd(FD_INIT), ad9361_phy(NULL) {

    SoapySDR::logf(SOAPY_SDR_INFO, "SoapyLiteXM2SDR initializing...");
    setvbuf(stdout, NULL, _IOLBF, 0);

#ifdef DEBUG
    SoapySDR::logf(SOAPY_SDR_INFO, "Received arguments:");
    for (const auto &arg : args) {
        SoapySDR::logf(SOAPY_SDR_INFO, "  %s: %s", arg.first.c_str(), arg.second.c_str());
    }
#endif

#if USE_LITEPCIE
    /* Open LitePCIe descriptor. */
    if (args.count("path") == 0) {
        /* If path is not present, then findLiteXM2SDR had zero devices enumerated. */
        throw std::runtime_error("No LitePCIe devices found!");
    }
    std::string path = args.at("path");
    std::string dev_id = "pcie:" + path;
    int rc = m2sdr_open(&_dev, dev_id.c_str());
    if (rc != 0) {
        throw std::runtime_error("SoapyLiteXM2SDR(): failed to open " + path + " (" + m2sdr_strerror(rc) + ")");
    }
    _fd = static_cast<litex_m2sdr_device_desc_t>(m2sdr_get_fd(_dev));
    /* Global file descriptor for AD9361 lib. */
    _spi_id = spi_register_fd(_fd);

    SoapySDR::logf(SOAPY_SDR_INFO, "Opened devnode %s, serial %s", path.c_str(), getLiteXM2SDRSerial(_dev).c_str());
#elif USE_LITEETH
    /* Prepare EtherBone / Ethernet streamer */
    std::string eth_ip;
    if (args.count("eth_ip") == 0)
        eth_ip = "192.168.1.50";
    else
        eth_ip = args.at("eth_ip");
    _eth_ip = eth_ip;

    /* EtherBone */
    std::string dev_id = "eth:" + eth_ip + ":1234";
    int rc = m2sdr_open(&_dev, dev_id.c_str());
    if (rc != 0) {
        throw std::runtime_error(
            "Can't connect to EtherBone at " + eth_ip +
            ":1234 (hint: set eth_ip=... for the board IP, error: " +
            std::string(m2sdr_strerror(rc)) + ")");
    }
    _fd = reinterpret_cast<litex_m2sdr_device_desc_t>(m2sdr_get_handle(_dev));
    _spi_id = spi_register_fd(_fd);

    SoapySDR::logf(SOAPY_SDR_INFO, "Opened devnode %s, serial %s", eth_ip.c_str(), getLiteXM2SDRSerial(_dev).c_str());

    std::string eth_mode = "udp";
    if (args.count("eth_mode") > 0)
        eth_mode = args.at("eth_mode");
    if (eth_mode == "udp")
        _eth_mode = SoapyLiteXM2SDREthernetMode::UDP;
    else if (eth_mode == "vrt")
        _eth_mode = SoapyLiteXM2SDREthernetMode::VRT;
    else
        throw std::runtime_error("Invalid eth_mode: " + eth_mode + " (supported: udp, vrt)");

    uint32_t local_ip = 0;
    rc = m2sdr_liteeth_get_local_ip(_dev, &local_ip);
    if (rc != M2SDR_ERR_OK) {
        throw std::runtime_error(
            "Could not determine local IP for LiteEth streaming (" +
            std::string(m2sdr_strerror(rc)) + ")");
    }
    SoapySDR::logf(SOAPY_SDR_INFO, "Using local IP: %s for streaming",
                   ptp_ipv4_to_string(local_ip).c_str());
#endif

    /* Configure Mode based on _bitMode */
    SoapySDR::log(SOAPY_SDR_INFO, "Configuring bitmode");
    m2sdr_set_bitmode(_dev, _bitMode == 8);


    /* Configure PCIe Synchronizer and DMA Headers. */
#if USE_LITEPCIE
    SoapySDR::log(SOAPY_SDR_INFO, "Configuring PCIe DMA headers");
    /* Enable Synchronizer */
#ifdef CSR_PCIE_DMA0_SYNCHRONIZER_BYPASS_ADDR
    litex_m2sdr_writel(_dev, CSR_PCIE_DMA0_SYNCHRONIZER_BYPASS_ADDR, 0);
#else
    SoapySDR::log(SOAPY_SDR_WARNING, "PCIe DMA synchronizer CSR not present; skipping synchronizer setup");
#endif

    /* DMA RX Header */
    #if defined(_RX_DMA_HEADER_TEST)
        /* Enable */
        m2sdr_set_rx_header(_dev, true, false);
    #else
        /* Disable */
        m2sdr_set_rx_header(_dev, false, false);
    #endif

    /* DMA TX Header */
    #if defined(_TX_DMA_HEADER_TEST)
        /* Enable */
        m2sdr_set_tx_header(_dev, true);
    #else
        /* Disable */
        m2sdr_set_tx_header(_dev, false);
    #endif

    /* Disable DMA Loopback. */
    m2sdr_set_dma_loopback(_dev, false);
#endif

    bool do_init = true;
    if (args.count("bypass_init") > 0) {
        std::cout << args.at("bypass_init")[0] << std::endl;
        do_init = args.at("bypass_init")[0] == '0';
    }

    if (args.count("bitmode") > 0) {
        _bitMode = std::stoi(args.at("bitmode"));
    } else {
        _bitMode = 16;
    }

    if (args.count("oversampling") > 0) {
        _oversampling = std::stoi(args.at("oversampling"));
    }
    if (args.count("ad9361_fir_profile") > 0) {
        _ad9361_fir_profile = args.at("ad9361_fir_profile");
    } else if (args.count("fir_profile") > 0) {
        _ad9361_fir_profile = args.at("fir_profile");
    }

    AD9361_RXFIRConfig rx_fir_cfg_base;
    AD9361_TXFIRConfig tx_fir_cfg_base;
    std::string fir_profile_canonical;
    if (!select_ad9361_fir_profile_1x(_ad9361_fir_profile, rx_fir_cfg_base, tx_fir_cfg_base, fir_profile_canonical)) {
        throw std::runtime_error(
            "Invalid ad9361_fir_profile '" + _ad9361_fir_profile +
            "' (supported: legacy, bypass, match, wide)");
    }
    _ad9361_fir_profile = fir_profile_canonical;
    SoapySDR::logf(SOAPY_SDR_INFO, "AD9361 1x FIR profile: %s", _ad9361_fir_profile.c_str());

    /* Expose only the board-connected RF ports. The broader AD9361 antenna
     * enum remains available in the lower-level driver, but those names do not
     * map cleanly to user-facing M2SDR connectors here. */
    _rx_antennas = {"A_BALANCED"};
    _tx_antennas = {"A"};
    if (args.count("rx_antenna_list") > 0 || args.count("tx_antenna_list") > 0) {
        throw std::runtime_error(
            "Custom antenna lists are not supported; use RX=A_BALANCED and TX=A");
    }

    _rx_agc_mode = RF_GAIN_SLOWATTACK_AGC;
    if (args.count("rx_agc_mode") > 0)
        _rx_agc_mode = parse_agc_mode(args.at("rx_agc_mode"));

    /* RefClk Selection */
    int64_t refclk_hz        = 38400000;   /* Default 38.4 MHz. */
    std::string clock_source = "internal"; /* Default XO. */
    if (args.count("refclk_freq") > 0) {
        refclk_hz = static_cast<int64_t>(std::stod(args.at("refclk_freq")));
    }
    if (args.count("clock_source") > 0) {
        clock_source = args.at("clock_source");
    }
    if (do_init) {

        /* Initialize SI5351 Clocking */
#ifdef CSR_SI5351_BASE
        SoapySDR::log(SOAPY_SDR_INFO, "Initializing SI5351");
        if (clock_source == "internal") {
            /* SI5351B, XO reference */
            litex_m2sdr_writel(_dev, CSR_SI5351_CONTROL_ADDR,
                SI5351B_VERSION * (1 << CSR_SI5351_CONTROL_VERSION_OFFSET));
            if (refclk_hz == 40000000) {
                if (!m2sdr_si5351_i2c_config_checked((void *)(intptr_t)_fd, SI5351_I2C_ADDR,
                    si5351_xo_40m_config,
                    sizeof(si5351_xo_40m_config)/sizeof(si5351_xo_40m_config[0])))
                    throw std::runtime_error("SI5351 internal-XO 40MHz config failed");
            } else {
                if (!m2sdr_si5351_i2c_config_checked((void *)(intptr_t)_fd, SI5351_I2C_ADDR,
                    si5351_xo_38p4m_config,
                    sizeof(si5351_xo_38p4m_config)/sizeof(si5351_xo_38p4m_config[0])))
                    throw std::runtime_error("SI5351 internal-XO 38.4MHz config failed");
            }
        } else if (clock_source == "external") {
            /* SI5351C, external 10 MHz CLKIN from u.FL */
            litex_m2sdr_writel(_dev, CSR_SI5351_CONTROL_ADDR,
                  SI5351C_VERSION               * (1 << CSR_SI5351_CONTROL_VERSION_OFFSET) |
                  SI5351C_10MHZ_CLK_IN_FROM_UFL * (1 << CSR_SI5351_CONTROL_CLKIN_SRC_OFFSET));
            if (refclk_hz == 40000000) {
                if (!m2sdr_si5351_i2c_config_checked((void *)(intptr_t)_fd, SI5351_I2C_ADDR,
                    si5351_clkin_10m_40m_config,
                    sizeof(si5351_clkin_10m_40m_config)/sizeof(si5351_clkin_10m_40m_config[0])))
                    throw std::runtime_error("SI5351 external 10MHz/40MHz config failed");
            } else {
                if (!m2sdr_si5351_i2c_config_checked((void *)(intptr_t)_fd, SI5351_I2C_ADDR,
                    si5351_clkin_10m_38p4m_config,
                    sizeof(si5351_clkin_10m_38p4m_config)/sizeof(si5351_clkin_10m_38p4m_config[0])))
                    throw std::runtime_error("SI5351 external 10MHz/38.4MHz config failed");
            }
        } else if ((clock_source == "fpga") ||
                   (clock_source == "si5351c-fpga") ||
                   (clock_source == "si5351c_fpga") ||
                   (clock_source == "pll")) {
            /* SI5351C, 10 MHz CLKIN regenerated from the FPGA clk10 path. */
            litex_m2sdr_writel(_dev, CSR_SI5351_CONTROL_ADDR,
                  SI5351C_VERSION                * (1 << CSR_SI5351_CONTROL_VERSION_OFFSET) |
                  SI5351C_10MHZ_CLK_IN_FROM_PLL * (1 << CSR_SI5351_CONTROL_CLKIN_SRC_OFFSET));
            if (refclk_hz == 40000000) {
                if (!m2sdr_si5351_i2c_config_checked((void *)(intptr_t)_fd, SI5351_I2C_ADDR,
                    si5351_clkin_10m_40m_config,
                    sizeof(si5351_clkin_10m_40m_config)/sizeof(si5351_clkin_10m_40m_config[0])))
                    throw std::runtime_error("SI5351 FPGA 10MHz/40MHz config failed");
            } else {
                if (!m2sdr_si5351_i2c_config_checked((void *)(intptr_t)_fd, SI5351_I2C_ADDR,
                    si5351_clkin_10m_38p4m_config,
                    sizeof(si5351_clkin_10m_38p4m_config)/sizeof(si5351_clkin_10m_38p4m_config[0])))
                    throw std::runtime_error("SI5351 FPGA 10MHz/38.4MHz config failed");
            }
        } else {
            throw std::runtime_error("Unsupported clock_source '" + clock_source + "' (supported: internal, external, fpga)");
        }
#endif

        /* Power-up AD9361 */
        SoapySDR::log(SOAPY_SDR_INFO, "Powering up AD9361");
        litex_m2sdr_writel(_dev, CSR_AD9361_CONFIG_ADDR, 0b11);

        /* Initialize AD9361 SPI. */
        SoapySDR::log(SOAPY_SDR_INFO, "Initializing AD9361 SPI");
        m2sdr_ad9361_spi_init((void *)(intptr_t)_fd, 1);
    }

    /* Initialize AD9361 RFIC. */
    SoapySDR::log(SOAPY_SDR_INFO, "Initializing AD9361 RFIC");
    default_init_param.reference_clk_rate = refclk_hz;
    default_init_param.gpio_resetb        = AD9361_GPIO_RESET_PIN;
    default_init_param.gpio_sync          = -1;
    default_init_param.gpio_cal_sw1       = -1;
    default_init_param.gpio_cal_sw2       = -1;
    default_init_param.id_no = _spi_id;
#if USE_LITEETH
    LiteEthRfOpTimeout ad9361_init_timeout("ad9361_init");
#endif
    int ad9361_rc = ad9361_init(&ad9361_phy, &default_init_param, do_init);
#if USE_LITEETH
    ad9361_init_timeout.throw_if_timed_out();
#endif
    if (ad9361_rc != 0 || ad9361_phy == nullptr) {
        throw std::runtime_error("ad9361_init failed (rc=" + std::to_string(ad9361_rc) + ")");
    }
    m2sdr_rf_bind(_dev, ad9361_phy);

    if (do_init) {
        /* Configure AD9361 TX/RX FIRs. */
        SoapySDR::log(SOAPY_SDR_INFO, "Configuring AD9361 FIRs");
        ad9361_set_tx_fir_config(ad9361_phy, tx_fir_cfg_base);
        ad9361_set_rx_fir_config(ad9361_phy, rx_fir_cfg_base);

        /* Some defaults to avoid throwing. */
        SoapySDR::log(SOAPY_SDR_INFO, "Applying default RF settings");

        this->setClockSource("internal");

        /* Common for TX1/TX2 and RX1/RX2 */
        _rx_stream.samplerate   = 30.72e6;
        _tx_stream.samplerate   = 30.72e6;
        _rx_stream.frequency    = 1e6;
        _tx_stream.frequency    = 1e6;
        _rx_stream.bandwidth    = 30.72e6;
        _tx_stream.bandwidth    = 30.72e6;

        /* TX1/RX1. */
        _rx_stream.antenna[0]   = _rx_antennas.empty() ? "A_BALANCED" : _rx_antennas[0];
        _tx_stream.antenna[0]   = _tx_antennas.empty() ? "A" : _tx_antennas[0];
        _rx_stream.gainMode[0]  = true;
        _rx_stream.gain[0]      = 0;
        _tx_stream.gain[0]      = 20;
        _rx_stream.iqbalance[0] = 1.0;
        _tx_stream.iqbalance[0] = 1.0;
        SoapySDR::log(SOAPY_SDR_INFO, "Configuring RX/TX channel 0");
        channel_configure(SOAPY_SDR_RX, 0);
        channel_configure(SOAPY_SDR_TX, 0);

        /* TX2/RX2. */
        _rx_stream.antenna[1]   = _rx_antennas.empty() ? "A_BALANCED" : _rx_antennas[0];
        _tx_stream.antenna[1]   = _tx_antennas.empty() ? "A" : _tx_antennas[0];
        _rx_stream.gainMode[1]  = true;
        _rx_stream.gain[1]      = 0;
        _tx_stream.gain[1]      = 20;
        _rx_stream.iqbalance[1] = 1.0;
        _tx_stream.iqbalance[1] = 1.0;
        SoapySDR::log(SOAPY_SDR_INFO, "Configuring RX/TX channel 1");
        channel_configure(SOAPY_SDR_RX, 1);
        channel_configure(SOAPY_SDR_TX, 1);
    }

    if (args.count("rx_antenna0") > 0)
        _rx_stream.antenna[0] = args.at("rx_antenna0");
    if (args.count("rx_antenna1") > 0)
        _rx_stream.antenna[1] = args.at("rx_antenna1");
    if (args.count("tx_antenna0") > 0)
        _tx_stream.antenna[0] = args.at("tx_antenna0");
    if (args.count("tx_antenna1") > 0)
        _tx_stream.antenna[1] = args.at("tx_antenna1");

    if (!antenna_allowed(_rx_antennas, _rx_stream.antenna[0]) ||
        !antenna_allowed(_rx_antennas, _rx_stream.antenna[1])) {
        throw std::runtime_error("Invalid RX antenna selection");
    }
    if (!antenna_allowed(_tx_antennas, _tx_stream.antenna[0]) ||
        !antenna_allowed(_tx_antennas, _tx_stream.antenna[1])) {
        throw std::runtime_error("Invalid TX antenna selection");
    }

#if USE_LITEPCIE
    /* Set-up the DMA. */
    checked_ioctl(_fd, LITEPCIE_IOCTL_MMAP_DMA_INFO, &_dma_mmap_info);
    _dma_buf = NULL;
#endif

    SoapySDR::log(SOAPY_SDR_INFO, "SoapyLiteXM2SDR initialization complete");
}

/***************************************************************************************************
 *                                          Destructor
 **************************************************************************************************/

SoapyLiteXM2SDR::~SoapyLiteXM2SDR(void) {
    SoapySDR::log(SOAPY_SDR_INFO, "Power down and cleanup");
    spi_unregister_fd(_spi_id);
    if (_rx_stream.opened) {
        stopRxStreamUnlocked();
#if USE_LITEPCIE
         /* Release the DMA engine. */
        if (_rx_stream.dma.buf_rd != NULL) {
            litepcie_dma_cleanup(&_rx_stream.dma);
        }
        _rx_stream.buf = NULL;
#elif USE_LITEETH
        std::free(_rx_stream.buf);
        _rx_stream.buf = NULL;
#endif
        _rx_stream.opened = false;
    }

    if (_tx_stream.opened) {
        stopTxStreamUnlocked();
#if USE_LITEPCIE
        /* Release the DMA engine. */
        if (_tx_stream.dma.buf_wr) {
            litepcie_dma_cleanup(&_tx_stream.dma);
        }
#elif USE_LITEETH
        std::free(_tx_stream.buf);
#endif
        _tx_stream.buf = NULL;
        _tx_stream.opened = false;
    }
    cleanupLiteEthUdpIfIdleUnlocked();

    /* Crossbar Mux/Demux : Select PCIe streaming */
    litex_m2sdr_writel(_dev, CSR_CROSSBAR_MUX_SEL_ADDR,   0);
    litex_m2sdr_writel(_dev, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);

    /* Power-Down AD9361 */
    litex_m2sdr_writel(_dev, CSR_AD9361_CONFIG_ADDR, 0b00);

#if USE_LITEETH
    if (_udp_inited) {
        liteeth_udp_cleanup(&_udp);
        _udp_inited = false;
    }
    if (_dev) {
        m2sdr_rf_bind(_dev, nullptr);
        m2sdr_close(_dev);
        _dev = nullptr;
    }
#endif
#if USE_LITEPCIE
    _fd = FD_INIT;
#elif USE_LITEETH
    _fd = FD_INIT;
#endif
}

/***************************************************************************************************
 *                                 Channel configuration
 **************************************************************************************************/

void SoapyLiteXM2SDR::channel_configure(const int direction, const size_t channel) {
    if (direction == SOAPY_SDR_TX) {
        this->setSampleRate(SOAPY_SDR_TX, channel, _tx_stream.samplerate);
        this->setAntenna(SOAPY_SDR_TX,    channel, _tx_stream.antenna[channel]);
        this->setFrequency(SOAPY_SDR_TX,  channel, "RF", _tx_stream.frequency);
        this->setBandwidth(SOAPY_SDR_TX,  channel, _tx_stream.bandwidth);
        this->setGain(SOAPY_SDR_TX,       channel, _tx_stream.gain[channel]);
        this->setIQBalance(SOAPY_SDR_TX,  channel, _tx_stream.iqbalance[channel]);
    }
    if (direction == SOAPY_SDR_RX) {
        this->setSampleRate(SOAPY_SDR_RX, channel, _rx_stream.samplerate);
        this->setAntenna(SOAPY_SDR_RX,    channel, _rx_stream.antenna[channel]);
        this->setFrequency(SOAPY_SDR_RX,  channel, "RF", _rx_stream.frequency);
        this->setBandwidth(SOAPY_SDR_RX,  channel, _rx_stream.bandwidth);
        this->setGainMode(SOAPY_SDR_RX,   channel, _rx_stream.gainMode[channel]);
        if (!_rx_stream.gainMode[channel])
            this->setGain(SOAPY_SDR_RX, channel, _rx_stream.gain[channel]);
        this->setIQBalance(SOAPY_SDR_RX,  channel, _rx_stream.iqbalance[channel]);
    }

}

void SoapyLiteXM2SDR::invalidateRfHardwareCache() {
    _sampleRateHwApplied = false;
    _bandwidthHwApplied = false;
    _rxFrequencyHwApplied = false;
    _txFrequencyHwApplied = false;
    _txAttHwApplied = false;
    _bitModeHwApplied = false;
}

/***************************************************************************************************
 *                                  Identification API
 **************************************************************************************************/

std::string SoapyLiteXM2SDR::getDriverKey(void) const {
    return "LiteX-M2SDR";
}

std::string SoapyLiteXM2SDR::getHardwareKey(void) const {
    std::string key = "LiteX-M2SDR";

#ifdef CSR_CAPABILITY_BOARD_INFO_ADDR
    {
        struct m2sdr_capabilities caps;
        uint32_t board_info = 0;
        if (m2sdr_get_capabilities(_dev, &caps) == 0)
            board_info = caps.board_info;
        const int variant = (board_info >> CSR_CAPABILITY_BOARD_INFO_VARIANT_OFFSET) &
                            ((1 << CSR_CAPABILITY_BOARD_INFO_VARIANT_SIZE) - 1);
        switch (variant) {
        case 0:
            key += "-m2";
            break;
        case 1:
            key += "-baseboard";
            break;
        default:
            key += "-unknown";
            break;
        }
    }
#endif

#if USE_LITEPCIE
    key += "-pcie";
#elif USE_LITEETH
    key += "-eth";
#endif

    return key;
}

/***************************************************************************************************
*                                     Channel API
***************************************************************************************************/

size_t SoapyLiteXM2SDR::getNumChannels(const int) const {
    return 2;
}

bool SoapyLiteXM2SDR::getFullDuplex(const int, const size_t) const {
    return true;
}

/***************************************************************************************************
 *                                     Antenna API
 **************************************************************************************************/

std::vector<std::string> SoapyLiteXM2SDR::listAntennas(
    const int direction,
    const size_t) const {
    if (direction == SOAPY_SDR_RX) {
        if (!_rx_antennas.empty())
            return _rx_antennas;
        return {"A_BALANCED"};
    }
    if (direction == SOAPY_SDR_TX) {
        if (!_tx_antennas.empty())
            return _tx_antennas;
        return {"A"};
    }
    return {};
}

void SoapyLiteXM2SDR::setAntenna(
    const int direction,
    const size_t channel,
    const std::string &name) {
    std::lock_guard<std::mutex> lock(_mutex);

    /* Keep the Soapy-facing API aligned with the board RF connectors rather
     * than exposing the full AD9361 port enum. */
    if (direction == SOAPY_SDR_RX) {
        if (!antenna_allowed(_rx_antennas, name))
            throw std::runtime_error("Unsupported RX antenna: " + name + " (supported: A_BALANCED)");
        _rx_stream.antenna[channel] = name;
    }
    if (direction == SOAPY_SDR_TX) {
        if (!antenna_allowed(_tx_antennas, name))
            throw std::runtime_error("Unsupported TX antenna: " + name + " (supported: A)");
        _tx_stream.antenna[channel] = name;
    }
}

std::string SoapyLiteXM2SDR::getAntenna(
    const int direction,
    const size_t channel) const {
    if (direction == SOAPY_SDR_RX)
        return _rx_stream.antenna[channel];
    return _tx_stream.antenna[channel];
}

/***************************************************************************************************
 *                                 Frontend corrections API
 **************************************************************************************************/

bool SoapyLiteXM2SDR::hasDCOffsetMode(
    const int /*direction*/,
    const size_t /*channel*/) const {
    return false;
}

/***************************************************************************************************
 *                                           Gain API
 **************************************************************************************************/

std::vector<std::string> SoapyLiteXM2SDR::listGains(
    const int direction,
    const size_t) const {
    std::vector<std::string> gains;

    /* TX */
    if (direction == SOAPY_SDR_TX) {
        gains.push_back("ATT");
    }

    /* RX */
    if (direction == SOAPY_SDR_RX) {
        gains.push_back("PGA");
    }
    return gains;
}

bool SoapyLiteXM2SDR::hasGainMode(
    const int direction,
    const size_t /*channel*/) const {

    /* TX */
    if (direction == SOAPY_SDR_TX)
        return false;

    /* RX */
    if (direction == SOAPY_SDR_RX)
        return true;

    /* Fallback */
    return false;
}

void SoapyLiteXM2SDR::setGainMode(const int direction, const size_t channel,
    const bool automatic)
{
    std::lock_guard<std::mutex> lock(_mutex);
    /* N/A. */
    if (direction == SOAPY_SDR_TX)
        return;

    _rx_stream.gainMode[channel] = automatic;
#if USE_LITEETH
    LiteEthRfOpTimeout timeout("setGainMode");
#endif
    ad9361_set_rx_gain_control_mode(ad9361_phy, channel,
        (automatic ? _rx_agc_mode : RF_GAIN_MGC));
#if USE_LITEETH
    timeout.throw_if_timed_out();
#endif
}

bool SoapyLiteXM2SDR::getGainMode(const int direction, const size_t channel) const
{

    /* TX */
    if (direction == SOAPY_SDR_TX)
        return false;

    /* RX */
    if (direction == SOAPY_SDR_RX) {
        uint8_t gc_mode;
#if USE_LITEETH
        LiteEthRfOpTimeout timeout("getGainMode");
#endif
        ad9361_get_rx_gain_control_mode(ad9361_phy, channel, &gc_mode);
#if USE_LITEETH
        timeout.throw_if_timed_out();
#endif
        return (gc_mode != RF_GAIN_MGC);
    }

    /* Fallback */
    return false;
}

void SoapyLiteXM2SDR::setGain(
    int direction,
    size_t channel,
    const double value) {
    std::lock_guard<std::mutex> lock(_mutex);

    /* TX */
    if (direction == SOAPY_SDR_TX) {
        if (value < 0.0) {
            SoapySDR::logf(SOAPY_SDR_ERROR,
                "TX ch%zu: attenuation must be positive; use ATT in dB", channel);
            return;
        }
        _tx_stream.gain[channel] = value;
        SoapySDR::logf(SOAPY_SDR_DEBUG, "TX ch%zu: %.3f dB Attenuation", channel, value);

        int64_t att = static_cast<int64_t>(value);
        if (!_txAttHwApplied || _txAttHw != att) {
#if USE_LITEETH
            LiteEthRfOpTimeout timeout("setGain(TX)");
#endif
            int rc = m2sdr_set_tx_att(_dev, att);
            if (rc != 0) {
                SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_tx_att(TX) failed: %s", m2sdr_strerror(rc));
            } else {
                _txAttHwApplied = true;
                _txAttHw = att;
            }
#if USE_LITEETH
            timeout.throw_if_timed_out();
#endif
        }
    }

    /* RX */
    if (SOAPY_SDR_RX == direction) {
        uint8_t gc_mode = RF_GAIN_MGC;
        _rx_stream.gain[channel] = value;
#if USE_LITEETH
        LiteEthRfOpTimeout timeout("setGain(RX)");
#endif
        ad9361_get_rx_gain_control_mode(ad9361_phy, channel, &gc_mode);
        if (gc_mode != RF_GAIN_MGC) {
            ad9361_set_rx_gain_control_mode(ad9361_phy, channel, RF_GAIN_MGC);
            _rx_stream.gainMode[channel] = false;
        }
        SoapySDR::logf(SOAPY_SDR_DEBUG, "RX ch%zu: %.3f dB Gain", channel, value);
        int rc = m2sdr_set_rx_gain_chan(_dev, (unsigned)channel, value);
        if (rc != 0) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_rx_gain_chan(RX) failed: %s", m2sdr_strerror(rc));
        }
#if USE_LITEETH
        timeout.throw_if_timed_out();
#endif
    }
}

void SoapyLiteXM2SDR::setGain(
    const int direction,
    const size_t channel,
    const std::string &name,
    const double value)
{
    /* TX */
    if (direction == SOAPY_SDR_TX) {
        if (name == "ATT") {
            _tx_stream.gain[channel] = value;
            SoapySDR::logf(SOAPY_SDR_DEBUG, "TX ch%zu: ATT %.3f dB Attenuation", channel, value);
            int64_t att = static_cast<int64_t>(value);
            if (!_txAttHwApplied || _txAttHw != att) {
#if USE_LITEETH
                LiteEthRfOpTimeout timeout("setGain(TX)");
#endif
                int rc = m2sdr_set_tx_att(_dev, att);
                if (rc != 0) {
                    SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_tx_att(TX) failed: %s", m2sdr_strerror(rc));
                } else {
                    _txAttHwApplied = true;
                    _txAttHw = att;
                }
#if USE_LITEETH
                timeout.throw_if_timed_out();
#endif
            }
            return;
        }
    }

    /* RX */
    if (name == "PGA" || name == "RF" || name == "GAIN") {
        uint8_t gc_mode = RF_GAIN_MGC;
        _rx_stream.gain[channel] = value;
#if USE_LITEETH
        LiteEthRfOpTimeout timeout("setGain(RX)");
#endif
        ad9361_get_rx_gain_control_mode(ad9361_phy, channel, &gc_mode);
        if (gc_mode != RF_GAIN_MGC) {
            ad9361_set_rx_gain_control_mode(ad9361_phy, channel, RF_GAIN_MGC);
            _rx_stream.gainMode[channel] = false;
        }
        SoapySDR::logf(SOAPY_SDR_DEBUG, "RX ch%zu: RF %.3f dB Gain", channel, value);
        int rc = m2sdr_set_rx_gain_chan(_dev, (unsigned)channel, value);
        if (rc != 0) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_rx_gain_chan(RX) failed: %s", m2sdr_strerror(rc));
        }
#if USE_LITEETH
        timeout.throw_if_timed_out();
#endif
        return;
    }

    /* Fallback */
    setGain(direction, channel, value);
}

double SoapyLiteXM2SDR::getGain(
    const int direction,
    const size_t channel) const
{
    int32_t gain = 0;

    /* TX */
    if (direction == SOAPY_SDR_TX) {
        uint32_t atten_mdb = 0;
#if USE_LITEETH
        LiteEthRfOpTimeout timeout("getGain(TX)");
#endif
        ad9361_get_tx_attenuation(ad9361_phy, channel, &atten_mdb);
#if USE_LITEETH
        timeout.throw_if_timed_out();
#endif
        gain = (int32_t)(atten_mdb / 1000);
    }

    /* RX */
    if (direction == SOAPY_SDR_RX) {
#if USE_LITEETH
        LiteEthRfOpTimeout timeout("getGain(RX)");
#endif
        ad9361_get_rx_rf_gain(ad9361_phy, channel, &gain);
#if USE_LITEETH
        timeout.throw_if_timed_out();
#endif
    }

    return static_cast<double>(gain);
}

double SoapyLiteXM2SDR::getGain(
    const int direction,
    const size_t channel,
    const std::string &name) const
{
    /* TX */
    if (direction == SOAPY_SDR_TX) {
        uint32_t atten_mdb = 0;
#if USE_LITEETH
        LiteEthRfOpTimeout timeout("getGain(TX)");
#endif
        ad9361_get_tx_attenuation(ad9361_phy, channel, &atten_mdb);
#if USE_LITEETH
        timeout.throw_if_timed_out();
#endif
        double atten_db = atten_mdb / 1000.0;
        if (name == "ATT")
            return atten_db;
    }

    /* RX */
    if (direction == SOAPY_SDR_RX) {
        if (name == "PGA" || name == "RF" || name == "GAIN")
            return getGain(direction, channel);
    }

    /* Fallback */
    return 0;
}

SoapySDR::Range SoapyLiteXM2SDR::getGainRange(
    const int direction,
    const size_t /*channel*/) const
{

    /* TX */
    if (direction == SOAPY_SDR_TX)
        return(SoapySDR::Range(0, 89));

    /* RX */
    if (direction == SOAPY_SDR_RX)
        return(SoapySDR::Range(0, 73));

    /* Fallback */
    return(SoapySDR::Range(0,0));
}

SoapySDR::Range SoapyLiteXM2SDR::getGainRange(
    const int direction,
    const size_t /*channel*/,
    const std::string &name) const
{

    /* TX */
    if (direction == SOAPY_SDR_TX) {
        if (name == "ATT")
            return(SoapySDR::Range(0, 89));
    }

    /* RX */
    if (direction == SOAPY_SDR_RX) {
        if (name == "PGA" || name == "RF" || name == "GAIN")
            return(SoapySDR::Range(0, 73));
    }

    /* Fallback */
    return(SoapySDR::Range(0,0));
}


/***************************************************************************************************
 *                                     Frequency API
 **************************************************************************************************/

void SoapyLiteXM2SDR::setFrequency(
    int direction,
    size_t channel,
    double frequency,
    const SoapySDR::Kwargs &args) {
    setFrequency(direction, channel, "RF", frequency, args);
}

void SoapyLiteXM2SDR::setFrequency(
    const int direction,
    const size_t channel,
    const std::string &name,
    const double frequency,
    const SoapySDR::Kwargs &/*args*/) {
    std::unique_lock<std::mutex> lock(_mutex);

    if (name != "RF" && name != "BB") {
        throw std::runtime_error("SoapyLiteXM2SDR::setFrequency(): unsupported name " + name);
    }

    SoapySDR::logf(SOAPY_SDR_DEBUG,
        "SoapyLiteXM2SDR::setFrequency(%s, ch%d, %s%s, %f MHz)",
        dir2Str(direction),
        channel,
        name.c_str(),
        (name == "BB") ? " (baseband)" : "",
        frequency / 1e6);
    _cachedFreqValues[direction][channel][name] = frequency;
    if (name == "BB") {
        return; /* No baseband NCO support; cache only. */
    }
    if (direction == SOAPY_SDR_TX)
        _tx_stream.frequency = frequency;
    if (direction == SOAPY_SDR_RX)
        _rx_stream.frequency = frequency;

    uint64_t lo_freq = static_cast<uint64_t>(frequency);

#if USE_LITEETH
    LiteEthRfOpTimeout timeout("setFrequency");
#endif
    if (direction == SOAPY_SDR_TX) {
        if (_txFrequencyHwApplied && _txFrequencyHw == lo_freq)
            return;
        int rc = m2sdr_set_frequency(_dev, M2SDR_TX, lo_freq);
        if (rc != 0) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_frequency(TX) failed: %s", m2sdr_strerror(rc));
        } else {
            _txFrequencyHwApplied = true;
            _txFrequencyHw = lo_freq;
        }
    }
    if (direction == SOAPY_SDR_RX) {
        if (_rxFrequencyHwApplied && _rxFrequencyHw == lo_freq)
            return;
        int rc = m2sdr_set_frequency(_dev, M2SDR_RX, lo_freq);
        if (rc != 0) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_frequency(RX) failed: %s", m2sdr_strerror(rc));
        } else {
            _rxFrequencyHwApplied = true;
            _rxFrequencyHw = lo_freq;
        }
    }
#if USE_LITEETH
    timeout.throw_if_timed_out();
#endif
}

double SoapyLiteXM2SDR::getFrequency(
    const int direction,
    const size_t channel,
    const std::string &name) const {

    if (name != "RF" && name != "BB") {
        throw std::runtime_error("SoapyLiteXM2SDR::getFrequency(): unsupported name " + name);
    }

    auto dirIt = _cachedFreqValues.find(direction);
    if (dirIt != _cachedFreqValues.end()) {
        auto chIt = dirIt->second.find(channel);
        if (chIt != dirIt->second.end()) {
            auto nameIt = chIt->second.find(name);
            if (nameIt != chIt->second.end()) {
                return nameIt->second;
            }
        }
    }

    if (name == "BB") {
        return 0.0;
    }

    uint64_t lo_freq = 0;

    if (direction == SOAPY_SDR_TX) {
#if USE_LITEETH
        LiteEthRfOpTimeout timeout("getFrequency(TX)");
#endif
        ad9361_get_tx_lo_freq(ad9361_phy, &lo_freq);
#if USE_LITEETH
        timeout.throw_if_timed_out();
#endif
    }

    if (direction == SOAPY_SDR_RX) {
#if USE_LITEETH
        LiteEthRfOpTimeout timeout("getFrequency(RX)");
#endif
        ad9361_get_rx_lo_freq(ad9361_phy, &lo_freq);
#if USE_LITEETH
        timeout.throw_if_timed_out();
#endif
    }

    return static_cast<double>(lo_freq);
}

std::vector<std::string> SoapyLiteXM2SDR::listFrequencies(
    const int /*direction*/,
    const size_t /*channel*/) const {
    std::vector<std::string> opts;
    opts.push_back("RF");
    return opts;
}

SoapySDR::RangeList SoapyLiteXM2SDR::getFrequencyRange(
    const int direction,
    const size_t /*channel*/,
    const std::string &name) const {

    if (name == "BB") {
        return(SoapySDR::RangeList(1, SoapySDR::Range(0, 0)));
    }

    if (direction == SOAPY_SDR_TX)
        return(SoapySDR::RangeList(1, SoapySDR::Range(47000000, 6000000000ull)));

    if (direction == SOAPY_SDR_RX)
        return(SoapySDR::RangeList(1, SoapySDR::Range(70000000, 6000000000ull)));

    return(SoapySDR::RangeList(1, SoapySDR::Range(0, 0)));
}

SoapySDR::ArgInfoList SoapyLiteXM2SDR::getFrequencyArgsInfo(
    const int /*direction*/,
    const size_t /*channel*/) const {
    /* No baseband NCO is exposed by the current FPGA path, so do not advertise
     * Soapy's OFFSET/BB compensation knobs. Applications should tune the RF LO
     * directly to the requested center frequency. */
    return {};
}

/***************************************************************************************************
 *                                        Sample Rate API
 **************************************************************************************************/

void SoapyLiteXM2SDR::setSampleMode() {
    uint32_t bit_mode = (_bitMode == 8) ? 8 : 16;

    /* 8-bit mode */
    if (_bitMode == 8) {
        _bytesPerSample  = 1;
        _bytesPerComplex = 2;
        _samplesScaling  = 127.0; /* Normalize 8-bit ADC values to [-1.0, 1.0]. */
    /* 16-bit mode */
    } else {
        _bytesPerSample  = 2;
        _bytesPerComplex = 4;
        _samplesScaling  = 2047.0; /* Normalize 12-bit ADC values to [-1.0, 1.0]. */
    }

    if (_bitModeHwApplied && _bitModeHw == bit_mode)
        return;

#if USE_LITEETH
    LiteEthRfOpTimeout timeout("setSampleMode");
#endif
    int rc = m2sdr_set_bitmode(_dev, bit_mode == 8);
    if (rc != 0) {
        SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_bitmode failed: %s", m2sdr_strerror(rc));
    } else {
        _bitModeHwApplied = true;
        _bitModeHw = bit_mode;
    }
#if USE_LITEETH
    timeout.throw_if_timed_out();
#endif
}

void SoapyLiteXM2SDR::setSampleRate(
    const int direction,
    const size_t channel,
    const double rate) {
    std::lock_guard<std::mutex> lock(_mutex);

    /* Check if the requested sample rate is below 0.55 Msps and throw an exception if so */
    if (rate < 550000.0) {
        throw std::runtime_error("Sample rates below 0.55 Msps are not supported (limitation to be removed soon)");
    }

    SoapySDR::logf(SOAPY_SDR_DEBUG,
        "setSampleRate(%s, %ld, %g MHz)",
        dir2Str(direction),
        channel,
        rate / 1e6);

    uint32_t sample_rate = static_cast<uint32_t>(rate);
    _rateMult = 1.0;

#if USE_LITEPCIE
    /* For PCIe, if the sample rate is above 61.44 MSPS, force 8-bit mode + oversampling. */
    if (rate > LITEPCIE_8BIT_THRESHOLD) {
        if (_bitMode != 8) {
            SoapySDR::logf(SOAPY_SDR_WARNING,
                "Sample rate %.2f MSPS requires 8-bit + oversampling on PCIe, overriding bitmode",
                rate / 1e6);
        }
        _bitMode      = 8;
        _oversampling = 1;
    } else {
        _oversampling = 0;
    }
#elif USE_LITEETH
    /* For Ethernet, if the sample rate is above 20 MSPS, switch to 8-bit mode. */
    if (rate > LITEETH_8BIT_THRESHOLD) {
        _bitMode      = 8;
        _oversampling = 0;
    } else {
        _bitMode      = 16;
        _oversampling = 0;
    }
#endif

    /* If oversampling is enabled, double the rate multiplier. */
    if (_oversampling) {
        _rateMult = 2.0;
    }

    int64_t hw_sample_rate = static_cast<int64_t>(sample_rate / _rateMult);

    if (direction == SOAPY_SDR_TX) {
        _tx_stream.samplerate = rate;
    }
    if (direction == SOAPY_SDR_RX) {
        _rx_stream.samplerate = rate;
    }

    if (_sampleRateHwApplied &&
        _sampleRateHw == hw_sample_rate &&
        _sampleRateHwBitMode == _bitMode &&
        _sampleRateHwOversampling == _oversampling &&
        _sampleRateHwFirProfile == _ad9361_fir_profile) {
        setSampleMode();
        if (direction == SOAPY_SDR_RX && _rx_stream.opened) {
            _rx_stream.time0_ns = this->getHardwareTime("");
            _rx_stream.time0_count = _rx_stream.user_count;
            _rx_stream.time_valid = (_rx_stream.samplerate > 0.0);
            _rx_stream.last_time_ns = _rx_stream.time0_ns;
            _rx_stream.time_warned = false;
        }
        return;
    }

#if USE_LITEETH
    LiteEthRfOpTimeout timeout("setSampleRate");
#endif
    /* Check and set FIR decimation/interpolation if actual rate is below 2.5 Msps */
    double actual_rate = rate / _rateMult;
    if (actual_rate < 1250000.0) {
        SoapySDR::logf(SOAPY_SDR_DEBUG, "Setting FIR decimation/interpolation to 4 for rate %f < 1.25 Msps", actual_rate);
        ad9361_phy->rx_fir_dec    = 4;
        ad9361_phy->tx_fir_int    = 4;
        ad9361_phy->bypass_rx_fir = 0;
        ad9361_phy->bypass_tx_fir = 0;
        AD9361_RXFIRConfig rx_fir_cfg = rx_fir_config_dec4;
        AD9361_TXFIRConfig tx_fir_cfg = tx_fir_config_int4;
        rx_fir_cfg.rx_dec = 4;
        tx_fir_cfg.tx_int = 4;
        ad9361_set_rx_fir_config(ad9361_phy, rx_fir_cfg);
        ad9361_set_tx_fir_config(ad9361_phy, tx_fir_cfg);
        ad9361_set_rx_fir_en_dis(ad9361_phy, 1);
        ad9361_set_tx_fir_en_dis(ad9361_phy, 1);
    }
    else if (actual_rate < 2500000.0) {
        SoapySDR::logf(SOAPY_SDR_DEBUG, "Setting FIR decimation/interpolation to 2 for rate %f < 2.5 Msps", actual_rate);
        ad9361_phy->rx_fir_dec    = 2;
        ad9361_phy->tx_fir_int    = 2;
        ad9361_phy->bypass_rx_fir = 0;
        ad9361_phy->bypass_tx_fir = 0;
        AD9361_RXFIRConfig rx_fir_cfg = rx_fir_config_dec2;
        AD9361_TXFIRConfig tx_fir_cfg = tx_fir_config_int2;
        rx_fir_cfg.rx_dec = 2;
        tx_fir_cfg.tx_int = 2;
        ad9361_set_rx_fir_config(ad9361_phy, rx_fir_cfg);
        ad9361_set_tx_fir_config(ad9361_phy, tx_fir_cfg);
        ad9361_set_rx_fir_en_dis(ad9361_phy, 1);
        ad9361_set_tx_fir_en_dis(ad9361_phy, 1);
    }
    else {
        /* Restore default FIR configs when above 2.5 Msps. */
        ad9361_phy->rx_fir_dec    = 1;
        ad9361_phy->tx_fir_int    = 1;
        ad9361_phy->bypass_rx_fir = 0;
        ad9361_phy->bypass_tx_fir = 0;
        AD9361_RXFIRConfig rx_fir_cfg;
        AD9361_TXFIRConfig tx_fir_cfg;
        std::string canonical;
        if (!select_ad9361_fir_profile_1x(_ad9361_fir_profile, rx_fir_cfg, tx_fir_cfg, canonical)) {
            throw std::runtime_error("Invalid cached ad9361_fir_profile: " + _ad9361_fir_profile);
        }
        ad9361_set_rx_fir_config(ad9361_phy, rx_fir_cfg);
        ad9361_set_tx_fir_config(ad9361_phy, tx_fir_cfg);
        ad9361_set_rx_fir_en_dis(ad9361_phy, 1);
        ad9361_set_tx_fir_en_dis(ad9361_phy, 1);
    }
#if USE_LITEETH
    timeout.throw_if_timed_out();
#endif

    {
        int rc = m2sdr_set_sample_rate(_dev, hw_sample_rate);
        if (rc != 0) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_sample_rate failed: %s", m2sdr_strerror(rc));
        } else {
            _sampleRateHwApplied = true;
            _sampleRateHw = hw_sample_rate;
            _sampleRateHwBitMode = _bitMode;
            _sampleRateHwOversampling = _oversampling;
            _sampleRateHwFirProfile = _ad9361_fir_profile;
        }
    }
#if USE_LITEETH
    timeout.throw_if_timed_out();
#endif

    /* If oversampling is enabled, enable oversampling on the hardware. */
    if (_oversampling) {
        ad9361_enable_oversampling(ad9361_phy);
#if USE_LITEETH
        timeout.throw_if_timed_out();
#endif
    }

    /* Finally, update the sample mode (bit depth) based on the new configuration. */
    setSampleMode();

    if (direction == SOAPY_SDR_RX && _rx_stream.opened) {
        _rx_stream.time0_ns = this->getHardwareTime("");
        _rx_stream.time0_count = _rx_stream.user_count;
        _rx_stream.time_valid = (_rx_stream.samplerate > 0.0);
        _rx_stream.last_time_ns = _rx_stream.time0_ns;
        _rx_stream.time_warned = false;
    }
}


double SoapyLiteXM2SDR::getSampleRate(
    const int direction,
    const size_t) const {

    uint32_t sample_rate = 0;

    if (direction == SOAPY_SDR_TX) {
#if USE_LITEETH
        LiteEthRfOpTimeout timeout("getSampleRate(TX)");
#endif
        ad9361_get_tx_sampling_freq(ad9361_phy, &sample_rate);
#if USE_LITEETH
        timeout.throw_if_timed_out();
#endif
    }
    if (direction == SOAPY_SDR_RX) {
#if USE_LITEETH
        LiteEthRfOpTimeout timeout("getSampleRate(RX)");
#endif
        ad9361_get_rx_sampling_freq(ad9361_phy, &sample_rate);
#if USE_LITEETH
        timeout.throw_if_timed_out();
#endif
    }

    return static_cast<double>(_rateMult*sample_rate);
}

std::vector<double> SoapyLiteXM2SDR::listSampleRates(
    const int /*direction*/,
    const size_t /*channel*/) const {
    std::vector<double> sampleRates;

    /* Standard SampleRates */
    sampleRates.push_back(1.0e6);     /*      1 MSPS. */
    sampleRates.push_back(2.5e6);     /*    2.5 MSPS. */
    sampleRates.push_back(5.0e6);     /*      5 MSPS. */
    sampleRates.push_back(10.0e6);    /*     10 MSPS. */
    sampleRates.push_back(20.0e6);    /*     20 MSPS. */

    /* LTE / 5G NR SampleRates */
    sampleRates.push_back(1.92e6);    /*  1.92 MSPS (LTE 1.4 MHz BW). */
    sampleRates.push_back(3.84e6);    /*  3.84 MSPS (LTE 3 MHz BW).   */
    sampleRates.push_back(7.68e6);    /*  7.68 MSPS (LTE 5 MHz BW).   */
    sampleRates.push_back(15.36e6);   /* 15.36 MSPS (LTE 10 MHz BW).  */
    sampleRates.push_back(23.04e6);   /* 23.04 MSPS (LTE 15 MHz BW).  */
    sampleRates.push_back(30.72e6);   /* 30.72 MSPS (LTE 20 MHz BW).  */
    sampleRates.push_back(61.44e6);   /* 61.44 MSPS (LTE 40 MHz BW via 2x 20 MHz CA, 5G NR 50 MHz BW). */
    sampleRates.push_back(122.88e6);  /* 122.88 MSPS (LTE 80 MHz BW via 4x 20 MHz CA, 5G NR 100 MHz BW). */

    /* Return supported SampleRates */
    return sampleRates;
}

SoapySDR::RangeList SoapyLiteXM2SDR::getSampleRateRange(
    const int /*direction*/,
    const size_t  /*channel*/) const {
    SoapySDR::RangeList results;
    results.push_back(SoapySDR::Range(0.55e6, 122.88e6));
    return results;
}

/***************************************************************************************************
*                                        Stream API
***************************************************************************************************/

std::vector<std::string> SoapyLiteXM2SDR::getStreamFormats(
    const int /*direction*/,
    const size_t /*channel*/) const {
    std::vector<std::string> formats;
    formats.push_back(SOAPY_SDR_CF32);
    formats.push_back(SOAPY_SDR_CS16);
    formats.push_back(SOAPY_SDR_CS8);
    return formats;
}

/***************************************************************************************************
 *                                      Bandwidth API
 **************************************************************************************************/

void SoapyLiteXM2SDR::setBandwidth(
    const int direction,
    const size_t channel,
    const double bw) {
    if (bw == 0.0)
        return;
    std::lock_guard<std::mutex> lock(_mutex);

    uint32_t bwi = static_cast<uint32_t>(bw);

    if (direction == SOAPY_SDR_TX)
        _tx_stream.bandwidth = bw;
    if (direction == SOAPY_SDR_RX)
        _rx_stream.bandwidth = bw;

    if (_bandwidthHwApplied && _bandwidthHw == bwi)
        return;

#if USE_LITEETH
    LiteEthRfOpTimeout timeout("setBandwidth");
#endif
    int rc = m2sdr_set_bandwidth(_dev, bwi);
    if (rc != 0) {
        SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_bandwidth failed: %s", m2sdr_strerror(rc));
    } else {
        _bandwidthHwApplied = true;
        _bandwidthHw = bwi;
    }
#if USE_LITEETH
    timeout.throw_if_timed_out();
#endif
}

double SoapyLiteXM2SDR::getBandwidth(
    const int direction,
    const size_t /*channel*/) const {

    uint32_t bw = 0;

    if (direction == SOAPY_SDR_TX) {
#if USE_LITEETH
        LiteEthRfOpTimeout timeout("getBandwidth(TX)");
#endif
        ad9361_get_tx_rf_bandwidth(ad9361_phy, &bw);
#if USE_LITEETH
        timeout.throw_if_timed_out();
#endif
    }
    if (direction == SOAPY_SDR_RX) {
#if USE_LITEETH
        LiteEthRfOpTimeout timeout("getBandwidth(RX)");
#endif
        ad9361_get_rx_rf_bandwidth(ad9361_phy, &bw);
#if USE_LITEETH
        timeout.throw_if_timed_out();
#endif
    }

    return static_cast<double>(bw);
}

SoapySDR::RangeList SoapyLiteXM2SDR::getBandwidthRange(
    const int /*direction*/,
    const size_t /*channel*/) const {
    SoapySDR::RangeList results;
        /* AD9361 supports a bandwidth range from 200 kHz to 56 MHz. */
        results.push_back(SoapySDR::Range(0.2e6, 56.0e6));
    return results;
}

/***************************************************************************************************
 *                                   Clocking API
 **************************************************************************************************/

std::vector<std::string> SoapyLiteXM2SDR::listTimeSources(void) const
{
    std::vector<std::string> sources = {"internal"};

    if (has_eth_ptp_support(_dev))
        sources.push_back("ptp");

    return sources;
}

void SoapyLiteXM2SDR::setTimeSource(const std::string &source)
{
    if (source == "internal") {
        if (!has_eth_ptp_support(_dev))
            return;
    } else if (source != "ptp") {
        throw std::runtime_error("Unsupported time source: " + source);
    }

    if (!has_eth_ptp_support(_dev))
        throw std::runtime_error("PTP time source is not available on this bitstream");

    struct m2sdr_ptp_discipline_config cfg;
    int rc = m2sdr_get_ptp_discipline_config(_dev, &cfg);
    if (rc != 0)
        throw std::runtime_error("m2sdr_get_ptp_discipline_config() failed: " + std::string(m2sdr_strerror(rc)));

    cfg.enable = (source == "ptp");

    rc = m2sdr_set_ptp_discipline_config(_dev, &cfg);
    if (rc != 0)
        throw std::runtime_error("m2sdr_set_ptp_discipline_config() failed: " + std::string(m2sdr_strerror(rc)));
}

std::string SoapyLiteXM2SDR::getTimeSource(void) const
{
    if (!has_eth_ptp_support(_dev))
        return "internal";

    struct m2sdr_ptp_status status;
    int rc = m2sdr_get_ptp_status(_dev, &status);
    if (rc != 0)
        throw std::runtime_error("m2sdr_get_ptp_status() failed: " + std::string(m2sdr_strerror(rc)));

    return status.enabled ? "ptp" : "internal";
}

/***************************************************************************************************
 *                                    Time API
 **************************************************************************************************/

bool SoapyLiteXM2SDR::hasHardwareTime(const std::string &) const {
    return true;
}

long long SoapyLiteXM2SDR::getHardwareTime(const std::string &) const
{
    uint64_t time_ns = 0;
    int rc = m2sdr_get_time(_dev, &time_ns);

    if (rc != 0)
        throw std::runtime_error("m2sdr_get_time() failed: " + std::string(m2sdr_strerror(rc)));

    SoapySDR::logf(SOAPY_SDR_DEBUG, "Hardware time (ns): %lld", static_cast<long long>(time_ns));
    return static_cast<long long>(time_ns);
}

void SoapyLiteXM2SDR::setHardwareTime(const long long timeNs, const std::string &)
{
    int rc = m2sdr_set_time(_dev, static_cast<uint64_t>(timeNs));

    if (rc != 0)
        throw std::runtime_error("m2sdr_set_time() failed: " + std::string(m2sdr_strerror(rc)));

    SoapySDR::logf(SOAPY_SDR_DEBUG, "Hardware time set to (ns): %lld", timeNs);
}

/***************************************************************************************************
 *                                    Sensors API
 **************************************************************************************************/

std::vector<std::string> SoapyLiteXM2SDR::listSensors(void) const {
    std::vector<std::string> sensors;
#ifdef CSR_XADC_BASE
    sensors.push_back("fpga_temp");
    sensors.push_back("fpga_vccint");
    sensors.push_back("fpga_vccaux");
    sensors.push_back("fpga_vccbram");
#endif
    sensors.push_back("ad9361_temp");
    if (has_eth_ptp_support(_dev)) {
        sensors.push_back("ptp_locked");
        sensors.push_back("ptp_time_locked");
        sensors.push_back("ptp_holdover");
        sensors.push_back("ptp_state");
        sensors.push_back("ptp_master_ip");
        sensors.push_back("ptp_master_clock_id");
        sensors.push_back("ptp_master_port");
        sensors.push_back("ptp_master_port_id");
        sensors.push_back("ptp_last_error_ns");
    }
#if USE_LITEETH
    sensors.push_back("liteeth_rx_buffers");
    sensors.push_back("liteeth_rx_ring_full_events");
    sensors.push_back("liteeth_rx_flushes");
    sensors.push_back("liteeth_rx_flush_bytes");
    sensors.push_back("liteeth_rx_kernel_drops");
    sensors.push_back("liteeth_rx_source_drops");
    sensors.push_back("liteeth_rx_timeout_recoveries");
    sensors.push_back("liteeth_rx_recv_errors");
    sensors.push_back("liteeth_tx_buffers");
    sensors.push_back("liteeth_tx_bytes");
    sensors.push_back("liteeth_tx_send_errors");
    sensors.push_back("liteeth_udp_rcvbuf_requested");
    sensors.push_back("liteeth_udp_rcvbuf_actual");
    sensors.push_back("liteeth_udp_sndbuf_requested");
    sensors.push_back("liteeth_udp_sndbuf_actual");
    sensors.push_back("liteeth_vrt_packets");
    sensors.push_back("liteeth_vrt_sequence_gaps");
    sensors.push_back("liteeth_vrt_packets_lost");
#endif
    return sensors;
}

SoapySDR::ArgInfo SoapyLiteXM2SDR::getSensorInfo(
    const std::string &key) const {
    SoapySDR::ArgInfo info;

    std::size_t dash = key.find("_");
    if (dash < std::string::npos) {
        std::string deviceStr = key.substr(0, dash);
        std::string sensorStr = key.substr(dash + 1);

        /* FPGA Sensors */
#ifdef CSR_XADC_BASE
        if (deviceStr == "fpga") {
            /* Temp. */
            if (sensorStr == "temp") {
                info.key         = "temp";
                info.value       = "0.0";
                info.units       = "°C";
                info.description = "FPGA temperature";
                info.type        = SoapySDR::ArgInfo::FLOAT;
            /* VCCINT. */
            } else if (sensorStr == "vccint") {
                info.key         = "vccint";
                info.value       = "0.0";
                info.units       = "V";
                info.description = "FPGA internal supply voltage";
                info.type        = SoapySDR::ArgInfo::FLOAT;
            /* VCCAUX. */
            } else if (sensorStr == "vccaux") {
                info.key         = "vccaux";
                info.value       = "0.0";
                info.units       = "V";
                info.description = "FPGA auxiliary supply voltage";
                info.type        = SoapySDR::ArgInfo::FLOAT;
            /* VCCBRAM. */
            } else if (sensorStr == "vccbram") {
                info.key         = "vccbram";
                info.value       = "0.0";
                info.units       = "V";
                info.description = "FPGA block RAM supply voltage";
                info.type        = SoapySDR::ArgInfo::FLOAT;
            } else {
                throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown sensor");
            }
            return info;
        }
#endif
        /* AD9361 Sensors */
        if (deviceStr == "ad9361") {
            /* Temp. */
            if (sensorStr == "temp") {
                info.key         = "temp";
                info.value       = "0.0";
                info.units       = "°C";
                info.description = "AD9361 temperature";
                info.type        = SoapySDR::ArgInfo::FLOAT;
            } else {
                throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown sensor");
            }
            return info;
        }

        if (deviceStr == "ptp") {
            if (sensorStr == "locked") {
                info.key         = "locked";
                info.value       = "false";
                info.description = "LiteEth PTP core locked to a master";
                info.type        = SoapySDR::ArgInfo::BOOL;
            } else if (sensorStr == "time_locked") {
                info.key         = "time_locked";
                info.value       = "false";
                info.description = "Board time within the PTP lock window";
                info.type        = SoapySDR::ArgInfo::BOOL;
            } else if (sensorStr == "holdover") {
                info.key         = "holdover";
                info.value       = "false";
                info.description = "PTP discipline running in holdover";
                info.type        = SoapySDR::ArgInfo::BOOL;
            } else if (sensorStr == "state") {
                info.key         = "state";
                info.value       = "manual";
                info.description = "PTP discipline state machine";
                info.type        = SoapySDR::ArgInfo::STRING;
            } else if (sensorStr == "master_ip") {
                info.key         = "master_ip";
                info.value       = "0.0.0.0";
                info.description = "Current PTP master IPv4 address";
                info.type        = SoapySDR::ArgInfo::STRING;
            } else if (sensorStr == "master_clock_id") {
                info.key         = "master_clock_id";
                info.value       = "0000000000000000";
                info.description = "Current PTP master clockIdentity";
                info.type        = SoapySDR::ArgInfo::STRING;
            } else if (sensorStr == "master_port") {
                info.key         = "master_port";
                info.value       = "0";
                info.description = "Current PTP master portNumber";
                info.type        = SoapySDR::ArgInfo::INT;
            } else if (sensorStr == "master_port_id") {
                info.key         = "master_port_id";
                info.value       = "0000000000000000:0";
                info.description = "Current PTP master sourcePortIdentity";
                info.type        = SoapySDR::ArgInfo::STRING;
            } else if (sensorStr == "last_error_ns") {
                info.key         = "last_error_ns";
                info.value       = "0";
                info.description = "Last signed PTP minus board-time error in nanoseconds";
                info.type        = SoapySDR::ArgInfo::INT;
            } else {
                throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown sensor");
            }
            return info;
        }

#if USE_LITEETH
        if (deviceStr == "liteeth") {
            info.key = sensorStr;
            info.value = "0";
            info.type = SoapySDR::ArgInfo::INT;
            if (sensorStr == "rx_buffers") {
                info.description = "Complete LiteEth UDP RX buffers assembled in userspace";
            } else if (sensorStr == "rx_ring_full_events") {
                info.description = "LiteEth UDP RX process calls that found the userspace ring full";
            } else if (sensorStr == "rx_flushes") {
                info.description = "LiteEth UDP RX stale-data flush events";
            } else if (sensorStr == "rx_flush_bytes") {
                info.description = "Bytes discarded by LiteEth UDP RX stale-data flushes";
            } else if (sensorStr == "rx_kernel_drops") {
                info.description = "Kernel-reported UDP RX queue drops from SO_RXQ_OVFL";
            } else if (sensorStr == "rx_source_drops") {
                info.description = "LiteEth UDP RX packets discarded by source-IP filtering";
            } else if (sensorStr == "rx_timeout_recoveries") {
                info.description = "LiteEth RX timeout recovery cycles";
            } else if (sensorStr == "rx_recv_errors") {
                info.description = "LiteEth UDP RX socket receive errors";
            } else if (sensorStr == "tx_buffers") {
                info.description = "Complete LiteEth UDP TX buffers submitted by userspace";
            } else if (sensorStr == "tx_bytes") {
                info.description = "LiteEth UDP TX payload bytes submitted by userspace";
            } else if (sensorStr == "tx_send_errors") {
                info.description = "LiteEth UDP TX socket send errors";
            } else if (sensorStr == "udp_rcvbuf_requested") {
                info.description = "Requested LiteEth UDP SO_RCVBUF size in bytes";
            } else if (sensorStr == "udp_rcvbuf_actual") {
                info.description = "Actual Linux LiteEth UDP SO_RCVBUF size in bytes";
            } else if (sensorStr == "udp_sndbuf_requested") {
                info.description = "Requested LiteEth UDP SO_SNDBUF size in bytes";
            } else if (sensorStr == "udp_sndbuf_actual") {
                info.description = "Actual Linux LiteEth UDP SO_SNDBUF size in bytes";
            } else if (sensorStr == "vrt_packets") {
                info.description = "LiteEth VRT RX packets accepted by the Soapy parser";
            } else if (sensorStr == "vrt_sequence_gaps") {
                info.description = "LiteEth VRT RX packet-count discontinuities";
            } else if (sensorStr == "vrt_packets_lost") {
                info.description = "Modulo-16 LiteEth VRT RX packets skipped across sequence gaps";
            } else {
                throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown sensor");
            }
            return info;
        }
#endif

        throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown device");
    }
    throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown key");
}

std::string SoapyLiteXM2SDR::readSensor(
    const std::string &key) const {
    std::string sensorValue;

    std::size_t dash = key.find("_");
    if (dash < std::string::npos) {
        std::string deviceStr = key.substr(0, dash);
        std::string sensorStr = key.substr(dash + 1);

         /* FPGA Sensors */
#ifdef CSR_XADC_BASE
        if (deviceStr == "fpga") {
            struct m2sdr_fpga_sensors sensors;
            if (m2sdr_get_fpga_sensors(_dev, &sensors) != 0) {
                throw std::runtime_error("SoapyLiteXM2SDR::readSensor(" + key + ") failed");
            }
            /* Temp. */
            if (sensorStr == "temp") {
                sensorValue = std::to_string(sensors.temperature_c);
            /* VCCINT. */
            } else if (sensorStr == "vccint") {
                sensorValue = std::to_string(sensors.vccint_v);
            /* VCCAUX. */
            } else if (sensorStr == "vccaux") {
                sensorValue = std::to_string(sensors.vccaux_v);
            /* VCCBRAM. */
            } else if (sensorStr == "vccbram") {
                sensorValue = std::to_string(sensors.vccbram_v);
            } else {
                throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown sensor");
            }
            return sensorValue;
        }
#endif
        /* AD9361 Sensors */
        if (deviceStr == "ad9361") {
            /* Temp. */
            if (sensorStr == "temp") {
                sensorValue = std::to_string(ad9361_get_temp(ad9361_phy)/1000); /* FIXME/CHECKME*/
            } else {
                throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown sensor");
            }
            return sensorValue;
        }

        if (deviceStr == "ptp") {
            struct m2sdr_ptp_status status;
            if (m2sdr_get_ptp_status(_dev, &status) != 0)
                throw std::runtime_error("SoapyLiteXM2SDR::readSensor(" + key + ") failed");

            if (sensorStr == "locked") {
                sensorValue = status.ptp_locked ? "true" : "false";
            } else if (sensorStr == "time_locked") {
                sensorValue = status.time_locked ? "true" : "false";
            } else if (sensorStr == "holdover") {
                sensorValue = status.holdover ? "true" : "false";
            } else if (sensorStr == "state") {
                sensorValue = ptp_state_to_string(status.state);
            } else if (sensorStr == "master_ip") {
                sensorValue = ptp_ipv4_to_string(status.master_ip);
            } else if (sensorStr == "master_clock_id") {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%016" PRIx64, status.master_port.clock_id);
                sensorValue = buf;
            } else if (sensorStr == "master_port") {
                sensorValue = std::to_string(status.master_port.port_number);
            } else if (sensorStr == "master_port_id") {
                sensorValue = ptp_port_identity_to_string(status.master_port);
            } else if (sensorStr == "last_error_ns") {
                sensorValue = std::to_string(status.last_error_ns);
            } else {
                throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown sensor");
            }
            return sensorValue;
        }

#if USE_LITEETH
        if (deviceStr == "liteeth") {
            if (sensorStr == "rx_buffers") {
                sensorValue = std::to_string(_udp.rx_buffers);
            } else if (sensorStr == "rx_ring_full_events") {
                sensorValue = std::to_string(_udp.rx_ring_full_events);
            } else if (sensorStr == "rx_flushes") {
                sensorValue = std::to_string(_udp.rx_flushes);
            } else if (sensorStr == "rx_flush_bytes") {
                sensorValue = std::to_string(_udp.rx_flush_bytes);
            } else if (sensorStr == "rx_kernel_drops") {
                sensorValue = std::to_string(_udp.rx_kernel_drops);
            } else if (sensorStr == "rx_source_drops") {
                sensorValue = std::to_string(_udp.rx_source_drops);
            } else if (sensorStr == "rx_timeout_recoveries") {
                sensorValue = std::to_string(_rx_stream.rx_timeout_recoveries);
            } else if (sensorStr == "rx_recv_errors") {
                sensorValue = std::to_string(_udp.rx_recv_errors);
            } else if (sensorStr == "tx_buffers") {
                sensorValue = std::to_string(_udp.tx_buffers);
            } else if (sensorStr == "tx_bytes") {
                sensorValue = std::to_string(_udp.tx_bytes);
            } else if (sensorStr == "tx_send_errors") {
                sensorValue = std::to_string(_udp.tx_send_errors);
            } else if (sensorStr == "udp_rcvbuf_requested") {
                sensorValue = std::to_string(_udp.so_rcvbuf_bytes);
            } else if (sensorStr == "udp_rcvbuf_actual") {
                int value = 0;
                (void)liteeth_udp_get_so_rcvbuf(const_cast<struct liteeth_udp_ctrl *>(&_udp), &value);
                sensorValue = std::to_string(value);
            } else if (sensorStr == "udp_sndbuf_requested") {
                sensorValue = std::to_string(_udp.so_sndbuf_bytes);
            } else if (sensorStr == "udp_sndbuf_actual") {
                int value = 0;
                (void)liteeth_udp_get_so_sndbuf(const_cast<struct liteeth_udp_ctrl *>(&_udp), &value);
                sensorValue = std::to_string(value);
            } else if (sensorStr == "vrt_packets") {
                sensorValue = std::to_string(_rx_stream.vrt_packets);
            } else if (sensorStr == "vrt_sequence_gaps") {
                sensorValue = std::to_string(_rx_stream.vrt_sequence_gaps);
            } else if (sensorStr == "vrt_packets_lost") {
                sensorValue = std::to_string(_rx_stream.vrt_packets_lost);
            } else {
                throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown sensor");
            }
            return sensorValue;
        }
#endif

        throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown device");
    }
    throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown key");
}
