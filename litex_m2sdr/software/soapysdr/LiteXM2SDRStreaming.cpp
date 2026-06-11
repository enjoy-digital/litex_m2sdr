/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2026 Enjoy Digital.
 * Copyright (c) 2021 Julia Computing.
 * Copyright (c) 2015-2015 Fairwaves, Inc.
 * Copyright (c) 2015-2015 Rice University
 * SPDX-License-Identifier: Apache-2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <chrono>
#include <cassert>
#include <thread>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cctype>
#include <limits>

#include "ad9361/ad9361.h"
#include "ad9361/ad9361_api.h"

#include "LiteXM2SDRDevice.hpp"

#if USE_LITEETH
#include "liteeth_udp.h"
#endif

/* Parse Soapy/gr-osmosdr channel arguments:
 * "0", "1", "0,1", "[0,1]", "{0}", "{0,1}", "0 1". */
static std::vector<size_t> parse_channel_list(const std::string &channels_str)
{
    std::string s = channels_str;
    for (char &c : s) {
        if (c == '[' || c == ']' || c == '{' || c == '}')
            c = ' ';
        if (c == ',')
            c = ' ';
    }

    std::stringstream ss(s);
    std::vector<size_t> chans;
    std::string tok;
    while (ss >> tok) {
        char *end = nullptr;
        long v = std::strtol(tok.c_str(), &end, 10);
        if (!end || *end != '\0')
            throw std::runtime_error("Invalid channel token: " + tok);
        if (v < 0 || v > 1)
            throw std::runtime_error("Invalid channel index: must be 0 or 1");
        if (std::find(chans.begin(), chans.end(), static_cast<size_t>(v)) == chans.end())
            chans.push_back(static_cast<size_t>(v));
    }

    if (chans.empty())
        throw std::runtime_error("No channels parsed from: " + channels_str);
    if (chans.size() > 2)
        throw std::runtime_error("Invalid channel count: must be 1 or 2 channels");
    if (chans.size() == 2) {
        std::sort(chans.begin(), chans.end());
        if (!(chans[0] == 0 && chans[1] == 1))
            throw std::runtime_error("Dual channels must be 0 and 1");
    }
    return chans;
}

static inline int32_t arithmetic_shift_right(int32_t value, unsigned shift)
{
    if (value >= 0)
        return value >> shift;
    return -(((-value) + ((1 << shift) - 1)) >> shift);
}

static inline int8_t clamp_to_sc8(int32_t value)
{
    if (value > 127)
        return 127;
    if (value < -128)
        return -128;
    return static_cast<int8_t>(value);
}

static inline int8_t cf32_to_sc8(float sample, double scale)
{
    if (!std::isfinite(sample))
        return 0;
    const double scaled = sample * scale;
    if (scaled > 127.0)
        return 127;
    if (scaled < -128.0)
        return -128;
    return static_cast<int8_t>(std::lround(scaled));
}

static inline int8_t sc16_q11_to_sc8(int16_t sample)
{
    const int32_t offset = sample < 0 ? 7 : 8;
    return clamp_to_sc8(arithmetic_shift_right(static_cast<int32_t>(sample) + offset, 4));
}

static enum m2sdr_format soapy_stream_format_to_m2sdr(const std::string &format, uint32_t bit_mode);

static std::string get_kwargs_string(
    const SoapySDR::Kwargs &stream_args,
    const SoapySDR::Kwargs &device_args,
    const char *key,
    const char *fallback)
{
    auto it = stream_args.find(key);
    if (it != stream_args.end())
        return it->second;

    it = device_args.find(key);
    if (it != device_args.end())
        return it->second;

    return fallback;
}

static size_t get_kwargs_size(
    const SoapySDR::Kwargs &stream_args,
    const SoapySDR::Kwargs &device_args,
    const char *key,
    size_t fallback)
{
    const std::string fallback_value = std::to_string(fallback);
    const std::string value = get_kwargs_string(
        stream_args, device_args, key, fallback_value.c_str());

    return static_cast<size_t>(std::stoull(value));
}

static int get_kwargs_int(
    const SoapySDR::Kwargs &stream_args,
    const SoapySDR::Kwargs &device_args,
    const char *key,
    int fallback)
{
    const std::string fallback_value = std::to_string(fallback);
    const std::string value = get_kwargs_string(
        stream_args, device_args, key, fallback_value.c_str());

    return static_cast<int>(std::stol(value));
}

static long long get_kwargs_long_long(
    const SoapySDR::Kwargs &stream_args,
    const SoapySDR::Kwargs &device_args,
    const char *key,
    long long fallback)
{
    const std::string fallback_value = std::to_string(fallback);
    const std::string value = get_kwargs_string(
        stream_args, device_args, key, fallback_value.c_str());

    return std::stoll(value);
}

static bool has_kwargs_key(
    const SoapySDR::Kwargs &stream_args,
    const SoapySDR::Kwargs &device_args,
    const char *key)
{
    return stream_args.find(key) != stream_args.end() ||
           device_args.find(key) != device_args.end();
}

static bool get_kwargs_timed_tx_enabled(
    const SoapySDR::Kwargs &stream_args,
    const SoapySDR::Kwargs &device_args)
{
    std::string value = get_kwargs_string(stream_args, device_args, "timed_tx", "software");
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (value == "software" || value == "sw" || value == "on" ||
        value == "true" || value == "1") {
        return true;
    }
    if (value == "off" || value == "none" || value == "false" ||
        value == "0") {
        return false;
    }

    throw std::runtime_error("Invalid timed_tx: " + value + " (supported: software, off)");
}

#if USE_LITEETH
static constexpr uint64_t LITEETH_PACE_RESYNC_BUFFERS = 1024;
static constexpr size_t VRT_SIGNAL_HEADER_BYTES = 20;
static constexpr size_t VRT_RX_DATA_WORDS_DEFAULT = 256;
static constexpr size_t VRT_RX_PAYLOAD_BYTES_DEFAULT = VRT_RX_DATA_WORDS_DEFAULT * sizeof(uint32_t);
static constexpr size_t VRT_RX_PACKET_BYTES_DEFAULT = VRT_SIGNAL_HEADER_BYTES + VRT_RX_PAYLOAD_BYTES_DEFAULT;

static inline uint32_t read_be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
           (static_cast<uint32_t>(p[3]) <<  0);
}

static inline uint64_t read_be64(const uint8_t *p)
{
    return (static_cast<uint64_t>(read_be32(p + 0)) << 32) |
           (static_cast<uint64_t>(read_be32(p + 4)) <<  0);
}

static void log_liteeth_socket_buffer(const char *name, int requested, int actual)
{
    SoapySDR::logf(SOAPY_SDR_INFO,
        "LiteEth UDP %s: requested=%d actual=%d",
        name, requested, actual);
    if (requested > 0 && actual > 0 && actual < requested) {
        SoapySDR::logf(SOAPY_SDR_WARNING,
            "LiteEth UDP %s capped by Linux; increase net.core.%s_max for more headroom",
            name, std::strcmp(name, "SO_RCVBUF") == 0 ? "rmem" : "wmem");
    }
}

static uint16_t get_kwargs_port(
    const SoapySDR::Kwargs &stream_args,
    const SoapySDR::Kwargs &device_args,
    const char *key,
    uint16_t fallback)
{
    const std::string fallback_value = std::to_string(fallback);
    const std::string value = get_kwargs_string(
        stream_args, device_args, key, fallback_value.c_str());
    size_t end = 0;
    unsigned long port = std::stoul(value, &end, 10);

    if (end != value.size() || port == 0 || port > 65535)
        throw std::runtime_error("Invalid LiteEth UDP port: " + value);
    return static_cast<uint16_t>(port);
}

struct LiteEthUdpSettings {
    std::string log_name;
    std::string remote_ip;
    uint16_t port = 2345;
    bool rx_enable = true;
    bool tx_enable = true;
    size_t buffer_bytes = 0;
    size_t buffer_count = 0;
    int rcvbuf_bytes = 0;
    int sndbuf_bytes = 0;
};

static LiteEthUdpSettings make_liteeth_rx_udp_settings(
    const SoapySDR::Kwargs &stream_args,
    const SoapySDR::Kwargs &device_args,
    const std::string &eth_ip,
    bool is_vrt,
    size_t bytes_per_complex,
    size_t channel_count)
{
    LiteEthUdpSettings settings;

    settings.log_name = is_vrt ? "LiteEth VRT/UDP" : "LiteEth UDP";
    settings.remote_ip = get_kwargs_string(stream_args, device_args, "udp_ip", eth_ip.c_str());
    settings.port = is_vrt
        ? get_kwargs_port(stream_args, device_args, "vrt_port", 4991)
        : get_kwargs_port(stream_args, device_args, "udp_port", 2345);
    settings.tx_enable = !is_vrt;
    settings.buffer_bytes = is_vrt
        ? VRT_RX_PACKET_BYTES_DEFAULT
        : get_kwargs_size(stream_args, device_args, "udp_buf_complex", 4096) *
            bytes_per_complex * channel_count;
    settings.buffer_count = get_kwargs_size(stream_args, device_args, "udp_buf_count", 64);
    settings.rcvbuf_bytes = get_kwargs_int(stream_args, device_args, "udp_rcvbuf", 8 * 1024 * 1024);
    return settings;
}

static LiteEthUdpSettings make_liteeth_tx_udp_settings(
    const SoapySDR::Kwargs &stream_args,
    const SoapySDR::Kwargs &device_args,
    const std::string &eth_ip,
    size_t bytes_per_complex,
    size_t channel_count)
{
    LiteEthUdpSettings settings;

    settings.log_name = "LiteEth UDP";
    settings.remote_ip = get_kwargs_string(stream_args, device_args, "udp_ip", eth_ip.c_str());
    settings.port = get_kwargs_port(stream_args, device_args, "udp_port", 2345);
    settings.buffer_bytes = get_kwargs_size(stream_args, device_args, "udp_buf_complex", 4096) *
        bytes_per_complex * channel_count;
    settings.buffer_count = get_kwargs_size(stream_args, device_args, "udp_buf_count", 64);
    settings.sndbuf_bytes = get_kwargs_int(stream_args, device_args, "udp_sndbuf", 8 * 1024 * 1024);
    return settings;
}

