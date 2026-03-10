/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2026 Enjoy Digital.
 * SPDX-License-Identifier: Apache-2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <unistd.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cctype>

#include "liblitepcie.h"
#include "libm2sdr.h"
#include "etherbone.h"

#include "LiteXM2SDRDevice.hpp"

#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Types.hpp>

namespace {
std::vector<std::string> split_list(const std::string &value)
{
    std::vector<std::string> out;
    std::string token;
    auto trim_token = [](const std::string &s) {
        size_t start = 0;
        size_t end = s.size();
        while (start < end && std::isspace(static_cast<unsigned char>(s[start])))
            start++;
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
            end--;
        return s.substr(start, end - start);
    };
    for (char ch : value) {
        if (ch == ',') {
            if (!token.empty()) {
                std::string trimmed = trim_token(token);
                if (!trimmed.empty())
                    out.push_back(trimmed);
            }
            token.clear();
            continue;
        }
        token.push_back(ch);
    }
    if (!token.empty()) {
        std::string trimmed = trim_token(token);
        if (!trimmed.empty())
            out.push_back(trimmed);
    }
    return out;
}

std::string parse_agc_mode(const std::string &mode)
{
    if (mode == "slow" || mode == "slowattack")
        return "slowattack";
    if (mode == "fast" || mode == "fastattack")
        return "fastattack";
    if (mode == "hybrid")
        return "hybrid";
    if (mode == "manual" || mode == "mgc")
        return "manual";
    throw std::runtime_error("Invalid rx_agc_mode: " + mode);
}

bool antenna_allowed(const std::vector<std::string> &ants, const std::string &name)
{
    if (ants.empty())
        return true;
    return std::find(ants.begin(), ants.end(), name) != ants.end();
}

std::string normalize_antenna_name(const std::string &rfic_name, int direction, const std::string &name)
{
    /* GUI clients such as Gqrx often persist generic "RX"/"TX" antenna names.
     * Accept those aliases on AD9361 as well so reopening an existing session
     * does not fail just because the backend now exposes a more specific name. */
    if (rfic_name == "ad9361") {
        if (direction == SOAPY_SDR_RX && name == "RX")
            return "A_BALANCED";
        if (direction == SOAPY_SDR_TX && name == "TX")
            return "A";
    }
    return name;
}

std::vector<std::string> normalize_antenna_list(
    const std::string &rfic_name,
    int direction,
    const std::vector<std::string> &antennas)
{
    std::vector<std::string> normalized;
    normalized.reserve(antennas.size());
    for (const auto &antenna : antennas) {
        const std::string canonical = normalize_antenna_name(rfic_name, direction, antenna);
        if (std::find(normalized.begin(), normalized.end(), canonical) == normalized.end())
            normalized.push_back(canonical);
    }
    return normalized;
}

std::vector<std::string> default_rx_antennas_for_backend(const std::string &rfic_name)
{
    if (rfic_name == "ad9361")
        return {"A_BALANCED"};
    return {"RX"};
}

std::vector<std::string> default_tx_antennas_for_backend(const std::string &rfic_name)
{
    if (rfic_name == "ad9361")
        return {"A"};
    return {"TX"};
}
} // namespace

/***************************************************************************************************
 *                                     Constructor
 **************************************************************************************************/

#if USE_LITEETH
static std::string getLocalIPAddressToReach(const std::string &remote_ip, uint16_t remote_port)
{
    struct sockaddr_in remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(remote_port);
    if (inet_pton(AF_INET, remote_ip.c_str(), &remote_addr.sin_addr) != 1)
        throw std::runtime_error("Invalid remote IP address");

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        throw std::runtime_error("Failed to create socket");

    if (connect(sock, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) < 0) {
        close(sock);
        throw std::runtime_error("Failed to connect socket");
    }

    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(sock, (struct sockaddr*)&local_addr, &addr_len) < 0) {
        close(sock);
        throw std::runtime_error("getsockname() failed");
    }

    close(sock);

    char buf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &local_addr.sin_addr, buf, sizeof(buf)))
        throw std::runtime_error("inet_ntop failed");

    return std::string(buf);
}
#endif

std::string getLiteXM2SDRSerial(struct m2sdr_dev *dev);
std::string getLiteXM2SDRIdentification(struct m2sdr_dev *dev);
static std::string getLiteXM2SDRRficName(struct m2sdr_dev *dev);

static std::string getLiteXM2SDRRficName(struct m2sdr_dev *dev)
{
    char name[32] = {0};
    if (m2sdr_get_rfic_name(dev, name, sizeof(name)) != 0 || name[0] == '\0')
        return "unknown-rfic";
    return std::string(name);
}

static bool getLiteXM2SDRRficCaps(struct m2sdr_dev *dev, struct m2sdr_rfic_caps *caps)
{
    return dev && caps && m2sdr_get_rfic_caps(dev, caps) == 0;
}

