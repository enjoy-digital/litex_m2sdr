/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2026 Enjoy Digital.
 * SPDX-License-Identifier: Apache-2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include <mutex>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <iostream>
#include <memory>
#include <map>
#include <vector>
#include <string>
#include <cstdint>
#include <chrono>

#include "m2sdr.h"

#include <SoapySDR/Constants.h>
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Time.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Types.hpp>

extern "C" {
#include "liteeth_udp.h"
}

enum class SoapyLiteXM2SDREthernetMode {
    UDP = 0,
    VRT = 1,
};

#define DEBUG

/* RX DMA headers are runtime-probed (rx_dma_header device arg); TX header
 * insertion remains experimental. */
//#define _TX_DMA_HEADER_TEST

/* Thresholds above which we switch to 8-bit mode: */
#define LITEPCIE_8BIT_THRESHOLD  61.44e6
#define LITEETH_8BIT_THRESHOLD   20.0e6

#define DLL_EXPORT __attribute__ ((visibility ("default")))

#define FD_INIT NULL
typedef void *litex_m2sdr_device_desc_t;

static inline uint32_t litex_m2sdr_readl(struct m2sdr_dev *dev, uint32_t addr)
{
    uint32_t val = 0;
    m2sdr_reg_read(dev, addr, &val);
    return val;
}

static inline void litex_m2sdr_writel(struct m2sdr_dev *dev, uint32_t addr, uint32_t val)
{
    m2sdr_reg_write(dev, addr, val);
}

class DLL_EXPORT SoapyLiteXM2SDR : public SoapySDR::Device {
 /**************************************************************************************************
 *                                        PUBLIC
 **************************************************************************************************/
  public:
    SoapyLiteXM2SDR(const SoapySDR::Kwargs &args);
    ~SoapyLiteXM2SDR(void);

    /***********************************************************************************************
    *                                 Channel configuration
    ***********************************************************************************************/
    void channel_configure(const int direction, const size_t channel);

    /***********************************************************************************************
    *                              Identification API
    ***********************************************************************************************/
    std::string getDriverKey(void) const override;
    std::string getHardwareKey(void) const override;

    /***********************************************************************************************
    *                                 Channels API
    ***********************************************************************************************/
    size_t getNumChannels(const int) const override;
    bool getFullDuplex(const int, const size_t) const override;

    /***********************************************************************************************
    *                                  Stream API
    ***********************************************************************************************/
    std::string getNativeStreamFormat(
        const int /*direction*/,
        const size_t /*channel*/,
        double &fullScale) const override {
        fullScale = 1.0;
        return SOAPY_SDR_CF32;
    }

    SoapySDR::Stream *setupStream(
        const int direction,
        const std::string &format,
        const std::vector<size_t> &channels,
        const SoapySDR::Kwargs &args) override;

    void closeStream(SoapySDR::Stream *stream) override;

    int activateStream(
        SoapySDR::Stream *stream,
        const int flags,
        const long long timeNs,
        const size_t numElems) override;

    int deactivateStream(
        SoapySDR::Stream *stream,
        const int flags,
        const long long timeNs) override;

    size_t getStreamMTU(SoapySDR::Stream *stream) const override;

    size_t getNumDirectAccessBuffers(SoapySDR::Stream *stream) override;

    int getDirectAccessBufferAddrs(
        SoapySDR::Stream *stream,
        const size_t handle,
        void **buffs) override;

    int acquireReadBuffer(
        SoapySDR::Stream *stream,
        size_t &handle,
        const void **buffs,
        int &flags,
        long long &timeNs,
        const long timeoutUs) override;

    void releaseReadBuffer(
        SoapySDR::Stream *stream,
        size_t handle) override;

    int acquireWriteBuffer(
        SoapySDR::Stream *stream,
        size_t &handle,
        void **buffs,
        const long timeoutUs) override;

    void releaseWriteBuffer(
        SoapySDR::Stream *stream,
        size_t handle,
        const size_t numElems,
        int &flags,
        const long long timeNs = 0) override;

    std::vector<std::string> getStreamFormats(
        const int direction,
        const size_t channel) const override;

    int readStream(
        SoapySDR::Stream *stream,
        void * const *buffs,
        const size_t numElems,
        int &flags,
        long long &timeNs,
        const long timeoutUs = 100000) override;