static void init_liteeth_udp(
    struct liteeth_udp_ctrl *udp,
    const std::string &source_filter_ip,
    const LiteEthUdpSettings &settings)
{
    if (liteeth_udp_init(udp,
                         /*listen_ip*/  nullptr, /*listen_port*/  settings.port,
                         /*remote_ip*/  settings.remote_ip.c_str(), /*remote_port*/ settings.port,
                         /*rx_enable*/  settings.rx_enable ? 1 : 0,
                         /*tx_enable*/  settings.tx_enable ? 1 : 0,
                         /*buffer_size*/ settings.buffer_bytes,
                         /*buffer_count*/settings.buffer_count,
                         /*nonblock*/    0) < 0) {
        throw std::runtime_error("UDP init failed.");
    }
    if (liteeth_udp_set_rx_source_filter(udp, source_filter_ip.c_str(), 0) != 0) {
        throw std::runtime_error("LiteEth UDP RX source filter setup failed.");
    }
    if (settings.rcvbuf_bytes > 0 && liteeth_udp_set_so_rcvbuf(udp, settings.rcvbuf_bytes) != 0) {
        SoapySDR::logf(SOAPY_SDR_WARNING,
            "Failed to set LiteEth UDP SO_RCVBUF to %d bytes", settings.rcvbuf_bytes);
    }
    if (settings.sndbuf_bytes > 0 && liteeth_udp_set_so_sndbuf(udp, settings.sndbuf_bytes) != 0) {
        SoapySDR::logf(SOAPY_SDR_WARNING,
            "Failed to set LiteEth UDP SO_SNDBUF to %d bytes", settings.sndbuf_bytes);
    }

    int actual_rcvbuf_bytes = 0;
    if (settings.rcvbuf_bytes > 0 &&
        liteeth_udp_get_so_rcvbuf(udp, &actual_rcvbuf_bytes) == 0) {
        log_liteeth_socket_buffer("SO_RCVBUF", settings.rcvbuf_bytes, actual_rcvbuf_bytes);
    }
    int actual_sndbuf_bytes = 0;
    if (settings.sndbuf_bytes > 0 &&
        liteeth_udp_get_so_sndbuf(udp, &actual_sndbuf_bytes) == 0) {
        log_liteeth_socket_buffer("SO_SNDBUF", settings.sndbuf_bytes, actual_sndbuf_bytes);
    }

    SoapySDR::logf(SOAPY_SDR_INFO, "%s init: remote=%s:%u",
                   settings.log_name.c_str(),
                   settings.remote_ip.c_str(),
                   static_cast<unsigned>(settings.port));
}

struct m2sdr_liteeth_rx_stream_config SoapyLiteXM2SDR::makeLiteEthRxStreamConfig() const
{
    struct m2sdr_liteeth_rx_stream_config config;

    m2sdr_liteeth_rx_stream_config_init(&config);
    config.mode = (_eth_mode == SoapyLiteXM2SDREthernetMode::VRT)
        ? M2SDR_LITEETH_RX_MODE_VRT
        : M2SDR_LITEETH_RX_MODE_UDP;
    config.udp_port = _liteeth_rx_port;
    return config;
}
#endif

/* RX DMA headers are enabled at runtime when the gateware supports them
 * (see _rx_dma_header_bytes); they carry the hardware RX timestamps. */

/* TX DMA Header */
#if USE_LITEPCIE && defined(_TX_DMA_HEADER_TEST)
static constexpr size_t TX_DMA_HEADER_SIZE = 16;
#else
static constexpr size_t TX_DMA_HEADER_SIZE = 0;
#endif

/* Setup and configure a stream for RX or TX. */
SoapySDR::Stream *SoapyLiteXM2SDR::setupStream(
    const int direction,
    const std::string &format,
    const std::vector<size_t> &channels,
    const SoapySDR::Kwargs &args) {
    std::unique_lock<std::mutex> lock(_mutex);

    SoapySDR::Kwargs searchArgs = args;
    if (searchArgs.empty())
        searchArgs = _deviceArgs;

    /* Variable to hold the selected channels */
    std::vector<size_t> selected_channels;

    if (direction == SOAPY_SDR_RX) {
        if (_rx_stream.opened) {
            throw std::runtime_error("RX stream already opened.");
        }

        /* Determine channels: prioritize searchArgs over the provided vector */
        auto it = searchArgs.find("channels");
        if (it != searchArgs.end()) {
            selected_channels = parse_channel_list(it->second);
        } else if (!channels.empty()) {
            /* Use the provided channels vector if no device argument overrides it */
            selected_channels = channels;
        } else {
            /* Default to channel 0 if nothing provided */
            selected_channels = {0};
        }

        /* Validate selected channels */
        if (selected_channels.size() > 2) {
            throw std::runtime_error("Invalid RX channel count: must be 1 or 2 channels");
        }
        for (size_t chan : selected_channels) {
            if (chan > 1) {
                throw std::runtime_error("Invalid RX channel index: must be 0 (RX1) or 1 (RX2)");
            }
        }
        if (selected_channels.size() == 2 && (selected_channels[0] != 0 || selected_channels[1] != 1)) {
            throw std::runtime_error("Dual RX channels must be {0, 1} for RX1+RX2");
        }
        if (_tx_stream.opened && selected_channels.size() != _tx_stream.channels.size()) {
            throw std::runtime_error("RX/TX channel count mismatch; close TX or use matching channels");
        }

        if (isLitePCIe()) {
            if (format == SOAPY_SDR_CS8) {
                _bitMode = 8;
                _bitModeExplicit = true;
                setSampleMode();
            }

            enum m2sdr_format m2fmt = soapy_stream_format_to_m2sdr(format, _bitMode);
            m2sdr_stream_config_t config;
            struct m2sdr_stream_info info;

            m2sdr_stream_config_init(&config);
            config.direction = M2SDR_RX;
            config.format = m2fmt;
            config.zero_copy = true;
            config.rx_header_enable = _rx_dma_header_bytes != 0;
            config.rx_strip_header = _rx_dma_header_bytes != 0;
            config.buffer_size = m2sdr_bytes_to_samples(m2fmt, M2SDR_BUFFER_BYTES - _rx_dma_header_bytes);
            int rc = m2sdr_stream_configure(_dev, &config);
            if (rc != M2SDR_ERR_OK)
                throw std::runtime_error("m2sdr_stream_configure(RX) failed: " + std::string(m2sdr_strerror(rc)));
            rc = m2sdr_stream_get_info(_dev, M2SDR_RX, &info);
            if (rc != M2SDR_ERR_OK)
                throw std::runtime_error("m2sdr_stream_get_info(RX) failed: " + std::string(m2sdr_strerror(rc)));
            if (!info.buffer_base || !info.buffer_count || !info.buffer_stride)
                throw std::runtime_error("m2sdr_stream_get_info(RX) returned incomplete DMA info");

            _rx_stream.buf = info.buffer_base;
            _rx_buf_size   = info.buffer_bytes;
            _rx_buf_count  = info.buffer_count;
            _rx_buf_stride = info.buffer_stride;
        } else if (isLiteEth()) {
            /* Lazy-init UDP helper (enable RX+TX to mirror DMA symmetry). */
            const bool is_vrt = (_eth_mode == SoapyLiteXM2SDREthernetMode::VRT);
            const LiteEthUdpSettings udp_settings = make_liteeth_rx_udp_settings(
                searchArgs, _deviceArgs, _eth_ip, is_vrt,
                _bytesPerComplex, selected_channels.size());
            _liteeth_rx_port = udp_settings.port;
            if (!_udp_inited) {
                init_liteeth_udp(&_udp, _eth_ip, udp_settings);
                _udp_inited = true;
            } else {
                if (_nChannels && selected_channels.size() != _nChannels) {
                    throw std::runtime_error("LiteEth UDP buffers already initialized with a different channel count");
                }
            }
            {
                struct m2sdr_liteeth_rx_stream_config rx_config = makeLiteEthRxStreamConfig();
                int rc = m2sdr_liteeth_rx_stream_prepare(_dev, &rx_config);
                if (rc != M2SDR_ERR_OK) {
                    throw std::runtime_error(
                        "LiteEth RX stream prepare failed: " + std::string(m2sdr_strerror(rc)));
                }
            }

            _rx_buf_size  = (_eth_mode == SoapyLiteXM2SDREthernetMode::VRT)
                            ? VRT_RX_PAYLOAD_BYTES_DEFAULT
                            : _udp.buf_size;
            _rx_buf_count = _udp.buf_count;

            _rx_stream.buf = std::malloc(_rx_buf_size * _rx_buf_count);
            if (!_rx_stream.buf) {
                throw std::runtime_error("malloc() failed for RX staging buffer.");
            }
        } else {
            throw std::runtime_error("Unsupported RX transport");
        }

        _rx_stream.opened = true;
        _rx_stream.format = format;
        if (format == SOAPY_SDR_CS8) {
            _bitMode = 8;
            _bitModeExplicit = true;
            setSampleMode();
        }
        _rx_stream.remainderHandle = -1;
        _rx_stream.remainderSamps  = 0;
        _rx_stream.remainderOffset = 0;

        /* Log the selected RX channels for debugging */
        if (selected_channels.size() == 1) {
            SoapySDR_logf(SOAPY_SDR_INFO, "RX setupStream: Selected channel %zu", selected_channels[0]);
        } else {
            SoapySDR_logf(SOAPY_SDR_INFO, "RX setupStream: Selected channels %zu, %zu",
                          selected_channels[0], selected_channels[1]);
        }

        _rx_stream.channels = selected_channels;
        _nChannels = _rx_stream.channels.size();
        SoapySDR_logf(SOAPY_SDR_DEBUG,
            "RX setupStream: buf_size=%zu channels=%u bytes_per_complex=%u mtu=%zu",
            _rx_buf_size, _nChannels, _bytesPerComplex, this->getStreamMTU(RX_STREAM));
    } else if (direction == SOAPY_SDR_TX) {
        if (_tx_stream.opened) {
            throw std::runtime_error("TX stream already opened.");
        }

        /* Determine channels: override provided vector if searchArgs contains "channels" */
        auto it = searchArgs.find("channels");
        if (it != searchArgs.end()) {
            selected_channels = parse_channel_list(it->second);
        } else if (!channels.empty()) {
            /* Use the provided channels vector if no override is present */
            selected_channels = channels;
        } else {
            /* Default to TX1 if nothing is specified */
            selected_channels = {0};
        }

        /* Validate selected channels */
        if (selected_channels.size() > 2) {
            throw std::runtime_error("Invalid TX channel count: must be 1 or 2 channels");
        }
        for (size_t chan : selected_channels) {
            if (chan > 1) {
                throw std::runtime_error("Invalid TX channel index: must be 0 (TX1) or 1 (TX2)");
            }
        }
        if (selected_channels.size() == 2 && (selected_channels[0] != 0 || selected_channels[1] != 1)) {
            throw std::runtime_error("Dual TX channels must be {0, 1} for TX1+TX2");
        }
        if (_rx_stream.opened && selected_channels.size() != _rx_stream.channels.size()) {
            throw std::runtime_error("RX/TX channel count mismatch; close RX or use matching channels");
        }

        if (isLitePCIe()) {
            if (format == SOAPY_SDR_CS8) {
                _bitMode = 8;
                _bitModeExplicit = true;
                setSampleMode();
            }

            enum m2sdr_format m2fmt = soapy_stream_format_to_m2sdr(format, _bitMode);
            m2sdr_stream_config_t config;
            struct m2sdr_stream_info info;

            m2sdr_stream_config_init(&config);
            config.direction = M2SDR_TX;
            config.format = m2fmt;
            config.zero_copy = true;
            config.tx_header_enable = TX_DMA_HEADER_SIZE != 0;
            config.buffer_size = m2sdr_bytes_to_samples(m2fmt, M2SDR_BUFFER_BYTES - TX_DMA_HEADER_SIZE);
            int rc = m2sdr_stream_configure(_dev, &config);
            if (rc != M2SDR_ERR_OK)
                throw std::runtime_error("m2sdr_stream_configure(TX) failed: " + std::string(m2sdr_strerror(rc)));
            rc = m2sdr_stream_get_info(_dev, M2SDR_TX, &info);
            if (rc != M2SDR_ERR_OK)
                throw std::runtime_error("m2sdr_stream_get_info(TX) failed: " + std::string(m2sdr_strerror(rc)));
            if (!info.buffer_base || !info.buffer_count || !info.buffer_stride)
                throw std::runtime_error("m2sdr_stream_get_info(TX) returned incomplete DMA info");

            _tx_stream.buf = info.buffer_base;
            _tx_buf_size   = info.buffer_bytes;
            _tx_buf_count  = info.buffer_count;
            _tx_buf_stride = info.buffer_stride;
        } else if (isLiteEth()) {
            if (_eth_mode == SoapyLiteXM2SDREthernetMode::VRT) {
                throw std::runtime_error("Soapy TX streaming is not supported in eth_mode=vrt");
            }
            {
                const std::string tx_pacing = get_kwargs_string(searchArgs, _deviceArgs, "tx_pacing", "rate");
                if (tx_pacing == "rate") {
                    _tx_stream.rate_pacing = true;
                } else if (tx_pacing == "none") {
                    _tx_stream.rate_pacing = false;
                } else {
                    throw std::runtime_error("Invalid tx_pacing: " + tx_pacing + " (supported: rate, none)");
                }
                SoapySDR::logf(SOAPY_SDR_INFO, "LiteEth TX pacing: %s", tx_pacing.c_str());
            }
            if (!_udp_inited) {
                const LiteEthUdpSettings udp_settings = make_liteeth_tx_udp_settings(
                    searchArgs, _deviceArgs, _eth_ip,
                    _bytesPerComplex, selected_channels.size());
                init_liteeth_udp(&_udp, _eth_ip, udp_settings);
                _udp_inited = true;
            } else {
                if (_nChannels && selected_channels.size() != _nChannels) {
                    throw std::runtime_error("LiteEth UDP buffers already initialized with a different channel count");
                }
            }

            _tx_buf_size  = _udp.buf_size;
            _tx_buf_count = _udp.buf_count;

            _tx_stream.buf = std::malloc(_tx_buf_size * _tx_buf_count);
            if (!_tx_stream.buf) {
                throw std::runtime_error("malloc() failed for TX staging buffer.");
            }
        } else {
            throw std::runtime_error("Unsupported TX transport");
        }

        _tx_stream.opened = true;
        _tx_stream.format = format;
        if (format == SOAPY_SDR_CS8) {
            _bitMode = 8;
            _bitModeExplicit = true;
            setSampleMode();
        }
        _tx_stream.remainderHandle = -1;
        _tx_stream.remainderSamps  = 0;
        _tx_stream.remainderOffset = 0;
        _tx_stream.remainderFlags  = 0;
        _tx_stream.remainderTimeNs = 0;

        _tx_stream.channels = selected_channels;
        _nChannels = _tx_stream.channels.size();

        _tx_stream.timed_tx_enabled = get_kwargs_timed_tx_enabled(searchArgs, _deviceArgs);
        /* The hardware emits samples as soon as DMA delivers them, so any lead
         * shifts the actual emission that far ahead of the requested timestamps.
         * Applications that schedule writes in advance (srsRAN: ~4 subframes)
         * provide their own queue slack; lead stays available for apps that
         * write with timestamps close to "now" and accept the offset. */
        _tx_stream.timed_tx_lead_buffers =
            get_kwargs_size(searchArgs, _deviceArgs, "tx_lead_buffers", 0);
        _tx_stream.timed_tx_latency_ns =
            get_kwargs_long_long(searchArgs, _deviceArgs, "tx_latency_ns", 0);
        _tx_stream.timed_tx_late_margin_configured =
            has_kwargs_key(searchArgs, _deviceArgs, "tx_late_margin_ns");
        _tx_stream.timed_tx_late_margin_ns =
            get_kwargs_long_long(searchArgs, _deviceArgs, "tx_late_margin_ns", 0);
        if (_tx_stream.timed_tx_late_margin_ns < 0)
            throw std::runtime_error("tx_late_margin_ns must be non-negative");
        _tx_stream.tx_timeline_valid = false;
        _tx_stream.time_error = false;
        _tx_stream.status_time_ns = 0;
        refreshTimedTxDefaults();

        SoapySDR_logf(SOAPY_SDR_INFO,
            "TX timed mode: %s lead_buffers=%zu latency_ns=%lld late_margin_ns=%lld",
            _tx_stream.timed_tx_enabled ? "software" : "off",
            _tx_stream.timed_tx_lead_buffers,
            (long long)_tx_stream.timed_tx_latency_ns,
            (long long)_tx_stream.timed_tx_late_margin_ns);
    } else {
        throw std::runtime_error("Invalid direction.");
    }

    auto channelMask = [](const std::vector<size_t> &chans) {
        uint32_t mask = 0;
        for (size_t chan : chans)
            mask |= 1u << chan;
        return mask;
    };
    const uint32_t rx_channel_mask = _rx_stream.opened ? channelMask(_rx_stream.channels) : 0;
    const uint32_t tx_channel_mask = _tx_stream.opened ? channelMask(_tx_stream.channels) : 0;
    const unsigned rx_channel = (_rx_stream.opened && !_rx_stream.channels.empty()) ?
        static_cast<unsigned>(_rx_stream.channels[0]) : 0;
    const unsigned tx_channel = (_tx_stream.opened && !_tx_stream.channels.empty()) ?
        static_cast<unsigned>(_tx_stream.channels[0]) : 0;
    const bool channel_mode_changed = !_channelModeHwApplied ||
        _channelModeHw != _nChannels ||
        _rxChannelMaskHw != rx_channel_mask ||
        _txChannelMaskHw != tx_channel_mask;
    try {
        if (channel_mode_changed) {
            /* Re-runs the AD9361 bring-up, so active Soapy settings are
             * reapplied below; the phy handle changes across the re-init. */
            int rc = m2sdr_set_channel_mode(_dev, _nChannels, rx_channel, tx_channel);
            if (rc != M2SDR_ERR_OK)
                throw std::runtime_error("m2sdr_set_channel_mode failed: " + std::string(m2sdr_strerror(rc)));
            ad9361_phy = static_cast<struct ad9361_rf_phy *>(m2sdr_rf_phy(_dev));
            if (!ad9361_phy)
                throw std::runtime_error("m2sdr_set_channel_mode left no AD9361 phy bound");

            _channelModeHwApplied = true;
            _channelModeHw = _nChannels;
            _rxChannelMaskHw = rx_channel_mask;
            _txChannelMaskHw = tx_channel_mask;
            invalidateRfHardwareCache();
        }
    } catch (...) {
        /* setupStream() throwing means the caller gets no stream handle and
         * will not call closeStream(): roll back the stream opened by this
         * call so a retry is not rejected as "already opened". */
        Stream &fail_stream = (direction == SOAPY_SDR_RX)
            ? static_cast<Stream &>(_rx_stream)
            : static_cast<Stream &>(_tx_stream);
        if (isLitePCIe()) {
            (void)m2sdr_stream_release(_dev,
                direction == SOAPY_SDR_RX ? M2SDR_RX : M2SDR_TX);
        } else if (isLiteEth()) {
            std::free(fail_stream.buf);
        }
        fail_stream.buf = NULL;
        fail_stream.opened = false;
        const Stream &other_stream = (direction == SOAPY_SDR_RX)
            ? static_cast<const Stream &>(_tx_stream)
            : static_cast<const Stream &>(_rx_stream);
        if (other_stream.opened)
            _nChannels = other_stream.channels.size();
        throw;
    }

    lock.unlock();

    if (channel_mode_changed) {
        if (_rx_stream.opened) {
            for (size_t chan : _rx_stream.channels)
                channel_configure(SOAPY_SDR_RX, chan);
        }
        if (_tx_stream.opened) {
            for (size_t chan : _tx_stream.channels)
                channel_configure(SOAPY_SDR_TX, chan);
        }
    }

    return direction == SOAPY_SDR_RX ? RX_STREAM : TX_STREAM;
}