#if USE_LITEPCIE
void dma_set_loopback(int fd, bool loopback_enable) {
    struct litepcie_ioctl_dma m;
    m.loopback_enable = loopback_enable ? 1 : 0;
    checked_ioctl(fd, LITEPCIE_IOCTL_DMA, &m);
}
#endif

SoapyLiteXM2SDR::SoapyLiteXM2SDR(const SoapySDR::Kwargs &args)
    : _deviceArgs(args), _rx_buf_size(0), _tx_buf_size(0), _rx_buf_count(0), _tx_buf_count(0),
#if USE_LITEETH
      _udp_inited(false),
#endif
      _fd(FD_INIT) {

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

    SoapySDR::logf(SOAPY_SDR_INFO, "Opened devnode %s, serial %s", path.c_str(), getLiteXM2SDRSerial(_dev).c_str());
#elif USE_LITEETH
    /* Prepare EtherBone / Ethernet streamer */
    std::string eth_ip;
    if (args.count("eth_ip") == 0)
        eth_ip = "192.168.1.50";
    else
        eth_ip = args.at("eth_ip");

    /* EtherBone */
    std::string dev_id = "eth:" + eth_ip + ":1234";
    int rc = m2sdr_open(&_dev, dev_id.c_str());
    if (rc != 0) {
        throw std::runtime_error(
            "Can't connect to EtherBone at " + eth_ip +
            ":1234 (hint: set eth_ip=... for the board IP, error: " +
            std::string(m2sdr_strerror(rc)) + ")");
    }
    _fd = reinterpret_cast<litex_m2sdr_device_desc_t>(m2sdr_get_eb_handle(_dev));

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

    /* Ethernet FPGA streamer configuration */

    /* Determine the local IP that the board sees */
    std::string local_ip = getLocalIPAddressToReach(eth_ip, 1234);

    struct in_addr ip_addr_struct;
    if (inet_pton(AF_INET, local_ip.c_str(), &ip_addr_struct) != 1) {
        throw std::runtime_error("Invalid local IP address determined");
    }

    uint32_t ip_addr_val = ntohl(ip_addr_struct.s_addr);

    /* Write the PC's IP to the FPGA's ETH_STREAMER IP register */
    litex_m2sdr_writel(_dev, CSR_ETH_RX_STREAMER_IP_ADDRESS_ADDR, ip_addr_val);

    SoapySDR::logf(SOAPY_SDR_INFO, "Using local IP: %s for streaming", local_ip.c_str());

    if (_eth_mode == SoapyLiteXM2SDREthernetMode::VRT) {
        /* Route RX to Ethernet on the main crossbar. */
        litex_m2sdr_writel(_dev, CSR_CROSSBAR_DEMUX_SEL_ADDR, 1);
#ifdef CSR_ETH_RX_MODE_ADDR
        litex_m2sdr_writel(_dev, CSR_ETH_RX_MODE_ADDR, 2); /* Ethernet RX branch -> VRT */
#else
        throw std::runtime_error("eth_mode=vrt requested, but FPGA bitstream lacks eth_rx_mode CSR (rebuild with --with-eth-vrt)");
#endif

    _rficName = getLiteXM2SDRRficName(_dev);
#ifdef CSR_VRT_STREAMER_VRT_STREAMER_ENABLE_ADDR
        litex_m2sdr_writel(_dev, CSR_VRT_STREAMER_VRT_STREAMER_ENABLE_ADDR, 0);
        litex_m2sdr_writel(_dev, CSR_VRT_STREAMER_VRT_STREAMER_IP_ADDRESS_ADDR, ip_addr_val);
        if (args.count("vrt_port") > 0) {
            litex_m2sdr_writel(_dev, CSR_VRT_STREAMER_VRT_STREAMER_UDP_PORT_ADDR,
                static_cast<uint32_t>(std::stoul(args.at("vrt_port"))));
        }
        litex_m2sdr_writel(_dev, CSR_VRT_STREAMER_VRT_STREAMER_ENABLE_ADDR, 1);
#else
        throw std::runtime_error("eth_mode=vrt requested, but FPGA bitstream lacks vrt_streamer CSR (rebuild with --with-eth-vrt)");
#endif
        SoapySDR::logf(SOAPY_SDR_INFO, "Enabled FPGA VRT RX streaming");
    }
#endif

    _rficName = getLiteXM2SDRRficName(_dev);

    /* Configure PCIe Synchronizer and DMA Headers. */
#if USE_LITEPCIE
    SoapySDR::log(SOAPY_SDR_INFO, "Configuring PCIe DMA headers");
    /* Enable Synchronizer */
    litex_m2sdr_writel(_dev, CSR_PCIE_DMA0_SYNCHRONIZER_BYPASS_ADDR, 0);

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

    /* Keep argument parsing in Soapy, but push RFIC-specific validation and
     * hardware programming into libm2sdr. */
    if (args.count("iq_bits") > 0) {
        _iqBits = static_cast<uint32_t>(std::stoul(args.at("iq_bits")));
    } else if (args.count("bitmode") > 0) {
        /* Backward-compatible alias:
         * bitmode=8 -> iq_bits=8
         * bitmode=16 -> iq_bits=12 (AD9361 native precision in 16-bit container) */
        const uint32_t legacy = static_cast<uint32_t>(std::stoul(args.at("bitmode")));
        _iqBits = (legacy == 8u) ? 8u : 12u;
    } else {
        _iqBits = 12u;
    }
    _bitMode = (_iqBits <= 8u) ? 8u : 16u;

    if (args.count("oversampling") > 0) {
        _oversampling = std::stoi(args.at("oversampling"));
    }
    if (args.count("ad9361_fir_profile") > 0) {
        _ad9361_fir_profile = args.at("ad9361_fir_profile");
    } else if (args.count("fir_profile") > 0) {
        _ad9361_fir_profile = args.at("fir_profile");
    }

    /* FIR profile is still backend-specific, so it stays in the namespaced
     * property path until another RFIC needs a generic equivalent. */
    if (_rficName == "ad9361") {
        int prop_rc = m2sdr_set_property(_dev, "ad9361.fir_profile", _ad9361_fir_profile.c_str());

        if (prop_rc != 0) {
            throw std::runtime_error(
                "Invalid ad9361_fir_profile '" + _ad9361_fir_profile +
                "' (supported: legacy, bypass, match, wide)");
        }
        char fir_profile_canonical[32];
        if (m2sdr_get_property(_dev, "ad9361.fir_profile", fir_profile_canonical, sizeof(fir_profile_canonical)) == 0)
            _ad9361_fir_profile = fir_profile_canonical;
        SoapySDR::logf(SOAPY_SDR_INFO, "AD9361 1x FIR profile: %s", _ad9361_fir_profile.c_str());
    }

    _rx_antennas = default_rx_antennas_for_backend(_rficName);
    _tx_antennas = default_tx_antennas_for_backend(_rficName);
    if (args.count("rx_antenna_list") > 0)
        _rx_antennas = normalize_antenna_list(
            _rficName, SOAPY_SDR_RX, split_list(args.at("rx_antenna_list")));
    if (args.count("tx_antenna_list") > 0)
        _tx_antennas = normalize_antenna_list(
            _rficName, SOAPY_SDR_TX, split_list(args.at("tx_antenna_list")));

    _rx_stream.antenna[0] = _rx_antennas.empty() ? default_rx_antennas_for_backend(_rficName)[0] : _rx_antennas[0];
    _rx_stream.antenna[1] = _rx_stream.antenna[0];
    _tx_stream.antenna[0] = _tx_antennas.empty() ? default_tx_antennas_for_backend(_rficName)[0] : _tx_antennas[0];
    _tx_stream.antenna[1] = _tx_stream.antenna[0];

    _rx_agc_mode = "slowattack";
    if (_rficName == "ad9361" && args.count("rx_agc_mode") > 0)
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
    /* Soapy now delegates RFIC bring-up to libm2sdr. The plugin remains
     * responsible for default selection and Soapy-facing behavior, while
     * backend-specific sequencing stays in one place. */
    if (_rficName == "ad9361") {
        struct m2sdr_config cfg;
        int rc;

        SoapySDR::log(SOAPY_SDR_INFO, do_init ? "Initializing RFIC through libm2sdr" :
                                            "Attaching to existing RFIC state through libm2sdr");

        m2sdr_config_init(&cfg);
        cfg.sample_rate       = 30720000;
        cfg.bandwidth         = 30720000;
        cfg.refclk_freq       = refclk_hz;
        cfg.tx_freq           = 1000000;
        cfg.rx_freq           = 1000000;
        cfg.tx_gain           = -20;
        cfg.rx_gain1          = 0;
        cfg.rx_gain2          = 0;
        cfg.enable_8bit_mode  = (_iqBits <= 8u);
        cfg.enable_oversample = (_oversampling != 0);
        cfg.bypass_rfic_init  = !do_init;
        cfg.channel_layout    = M2SDR_CHANNEL_LAYOUT_2T2R;
        cfg.clock_source      = (clock_source == "external") ? M2SDR_CLOCK_SOURCE_EXTERNAL
                                                             : M2SDR_CLOCK_SOURCE_INTERNAL;

        rc = m2sdr_apply_config(_dev, &cfg);
        if (rc != 0) {
            throw std::runtime_error("m2sdr_apply_config failed (" + std::string(m2sdr_strerror(rc)) + ")");
        }
    }

    if (_rficName == "ad9361" && do_init) {
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
        _rx_stream.gainMode[0]  = false;
        _rx_stream.gain[0]      = 0;
        _tx_stream.gain[0]      = 20;
        _rx_stream.iqbalance[0] = 1.0;
        _tx_stream.iqbalance[0] = 1.0;
        SoapySDR::log(SOAPY_SDR_INFO, "Configuring RX/TX channel 0");
        channel_configure(SOAPY_SDR_RX, 0);
        channel_configure(SOAPY_SDR_TX, 0);

        /* TX2/RX2. */
        _rx_stream.gainMode[1]  = false;
        _rx_stream.gain[1]      = 0;
        _tx_stream.gain[1]      = 20;
        _rx_stream.iqbalance[1] = 1.0;
        _tx_stream.iqbalance[1] = 1.0;
        SoapySDR::log(SOAPY_SDR_INFO, "Configuring RX/TX channel 1");
        channel_configure(SOAPY_SDR_RX, 1);
        channel_configure(SOAPY_SDR_TX, 1);
    }

    if (args.count("rx_antenna0") > 0)
        _rx_stream.antenna[0] = normalize_antenna_name(_rficName, SOAPY_SDR_RX, args.at("rx_antenna0"));
    if (args.count("rx_antenna1") > 0)
        _rx_stream.antenna[1] = normalize_antenna_name(_rficName, SOAPY_SDR_RX, args.at("rx_antenna1"));
    if (args.count("tx_antenna0") > 0)
        _tx_stream.antenna[0] = normalize_antenna_name(_rficName, SOAPY_SDR_TX, args.at("tx_antenna0"));
    if (args.count("tx_antenna1") > 0)
        _tx_stream.antenna[1] = normalize_antenna_name(_rficName, SOAPY_SDR_TX, args.at("tx_antenna1"));

    if (!antenna_allowed(_rx_antennas, _rx_stream.antenna[0]) ||
        !antenna_allowed(_rx_antennas, _rx_stream.antenna[1])) {
        throw std::runtime_error("Invalid RX antenna selection");
    }
    if (!antenna_allowed(_tx_antennas, _tx_stream.antenna[0]) ||
        !antenna_allowed(_tx_antennas, _tx_stream.antenna[1])) {
        throw std::runtime_error("Invalid TX antenna selection");
    }

    setSampleMode();

    SoapySDR::logf(SOAPY_SDR_INFO, "Active RFIC backend: %s", _rficName.c_str());

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
    if (_rx_stream.opened) {
#if USE_LITEPCIE
         /* Release the DMA engine. */
        if (_rx_stream.dma.buf_rd != NULL) {
            litepcie_dma_cleanup(&_rx_stream.dma);
        }
        _rx_stream.buf = NULL;
#elif USE_LITEETH
        /* nothing to stop explicitly in UDP helper */
#endif
        _rx_stream.opened = false;
    }

#if USE_LITEPCIE
    if (_tx_stream.opened) {
        /* Release the DMA engine. */
        if (_tx_stream.dma.buf_wr) {
            litepcie_dma_cleanup(&_tx_stream.dma);
        }
        _rx_stream.buf = NULL;
        _tx_stream.opened = false;
    }
#endif

    /* Crossbar Mux/Demux : Select PCIe streaming */
    litex_m2sdr_writel(_dev, CSR_CROSSBAR_MUX_SEL_ADDR,   0);
    litex_m2sdr_writel(_dev, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);

    if (_rficName == "ad9361") {
        /* Power-Down AD9361 */
        litex_m2sdr_writel(_dev, CSR_AD9361_CONFIG_ADDR, 0b00);
    }

#if USE_LITEETH
    if (_udp_inited) {
        liteeth_udp_cleanup(&_udp);
        _udp_inited = false;
    }
#ifdef CSR_VRT_STREAMER_VRT_STREAMER_ENABLE_ADDR
    if (_eth_mode == SoapyLiteXM2SDREthernetMode::VRT) {
        litex_m2sdr_writel(_dev, CSR_VRT_STREAMER_VRT_STREAMER_ENABLE_ADDR, 0);
    }
#endif
    if (_dev) {
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
        this->setGain(SOAPY_SDR_RX,       channel, _rx_stream.gain[channel]);
        this->setIQBalance(SOAPY_SDR_RX,  channel, _rx_stream.iqbalance[channel]);
    }

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

    key += "-";
    key += getLiteXM2SDRRficName(_dev);

    return key;
}