    int writeStream(
        SoapySDR::Stream *stream,
        const void * const *buffs,
        const size_t numElems,
        int &flags,
        const long long timeNs = 0,
        const long timeoutUs = 100000) override;

    int readStreamStatus(
        SoapySDR::Stream *stream,
        size_t &chanMask,
        int &flags,
        long long &timeNs,
        const long timeoutUs) override;

    /***********************************************************************************************
    *                                    Antenna API
    ***********************************************************************************************/
    std::vector<std::string> listAntennas(
        const int direction,
        const size_t channel) const override;

    void setAntenna(
        const int direction,
        const size_t channel,
        const std::string &name) override;

    std::string getAntenna(
        const int direction,
        const size_t channel) const override;

    std::map<int, std::map<size_t, std::string>> _cachedAntValues;

    /***********************************************************************************************
    *                               Frontend corrections API
    ***********************************************************************************************/
    bool hasDCOffsetMode(
        const int direction,
        const size_t channel) const override;

    /***********************************************************************************************
    *                                      Gain API
    ***********************************************************************************************/
    std::vector<std::string> listGains(
        const int direction,
        const size_t channel) const override;

    bool hasGainMode(
        const int direction,
        const size_t channel) const override;

    void setGainMode(const int direction,
        const size_t channel,
        const bool automatic) override;

    bool getGainMode(const int direction,
        const size_t channel) const override;

    void setGain(
        int direction,
        size_t channel,
        const double value) override;

    void setGain(
        const int direction,
        const size_t channel,
        const std::string &name,
        const double value) override;

    double getGain(
        const int direction,
        const size_t channel) const override;

    double getGain(
        const int direction,
        const size_t channel,
        const std::string &name) const override;

    SoapySDR::Range getGainRange(
        const int direction,
        const size_t channel) const override;

    SoapySDR::Range getGainRange(
        const int direction,
        const size_t channel,
        const std::string &name) const override;

    std::map<int, std::map<size_t, std::map<std::string, double>>> _cachedGainValues;

    /***********************************************************************************************
    *                                      Frequency API
    ***********************************************************************************************/
    void setFrequency(
        int direction,
        size_t channel,
        double frequency,
        const SoapySDR::Kwargs &args) override;

    void setFrequency(
        const int direction,
        const size_t channel,
        const std::string &,
        const double frequency,
        const SoapySDR::Kwargs &args = SoapySDR::Kwargs()) override;

    double getFrequency(
        const int direction,
        const size_t channel,
        const std::string &name) const override;

    std::vector<std::string> listFrequencies(
        const int,
        const size_t) const override;

    SoapySDR::RangeList getFrequencyRange(
        const int,
        const size_t,
        const std::string &) const override;

    SoapySDR::ArgInfoList getFrequencyArgsInfo(
        const int direction,
        const size_t channel) const override;

    std::map<int, std::map<size_t, std::map<std::string, double>>> _cachedFreqValues;

    /***********************************************************************************************
    *                                    Sample Rate  API
    ***********************************************************************************************/
    void setSampleRate(
        const int direction,
        const size_t,
        const double rate) override;

    double getSampleRate(
        const int direction,
        const size_t) const override;

    std::vector<double> listSampleRates(
        const int direction,
        const size_t channel) const override;

    SoapySDR::RangeList getSampleRateRange(
        const int direction,
        const size_t) const override;

    std::map<int, double> _cachedSampleRates;

    /***********************************************************************************************
    *                                    Bandwidth API
    ***********************************************************************************************/
    void setBandwidth(
        const int direction,
        const size_t channel,
        const double bw) override;

    double getBandwidth(
        const int direction,
        const size_t channel) const override;

    SoapySDR::RangeList getBandwidthRange(
        const int direction,
        const size_t channel) const override;

    /***********************************************************************************************
    *                                    Clocking API
    ***********************************************************************************************/
    std::vector<std::string> listTimeSources(void) const override;
    void setTimeSource(const std::string &source) override;
    std::string getTimeSource(void) const override;

    /***********************************************************************************************
    *                                     Time API
    ***********************************************************************************************/
    bool hasHardwareTime(const std::string &) const override;
    long long getHardwareTime(const std::string &) const override;
    void setHardwareTime(const long long timeNs, const std::string &) override;