/* Close the specified stream and release associated resources. */
void SoapyLiteXM2SDR::closeStream(SoapySDR::Stream *stream) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (stream == RX_STREAM) {
        stopRxStreamUnlocked();
        if (isLitePCIe()) {
            (void)m2sdr_stream_release(_dev, M2SDR_RX);
        } else if (isLiteEth()) {
            std::free(_rx_stream.buf);
        }
        _rx_stream.buf = NULL;
        _rx_stream.opened = false;
    } else if (stream == TX_STREAM) {
        stopTxStreamUnlocked();
        if (isLitePCIe()) {
            (void)m2sdr_stream_release(_dev, M2SDR_TX);
        } else if (isLiteEth()) {
            std::free(_tx_stream.buf);
        }
        _tx_stream.buf = NULL;
        _tx_stream.opened = false;
    }

    cleanupLiteEthUdpIfIdleUnlocked();
}

/* Activate the specified stream (configure the DMA engines). */
int SoapyLiteXM2SDR::activateStream(
    SoapySDR::Stream *stream,
    const int flags,
    const long long timeNs,
    const size_t /*numElems*/) {

    /* RX */
    if (stream == RX_STREAM) {
        if (isLitePCIe()) {
            for (size_t i = 0; i < _rx_stream.channels.size(); i++)
                channel_configure(SOAPY_SDR_RX, _rx_stream.channels[i]);
            /* Crossbar Demux: Select PCIe streaming */
            litex_m2sdr_writel(_dev, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);
            /* Re-enable the DMA and resync ring counters (the kernel clears
             * them across a deactivate/activate cycle); drop any buffer still
             * held from before the restart. */
            int rc = m2sdr_stream_activate(_dev, M2SDR_RX);
            if (rc != M2SDR_ERR_OK) {
                SoapySDR::logf(SOAPY_SDR_ERROR,
                    "PCIe RX stream activation failed: %s", m2sdr_strerror(rc));
                return SOAPY_SDR_STREAM_ERROR;
            }
            _rx_stream.remainderHandle = -1;
            _rx_stream.remainderSamps = 0;
            _rx_stream.remainderOffset = 0;
        } else if (isLiteEth()) {
            if (_udp_inited)
                liteeth_udp_flush_rx(&_udp);
            _rx_stream.remainderHandle = -1;
            _rx_stream.remainderSamps = 0;
            _rx_stream.remainderOffset = 0;
            _rx_stream.vrt_sequence_valid = false;
            _rx_stream.rx_timeout_recovery_armed = true;
            {
                struct m2sdr_liteeth_rx_stream_config rx_config = makeLiteEthRxStreamConfig();
                int rc = m2sdr_liteeth_rx_stream_activate(_dev, &rx_config);
                if (rc != M2SDR_ERR_OK) {
                    SoapySDR::logf(SOAPY_SDR_ERROR,
                        "LiteEth RX stream activation failed: %s", m2sdr_strerror(rc));
                    return SOAPY_SDR_STREAM_ERROR;
                }
            }
            /* UDP helper is ready; nothing to start explicitly. */
        }
        _rx_stream.user_count = 0;
        _rx_stream.pendingReadBufs.clear();
        _rx_stream.burst_end = false;
        _rx_stream.time0_ns = this->getHardwareTime("");
        _rx_stream.time0_count = _rx_stream.user_count;
        _rx_stream.time_valid = (_rx_stream.samplerate > 0.0);
        /* Capture starts while activation is still configuring the datapath,
         * so the first buffers can legitimately carry hardware timestamps
         * slightly older than the anchor read above: only seed the
         * monotonicity clamp when timestamps are synthesized from it. */
        _rx_stream.last_time_ns = (_rx_dma_header_bytes != 0)
            ? std::numeric_limits<long long>::min()
            : _rx_stream.time0_ns;
        _rx_stream.time_warned = false;
        _rx_stream.hw_time_warned = false;
        if (flags & SOAPY_SDR_HAS_TIME) {
            SoapySDR::logf(SOAPY_SDR_WARNING,
                "RX timed activation requested for %lld ns; timing not supported, starting immediately",
                (long long)timeNs);
        }

    /* TX */
    } else if (stream == TX_STREAM) {
        if (isLitePCIe()) {
            for (size_t i = 0; i < _tx_stream.channels.size(); i++)
                channel_configure(SOAPY_SDR_TX, _tx_stream.channels[i]);
            /* Crossbar Mux: Select PCIe streaming */
            litex_m2sdr_writel(_dev, CSR_CROSSBAR_MUX_SEL_ADDR, 0);
            int rc = m2sdr_stream_activate(_dev, M2SDR_TX);
            if (rc != M2SDR_ERR_OK) {
                SoapySDR::logf(SOAPY_SDR_ERROR,
                    "PCIe TX stream activation failed: %s", m2sdr_strerror(rc));
                return SOAPY_SDR_STREAM_ERROR;
            }
            _tx_stream.user_count = 0;
        } else if (isLiteEth()) {
            int rc = m2sdr_liteeth_tx_stream_activate(_dev);
            if (rc != M2SDR_ERR_OK) {
                SoapySDR::logf(SOAPY_SDR_ERROR,
                    "LiteEth TX stream activation failed: %s", m2sdr_strerror(rc));
                return SOAPY_SDR_STREAM_ERROR;
            }
            _tx_stream.pace_host_anchor = std::chrono::steady_clock::now();
            _tx_stream.pace_board_valid = false;
            try {
                _tx_stream.pace_board_start_ns = this->getHardwareTime("");
                _tx_stream.pace_board_anchor_ns = _tx_stream.pace_board_start_ns;
                _tx_stream.pace_board_valid = true;
            } catch (const std::exception &e) {
                SoapySDR::logf(SOAPY_SDR_WARNING,
                    "LiteEth TX pacing falls back to the host clock: %s", e.what());
            }
            _tx_stream.paced_buffers = 0;
            _tx_stream.user_count = 0;
        }
        _tx_stream.pendingWriteBufs.clear();
        _tx_stream.burst_end = false;
        _tx_stream.remainderHandle = -1;
        _tx_stream.remainderSamps = 0;
        _tx_stream.remainderOffset = 0;
        _tx_stream.remainderFlags = 0;
        _tx_stream.remainderTimeNs = 0;
        _tx_stream.time_error = false;
        _tx_stream.status_time_ns = 0;
        resetTimedTxTimeline();
        if (_tx_stream.timed_tx_enabled)
            initTimedTxTimeline();
        if (flags & SOAPY_SDR_HAS_TIME) {
            SoapySDR::logf(SOAPY_SDR_DEBUG,
                "TX timed activation requested for %lld ns; timing is enforced by software TX placement",
                (long long)timeNs);
        }
    }

    return 0;
}