/***************************************************************************************************
*                                     Channel API
***************************************************************************************************/

size_t SoapyLiteXM2SDR::getNumChannels(const int) const {
    struct m2sdr_rfic_caps caps;

    if (getLiteXM2SDRRficCaps(_dev, &caps))
        return std::max<size_t>(caps.rx_channels, caps.tx_channels);
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
        return default_rx_antennas_for_backend(_rficName);
    }
    if (direction == SOAPY_SDR_TX) {
        if (!_tx_antennas.empty())
            return _tx_antennas;
        return default_tx_antennas_for_backend(_rficName);
    }
    return {};
}

void SoapyLiteXM2SDR::setAntenna(
    const int direction,
    const size_t channel,
    const std::string &name) {
    std::lock_guard<std::mutex> lock(_mutex);
    const std::string canonical = normalize_antenna_name(_rficName, direction, name);
    if (direction == SOAPY_SDR_RX) {
        if (!antenna_allowed(_rx_antennas, canonical))
            throw std::runtime_error("Unsupported RX antenna: " + name);
        _rx_stream.antenna[channel] = canonical;
    }
    if (direction == SOAPY_SDR_TX) {
        if (!antenna_allowed(_tx_antennas, canonical))
            throw std::runtime_error("Unsupported TX antenna: " + name);
        _tx_stream.antenna[channel] = canonical;
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
    struct m2sdr_rfic_caps caps;

    /* TX */
    if (direction == SOAPY_SDR_TX)
        return false;

    /* RX */
    if (direction == SOAPY_SDR_RX)
        return getLiteXM2SDRRficCaps(_dev, &caps) &&
               (caps.features & M2SDR_RFIC_FEATURE_RX_GAIN_MODE) != 0;

    /* Fallback */
    return false;
}

void SoapyLiteXM2SDR::setGainMode(const int direction, const size_t channel,
    const bool automatic)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const std::string key = std::string("ad9361.rx") + std::to_string(channel) + "_gain_mode";
    const std::string mode = automatic ? _rx_agc_mode : "manual";

    /* N/A. */
    if (direction == SOAPY_SDR_TX)
        return;
    if (_rficName != "ad9361")
        throw std::runtime_error("Gain mode control is not implemented for RFIC backend " + _rficName);

    /* Gain-mode strings are backend properties because the generic Soapy toggle
     * still fans out to AD9361-specific policies such as slowattack/hybrid. */
    _rx_stream.gainMode[channel] = automatic;
    if (m2sdr_set_property(_dev, key.c_str(), mode.c_str()) != 0)
        throw std::runtime_error("Failed to set RX gain mode for channel " + std::to_string(channel));
}

