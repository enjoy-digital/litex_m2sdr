/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2025 Enjoy Digital.
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

#include "liblitepcie.h"
#include "etherbone.h"
#include "libm2sdr.h"

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Time.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Types.hpp>

#if USE_LITEETH
extern "C" {
#include "liteeth_udp.h"
}
#endif

#define DEBUG

//#define _RX_DMA_HEADER_TEST
// #define _TX_DMA_HEADER_TEST

/* Thresholds above which we switch to 8-bit mode: */
#define LITEPCIE_8BIT_THRESHOLD  61.44e6
#define LITEETH_8BIT_THRESHOLD   20.0e6

#define DLL_EXPORT __attribute__ ((visibility ("default")))

#if USE_LITEPCIE
#define FD_INIT -1
#define litex_m2sdr_writel(_fd, _addr, _val) litepcie_writel(_fd, _addr, _val)
#define litex_m2sdr_readl(_fd, _addr) litepcie_readl(_fd, _addr)
typedef int litex_m2sdr_device_desc_t;
#elif USE_LITEETH
#define FD_INIT NULL
#define litex_m2sdr_writel(_fd, _addr, _val) eb_write32(_fd, _val, _addr)
#define litex_m2sdr_readl(_fd, _addr) eb_read32(_fd, _addr)
typedef struct eb_connection *litex_m2sdr_device_desc_t;
#endif

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
        const long long tx_timestamp = 0,
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
    /***************************************************************************************************
    *                                        Timestamp API
    **************************************************************************************************/
    void setTXTimestampMode(bool enable);
    bool getTXTimestampMode() const;
    uint64_t getTXSampleCount() const;
    uint64_t getTXBaseTimestamp() const;
    uint64_t getTXNextTimestamp() const;
    long long getRFICTime() const;
    void enableScheduler(bool enable) const;
    
    void enableHeader(bool enable) const;
    long long getHeaderLastTimestamp() const;
 


    /***********************************************************************************************
    *                                    Clocking API
    ***********************************************************************************************/

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
    SoapySDR::Stream *const TX_STREAM = (SoapySDR::Stream *)0x1;
    SoapySDR::Stream *const RX_STREAM = (SoapySDR::Stream *)0x2;

    struct litepcie_ioctl_mmap_dma_info _dma_mmap_info;
    void *_dma_buf;

    size_t _rx_buf_size = 0;
    size_t _tx_buf_size = 0;
    size_t _rx_buf_count = 0;
    size_t _tx_buf_count = 0;

#if USE_LITEETH
    struct liteeth_udp_ctrl _udp;
    bool _udp_inited = false;
#endif

    struct Stream {
        Stream() :
            opened(false),
            buf(nullptr),
            hw_count(0), sw_count(0), user_count(0),
            remainderHandle(-1), remainderSamps(0),
            remainderOffset(0), remainderBuff(nullptr),
            format() {}

        bool opened;
        void *buf;
        struct pollfd fds{};
        int64_t hw_count, sw_count, user_count;

        int32_t remainderHandle;
        size_t remainderSamps;
        size_t remainderOffset;
        int8_t* remainderBuff;
        std::string format;
        std::vector<size_t> channels;
#if USE_LITEPCIE
        struct litepcie_dma_ctrl dma;
#endif
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
    };

    struct TXStream: Stream {
        double gain[2]{};
        double iqbalance[2]{};
        double samplerate = 0.0;
        double bandwidth  = 0.0;
        double frequency  = 0.0;
        std::string antenna[2];

        bool underflow = false;

        bool   burst_end   = false;
        int32_t burst_samps = 0;
        // timestamp management
        bool timestamp_mode;
        uint64_t sample_count;
        uint64_t base_timestamp;
        uint64_t next_timestamp;
    };

    RXStream _rx_stream;
    TXStream _tx_stream;

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