/* Deactivate the specified stream (disable DMA engine). */
int SoapyLiteXM2SDR::deactivateStream(
    SoapySDR::Stream *stream,
    const int flags,
    const long long timeNs) {
    if (stream == RX_STREAM) {
        stopRxStreamUnlocked();
        if (flags & SOAPY_SDR_HAS_TIME) {
            SoapySDR::logf(SOAPY_SDR_WARNING,
                "RX timed deactivation requested for %lld ns; timing not supported, stopping immediately",
                (long long)timeNs);
        }
    } else if (stream == TX_STREAM) {
        stopTxStreamUnlocked();
        if (flags & SOAPY_SDR_HAS_TIME) {
            SoapySDR::logf(SOAPY_SDR_WARNING,
                "TX timed deactivation requested for %lld ns; timing not supported, stopping immediately",
                (long long)timeNs);
        }
    }
    return 0;
}

void SoapyLiteXM2SDR::stopRxStreamUnlocked()
{
    if (isLitePCIe()) {
        /* Disable the DMA engine for RX. */
        int rc = m2sdr_stream_deactivate(_dev, M2SDR_RX);
        if (rc != M2SDR_ERR_OK) {
            SoapySDR::logf(SOAPY_SDR_WARNING,
                "PCIe RX stream deactivation failed: %s", m2sdr_strerror(rc));
        }
    } else if (isLiteEth()) {
        int rc = m2sdr_liteeth_rx_stream_deactivate(_dev);
        if (rc != M2SDR_ERR_OK) {
            SoapySDR::logf(SOAPY_SDR_WARNING,
                "LiteEth RX stream deactivation failed: %s", m2sdr_strerror(rc));
        }
    }
    _rx_stream.burst_end = true;
}

void SoapyLiteXM2SDR::stopTxStreamUnlocked()
{
    if (isLitePCIe()) {
        /* Disable the DMA engine for TX. */
        int rc = m2sdr_stream_deactivate(_dev, M2SDR_TX);
        if (rc != M2SDR_ERR_OK) {
            SoapySDR::logf(SOAPY_SDR_WARNING,
                "PCIe TX stream deactivation failed: %s", m2sdr_strerror(rc));
        }
    } else if (isLiteEth()) {
        int rc = m2sdr_liteeth_tx_stream_deactivate(_dev);
        if (rc != M2SDR_ERR_OK) {
            SoapySDR::logf(SOAPY_SDR_WARNING,
                "LiteEth TX stream deactivation failed: %s", m2sdr_strerror(rc));
        }
    }
    _tx_stream.tx_timeline_valid = false;
}

void SoapyLiteXM2SDR::cleanupLiteEthUdpIfIdleUnlocked()
{
    if (isLiteEth() && _udp_inited && !_rx_stream.opened && !_tx_stream.opened) {
        liteeth_udp_cleanup(&_udp);
        _udp_inited = false;
        _nChannels = 0;
    }
}

/*******************************************************************
 * Direct buffer API
 ******************************************************************/

/* Retrieve the maximum transmission unit (MTU) for a stream. */
size_t SoapyLiteXM2SDR::getStreamMTU(SoapySDR::Stream *stream) const {
    if (stream == RX_STREAM) {
        /* Each sample is 2 * Complex{Int16}. */
        return _rx_buf_size / (_nChannels * _bytesPerComplex);
    } else if (stream == TX_STREAM) {
        return _tx_buf_size / (_nChannels * _bytesPerComplex);
    } else {
        throw std::runtime_error("SoapySDR::getStreamMTU(): Invalid stream.");
    }
}

/* Retrieve the number of direct access buffers available for a stream. */
size_t SoapyLiteXM2SDR::getNumDirectAccessBuffers(SoapySDR::Stream *stream) {
    if (stream == RX_STREAM) {
        return _rx_buf_count;
    } else if (stream == TX_STREAM) {
        return _tx_buf_count;
    } else {
        throw std::runtime_error("SoapySDR::getNumDirectAccessBuffers(): Invalid stream.");
    }
}

/* Retrieve buffer addresses for a direct access buffer. */
int SoapyLiteXM2SDR::getDirectAccessBufferAddrs(
    SoapySDR::Stream *stream,
    const size_t handle,
    void **buffs) {
    if (stream == RX_STREAM) {
        if (isLitePCIe())
            buffs[0] = (char *)_rx_stream.buf + handle * _rx_buf_stride + _rx_dma_header_bytes;
        else
            buffs[0] = (char *)_rx_stream.buf + handle * _rx_buf_size;
    } else if (stream == TX_STREAM) {
        if (isLitePCIe())
            buffs[0] = (char *)_tx_stream.buf + handle * _tx_buf_stride + TX_DMA_HEADER_SIZE;
        else
            buffs[0] = (char *)_tx_stream.buf + handle * _tx_buf_size;
    } else {
        throw std::runtime_error("SoapySDR::getDirectAccessBufferAddrs(): Invalid stream.");
    }
    return 0;
}

/***************************************************************************************************
 * Buffer Management
 **************************************************************************************************/

static inline long long samples_to_ns(double sample_rate, long long samples)
{
    if (sample_rate <= 0.0) {
        return 0;
    }
    const double ns = (static_cast<double>(samples) * 1e9) / sample_rate;
    return static_cast<long long>(std::llround(ns));
}

static inline size_t ns_to_samples_ceil(double sample_rate, long long ns)
{
    if (sample_rate <= 0.0 || ns <= 0) {
        return 0;
    }
    const double samples = (static_cast<double>(ns) * sample_rate) / 1e9;
    if (samples >= static_cast<double>(std::numeric_limits<size_t>::max())) {
        return std::numeric_limits<size_t>::max();
    }
    return static_cast<size_t>(std::ceil(samples));
}

static inline int timeout_us_to_ms(long timeout_us)
{
    if (timeout_us <= 0)
        return 0;
    return static_cast<int>((timeout_us + 999) / 1000);
}

static enum m2sdr_format soapy_stream_format_to_m2sdr(const std::string &format, uint32_t bit_mode)
{
    return (format == SOAPY_SDR_CS8 || bit_mode == 8) ?
        M2SDR_FORMAT_SC8_Q7 : M2SDR_FORMAT_SC16_Q11;
}

void SoapyLiteXM2SDR::refreshTimedTxDefaults()
{
    if (!_tx_stream.timed_tx_late_margin_configured) {
        if (_tx_stream.samplerate <= 0.0 || _tx_buf_size == 0 || _nChannels == 0) {
            _tx_stream.timed_tx_late_margin_ns = 0;
            return;
        }
        _tx_stream.timed_tx_late_margin_ns =
            samples_to_ns(_tx_stream.samplerate,
                          static_cast<long long>(this->getStreamMTU(TX_STREAM)));
    }
}

/* Software timed TX keeps a sample timeline anchored to board time. */
void SoapyLiteXM2SDR::resetTimedTxTimeline()
{
    _tx_stream.tx_timeline_valid = false;
    _tx_stream.tx_next_time_ns = 0;
}