bool SoapyLiteXM2SDR::getGainMode(const int direction, const size_t channel) const
{

    /* TX */
    if (direction == SOAPY_SDR_TX)
        return false;

    /* RX */
    if (direction == SOAPY_SDR_RX) {
        char value[32];

        if (_rficName != "ad9361")
            return false;
        if (m2sdr_get_property(_dev,
                               (std::string("ad9361.rx") + std::to_string(channel) + "_gain_mode").c_str(),
                               value,
                               sizeof(value)) != 0)
            return _rx_stream.gainMode[channel];
        return std::string(value) != "manual";
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
        _tx_stream.gain[channel] = value;
        double   att_db  = (value >= 0.0) ? value : -value;
        /* Clarify interpretation */
        if (value >= 0.0) {
            SoapySDR::logf(SOAPY_SDR_DEBUG, "TX ch%zu: %.3f dB Attenuation", channel, att_db);
        } else {
            SoapySDR::logf(SOAPY_SDR_WARNING,
                "TX ch%zu: negative gain value %.3f dB is deprecated; use ATT with positive dB",
                channel, value);
        }

        int rc = m2sdr_set_gain(_dev, M2SDR_TX, -att_db);
        if (rc != 0) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_gain(TX) failed: %s", m2sdr_strerror(rc));
        }
    }

    /* RX */
    if (SOAPY_SDR_RX == direction) {
        _rx_stream.gain[channel] = value;
        SoapySDR::logf(SOAPY_SDR_DEBUG, "RX ch%zu: %.3f dB Gain", channel, value);
        int rc = m2sdr_set_gain(_dev, M2SDR_RX, value);
        if (rc != 0) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_gain(RX) failed: %s", m2sdr_strerror(rc));
        }
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
            /* Positive attenuation in dB. */
            _tx_stream.gain[channel] = -value;
            SoapySDR::logf(SOAPY_SDR_DEBUG, "TX ch%zu: ATT %.3f dB Attenuation", channel, value);
            int rc = m2sdr_set_gain(_dev, M2SDR_TX, -value);
            if (rc != 0) {
                SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_gain(TX) failed: %s", m2sdr_strerror(rc));
            }
            return;
        }
        if (name == "GAIN") {
            /* Negative gain in dB. */
            _tx_stream.gain[channel] = value;
            SoapySDR::logf(SOAPY_SDR_WARNING,
                "TX ch%zu: GAIN is deprecated; use ATT with positive dB", channel);
            int rc = m2sdr_set_gain(_dev, M2SDR_TX, value);
            if (rc != 0) {
                SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_gain(TX) failed: %s", m2sdr_strerror(rc));
            }
            return;
        }
    }

    /* RX */
    if (name == "PGA" || name == "RF" || name == "GAIN") {
        _rx_stream.gain[channel] = value;
        SoapySDR::logf(SOAPY_SDR_DEBUG, "RX ch%zu: RF %.3f dB Gain", channel, value);
        int rc = m2sdr_set_gain(_dev, M2SDR_RX, value);
        if (rc != 0) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_gain(RX) failed: %s", m2sdr_strerror(rc));
        }
        return;
    }

    /* Fallback */
    setGain(direction, channel, value);
}