    /***********************************************************************************************
    *                                    Sensor API
    ***********************************************************************************************/
    std::vector<std::string> listSensors(void) const override;

    SoapySDR::ArgInfo getSensorInfo(const std::string &key) const override;

    std::string readSensor(const std::string &key) const override;


 /**************************************************************************************************
 *                                        PRIVATE
 **************************************************************************************************/
  private:
    SoapySDR::Kwargs _deviceArgs;
    struct m2sdr_dev *_dev = nullptr;
    SoapySDR::Stream *const TX_STREAM = (SoapySDR::Stream *)0x1;
    SoapySDR::Stream *const RX_STREAM = (SoapySDR::Stream *)0x2;

    enum m2sdr_transport_kind _transport = M2SDR_TRANSPORT_KIND_UNKNOWN;
    int _pcie_fd = -1;

    size_t _rx_buf_size = 0;
    size_t _tx_buf_size = 0;
    size_t _rx_buf_count = 0;
    size_t _tx_buf_count = 0;
    size_t _rx_buf_stride = 0;
    size_t _tx_buf_stride = 0;

    /* RX DMA headers carry the per-buffer hardware timestamp; probed at
     * construction since older bitstreams lack the header module. */
    bool _rx_dma_header_supported = false;
    size_t _rx_dma_header_bytes = 0;

    struct liteeth_udp_ctrl _udp;
    bool _udp_inited = false;
    SoapyLiteXM2SDREthernetMode _eth_mode = SoapyLiteXM2SDREthernetMode::UDP;
    std::string _eth_ip;
    uint16_t _liteeth_rx_port = 2345;
    struct m2sdr_liteeth_rx_stream_config makeLiteEthRxStreamConfig() const;

    struct Stream {
        Stream() :
            opened(false),
            buf(nullptr),
            user_count(0),
            remainderHandle(-1), remainderSamps(0),
            remainderOffset(0), remainderBuff(nullptr),
            format() {}

        bool opened;
        void *buf;
        int64_t user_count;

        int32_t remainderHandle;
        size_t remainderSamps;
        size_t remainderOffset;
        int8_t* remainderBuff;
        std::string format;
        std::vector<size_t> channels;
    };

    struct RXStream: Stream {
        double gain[2]{};
        bool   gainMode[2]{};
        double iqbalance[2]{};
        double samplerate = 0.0;
        double bandwidth  = 0.0;
        double frequency  = 0.0;
        std::string antenna[2];

        bool overflow  = false;
        bool burst_end = false;
        bool time_valid = false;
        long long time0_ns = 0;
        int64_t time0_count = 0;
        long long remainderTimeNs = 0;
        long long last_time_ns = 0;
        bool time_warned = false;
        bool hw_time_warned = false;
        bool vrt_sequence_valid = false;
        uint8_t vrt_sequence_last = 0;
        uint64_t vrt_packets = 0;
        uint64_t vrt_sequence_gaps = 0;
        uint64_t vrt_packets_lost = 0;
        bool rx_timeout_recovery_armed = false;
        uint64_t rx_timeout_recoveries = 0;
        std::map<size_t, void *> pendingReadBufs;
    };

    struct TXStream: Stream {
        double gain[2]{};
        double iqbalance[2]{};
        double samplerate = 0.0;
        double bandwidth  = 0.0;
        double frequency  = 0.0;
        std::string antenna[2];

        bool underflow = false;
        bool time_error = false;
        long long status_time_ns = 0;

        bool   burst_end   = false;
        int32_t burst_samps = 0;
        std::map<size_t, uint8_t*> pendingWriteBufs;
        bool rate_pacing = true;
        /* Pacing follows the board clock through periodically refreshed
         * (host, board) anchor pairs so long runs do not drift with the
         * host oscillator. */
        std::chrono::steady_clock::time_point pace_host_anchor;
        long long pace_board_start_ns = 0;
        long long pace_board_anchor_ns = 0;
        bool pace_board_valid = false;
        uint64_t paced_buffers = 0;

        bool timed_tx_enabled = true;
        size_t timed_tx_lead_buffers = 0;
        long long timed_tx_latency_ns = 0;
        long long timed_tx_late_margin_ns = 0;
        bool timed_tx_late_margin_configured = false;
        bool tx_timeline_valid = false;
        long long tx_next_time_ns = 0;
        int remainderFlags = 0;
        long long remainderTimeNs = 0;
    };