void SoapyLiteXM2SDR::initTimedTxTimeline()
{
    if (_tx_stream.samplerate <= 0.0) {
        resetTimedTxTimeline();
        return;
    }

    refreshTimedTxDefaults();
    const long long mtu_time_ns =
        samples_to_ns(_tx_stream.samplerate,
                      static_cast<long long>(this->getStreamMTU(TX_STREAM)));
    const long long lead_ns =
        samples_to_ns(_tx_stream.samplerate,
                      static_cast<long long>(_tx_stream.timed_tx_lead_buffers) *
                      static_cast<long long>(this->getStreamMTU(TX_STREAM)));
    _tx_stream.tx_next_time_ns =
        this->getHardwareTime("") + lead_ns + _tx_stream.timed_tx_latency_ns;
    _tx_stream.tx_timeline_valid = true;

    SoapySDR::logf(SOAPY_SDR_DEBUG,
        "TX timed timeline initialized: next=%lld ns lead=%lld ns latency=%lld ns mtu_time=%lld ns",
        (long long)_tx_stream.tx_next_time_ns,
        (long long)lead_ns,
        (long long)_tx_stream.timed_tx_latency_ns,
        (long long)mtu_time_ns);
}

int SoapyLiteXM2SDR::ensureTxRemainderBuffer(
    SoapySDR::Stream *stream,
    const long timeoutUs)
{
    if (_tx_stream.remainderHandle >= 0)
        return 0;

    size_t handle;
    int ret = this->acquireWriteBuffer(
        stream,
        handle,
        (void **)&_tx_stream.remainderBuff,
        timeoutUs);
    if (ret < 0)
        return ret;

    _tx_stream.remainderHandle = static_cast<int32_t>(handle);
    _tx_stream.remainderSamps = static_cast<size_t>(ret);
    _tx_stream.remainderOffset = 0;
    _tx_stream.remainderFlags = 0;
    _tx_stream.remainderTimeNs = 0;
    return 0;
}

/* Submit the current partial/full TX buffer; force is used for burst boundaries. */
int SoapyLiteXM2SDR::submitTxRemainder(
    SoapySDR::Stream *stream,
    const bool force)
{
    if (_tx_stream.remainderHandle < 0)
        return 0;
    if (_tx_stream.remainderOffset == 0)
        return 0;
    if (!force && _tx_stream.remainderSamps != 0)
        return 0;

    /* The DMA ring and UDP packets are fixed-size: a partial submit still
     * emits a full buffer, with the tail zero-filled by releaseWriteBuffer().
     * That tail occupies air time, so the timeline must account for it. */
    if (_tx_stream.timed_tx_enabled && _tx_stream.tx_timeline_valid &&
        _tx_stream.remainderSamps != 0) {
        _tx_stream.tx_next_time_ns +=
            samples_to_ns(_tx_stream.samplerate,
                          static_cast<long long>(_tx_stream.remainderSamps));
    }

    int submit_flags = _tx_stream.remainderFlags;
    const long long submit_time_ns = _tx_stream.remainderTimeNs;
    this->releaseWriteBuffer(stream,
                             _tx_stream.remainderHandle,
                             _tx_stream.remainderOffset,
                             submit_flags,
                             submit_time_ns);
    _tx_stream.remainderHandle = -1;
    _tx_stream.remainderSamps = 0;
    _tx_stream.remainderOffset = 0;
    _tx_stream.remainderBuff = nullptr;
    _tx_stream.remainderFlags = 0;
    _tx_stream.remainderTimeNs = 0;
    return 0;
}

/* Preserve timestamp metadata on the buffer containing the first payload sample. */
void SoapyLiteXM2SDR::markTxRemainderTime(const long long payloadTimeNs)
{
    if (_tx_stream.remainderHandle < 0)
        return;
    if (_tx_stream.remainderFlags & SOAPY_SDR_HAS_TIME)
        return;

    _tx_stream.remainderFlags |= SOAPY_SDR_HAS_TIME;
    _tx_stream.remainderTimeNs =
        payloadTimeNs -
        samples_to_ns(_tx_stream.samplerate,
                      static_cast<long long>(_tx_stream.remainderOffset));
}

int SoapyLiteXM2SDR::appendTxZeros(
    SoapySDR::Stream *stream,
    size_t numElems,
    const long timeoutUs)
{
    while (numElems > 0) {
        int ret = ensureTxRemainderBuffer(stream, timeoutUs);
        if (ret < 0)
            return ret;

        const size_t n = std::min(numElems, _tx_stream.remainderSamps);
        const size_t offset_bytes =
            _tx_stream.remainderOffset * _nChannels * _bytesPerComplex;
        std::memset(_tx_stream.remainderBuff + offset_bytes,
                    0,
                    n * _nChannels * _bytesPerComplex);

        _tx_stream.remainderSamps -= n;
        _tx_stream.remainderOffset += n;
        numElems -= n;

        if (_tx_stream.timed_tx_enabled && _tx_stream.tx_timeline_valid) {
            _tx_stream.tx_next_time_ns +=
                samples_to_ns(_tx_stream.samplerate, static_cast<long long>(n));
        }

        ret = submitTxRemainder(stream, false);
        if (ret < 0)
            return ret;
    }
    return 0;
}

int SoapyLiteXM2SDR::appendTxSamples(
    SoapySDR::Stream *stream,
    const void *const *buffs,
    size_t numElems,
    size_t userOffset,
    bool holdLast,
    const long timeoutUs)
{
    size_t appended = 0;

    while (numElems > 0) {
        int ret = ensureTxRemainderBuffer(stream, timeoutUs);
        if (ret < 0)
            return appended ? static_cast<int>(appended) : ret;

        const size_t n = std::min(numElems, _tx_stream.remainderSamps);
        const size_t remainderOffset =
            _tx_stream.remainderOffset * _nChannels * _bytesPerComplex;

        for (size_t i = 0; i < _tx_stream.channels.size(); i++) {
            this->interleave(
                buffs[i],
                _tx_stream.remainderBuff + remainderOffset +
                    (_tx_stream.channels[i] * _bytesPerComplex),
                n,
                _tx_stream.format,
                userOffset + appended
            );
        }

        _tx_stream.remainderSamps -= n;
        _tx_stream.remainderOffset += n;
        appended += n;
        numElems -= n;

        if (_tx_stream.timed_tx_enabled && _tx_stream.tx_timeline_valid) {
            _tx_stream.tx_next_time_ns +=
                samples_to_ns(_tx_stream.samplerate, static_cast<long long>(n));
        }

        if (!(holdLast && numElems == 0)) {
            ret = submitTxRemainder(stream, false);
            if (ret < 0)
                return appended ? static_cast<int>(appended) : ret;
        }
    }

    return static_cast<int>(appended);
}