double SoapyLiteXM2SDR::getGain(
    const int direction,
    const size_t channel) const
{
    int64_t gain = 0;
    const enum m2sdr_direction mdir = (direction == SOAPY_SDR_TX) ? M2SDR_TX : M2SDR_RX;

    (void)channel;
    if (m2sdr_get_gain(_dev, mdir, &gain) != 0)
        return 0.0;
    return static_cast<double>(gain);
}

double SoapyLiteXM2SDR::getGain(
    const int direction,
    const size_t channel,
    const std::string &name) const
{
    /* TX */
    if (direction == SOAPY_SDR_TX) {
        const double atten_db = -getGain(direction, channel);
        if (name == "ATT")
            return atten_db;     /* Positive attenuation. */
        if (name == "GAIN")
            return -atten_db;        /* Negative gain. */
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
    struct m2sdr_rfic_caps caps;

    if (getLiteXM2SDRRficCaps(_dev, &caps)) {
        if (direction == SOAPY_SDR_TX)
            return SoapySDR::Range((double)caps.min_tx_gain, (double)caps.max_tx_gain);
        if (direction == SOAPY_SDR_RX)
            return SoapySDR::Range((double)caps.min_rx_gain, (double)caps.max_rx_gain);
    }

    /* TX */
    if (direction == SOAPY_SDR_TX)
        return(SoapySDR::Range(-89, 0));

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
    struct m2sdr_rfic_caps caps;

    if (getLiteXM2SDRRficCaps(_dev, &caps)) {
        if (direction == SOAPY_SDR_TX) {
            if (name == "ATT")
                return SoapySDR::Range((double)(-caps.max_tx_gain), (double)(-caps.min_tx_gain));
            if (name == "GAIN")
                return SoapySDR::Range((double)caps.min_tx_gain, (double)caps.max_tx_gain);
        }
        if (direction == SOAPY_SDR_RX && (name == "PGA" || name == "RF" || name == "GAIN"))
            return SoapySDR::Range((double)caps.min_rx_gain, (double)caps.max_rx_gain);
    }

    /* TX */
    if (direction == SOAPY_SDR_TX) {
        if (name == "ATT")
            return(SoapySDR::Range(0, 89));
        if (name == "GAIN")
            return(SoapySDR::Range(-89, 0));
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

    if (direction == SOAPY_SDR_TX) {
        int rc = m2sdr_set_frequency(_dev, M2SDR_TX, lo_freq);
        if (rc != 0) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_frequency(TX) failed: %s", m2sdr_strerror(rc));
        }
    }
    if (direction == SOAPY_SDR_RX) {
        int rc = m2sdr_set_frequency(_dev, M2SDR_RX, lo_freq);
        if (rc != 0) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_frequency(RX) failed: %s", m2sdr_strerror(rc));
        }
    }
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
    const enum m2sdr_direction mdir = (direction == SOAPY_SDR_TX) ? M2SDR_TX : M2SDR_RX;

    if (m2sdr_get_frequency(_dev, mdir, &lo_freq) != 0)
        return 0.0;

    return static_cast<double>(lo_freq);
}