    RXStream _rx_stream;
    TXStream _tx_stream;
    std::vector<std::string> _rx_antennas;
    std::vector<std::string> _tx_antennas;
    enum m2sdr_rx_gain_mode _rx_agc_mode = M2SDR_RX_GAIN_MODE_SLOW_ATTACK_AGC;
    std::string _ad9361_fir_profile = "legacy"; /* legacy | bypass | match | wide */
    bool _sampleRateHwApplied = false;
    int64_t _sampleRateHw = 0;
    uint32_t _sampleRateHwBitMode = 0;
    uint32_t _sampleRateHwOversampling = 0;
    std::string _sampleRateHwFirProfile;
    bool _bandwidthHwApplied = false;
    uint32_t _bandwidthHw = 0;
    bool _rxFrequencyHwApplied = false;
    bool _txFrequencyHwApplied = false;
    uint64_t _rxFrequencyHw = 0;
    uint64_t _txFrequencyHw = 0;
    bool _txAttHwApplied = false;
    int64_t _txAttHw = 0;
    bool _bitModeHwApplied = false;
    uint32_t _bitModeHw = 0;
    bool _channelModeHwApplied = false;
    uint32_t _channelModeHw = 0;
    uint32_t _rxChannelMaskHw = 0;
    uint32_t _txChannelMaskHw = 0;

    void invalidateRfHardwareCache();
    void resetDatapathUnlocked();
    void stopRxStreamUnlocked();
    void stopTxStreamUnlocked();
    void cleanupLiteEthUdpIfIdleUnlocked();
    void resetTimedTxTimeline();
    void refreshTimedTxDefaults();
    void initTimedTxTimeline();
    int ensureTxRemainderBuffer(
        SoapySDR::Stream *stream,
        const long timeoutUs);
    int submitTxRemainder(
        SoapySDR::Stream *stream,
        const bool force);
    int appendTxZeros(
        SoapySDR::Stream *stream,
        size_t numElems,
        const long timeoutUs);
    int appendTxSamples(
        SoapySDR::Stream *stream,
        const void *const *buffs,
        size_t numElems,
        size_t userOffset,
        bool holdLast,
        const long timeoutUs);
    void markTxRemainderTime(const long long payloadTimeNs);
    bool isLitePCIe() const {
        return _transport == M2SDR_TRANSPORT_KIND_LITEPCIE;
    }
    bool isLiteEth() const {
        return _transport == M2SDR_TRANSPORT_KIND_LITEETH;
    }

    void interleaveCF32(
        const void *src,
        void *dst,
        uint32_t len,
        size_t offset);

    void deinterleaveCF32(
        const void *src,
        void *dst,
        uint32_t len,
        size_t offset);

    void interleaveCS16(
        const void *src,
        void *dst,
        uint32_t len,
        size_t offset);

    void deinterleaveCS16(
        const void *src,
        void *dst,
        uint32_t len,
        size_t offset);

    void interleaveCS8(
        const void *src,
        void *dst,
        uint32_t len,
        size_t offset);

    void deinterleaveCS8(
        const void *src,
        void *dst,
        uint32_t len,
        size_t offset);

    void interleave(
        const void *src,
        void *dst,
        uint32_t len,
        const std::string &format,
        size_t offset);

    void deinterleave(
        const void *src,
        void *dst,
        uint32_t len,
        const std::string &format,
        size_t offset);

    void setSampleMode();

    const char *dir2Str(const int direction) const {
        return (direction == SOAPY_SDR_RX) ? "RX" : "TX";
    }

    litex_m2sdr_device_desc_t _fd;
    struct ad9361_rf_phy *ad9361_phy;
    uint8_t _spi_id = 0;

    uint32_t _bitMode           = 16;
    uint32_t _oversampling      = 0;
    uint32_t _nChannels         = 2;
    uint32_t _samplesPerComplex = 2;
    uint32_t _bytesPerSample    = 2;
    uint32_t _bytesPerComplex   = 4;
    float    _samplesScaling    = 2047.0f;
    float    _rateMult          = 1.0f;

    // register protection
    std::mutex _mutex;
};