/* Acquire a buffer for reading. */
int SoapyLiteXM2SDR::acquireReadBuffer(
    SoapySDR::Stream *stream,
    size_t &handle,
    const void **buffs,
    int &flags,
    long long &timeNs,
    const long timeoutUs) {
    if (stream != RX_STREAM) {
        return SOAPY_SDR_STREAM_ERROR;
    }

    if (_rx_stream.burst_end)
        flags |= SOAPY_SDR_END_BURST;

    if (isLiteEth()) {
        /* Pump UDP helper once with caller timeout (ms). */
        liteeth_udp_process(&_udp, timeout_us_to_ms(timeoutUs));

        int avail = liteeth_udp_buffers_available_read(&_udp);
        if (avail <= 0 && timeoutUs != 0 && _rx_stream.rx_timeout_recovery_armed) {
            _rx_stream.rx_timeout_recovery_armed = false;
            _rx_stream.rx_timeout_recoveries++;
            _udp.rx_timeout_recoveries++;

            (void)m2sdr_liteeth_rx_stream_deactivate(_dev);
            liteeth_udp_flush_rx(&_udp);

            struct m2sdr_liteeth_rx_stream_config rx_config = makeLiteEthRxStreamConfig();
            int rc = m2sdr_liteeth_rx_stream_activate(_dev, &rx_config);
            if (rc == M2SDR_ERR_OK) {
                int retry_ms = timeout_us_to_ms(timeoutUs);
                if (retry_ms <= 0 || retry_ms > 50)
                    retry_ms = 50;
                liteeth_udp_process(&_udp, retry_ms);
                avail = liteeth_udp_buffers_available_read(&_udp);
            } else {
                SoapySDR::logf(SOAPY_SDR_WARNING,
                    "LiteEth RX timeout recovery activation failed: %s", m2sdr_strerror(rc));
            }
        }
        if (avail <= 0) {
            return SOAPY_SDR_TIMEOUT;
        }

        uint8_t *src = liteeth_udp_next_read_buffer(&_udp);
        if (!src) {
            return SOAPY_SDR_TIMEOUT;
        }
        _rx_stream.rx_timeout_recovery_armed = true;

        if (_eth_mode == SoapyLiteXM2SDREthernetMode::VRT) {
            if (_udp.buf_size < VRT_SIGNAL_HEADER_BYTES) {
                return SOAPY_SDR_STREAM_ERROR;
            }
            const uint32_t common = read_be32(src + 0);
            const uint32_t packet_type = (common >> 28) & 0xF;
            const uint32_t packet_count = (common >> 16) & 0xF;
            const uint32_t packet_words = (common & 0xFFFF);
            if (packet_type != 0x1 || packet_words < 5) {
                SoapySDR_logf(SOAPY_SDR_WARNING, "Invalid/unsupported VRT RX packet (type=%u words=%u)",
                              packet_type, packet_words);
                return SOAPY_SDR_STREAM_ERROR;
            }
            size_t payload_bytes = static_cast<size_t>(packet_words - 5) * sizeof(uint32_t);
            if (payload_bytes > (_udp.buf_size - VRT_SIGNAL_HEADER_BYTES))
                payload_bytes = _udp.buf_size - VRT_SIGNAL_HEADER_BYTES;
            if (payload_bytes > _rx_buf_size)
                payload_bytes = _rx_buf_size;

            const uint32_t tsi_type = (common >> 22) & 0x3;
            const uint32_t tsf_type = (common >> 20) & 0x3;
            if (tsi_type != 0 || tsf_type != 0) {
                const uint64_t tsi = read_be32(src + 8);
                const uint64_t tsf = read_be64(src + 12);
                timeNs = static_cast<long long>(tsi * 1000000000ULL + (tsf / 1000ULL));
                flags |= SOAPY_SDR_HAS_TIME;
            }

            if (_rx_stream.vrt_sequence_valid) {
                const uint8_t expected =
                    static_cast<uint8_t>((_rx_stream.vrt_sequence_last + 1) & 0xF);
                if (packet_count != expected) {
                    const uint8_t lost =
                        static_cast<uint8_t>((packet_count - expected) & 0xF);
                    if (_rx_stream.vrt_sequence_gaps < 8) {
                        SoapySDR_logf(SOAPY_SDR_WARNING,
                            "LiteEth VRT RX packet sequence gap: expected=%u got=%u lost_mod16=%u",
                            expected, packet_count, lost);
                    }
                    _rx_stream.vrt_sequence_gaps++;
                    _rx_stream.vrt_packets_lost += lost;
                }
            }
            _rx_stream.vrt_sequence_last = static_cast<uint8_t>(packet_count);
            _rx_stream.vrt_sequence_valid = true;
            _rx_stream.vrt_packets++;

            buffs[0] = src + VRT_SIGNAL_HEADER_BYTES;
            handle   = 0;
            return static_cast<int>(payload_bytes / (_nChannels * _bytesPerComplex));
        }

        buffs[0] = src;
        handle   = 0; /* dummy for LiteEth path */
        /* Raw UDP packets carry no timestamp; synthesize one from the board
         * time anchored at activation. Lost packets shift this late, so use
         * eth_mode=vrt when accurate RX time matters. */
        _rx_stream.user_count++;
        if (_rx_stream.time_valid && !(flags & SOAPY_SDR_HAS_TIME)) {
            const size_t samples_per_buffer = getStreamMTU(stream);
            timeNs = _rx_stream.time0_ns +
                     samples_to_ns(_rx_stream.samplerate,
                                   static_cast<long long>(_rx_stream.user_count - _rx_stream.time0_count) *
                                   static_cast<long long>(samples_per_buffer));
            flags |= SOAPY_SDR_HAS_TIME;
            if (timeNs < _rx_stream.last_time_ns)
                timeNs = _rx_stream.last_time_ns;
            else
                _rx_stream.last_time_ns = timeNs;
        }
        return getStreamMTU(stream);
    } else if (isLitePCIe()) {
        void *buffer = nullptr;
        unsigned total_samples = 0;
        int rc;
        if (timeoutUs == 0)
            rc = m2sdr_try_get_buffer(_dev, M2SDR_RX, &buffer, &total_samples);
        else
            rc = m2sdr_get_buffer(_dev, M2SDR_RX, &buffer, &total_samples,
                                  timeout_us_to_ms(timeoutUs));
        if (rc == M2SDR_ERR_TIMEOUT)
            return SOAPY_SDR_TIMEOUT;
        if (rc == M2SDR_ERR_OVERFLOW) {
            _rx_stream.pendingReadBufs.clear();
            _rx_stream.overflow = true;
            flags |= SOAPY_SDR_END_ABRUPT;
            _rx_stream.time0_ns = this->getHardwareTime("");
            _rx_stream.time0_count = _rx_stream.user_count;
            _rx_stream.time_valid = (_rx_stream.samplerate > 0.0);
            /* Hardware timestamps stay monotonic across drops on their own;
             * only the synthesized fallback needs re-anchoring. */
            if (_rx_dma_header_bytes == 0)
                _rx_stream.last_time_ns = _rx_stream.time0_ns;
            _rx_stream.time_warned = false;
            return SOAPY_SDR_OVERFLOW;
        }
        if (rc != M2SDR_ERR_OK) {
            SoapySDR::logf(SOAPY_SDR_ERROR,
                "m2sdr_get_buffer(RX) failed: %s", m2sdr_strerror(rc));
            return SOAPY_SDR_STREAM_ERROR;
        }

        buffs[0] = buffer;
        handle = static_cast<size_t>(_rx_stream.user_count++);
        _rx_stream.pendingReadBufs[handle] = buffer;

        const size_t samples_per_buffer = total_samples / _nChannels;
        if (_rx_stream.time_valid && !(flags & SOAPY_SDR_HAS_TIME)) {
            /* Prefer the hardware capture time from the DMA header; it stays
             * exact across drops and overflows. The anchor + buffer-count
             * scheme remains as fallback for header-less gateware. */
            struct m2sdr_metadata meta;
            bool have_hw_time = false;
            if (_rx_dma_header_bytes != 0 &&
                m2sdr_get_buffer_metadata(_dev, M2SDR_RX, buffer, &meta) == M2SDR_ERR_OK &&
                (meta.flags & M2SDR_META_FLAG_HAS_TIME)) {
                timeNs = static_cast<long long>(meta.timestamp);
                have_hw_time = true;
            } else {
                timeNs = _rx_stream.time0_ns +
                         samples_to_ns(_rx_stream.samplerate,
                                       static_cast<long long>(_rx_stream.user_count - _rx_stream.time0_count) *
                                       static_cast<long long>(samples_per_buffer));
            }
            flags |= SOAPY_SDR_HAS_TIME;
            if (timeNs < _rx_stream.last_time_ns) {
                if (have_hw_time && !_rx_stream.hw_time_warned) {
                    SoapySDR::logf(SOAPY_SDR_WARNING,
                        "RX hardware timestamp regressed: %lld < %lld",
                        (long long)timeNs, (long long)_rx_stream.last_time_ns);
                    _rx_stream.hw_time_warned = true;
                }
                timeNs = _rx_stream.last_time_ns;
            } else {
                _rx_stream.last_time_ns = timeNs;
            }
        }
        return static_cast<int>(samples_per_buffer);
    }
    return SOAPY_SDR_STREAM_ERROR;
}

/* Release a read buffer after use. */
void SoapyLiteXM2SDR::releaseReadBuffer(
    SoapySDR::Stream */*stream*/,
    size_t handle) {
    assert(handle != (size_t)-1 && "Attempt to release an invalid buffer (e.g., from an overflow).");

    if (isLitePCIe()) {
        auto it = _rx_stream.pendingReadBufs.find(handle);
        if (it != _rx_stream.pendingReadBufs.end()) {
            int rc = m2sdr_release_buffer(_dev, M2SDR_RX, it->second);
            if (rc != M2SDR_ERR_OK) {
                SoapySDR::logf(SOAPY_SDR_ERROR,
                    "m2sdr_release_buffer(RX) failed: %s", m2sdr_strerror(rc));
            }
            _rx_stream.pendingReadBufs.erase(it);
        }
    } else {
        (void)handle; /* UDP slot was advanced by next_read_buffer(). */
    }
}

/* Acquire a buffer for writing. */
int SoapyLiteXM2SDR::acquireWriteBuffer(
    SoapySDR::Stream *stream,
    size_t &handle,
    void **buffs,
    const long timeoutUs) {
    if (stream != TX_STREAM) {
        return SOAPY_SDR_STREAM_ERROR;
    }

    if (isLitePCIe()) {
        void *buffer = nullptr;
        unsigned total_samples = 0;
        int rc;
        if (timeoutUs == 0)
            rc = m2sdr_try_get_buffer(_dev, M2SDR_TX, &buffer, &total_samples);
        else
            rc = m2sdr_get_buffer(_dev, M2SDR_TX, &buffer, &total_samples,
                                  timeout_us_to_ms(timeoutUs));
        if (rc == M2SDR_ERR_TIMEOUT)
            return SOAPY_SDR_TIMEOUT;
        if (rc == M2SDR_ERR_UNDERFLOW) {
            _tx_stream.underflow = true;
            resetTimedTxTimeline();
            return SOAPY_SDR_UNDERFLOW;
        }
        if (rc != M2SDR_ERR_OK) {
            SoapySDR::logf(SOAPY_SDR_ERROR,
                "m2sdr_get_buffer(TX) failed: %s", m2sdr_strerror(rc));
            return SOAPY_SDR_STREAM_ERROR;
        }

        buffs[0] = buffer;
        handle = static_cast<size_t>(_tx_stream.user_count++);
        _tx_stream.pendingWriteBufs[handle] = static_cast<uint8_t *>(buffer);
        return static_cast<int>(total_samples / _nChannels);
    } else if (isLiteEth()) {
        uint8_t *dst = liteeth_udp_next_write_buffer(&_udp);
        if (!dst) {
            return SOAPY_SDR_TIMEOUT;
        }
        buffs[0] = dst;
        handle   = _tx_stream.user_count++;
        _tx_stream.pendingWriteBufs[handle] = dst;
        (void)timeoutUs;
        return getStreamMTU(stream);
    }
    return SOAPY_SDR_STREAM_ERROR;
}

/* Release a write buffer after use. */
void SoapyLiteXM2SDR::releaseWriteBuffer(
    SoapySDR::Stream *stream,
    size_t handle,
    const size_t numElems,
    int &flags,
    const long long timeNs) {
    if (flags & SOAPY_SDR_END_BURST) {
        _tx_stream.burst_end = true;
    }

    const size_t mtu = this->getStreamMTU(stream);
    if (numElems < mtu) {
        uint8_t *buf = nullptr;
        if (isLitePCIe()) {
            auto it = _tx_stream.pendingWriteBufs.find(handle);
            if (it != _tx_stream.pendingWriteBufs.end())
                buf = it->second;
        } else if (isLiteEth()) {
            auto it = _tx_stream.pendingWriteBufs.find(handle);
            if (it != _tx_stream.pendingWriteBufs.end()) {
                buf = it->second;
                _tx_stream.pendingWriteBufs.erase(it);
            } else {
                buf = reinterpret_cast<uint8_t*>(_tx_stream.remainderBuff);
            }
        } else {
            return;
        }
        if (buf) {
            const size_t offset_bytes = numElems * _nChannels * _bytesPerComplex;
            const size_t zero_bytes = (mtu - numElems) * _nChannels * _bytesPerComplex;
            std::memset(buf + offset_bytes, 0, zero_bytes);
        }
    }

    if (isLitePCIe()) {
        auto it = _tx_stream.pendingWriteBufs.find(handle);
        if (it != _tx_stream.pendingWriteBufs.end()) {
            struct m2sdr_metadata meta;
            struct m2sdr_metadata *meta_ptr = nullptr;

            if (flags & SOAPY_SDR_HAS_TIME) {
                std::memset(&meta, 0, sizeof(meta));
                meta.flags = M2SDR_META_FLAG_HAS_TIME;
                meta.timestamp = static_cast<uint64_t>(timeNs);
                meta_ptr = &meta;
            }

            int rc = m2sdr_submit_buffer(_dev, M2SDR_TX, it->second,
                                         static_cast<unsigned>(numElems * _nChannels),
                                         meta_ptr);
            if (rc != M2SDR_ERR_OK) {
                _tx_stream.underflow = true;
                resetTimedTxTimeline();
                SoapySDR::logf(SOAPY_SDR_ERROR,
                    "m2sdr_submit_buffer(TX) failed: %s", m2sdr_strerror(rc));
            }
            _tx_stream.pendingWriteBufs.erase(it);
        }
    } else if (isLiteEth()) {
        if (numElems >= mtu) {
            auto it = _tx_stream.pendingWriteBufs.find(handle);
            if (it != _tx_stream.pendingWriteBufs.end()) {
                _tx_stream.pendingWriteBufs.erase(it);
            }
        }
        if (_tx_stream.rate_pacing && _tx_stream.samplerate > 0.0) {
            /* Refresh the (host, board) anchor pair at a coarse cadence; the
             * board-time query costs an Etherbone round trip, while host
             * oscillator drift between refreshes stays in the ppm range. */
            if (_tx_stream.pace_board_valid &&
                (_tx_stream.paced_buffers % LITEETH_PACE_RESYNC_BUFFERS) == 0) {
                try {
                    _tx_stream.pace_board_anchor_ns = this->getHardwareTime("");
                    _tx_stream.pace_host_anchor = std::chrono::steady_clock::now();
                } catch (const std::exception &) {
                    /* Keep pacing on the previous anchors. */
                }
            }
            const long long samples =
                static_cast<long long>(_tx_stream.paced_buffers) *
                static_cast<long long>(mtu);
            const long long elapsed_ns =
                samples_to_ns(_tx_stream.samplerate, samples);
            const long long anchor_offset_ns = _tx_stream.pace_board_valid
                ? (_tx_stream.pace_board_start_ns + elapsed_ns -
                   _tx_stream.pace_board_anchor_ns)
                : elapsed_ns;
            const auto target =
                _tx_stream.pace_host_anchor +
                std::chrono::nanoseconds(anchor_offset_ns);
            const auto now = std::chrono::steady_clock::now();
            if (target > now)
                std::this_thread::sleep_until(target);
        }
        if (liteeth_udp_write_submit(&_udp) < 0) {
            _tx_stream.underflow = true;
            resetTimedTxTimeline();
            SoapySDR_logf(SOAPY_SDR_ERROR, "UDP write_submit failed.");
        } else {
            _tx_stream.paced_buffers++;
        }
    }
}