std::vector<std::string> SoapyLiteXM2SDR::listFrequencies(
    const int /*direction*/,
    const size_t /*channel*/) const {
    std::vector<std::string> opts;
    opts.push_back("RF");
    opts.push_back("BB");
    return opts;
}

SoapySDR::RangeList SoapyLiteXM2SDR::getFrequencyRange(
    const int direction,
    const size_t /*channel*/,
    const std::string &name) const {
    struct m2sdr_rfic_caps caps;

    if (name == "BB") {
        return(SoapySDR::RangeList(1, SoapySDR::Range(0, 0)));
    }

    if (getLiteXM2SDRRficCaps(_dev, &caps)) {
        if (direction == SOAPY_SDR_TX)
            return SoapySDR::RangeList(1, SoapySDR::Range((double)caps.min_tx_frequency, (double)caps.max_tx_frequency));
        if (direction == SOAPY_SDR_RX)
            return SoapySDR::RangeList(1, SoapySDR::Range((double)caps.min_rx_frequency, (double)caps.max_rx_frequency));
    }

    if (direction == SOAPY_SDR_TX)
        return(SoapySDR::RangeList(1, SoapySDR::Range(47000000, 6000000000ull)));
    if (direction == SOAPY_SDR_RX)
        return(SoapySDR::RangeList(1, SoapySDR::Range(70000000, 6000000000ull)));

    return(SoapySDR::RangeList(1, SoapySDR::Range(0, 0)));
}

/***************************************************************************************************
 *                                        Sample Rate API
 **************************************************************************************************/

/* listFrequencies now exposes RF and BB; BB currently aliases RF. */

void SoapyLiteXM2SDR::setSampleMode() {
    int rc = 0;

    _bitMode = (_iqBits <= 8u) ? 8u : 16u;

    /* 8-bit mode */
    if (_bitMode == 8) {
        _bytesPerSample  = 1;
        _bytesPerComplex = 2;
        _samplesScaling  = 127.0; /* Normalize 8-bit ADC values to [-1.0, 1.0]. */
        rc = m2sdr_set_iq_bits(_dev, 8u);
    /* 16-bit mode */
    } else {
        _bytesPerSample  = 2;
        _bytesPerComplex = 4;
        if (_iqBits > 1u && _iqBits <= 16u)
            _samplesScaling = static_cast<float>((1u << (_iqBits - 1u)) - 1u);
        else
            _samplesScaling = 2047.0f;
        rc = m2sdr_set_iq_bits(_dev, _iqBits);
    }

    if (rc != 0) {
        throw std::runtime_error(
            "Failed to set IQ bit depth " + std::to_string(_iqBits) +
            " (" + std::string(m2sdr_strerror(rc)) + ")");
    }
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
        if (_iqBits != 8u) {
            SoapySDR::logf(SOAPY_SDR_WARNING,
                "Sample rate %.2f MSPS requires 8-bit IQ + oversampling on PCIe, overriding iq_bits",
                rate / 1e6);
        }
        _iqBits       = 8u;
        _oversampling = 1;
    } else {
        _oversampling = 0;
    }
