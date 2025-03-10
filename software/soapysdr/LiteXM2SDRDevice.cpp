/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2025 Enjoy Digital.
 * SPDX-License-Identifier: Apache-2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <fcntl.h>
#include <unistd.h>
#include <memory>
#include <sys/mman.h>
#include <arpa/inet.h>

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

#if USE_LITEPCIE
static int spi_fd;
#elif USE_LITEETH
static struct eb_connection *eb_fd;
#endif

//#define AD9361_SPI_WRITE_DEBUG
//#define AD9361_SPI_READ_DEBUG

int spi_write_then_read(struct spi_device * /*spi*/,
                        const unsigned char *txbuf, unsigned n_tx,
                        unsigned char *rxbuf, unsigned n_rx)
{

    /* Single Byte Read. */
    if (n_tx == 2 && n_rx == 1) {
#if USE_LITEPCIE
        rxbuf[0] = m2sdr_ad9361_spi_read(spi_fd, txbuf[0] << 8 | txbuf[1]);
#elif USE_LITEETH
        rxbuf[0] = m2sdr_ad9361_eb_spi_read(eb_fd, txbuf[0] << 8 | txbuf[1]);
#endif

    /* Single Byte Write. */
    } else if (n_tx == 3 && n_rx == 0) {
#if USE_LITEPCIE
        m2sdr_ad9361_spi_write(spi_fd, txbuf[0] << 8 | txbuf[1], txbuf[2]);
#else
        m2sdr_ad9361_eb_spi_write(eb_fd, txbuf[0] << 8 | txbuf[1], txbuf[2]);
#endif

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

std::string getLiteXM2SDRSerial(litex_m2sdr_device_desc_t fd);
std::string getLiteXM2SDRIdentification(litex_m2sdr_device_desc_t fd);

#if USE_LITEPCIE
void dma_set_loopback(int fd, bool loopback_enable) {
    struct litepcie_ioctl_dma m;
    m.loopback_enable = loopback_enable ? 1 : 0;
    checked_ioctl(fd, LITEPCIE_IOCTL_DMA, &m);
}
#endif

SoapyLiteXM2SDR::SoapyLiteXM2SDR(const SoapySDR::Kwargs &args)
    : _deviceArgs(args), _rx_buf_size(0), _tx_buf_size(0), _rx_buf_count(0), _tx_buf_count(0),
    _rx_udp_receiver(NULL),
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
    _fd = open(path.c_str(), O_RDWR);
    if (_fd < 0)
        throw std::runtime_error("SoapyLiteXM2SDR(): failed to open " + path);
    /* Global file descriptor for AD9361 lib. */
    spi_fd = _fd;

    SoapySDR::logf(SOAPY_SDR_INFO, "Opened devnode %s, serial %s", path.c_str(), getLiteXM2SDRSerial(_fd).c_str());
#elif USE_LITEETH
    /* Prepare EtherBone / Ethernet streamer */
    std::string eth_ip;
    if (args.count("eth_ip") == 0)
        eth_ip = "192.168.1.50";
    else
        eth_ip = args.at("eth_ip");

    /* EtherBone */
    _fd = eb_connect(eth_ip.c_str(), "1234", 1);
    if (!_fd)
        throw std::runtime_error("Can't connect to EtherBone!");
    eb_fd = _fd;

    /* Ethernet streamer */
    try {
        _rx_udp_receiver = new LiteXM2SDRUPDRx(eth_ip, "2345", 0, 20, 1024/8, 8);
    } catch (std::exception &e) {
        throw std::runtime_error("can't prepare UDP RX Receiver");
    }

    SoapySDR::logf(SOAPY_SDR_INFO, "Opened devnode %s, serial %s", eth_ip.c_str(), getLiteXM2SDRSerial(_fd).c_str());

    /* Ethernet FPGA streamer configuration */

    /* Determine the local IP that the board sees */
    std::string local_ip = getLocalIPAddressToReach(eth_ip, 1234);

    struct in_addr ip_addr_struct;
    if (inet_pton(AF_INET, local_ip.c_str(), &ip_addr_struct) != 1) {
        throw std::runtime_error("Invalid local IP address determined");
    }

    uint32_t ip_addr_val = ntohl(ip_addr_struct.s_addr);

    /* Write the PC's IP to the FPGA's ETH_STREAMER IP register */
    litex_m2sdr_writel(_fd, CSR_ETH_RX_STREAMER_IP_ADDRESS_ADDR, ip_addr_val);

    SoapySDR::logf(SOAPY_SDR_INFO, "Using local IP: %s for streaming", local_ip.c_str());
#endif

    /* Configure Mode based on _bitMode */
    if (_bitMode == 8) {
        litex_m2sdr_writel(_fd, CSR_AD9361_BITMODE_ADDR, 1); /* 8-bit mode */
    } else {
        litex_m2sdr_writel(_fd, CSR_AD9361_BITMODE_ADDR, 0); /* 16-bit mode */
    }


    /* Configure PCIe Synchronizer and DMA Headers. */
#if USE_LITEPCIE
    /* Enable Synchronizer */
    litex_m2sdr_writel(_fd, CSR_PCIE_DMA0_SYNCHRONIZER_BYPASS_ADDR, 0);

    /* DMA RX Header */
    #if defined(_RX_DMA_HEADER_TEST)
        /* Enable */
        litex_m2sdr_writel(_fd, CSR_HEADER_RX_CONTROL_ADDR,
           (1 << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
           (1 << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET)
        );
    #else
        /* Disable */
        litex_m2sdr_writel(_fd, CSR_HEADER_RX_CONTROL_ADDR,
           (1 << CSR_HEADER_RX_CONTROL_ENABLE_OFFSET) |
           (0 << CSR_HEADER_RX_CONTROL_HEADER_ENABLE_OFFSET)
        );
    #endif

    /* DMA TX Header */
    #if defined(_TX_DMA_HEADER_TEST)
        /* Enable */
        litex_m2sdr_writel(_fd, CSR_HEADER_TX_CONTROL_ADDR,
           (1 << CSR_HEADER_TX_CONTROL_ENABLE_OFFSET) |
           (1 << CSR_HEADER_TX_CONTROL_HEADER_ENABLE_OFFSET)
        );
    #else
        /* Disable */
        litex_m2sdr_writel(_fd, CSR_HEADER_TX_CONTROL_ADDR,
           (1 << CSR_HEADER_TX_CONTROL_ENABLE_OFFSET) |
           (0 << CSR_HEADER_TX_CONTROL_HEADER_ENABLE_OFFSET)
        );
    #endif

    /* Disable DMA Loopback. */
    litex_m2sdr_writel(_fd, CSR_PCIE_DMA0_LOOPBACK_ENABLE_ADDR, 0);
#endif

    bool do_init = true;
    if (args.count("bypass_init") > 0) {
        std::cout << args.at("bypass_init")[0] << std::endl;
        do_init = args.at("bypass_init")[0] == '0';
    }

    if (args.count("bitmode") > 0) {
        _bitMode = std::stoi(args.at("bitmode"));
    }

    if (args.count("oversampling") > 0) {
        _oversampling = std::stoi(args.at("oversampling"));
    }

    if (do_init) {
#if USE_LITEPCIE
        /* Initialize SI531 Clocking. */
        m2sdr_si5351_i2c_config(_fd, SI5351_I2C_ADDR, si5351_xo_config, sizeof(si5351_xo_config)/sizeof(si5351_xo_config[0]));

        /* Initialize AD9361 SPI. */
        m2sdr_ad9361_spi_init(_fd, 1);
#else
        m2sdr_ad9361_eb_spi_init(_fd, 1);
#endif
    }

    /* Initialize AD9361 RFIC. */
    default_init_param.gpio_resetb  = AD9361_GPIO_RESET_PIN;
    default_init_param.gpio_sync    = -1;
    default_init_param.gpio_cal_sw1 = -1;
    default_init_param.gpio_cal_sw2 = -1;
    ad9361_init(&ad9361_phy, &default_init_param, do_init);

    if (do_init) {
        /* Configure AD9361 TX/RX FIRs. */
        ad9361_set_tx_fir_config(ad9361_phy, tx_fir_config);
        ad9361_set_rx_fir_config(ad9361_phy, rx_fir_config);

        /* Some defaults to avoid throwing. */

        this->setClockSource("internal");

        /* Common for TX1/TX2 and RX1/RX2 */
        _rx_stream.samplerate   = 30.72e6;
        _tx_stream.samplerate   = 30.72e6;
        _rx_stream.frequency    = 1e6;
        _tx_stream.frequency    = 1e6;
        _rx_stream.bandwidth    = 30.72e6;
        _tx_stream.bandwidth    = 30.72e6;

        /* TX1/RX1. */
        _rx_stream.antenna[0]   = "A_BALANCED";
        _tx_stream.antenna[0]   = "A";
        _rx_stream.gainMode[0]  = false;
        _rx_stream.gain[0]      = 0;
        _tx_stream.gain[0]      = -20;
        _rx_stream.iqbalance[0] = 1.0;
        _tx_stream.iqbalance[0] = 1.0;
        channel_configure(SOAPY_SDR_RX, 0);
        channel_configure(SOAPY_SDR_TX, 0);

        /* TX2/RX2. */
        _rx_stream.antenna[1]   = "A_BALANCED";
        _tx_stream.antenna[1]   = "A";
        _rx_stream.gainMode[1]  = false;
        _rx_stream.gain[1]      = 0;
        _tx_stream.gain[1]      = -20;
        _rx_stream.iqbalance[1] = 1.0;
        _tx_stream.iqbalance[1] = 1.0;
        channel_configure(SOAPY_SDR_RX, 1);
        channel_configure(SOAPY_SDR_TX, 1);
    }

#if USE_LITEPCIE
    /* Set-up the DMA. */
    checked_ioctl(_fd, LITEPCIE_IOCTL_MMAP_DMA_INFO, &_dma_mmap_info);
    _dma_buf = NULL;
#endif

    SoapySDR::log(SOAPY_SDR_INFO, "SoapyLiteXM2SDR initialization complete");
}

SoapyLiteXM2SDR::~SoapyLiteXM2SDR(void) {
    SoapySDR::log(SOAPY_SDR_INFO, "Power down and cleanup");
    if (_rx_stream.opened) {
#if USE_LITEPCIE
        litepcie_release_dma(_fd, 0, 1);

        munmap(_rx_stream.buf,
               _dma_mmap_info.dma_rx_buf_size * _dma_mmap_info.dma_rx_buf_count);
#elif USE_LITEETH
        _rx_udp_receiver->stop();
#endif
        _rx_stream.opened = false;
    }

#if USE_LITEPCIE
    if (_tx_stream.opened) {
        /* Release the DMA engine. */
        litepcie_release_dma(_fd, 1, 0);

        munmap(_tx_stream.buf,
               _dma_mmap_info.dma_tx_buf_size * _dma_mmap_info.dma_tx_buf_count);
        _tx_stream.opened = false;
    }
#endif

    /* Crossbar Demux: Select PCIe streaming */
    litex_m2sdr_writel(_fd, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);

#if USE_LITEPCIE
    close(_fd);
#elif USE_LITEETH
    if (_rx_udp_receiver) {
        delete _rx_udp_receiver;
        _rx_udp_receiver = NULL;
    }
    if (_fd) {
        eb_disconnect(&_fd);
        _fd = NULL;
    }
#endif
}

/***************************************************************************************************
 *                                 Channel configuration
 **************************************************************************************************/

void SoapyLiteXM2SDR::channel_configure(const int direction, const size_t channel) {
    if (direction == SOAPY_SDR_TX) {
        this->setSampleRate(SOAPY_SDR_TX, channel, _tx_stream.samplerate);
        this->setAntenna(SOAPY_SDR_TX,    channel, _tx_stream.antenna[channel]);
        this->setFrequency(SOAPY_SDR_TX,  channel, "BB", _tx_stream.frequency);
        this->setBandwidth(SOAPY_SDR_TX,  channel, _tx_stream.bandwidth);
        this->setGain(SOAPY_SDR_TX,       channel, _tx_stream.gain[channel]);
        this->setIQBalance(SOAPY_SDR_TX,  channel, _tx_stream.iqbalance[channel]);
    }
    if (direction == SOAPY_SDR_RX) {
        this->setSampleRate(SOAPY_SDR_RX, channel, _rx_stream.samplerate);
        this->setAntenna(SOAPY_SDR_RX,    channel, _rx_stream.antenna[channel]);
        this->setFrequency(SOAPY_SDR_RX,  channel, "BB", _rx_stream.frequency);
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
    return "R01";
}

/***************************************************************************************************
*                                     Channel API
***************************************************************************************************/

size_t SoapyLiteXM2SDR::getNumChannels(const int) const {
    return this->_nChannels;
}

bool SoapyLiteXM2SDR::getFullDuplex(const int, const size_t) const {
    return true;
}

/***************************************************************************************************
 *                                     Antenna API
 **************************************************************************************************/

/* FIXME: Correctly handle A/B Antennas. */

std::vector<std::string> SoapyLiteXM2SDR::listAntennas(
    const int direction,
    const size_t) const {
    std::vector<std::string> ants;
    if(direction == SOAPY_SDR_RX) ants.push_back( "A_BALANCED" );
    if(direction == SOAPY_SDR_TX) ants.push_back( "A" );
    return ants;
}

void SoapyLiteXM2SDR::setAntenna(
    const int direction,
    const size_t channel,
    const std::string &name) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (direction == SOAPY_SDR_RX)
        _rx_stream.antenna[channel] = name;
    if (direction == SOAPY_SDR_TX)
        _tx_stream.antenna[channel] = name;
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
    const int /*direction*/,
    const size_t) const {
    std::vector<std::string> gains;
    gains.push_back("PGA");
    return gains;
}

bool SoapyLiteXM2SDR::hasGainMode(
    const int direction,
    const size_t /*channel*/) const {
    if (direction == SOAPY_SDR_TX)
        return false;
    if (direction == SOAPY_SDR_RX)
        return true;
    return false;
}

void SoapyLiteXM2SDR::setGainMode(const int direction, const size_t channel,
    const bool automatic)
{
    std::lock_guard<std::mutex> lock(_mutex);
    /* N/A. */
    if (direction == SOAPY_SDR_TX)
        return;

    /* FIXME: AGC gain mode. */
    _rx_stream.gainMode[channel] = automatic;
    ad9361_set_rx_gain_control_mode(ad9361_phy, channel,
        (automatic ? RF_GAIN_SLOWATTACK_AGC : RF_GAIN_MGC));
}

bool SoapyLiteXM2SDR::getGainMode(const int direction, const size_t channel) const
{
    if (direction == SOAPY_SDR_TX)
        return false;
    if (direction == SOAPY_SDR_RX) {
        uint8_t gc_mode;
        ad9361_get_rx_gain_control_mode(ad9361_phy, channel, &gc_mode);
        return (gc_mode != RF_GAIN_MGC);
    }
    return false;
}

void SoapyLiteXM2SDR::setGain(
    int direction,
    size_t channel,
    const double value) {
    std::lock_guard<std::mutex> lock(_mutex);
    SoapySDR::logf(SOAPY_SDR_DEBUG,
        "SoapyLiteXM2SDR::setGain(%s, ch%d, %f dB)",
        dir2Str(direction),
        channel,
        value);

    if (SOAPY_SDR_TX == direction) {
        _tx_stream.gain[channel] = value;
        uint32_t atten = static_cast<uint32_t>(-value * 1000);
        ad9361_set_tx_attenuation(ad9361_phy, channel, atten);
    }
    if (SOAPY_SDR_RX == direction) {
        _rx_stream.gain[channel] = value;
        ad9361_set_rx_rf_gain(ad9361_phy, channel, value);
    }
}

void SoapyLiteXM2SDR::setGain(
    const int direction,
    const size_t channel,
    const std::string &name,
    const double value) {
    SoapySDR::logf(SOAPY_SDR_DEBUG,
        "SoapyLiteXM2SDR::setGain(%s, ch%d, %s, %f dB)",
        dir2Str(direction),
        channel,
        name.c_str(),
        value);
    setGain(direction, channel, value);
}

double SoapyLiteXM2SDR::getGain(
    const int direction,
    const size_t channel) const
{
    int32_t gain = 0;
    if (direction == SOAPY_SDR_TX) {
        ad9361_get_tx_attenuation(ad9361_phy, channel, (uint32_t *) &gain);
        gain = -gain/1000;
    }
    if (direction == SOAPY_SDR_RX) {
        ad9361_get_rx_rf_gain(ad9361_phy, channel, &gain);
    }

    return static_cast<double>(gain);
}

double SoapyLiteXM2SDR::getGain(
    const int direction,
    const size_t channel,
    const std::string &/*name*/) const
{
    return getGain(direction, channel);
}

SoapySDR::Range SoapyLiteXM2SDR::getGainRange(
    const int direction,
    const size_t /*channel*/) const {

    if (direction == SOAPY_SDR_TX)
        return(SoapySDR::Range(-89, 0));

    if (direction == SOAPY_SDR_RX)
        return(SoapySDR::Range(0, 73));

    return(SoapySDR::Range(0,0));
}

SoapySDR::Range SoapyLiteXM2SDR::getGainRange(
    const int direction,
    const size_t channel,
    const std::string &/*name*/) const {

    return getGainRange(direction, channel);
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

    SoapySDR::logf(SOAPY_SDR_DEBUG,
        "SoapyLiteXM2SDR::setFrequency(%s, ch%d, %s, %f MHz)",
        dir2Str(direction),
        channel,
        name.c_str(),
        frequency / 1e6);
    _cachedFreqValues[direction][channel][name] = frequency;
    if (direction == SOAPY_SDR_TX)
        _tx_stream.frequency = frequency;
    if (direction == SOAPY_SDR_RX)
        _rx_stream.frequency = frequency;

    uint64_t lo_freq = static_cast<uint64_t>(frequency);

    if (direction == SOAPY_SDR_TX)
        ad9361_set_tx_lo_freq(ad9361_phy, lo_freq);

    if (direction == SOAPY_SDR_RX)
        ad9361_set_rx_lo_freq(ad9361_phy, lo_freq);
}

double SoapyLiteXM2SDR::getFrequency(
    const int direction,
    const size_t /*channel*/,
    const std::string &/*name*/) const {

    uint64_t lo_freq = 0;

    if (direction == SOAPY_SDR_TX)
        ad9361_get_tx_lo_freq(ad9361_phy, &lo_freq);

    if (direction == SOAPY_SDR_RX)
        ad9361_get_rx_lo_freq(ad9361_phy, &lo_freq);

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
    const std::string &/*name*/) const {

    if (direction == SOAPY_SDR_TX)
        return(SoapySDR::RangeList(1, SoapySDR::Range(47000000, 6000000000ull)));

    if (direction == SOAPY_SDR_RX)
        return(SoapySDR::RangeList(1, SoapySDR::Range(70000000, 6000000000ull)));

    return(SoapySDR::RangeList(1, SoapySDR::Range(0, 0)));
}

/***************************************************************************************************
 *                                        Sample Rate API
 **************************************************************************************************/

/* FIXME: Improve listFrequencies. */

void SoapyLiteXM2SDR::setSampleMode() {
    /* 8-bit mode */
    if (_bitMode == 8) {
        _bytesPerSample  = 1;
        _bytesPerComplex = 2;
        _samplesScaling  = 128.0; /* Normalize 8-bit ADC values to [-1.0, 1.0]. */
        litex_m2sdr_writel(_fd, CSR_AD9361_BITMODE_ADDR, 1);
    /* 16-bit mode */
    } else {
        _bytesPerSample  = 2;
        _bytesPerComplex = 4;
        _samplesScaling  = 2048.0; /* Normalize 12-bit ADC values to [-1.0, 1.0]. */
        litex_m2sdr_writel(_fd, CSR_AD9361_BITMODE_ADDR, 0);
    }
}

void SoapyLiteXM2SDR::setSampleRate(
    const int direction,
    const size_t channel,
    const double rate) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string dirName ((direction == SOAPY_SDR_RX) ? "Rx" : "Tx");

    /* Check if the requested sample rate is below 0.55 Msps and throw an exception if so */
    if (rate < 550000.0) {
        throw std::runtime_error("Sample rates below 0.55 Msps are not supported (limitation to be removed soon)");
    }

    SoapySDR::logf(SOAPY_SDR_DEBUG,
        "setSampleRate(%s, %ld, %g MHz)",
        dirName,
        channel,
        rate / 1e6);
    uint32_t sample_rate = static_cast<uint32_t>(rate);
    _rateMult = 1.0;

     /* If the requested rate is 122.88 MSPS, configure for 8-bit mode with oversampling enabled;
        otherwise, use 16-bit mode and disable oversampling. */
    if (rate == 122.88e6) {
        _bitMode = 8; /* FIXME: We could keep 16-bit when PCIe > Gen2 X1. */
        _oversampling = 1;
    } else {
        _bitMode = 16;
        _oversampling = 0;
    }
    /* If oversampling is enabled and the rate exceeds 61.44 MSPS, double the rate multiplier to
       account for oversampling. */
    if (_oversampling & (rate > 61.44e6))
        _rateMult = 2.0;

    /* Check and set FIR decimation/interpolation if actual rate is below 2.5 Msps */
    double actual_rate = rate / _rateMult;
    if (actual_rate < 2500000.0) {
        SoapySDR::logf(SOAPY_SDR_INFO, "Setting FIR decimation/interpolation to 4 for rate %f < 2.5 Msps", actual_rate);
        ad9361_phy->rx_fir_dec    = 4;
        ad9361_phy->tx_fir_int    = 4;
        ad9361_phy->bypass_rx_fir = 0;
        ad9361_phy->bypass_tx_fir = 0;
        AD9361_RXFIRConfig rx_fir_cfg = rx_fir_config;
        AD9361_TXFIRConfig tx_fir_cfg = tx_fir_config;
        rx_fir_cfg.rx_dec = 4;
        tx_fir_cfg.tx_int = 4;
        ad9361_set_rx_fir_config(ad9361_phy, rx_fir_cfg);
        ad9361_set_tx_fir_config(ad9361_phy, tx_fir_cfg);
        ad9361_set_rx_fir_en_dis(ad9361_phy, 1);
        ad9361_set_tx_fir_en_dis(ad9361_phy, 1);
    }

    /* Set the sample rate for the TX and configure the hardware accordingly. */
    if (direction == SOAPY_SDR_TX) {
        _tx_stream.samplerate = rate;
        ad9361_set_tx_sampling_freq(ad9361_phy, sample_rate/_rateMult);
    }

    /* Set the sample rate for the TX and configure the hardware accordingly. */
    if (direction == SOAPY_SDR_RX) {
        _rx_stream.samplerate = rate;
        ad9361_set_rx_sampling_freq(ad9361_phy, sample_rate/_rateMult);
    }

    /* If oversampling is enabled and the rate multiplier indicates oversampling, enable
       oversampling on the hardware. */
    if (_oversampling & (_rateMult == 2.0)) {
        ad9361_enable_oversampling(ad9361_phy);
    }

     /* Finally, update the sample mode (bit depth) based on the new configuration. */
    setSampleMode();
}

double SoapyLiteXM2SDR::getSampleRate(
    const int direction,
    const size_t) const {

    uint32_t sample_rate = 0;

    if (direction == SOAPY_SDR_TX)
        ad9361_get_tx_sampling_freq(ad9361_phy, &sample_rate);
    if (direction == SOAPY_SDR_RX)
        ad9361_get_rx_sampling_freq(ad9361_phy, &sample_rate);

    return static_cast<double>(_rateMult*sample_rate);
}

std::vector<double> SoapyLiteXM2SDR::listSampleRates(
    const int /*direction*/,
    const size_t /*channel*/) const {
    std::vector<double> sampleRates;

    /* Standard SampleRates */
    sampleRates.push_back(25e6 / 96); /* 260.42 KSPS (Minimum sample rate). */
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
    if (_oversampling)
        sampleRates.push_back(122.88e6);  /* 122.88 MSPS (LTE 80 MHz BW via 4x 20 MHz CA, 5G NR 100 MHz BW). */

    /* Return supported SampleRates */
    return sampleRates;
}

SoapySDR::RangeList SoapyLiteXM2SDR::getSampleRateRange(
    const int /*direction*/,
    const size_t  /*channel*/) const {
    SoapySDR::RangeList results;
    if (_oversampling)
        results.push_back(SoapySDR::Range(25e6 / 96, 122.88e6));
    else
        results.push_back(SoapySDR::Range(25e6 / 96, 61.44e6));
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

    if (direction == SOAPY_SDR_TX) {
        _tx_stream.bandwidth = bw;
        ad9361_set_tx_rf_bandwidth(ad9361_phy, bwi);
    }
    if (direction == SOAPY_SDR_RX) {
        _rx_stream.bandwidth = bw;
        ad9361_set_rx_rf_bandwidth(ad9361_phy, bwi);
    }
}

double SoapyLiteXM2SDR::getBandwidth(
    const int direction,
    const size_t /*channel*/) const {

    uint32_t bw = 0;

    if (direction == SOAPY_SDR_TX)
        ad9361_get_tx_rf_bandwidth(ad9361_phy, &bw);
    if (direction == SOAPY_SDR_RX)
        ad9361_get_rx_rf_bandwidth(ad9361_phy, &bw);

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
    control_reg = litex_m2sdr_readl(_fd, CSR_TIME_GEN_CONTROL_ADDR);
    control_reg |= (1 << CSR_TIME_GEN_CONTROL_READ_OFFSET);
    litex_m2sdr_writel(_fd, CSR_TIME_GEN_CONTROL_ADDR, control_reg);
    control_reg = (1 << CSR_TIME_GEN_CONTROL_ENABLE_OFFSET);
    litex_m2sdr_writel(_fd, CSR_TIME_GEN_CONTROL_ADDR, control_reg);

    /* Read the upper/lower 32 bits of the 64-bit Time (ns). */
    time_ns |= (static_cast<int64_t>(litex_m2sdr_readl(_fd, CSR_TIME_GEN_READ_TIME_ADDR + 0)) << 32);
    time_ns |= (static_cast<int64_t>(litex_m2sdr_readl(_fd, CSR_TIME_GEN_READ_TIME_ADDR + 4)) <<  0);

    /* Debug log the hardware time in nanoseconds. */
    SoapySDR::logf(SOAPY_SDR_DEBUG, "Hardware time (ns): %lld", (long long)time_ns);

    return static_cast<long long>(time_ns);
}

void SoapyLiteXM2SDR::setHardwareTime(const long long timeNs, const std::string &)
{
    uint32_t control_reg = 0;

    /* Write the 64-bit Time (ns). */
    litex_m2sdr_writel(_fd, CSR_TIME_GEN_WRITE_TIME_ADDR + 0, static_cast<uint32_t>((timeNs >> 32) & 0xffffffff));
    litex_m2sdr_writel(_fd, CSR_TIME_GEN_WRITE_TIME_ADDR + 4, static_cast<uint32_t>((timeNs >>  0) & 0xffffffff));

    /* Pulse the WRITE bit Control Register. */
    control_reg = litex_m2sdr_readl(_fd, CSR_TIME_GEN_CONTROL_ADDR);
    control_reg |= (1 << CSR_TIME_GEN_CONTROL_WRITE_OFFSET);
    litex_m2sdr_writel(_fd, CSR_TIME_GEN_CONTROL_ADDR, control_reg);
    control_reg = (1 << CSR_TIME_GEN_CONTROL_ENABLE_OFFSET);
    litex_m2sdr_writel(_fd, CSR_TIME_GEN_CONTROL_ADDR, control_reg);

    /* Optional debug log. */
    SoapySDR::logf(SOAPY_SDR_DEBUG, "Hardware time set to (ns): %lld", (long long)timeNs);
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
            /* Temp. */
            if (sensorStr == "temp") {
                sensorValue = std::to_string(
                    (double)litex_m2sdr_readl(_fd, CSR_XADC_TEMPERATURE_ADDR) * 503.975 / 4096 - 273.15
                );
            /* VCCINT. */
            } else if (sensorStr == "vccint") {
                sensorValue = std::to_string(
                    (double)litex_m2sdr_readl(_fd, CSR_XADC_VCCINT_ADDR) / 4096 * 3
                );
            /* VCCAUX. */
            } else if (sensorStr == "vccaux") {
                sensorValue = std::to_string(
                    (double)litex_m2sdr_readl(_fd, CSR_XADC_VCCAUX_ADDR) / 4096 * 3
                );
            /* VCCBRAM. */
            } else if (sensorStr == "vccbram") {
                sensorValue = std::to_string(
                    (double)litex_m2sdr_readl(_fd, CSR_XADC_VCCBRAM_ADDR) / 4096 * 3
                );
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

        throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown device");
    }
    throw std::runtime_error("SoapyLiteXM2SDR::getSensorInfo(" + key + ") unknown key");
}