/* Interleave CF32 samples. */
void SoapyLiteXM2SDR::interleaveCF32(
    const void *src,
    void *dst,
    uint32_t len,
    size_t offset) {
    const float *samples_cf32 = reinterpret_cast<const float*>(src) + (offset * _samplesPerComplex);

    if (_bytesPerSample == 2) {
        int16_t *dst_int16 = reinterpret_cast<int16_t*>(dst) + (offset * 2 * _samplesPerComplex);
        for (uint32_t i = 0; i < len; i++) {
            dst_int16[0] = static_cast<int16_t>(samples_cf32[0] * _samplesScaling); /* I. */
            dst_int16[1] = static_cast<int16_t>(samples_cf32[1] * _samplesScaling); /* Q. */
            samples_cf32 += 2;
            dst_int16 += _nChannels * _samplesPerComplex;
        }
    } else if (_bytesPerSample == 1) {
        int8_t *dst_int8 = reinterpret_cast<int8_t*>(dst) + (offset * 2 * _samplesPerComplex);
        for (uint32_t i = 0; i < len; i++) {
            dst_int8[0] = cf32_to_sc8(samples_cf32[0], _samplesScaling); /* I. */
            dst_int8[1] = cf32_to_sc8(samples_cf32[1], _samplesScaling); /* Q. */
            samples_cf32 += 2;
            dst_int8 += _nChannels * _samplesPerComplex;
        }
    } else {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Unsupported _bytesPerSample value: %u.", _bytesPerSample);
    }
}

/* Deinterleave CF32 samples. */
void SoapyLiteXM2SDR::deinterleaveCF32(
    const void *src,
    void *dst,
    uint32_t len,
    size_t offset) {
    float *samples_cf32 = reinterpret_cast<float*>(dst) + (offset * _samplesPerComplex);

    if (_bytesPerSample == 2) {
        const int16_t *src_int16 = reinterpret_cast<const int16_t*>(src);

        for (uint32_t i = 0; i < len; i++) {
            /* Mask 12 LSB and sign-extend */
            int16_t i_sample = static_cast<int16_t>(src_int16[0] << 4) >> 4;  /* I. */
            int16_t q_sample = static_cast<int16_t>(src_int16[1] << 4) >> 4;  /* Q. */
            /* Scale to float (-1.0 to 1.0 range) */
            samples_cf32[0] = static_cast<float>(i_sample) / _samplesScaling; /* I. */
            samples_cf32[1] = static_cast<float>(q_sample) / _samplesScaling; /* Q. */
            samples_cf32 += 2;
            src_int16 += _nChannels * _samplesPerComplex;
        }
    } else if (_bytesPerSample == 1) {
        const int8_t *src_int8 = reinterpret_cast<const int8_t*>(src);

        for (uint32_t i = 0; i < len; i++) {
            /* Scale to float (-1.0 to 1.0 range) */
            samples_cf32[0] = static_cast<float>(src_int8[0]) / _samplesScaling; /* I. */
            samples_cf32[1] = static_cast<float>(src_int8[1]) / _samplesScaling; /* Q. */
            samples_cf32 += 2;
            src_int8 += _nChannels * _samplesPerComplex;
        }
    } else {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Unsupported _bytesPerSample value: %u.", _bytesPerSample);
    }
}

/* Interleave CS16 samples */
void SoapyLiteXM2SDR::interleaveCS16(
    const void *src,
    void *dst,
    uint32_t len,
    size_t offset) {
    const int16_t *samples_cs16 = reinterpret_cast<const int16_t*>(src) + (offset * _samplesPerComplex);

    if (_bytesPerSample == 2) {
        int16_t *dst_int16 = reinterpret_cast<int16_t*>(dst) + (offset * 2 * _samplesPerComplex);

        for (uint32_t i = 0; i < len; i++) {
            dst_int16[0] = samples_cs16[0]; /* I. */
            dst_int16[1] = samples_cs16[1]; /* Q. */
            samples_cs16 += 2;
            dst_int16 += _nChannels * _samplesPerComplex;
        }
    } else if (_bytesPerSample == 1) {
        int8_t *dst_int8 = reinterpret_cast<int8_t*>(dst) + (offset * 2 * _samplesPerComplex);

        for (uint32_t i = 0; i < len; i++) {
            dst_int8[0] = sc16_q11_to_sc8(samples_cs16[0]); /* I. */
            dst_int8[1] = sc16_q11_to_sc8(samples_cs16[1]); /* Q. */
            samples_cs16 += 2;
            dst_int8 += _nChannels * _samplesPerComplex;
        }
    } else {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Unsupported _bytesPerSample value: %u.", _bytesPerSample);
    }
}

/* Deinterleave CS16 samples */
void SoapyLiteXM2SDR::deinterleaveCS16(
    const void *src,
    void *dst,
    uint32_t len,
    size_t offset) {
    int16_t *samples_cs16 = reinterpret_cast<int16_t*>(dst) + (offset * _samplesPerComplex);

    if (_bytesPerSample == 2) {
        const int16_t *src_int16 = reinterpret_cast<const int16_t*>(src);

        for (uint32_t i = 0; i < len; i++) {
            /* Mask 12 LSB and sign-extend */
            samples_cs16[0] = static_cast<int16_t>(src_int16[0] << 4) >> 4;  /* I. */
            samples_cs16[1] = static_cast<int16_t>(src_int16[1] << 4) >> 4;  /* Q. */
            samples_cs16 += 2;
            src_int16 += _nChannels * _samplesPerComplex;
        }
    } else if (_bytesPerSample == 1) {
        const int8_t *src_int8 = reinterpret_cast<const int8_t*>(src);

        for (uint32_t i = 0; i < len; i++) {
            samples_cs16[0] = static_cast<int16_t>(src_int8[0]); /* I. */
            samples_cs16[1] = static_cast<int16_t>(src_int8[1]); /* Q. */
            samples_cs16 += 2;
            src_int8 += _nChannels * _samplesPerComplex;
        }
    } else {
        printf("Unsupported _bytesPerSample value: %u\n", _bytesPerSample);
    }
}

/* Interleave CS8 samples */
void SoapyLiteXM2SDR::interleaveCS8(
    const void *src,
    void *dst,
    uint32_t len,
    size_t offset) {
    const int8_t *samples_cs8 = reinterpret_cast<const int8_t*>(src) + (offset * _samplesPerComplex);

    if (_bytesPerSample == 1) {
        int8_t *dst_int8 = reinterpret_cast<int8_t*>(dst) + (offset * 2 * _samplesPerComplex);
        for (uint32_t i = 0; i < len; i++) {
            dst_int8[0] = samples_cs8[0]; /* I. */
            dst_int8[1] = samples_cs8[1]; /* Q. */
            samples_cs8 += 2;
            dst_int8 += _nChannels * _samplesPerComplex;
        }
    } else if (_bytesPerSample == 2) {
        int16_t *dst_int16 = reinterpret_cast<int16_t*>(dst) + (offset * 2 * _samplesPerComplex);
        for (uint32_t i = 0; i < len; i++) {
            dst_int16[0] = static_cast<int16_t>(samples_cs8[0]) << 4; /* I. */
            dst_int16[1] = static_cast<int16_t>(samples_cs8[1]) << 4; /* Q. */
            samples_cs8 += 2;
            dst_int16 += _nChannels * _samplesPerComplex;
        }
    } else {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Unsupported _bytesPerSample value: %u.", _bytesPerSample);
    }
}

/* Deinterleave CS8 samples */
void SoapyLiteXM2SDR::deinterleaveCS8(
    const void *src,
    void *dst,
    uint32_t len,
    size_t offset) {
    int8_t *samples_cs8 = reinterpret_cast<int8_t*>(dst) + (offset * _samplesPerComplex);

    if (_bytesPerSample == 1) {
        const int8_t *src_int8 = reinterpret_cast<const int8_t*>(src);
        for (uint32_t i = 0; i < len; i++) {
            samples_cs8[0] = src_int8[0]; /* I. */
            samples_cs8[1] = src_int8[1]; /* Q. */
            samples_cs8 += 2;
            src_int8 += _nChannels * _samplesPerComplex;
        }
    } else if (_bytesPerSample == 2) {
        const int16_t *src_int16 = reinterpret_cast<const int16_t*>(src);
        for (uint32_t i = 0; i < len; i++) {
            samples_cs8[0] = sc16_q11_to_sc8(src_int16[0]); /* I. */
            samples_cs8[1] = sc16_q11_to_sc8(src_int16[1]); /* Q. */
            samples_cs8 += 2;
            src_int16 += _nChannels * _samplesPerComplex;
        }
    } else {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Unsupported _bytesPerSample value: %u.", _bytesPerSample);
    }
}

/* Interleave samples */
void SoapyLiteXM2SDR::interleave(
    const void *src,
    void *dst,
    uint32_t len,
    const std::string &format,
    size_t offset) {
    if (format == SOAPY_SDR_CF32) {
        interleaveCF32(src, dst, len, offset);
    } else if (format == SOAPY_SDR_CS16) {
        interleaveCS16(src, dst, len, offset);
    } else if (format == SOAPY_SDR_CS8) {
        interleaveCS8(src, dst, len, offset);
    } else {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Unsupported format: %s.", format.c_str());
    }
}

/* Deinterleave samples */
void SoapyLiteXM2SDR::deinterleave(
    const void *src,
    void *dst,
    uint32_t len,
    const std::string &format,
    size_t offset) {
    if (format == SOAPY_SDR_CF32) {
        deinterleaveCF32(src, dst, len, offset);
    } else if (format == SOAPY_SDR_CS16) {
        deinterleaveCS16(src, dst, len, offset);
    } else if (format == SOAPY_SDR_CS8) {
        deinterleaveCS8(src, dst, len, offset);
    } else {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Unsupported format: %s.", format.c_str());
    }
}