#elif USE_LITEETH
    /* For Ethernet, if the sample rate is above 20 MSPS, switch to 8-bit mode. */
    if (rate > LITEETH_8BIT_THRESHOLD) {
        _iqBits       = 8u;
        _oversampling = 0;
    } else {
        if (_iqBits <= 8u)
            _iqBits = 12u;
        _oversampling = 0;
    }
#endif

    /* If oversampling is enabled, double the rate multiplier. */
    if (_oversampling) {
        _rateMult = 2.0;
    }

    /* RFIC-specific FIR and oversampling policy live in the backend. Soapy only
     * sets the requested policy and the common sample rate here. */
    if (_rficName == "ad9361") {
        int prop_rc = m2sdr_set_property(_dev, "ad9361.fir_profile", _ad9361_fir_profile.c_str());

        if (prop_rc != 0) {
            throw std::runtime_error(
                "Failed to set AD9361 FIR profile (" + std::string(m2sdr_strerror(prop_rc)) + ")");
        }
        /* oversampling is requested here as policy; the backend decides how it
         * maps to FIR decimation/interpolation and hardware sample clocks. */
        prop_rc = m2sdr_set_property(_dev, "ad9361.oversampling", _oversampling ? "1" : "0");
        if (prop_rc != 0) {
            throw std::runtime_error(
                "Failed to set AD9361 oversampling policy (" + std::string(m2sdr_strerror(prop_rc)) + ")");
        }
    }

    /* Set the sample rate for the TX and configure the hardware accordingly. */
    if (direction == SOAPY_SDR_TX) {
        _tx_stream.samplerate = rate;
    }
    if (direction == SOAPY_SDR_RX) {
        _rx_stream.samplerate = rate;
    }
    {
        int rc = m2sdr_set_sample_rate(_dev, (int64_t)(sample_rate/_rateMult));
        if (rc != 0) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_sample_rate failed: %s", m2sdr_strerror(rc));
        }
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
    int64_t sample_rate = 0;
    (void)direction;
    if (m2sdr_get_sample_rate(_dev, &sample_rate) != 0)
        return 0.0;
    return static_cast<double>(_rateMult * sample_rate);
}

std::vector<double> SoapyLiteXM2SDR::listSampleRates(
    const int /*direction*/,
    const size_t /*channel*/) const {
    struct m2sdr_rfic_caps caps;
    std::vector<double> sampleRates;
    std::vector<double> filtered;

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

    if (getLiteXM2SDRRficCaps(_dev, &caps)) {
        for (double rate : sampleRates) {
            if (rate >= (double)caps.min_sample_rate && rate <= (double)caps.max_sample_rate)
                filtered.push_back(rate);
        }
        if (!filtered.empty())
            return filtered;
        filtered.push_back((double)caps.min_sample_rate);
        if (caps.max_sample_rate != caps.min_sample_rate)
            filtered.push_back((double)caps.max_sample_rate);
        return filtered;
    }

    /* Return supported SampleRates */
    return sampleRates;
}

SoapySDR::RangeList SoapyLiteXM2SDR::getSampleRateRange(
    const int /*direction*/,
    const size_t  /*channel*/) const {
    SoapySDR::RangeList results;
    struct m2sdr_rfic_caps caps;

    if (getLiteXM2SDRRficCaps(_dev, &caps)) {
        results.push_back(SoapySDR::Range((double)caps.min_sample_rate, (double)caps.max_sample_rate));
        return results;
    }
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

    int rc = m2sdr_set_bandwidth(_dev, bwi);
    if (rc != 0) {
        SoapySDR::logf(SOAPY_SDR_ERROR, "m2sdr_set_bandwidth failed: %s", m2sdr_strerror(rc));
    }
}

double SoapyLiteXM2SDR::getBandwidth(
    const int direction,
    const size_t /*channel*/) const {
    int64_t bw = 0;
    (void)direction;
    if (m2sdr_get_bandwidth(_dev, &bw) != 0)
        return 0.0;
    return static_cast<double>(bw);
}

SoapySDR::RangeList SoapyLiteXM2SDR::getBandwidthRange(
    const int /*direction*/,
    const size_t /*channel*/) const {
    SoapySDR::RangeList results;
    struct m2sdr_rfic_caps caps;

    if (getLiteXM2SDRRficCaps(_dev, &caps)) {
        results.push_back(SoapySDR::Range((double)caps.min_bandwidth, (double)caps.max_bandwidth));
        return results;
    }
    results.push_back(SoapySDR::Range(0.2e6, 56.0e6));
    return results;
}

/***************************************************************************************************
 *                                   Clocking API
 **************************************************************************************************/

/***************************************************************************************************
 *                                    Time API
 **************************************************************************************************/

bool SoapyLiteXM2SDR::hasHardwareTime(const std::string &) const {
    return true;
}

long long SoapyLiteXM2SDR::getHardwareTime(const std::string &) const
{
    uint32_t control_reg = 0;
    int64_t time_ns = 0;

    /* Latch the 64-bit Time (ns) by pulsing READ bit of Control Register. */
    control_reg = litex_m2sdr_readl(_dev, CSR_TIME_GEN_CONTROL_ADDR);
    control_reg |= (1 << CSR_TIME_GEN_CONTROL_READ_OFFSET);
    litex_m2sdr_writel(_dev, CSR_TIME_GEN_CONTROL_ADDR, control_reg);
    control_reg = (1 << CSR_TIME_GEN_CONTROL_ENABLE_OFFSET);
    litex_m2sdr_writel(_dev, CSR_TIME_GEN_CONTROL_ADDR, control_reg);

    /* Read the upper/lower 32 bits of the 64-bit Time (ns). */
    time_ns |= (static_cast<int64_t>(litex_m2sdr_readl(_dev, CSR_TIME_GEN_READ_TIME_ADDR + 0)) << 32);
    time_ns |= (static_cast<int64_t>(litex_m2sdr_readl(_dev, CSR_TIME_GEN_READ_TIME_ADDR + 4)) <<  0);

    /* Debug log the hardware time in nanoseconds. */
    SoapySDR::logf(SOAPY_SDR_DEBUG, "Hardware time (ns): %lld", (long long)time_ns);

    return static_cast<long long>(time_ns);
}

void SoapyLiteXM2SDR::setHardwareTime(const long long timeNs, const std::string &)
{
    uint32_t control_reg = 0;

    /* Write the 64-bit Time (ns). */
    litex_m2sdr_writel(_dev, CSR_TIME_GEN_WRITE_TIME_ADDR + 0, static_cast<uint32_t>((timeNs >> 32) & 0xffffffff));
    litex_m2sdr_writel(_dev, CSR_TIME_GEN_WRITE_TIME_ADDR + 4, static_cast<uint32_t>((timeNs >>  0) & 0xffffffff));

    /* Pulse the WRITE bit Control Register. */
    control_reg = litex_m2sdr_readl(_dev, CSR_TIME_GEN_CONTROL_ADDR);
    control_reg |= (1 << CSR_TIME_GEN_CONTROL_WRITE_OFFSET);
    litex_m2sdr_writel(_dev, CSR_TIME_GEN_CONTROL_ADDR, control_reg);
    control_reg = (1 << CSR_TIME_GEN_CONTROL_ENABLE_OFFSET);
    litex_m2sdr_writel(_dev, CSR_TIME_GEN_CONTROL_ADDR, control_reg);

    /* Optional debug log. */
    SoapySDR::logf(SOAPY_SDR_DEBUG, "Hardware time set to (ns): %lld", (long long)timeNs);
}

/***************************************************************************************************
 *                                    Sensors API
 **************************************************************************************************/

std::vector<std::string> SoapyLiteXM2SDR::listSensors(void) const {
    std::vector<std::string> sensors;
    struct m2sdr_rfic_caps caps;
#ifdef CSR_XADC_BASE
    sensors.push_back("fpga_temp");
    sensors.push_back("fpga_vccint");
    sensors.push_back("fpga_vccaux");
    sensors.push_back("fpga_vccbram");
#endif
    if (getLiteXM2SDRRficCaps(_dev, &caps) &&
        (caps.features & M2SDR_RFIC_FEATURE_TEMP_SENSOR) != 0)
        sensors.push_back("rfic_temp");
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
        /* RFIC Sensors */
        if (deviceStr == "rfic") {
            /* Temp. */
            if (sensorStr == "temp") {
                info.key         = "temp";
                info.value       = "0.0";
                info.units       = "°C";
                info.description = "RFIC temperature";
                info.type        = SoapySDR::ArgInfo::FLOAT;
            } else {
                throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown sensor");
            }
            return info;
        }

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
        /* RFIC Sensors */
        if (deviceStr == "rfic") {
            /* Temp. */
            if (sensorStr == "temp") {
                /* Sensor naming is generic on the Soapy side, but the backend
                 * property key stays vendor-namespaced. */
                std::string property_key = _rficName + ".temperature_c";
                char value[32];
                if (m2sdr_get_property(_dev, property_key.c_str(), value, sizeof(value)) != 0)
                    throw std::runtime_error("SoapyLiteXM2SDR::readSensor(" + key + ") failed");
                sensorValue = value;
            } else {
                throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown sensor");
            }
            return sensorValue;
        }

        throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown device");
    }
    throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown key");
}