/* Read from the RX stream. */
int SoapyLiteXM2SDR::readStream(
    SoapySDR::Stream *stream,
    void *const *buffs,
    const size_t numElems,
    int &flags,
    long long &timeNs,
    const long timeoutUs) {
    if (stream != RX_STREAM) {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    /* Determine the number of samples to return, respecting the MTU. */
    size_t returnedElems = std::min(numElems, this->getStreamMTU(stream));

    size_t samp_avail = 0;

    /* If there's a remainder buffer from a previous read, process that first. */
    if (_rx_stream.remainderHandle >= 0) {
        const size_t n = std::min(_rx_stream.remainderSamps, returnedElems);
        const uint32_t remainderOffset = _rx_stream.remainderOffset *  _nChannels *_bytesPerComplex;

        if (n < returnedElems) {
            samp_avail = n;
        }

        if (_rx_stream.time_valid) {
            timeNs = _rx_stream.remainderTimeNs +
                     samples_to_ns(_rx_stream.samplerate, _rx_stream.remainderOffset);
            flags |= SOAPY_SDR_HAS_TIME;
            if (timeNs < _rx_stream.last_time_ns) {
                timeNs = _rx_stream.last_time_ns;
            } else {
                _rx_stream.last_time_ns = timeNs;
            }
        } else if (_rx_stream.samplerate <= 0.0 && !_rx_stream.time_warned) {
            SoapySDR::log(SOAPY_SDR_WARNING,
                "RX sample rate not set; not providing SOAPY_SDR_HAS_TIME");
            _rx_stream.time_warned = true;
        }

        /* Read out channels from the remainder buffer. */
        for (size_t i = 0; i < _rx_stream.channels.size(); i++) {
            const uint32_t chan = _rx_stream.channels[i];
            this->deinterleave(
                _rx_stream.remainderBuff + (remainderOffset + chan * _bytesPerComplex),
                buffs[i],
                n,
                _rx_stream.format,
                0
            );
        }
        _rx_stream.remainderSamps -= n;
        _rx_stream.remainderOffset += n;

        if (_rx_stream.remainderSamps == 0) {
            this->releaseReadBuffer(stream, _rx_stream.remainderHandle);
            _rx_stream.remainderHandle = -1;
            _rx_stream.remainderOffset = 0;
        }

        if (n == returnedElems) {
            return returnedElems;
        }
    }

    /* Acquire a new read buffer from the DMA engine / UDP helper. */
    size_t handle;
    int ret = this->acquireReadBuffer(
        stream,
        handle,
        (const void **)&_rx_stream.remainderBuff,
        flags,
        timeNs,
        timeoutUs);

    if (ret < 0) {
        if ((ret == SOAPY_SDR_TIMEOUT) && (samp_avail > 0)) {
            return samp_avail;
        }
        return ret;
    }

    _rx_stream.remainderHandle = handle;
    _rx_stream.remainderSamps = ret;
    _rx_stream.remainderTimeNs = timeNs;

    const size_t n = std::min((returnedElems - samp_avail), _rx_stream.remainderSamps);

    if (_rx_stream.time_valid) {
        timeNs = _rx_stream.remainderTimeNs +
                 samples_to_ns(_rx_stream.samplerate, _rx_stream.remainderOffset);
        flags |= SOAPY_SDR_HAS_TIME;
        if (timeNs < _rx_stream.last_time_ns) {
            timeNs = _rx_stream.last_time_ns;
        } else {
            _rx_stream.last_time_ns = timeNs;
        }
    } else if (_rx_stream.samplerate <= 0.0 && !_rx_stream.time_warned) {
        SoapySDR::log(SOAPY_SDR_WARNING,
            "RX sample rate not set; not providing SOAPY_SDR_HAS_TIME");
        _rx_stream.time_warned = true;
    }

    /* Read out channels from the new buffer. */
    for (size_t i = 0; i < _rx_stream.channels.size(); i++) {
        const uint32_t chan = _rx_stream.channels[i];
        this->deinterleave(
            _rx_stream.remainderBuff + (chan * _bytesPerComplex),
            buffs[i],
            n,
            _rx_stream.format,
            samp_avail
        );
    }
    _rx_stream.remainderSamps -= n;
    _rx_stream.remainderOffset += n;

    if (_rx_stream.remainderSamps == 0) {
        this->releaseReadBuffer(stream, _rx_stream.remainderHandle);
        _rx_stream.remainderHandle = -1;
        _rx_stream.remainderOffset = 0;
    }

    return returnedElems;
}

/* Write to the TX stream. */
int SoapyLiteXM2SDR::writeStream(
    SoapySDR::Stream *stream,
    const void *const *buffs,
    const size_t numElems,
    int &flags,
    const long long timeNs,
    const long timeoutUs) {
    if (stream != TX_STREAM) {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    const size_t returnedElems = std::min(numElems, this->getStreamMTU(stream));
    const bool forceFlush = (flags & (SOAPY_SDR_END_BURST | SOAPY_SDR_ONE_PACKET)) != 0;

    /* Initialize/rebuild the software timeline before interpreting timed writes. */
    if (_tx_stream.timed_tx_enabled && _tx_stream.samplerate > 0.0) {
        refreshTimedTxDefaults();
        if (!_tx_stream.tx_timeline_valid)
            initTimedTxTimeline();
    } else if (_tx_stream.timed_tx_enabled && (flags & SOAPY_SDR_HAS_TIME)) {
        SoapySDR::log(SOAPY_SDR_ERROR,
            "TX timed write requested before the TX sample rate was configured");
        return SOAPY_SDR_STREAM_ERROR;
    }

    if (returnedElems == 0) {
        if (forceFlush) {
            if (flags & SOAPY_SDR_END_BURST)
                _tx_stream.remainderFlags |= SOAPY_SDR_END_BURST;
            int ret = submitTxRemainder(stream, true);
            if (ret < 0)
                return ret;
        }
        return 0;
    }

    /* For timed bursts, pad future gaps with zeros and reject bursts that are too late. */
    if (flags & SOAPY_SDR_HAS_TIME) {
        if (_tx_stream.timed_tx_enabled) {
            if (!_tx_stream.tx_timeline_valid)
                initTimedTxTimeline();
            if (!_tx_stream.tx_timeline_valid) {
                SoapySDR::log(SOAPY_SDR_ERROR,
                    "TX timed timeline could not be initialized");
                return SOAPY_SDR_STREAM_ERROR;
            }

            /* A gap can also mean the DMA ring ran dry (burst gap, or a stale
             * anchor from activation). The timeline can never be behind the
             * board clock, so re-anchor before padding when it is; otherwise
             * the gap zeros would replay air time the hardware already spent
             * stalled. Steady-state contiguous writes keep the gap below the
             * margin and skip the board-time query. */
            if (timeNs > _tx_stream.tx_next_time_ns +
                         _tx_stream.timed_tx_late_margin_ns) {
                try {
                    const long long hw_floor_ns =
                        this->getHardwareTime("") + _tx_stream.timed_tx_latency_ns;
                    if (_tx_stream.tx_next_time_ns < hw_floor_ns)
                        _tx_stream.tx_next_time_ns = hw_floor_ns;
                } catch (const std::exception &e) {
                    SoapySDR::logf(SOAPY_SDR_WARNING,
                        "TX timed write could not read board time: %s", e.what());
                }
            }

            if (timeNs + _tx_stream.timed_tx_late_margin_ns <
                _tx_stream.tx_next_time_ns) {
                _tx_stream.time_error = true;
                _tx_stream.status_time_ns = timeNs;
                SoapySDR::logf(SOAPY_SDR_WARNING,
                    "TX timed write is late: target=%lld next=%lld margin=%lld",
                    (long long)timeNs,
                    (long long)_tx_stream.tx_next_time_ns,
                    (long long)_tx_stream.timed_tx_late_margin_ns);
                return SOAPY_SDR_TIME_ERROR;
            }

            if (timeNs > _tx_stream.tx_next_time_ns) {
                const long long gap_ns = timeNs - _tx_stream.tx_next_time_ns;
                const size_t gap_samps =
                    ns_to_samples_ceil(_tx_stream.samplerate, gap_ns);
                int ret = appendTxZeros(stream, gap_samps, timeoutUs);
                if (ret < 0)
                    return ret;
            }

            int ret = ensureTxRemainderBuffer(stream, timeoutUs);
            if (ret < 0)
                return ret;
            markTxRemainderTime(_tx_stream.tx_next_time_ns);
        } else {
            int ret = ensureTxRemainderBuffer(stream, timeoutUs);
            if (ret < 0)
                return ret;
            markTxRemainderTime(timeNs);
        }
    }

    int ret = appendTxSamples(stream, buffs, returnedElems, 0, forceFlush, timeoutUs);
    if (ret < 0)
        return ret;

    const int accepted = ret;
    /* srsRAN can end a burst with less than one MTU; submit that tail now. */
    if (forceFlush && static_cast<size_t>(accepted) == returnedElems) {
        if (flags & SOAPY_SDR_END_BURST)
            _tx_stream.remainderFlags |= SOAPY_SDR_END_BURST;
        ret = submitTxRemainder(stream, true);
        if (ret < 0 && accepted == 0)
            return ret;
    }

    return accepted;
}

/* Check the status of the TX/RX streams. */
int SoapyLiteXM2SDR::readStreamStatus(
    SoapySDR::Stream *stream,
    size_t &chanMask,
    int &flags,
    long long &timeNs,
    const long timeoutUs){

    if (stream != RX_STREAM && stream != TX_STREAM)
        return SOAPY_SDR_NOT_SUPPORTED;

    /* Calculate the timeout duration and exit time. */
    const auto timeout = std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(std::chrono::microseconds(timeoutUs));
    const auto exitTime = std::chrono::high_resolution_clock::now() + timeout;

    /* Poll for status events until the timeout expires; events raised by the
     * streaming threads during the wait must be reported, not swallowed. */
    while (true) {
        if (stream == RX_STREAM) {
            if (_rx_stream.overflow) {
                _rx_stream.overflow = false;
                SoapySDR::log(SOAPY_SDR_SSI, "O");
                return SOAPY_SDR_OVERFLOW;
            }
        } else {
            chanMask = 0;
            for (size_t chan : _tx_stream.channels)
                chanMask |= (1u << chan);
            if (_tx_stream.time_error) {
                _tx_stream.time_error = false;
                flags |= SOAPY_SDR_HAS_TIME;
                timeNs = _tx_stream.status_time_ns;
                SoapySDR::log(SOAPY_SDR_SSI, "T");
                return SOAPY_SDR_TIME_ERROR;
            }
            if (_tx_stream.underflow) {
                _tx_stream.underflow = false;
                SoapySDR::log(SOAPY_SDR_SSI, "U");
                return SOAPY_SDR_UNDERFLOW;
            }
        }

        /* Check if the timeout has expired. */
        const auto timeNow = std::chrono::high_resolution_clock::now();
        if (exitTime < timeNow) return SOAPY_SDR_TIMEOUT;

        /* Sleep for a fraction of the total timeout. */
        const auto sleepTimeUs = std::max<long>(1, std::min<long>(1000, timeoutUs/10));
        std::this_thread::sleep_for(std::chrono::microseconds(sleepTimeUs));
    }
}
