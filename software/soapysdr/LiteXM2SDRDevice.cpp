//
// SoapySDR driver for the LiteX M2SDR.
//
// Copyright (c) 2021-2024 Enjoy Digital.
// SPDX-License-Identifier: Apache-2.0
// http://www.apache.org/licenses/LICENSE-2.0
//

#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <memory>
#include <sys/mman.h>

#include "LiteXM2SDRDevice.hpp"

#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Logger.hpp>

/***************************************************************************************************
 *                                     Constructor
 **************************************************************************************************/

std::string getLiteXM2SDRSerial(int fd);
std::string getLiteXM2SDRIdentification(int fd);

void dma_set_loopback(int fd, bool loopback_enable) {
    struct litepcie_ioctl_dma m;
    m.loopback_enable = loopback_enable ? 1 : 0;
    checked_ioctl(fd, LITEPCIE_IOCTL_DMA, &m);
}

SoapyLiteXM2SDR::SoapyLiteXM2SDR(const SoapySDR::Kwargs &args)
    : _fd(-1),
    _masterClockRate(1.0e6), _refClockRate(26e6) {
    SoapySDR::logf(SOAPY_SDR_INFO, "SoapyLiteXM2SDR initializing...");
    setvbuf(stdout, NULL, _IOLBF, 0);

    // open LitePCIe descriptor
    if (args.count("path") == 0) {
        // if path is not present, then findLiteXM2SDR had zero devices enumerated
        throw std::runtime_error("No LitePCIe devices found!");
    }
    std::string path = args.at("path");
    _fd = open(path.c_str(), O_RDWR);
    if (_fd < 0)
        throw std::runtime_error("SoapyLiteXM2SDR(): failed to open " + path);

    SoapySDR::logf(SOAPY_SDR_INFO, "Opened devnode %s, serial %s", path.c_str(), getLiteXM2SDRSerial(_fd).c_str());

    /* bypass synchro */
    litepcie_writel(_fd, CSR_PCIE_DMA0_SYNCHRONIZER_BYPASS_ADDR, 1);

    // set clock to Internal Reference Clock
    this->setClockSource("internal");

    this->setMasterClockRate(64e6*2);

    // some defaults to avoid throwing
    _cachedSampleRates[SOAPY_SDR_RX] = 30.72e6;
    _cachedSampleRates[SOAPY_SDR_TX] = 30.72e6;

    /* TX/RX 1 */
    _cachedFreqValues[SOAPY_SDR_RX][0]["RF"] = 1e9;
    _cachedFreqValues[SOAPY_SDR_TX][0]["RF"] = 1e9;
    this->setAntenna(SOAPY_SDR_RX,   0, "A_BALANCED");
    this->setAntenna(SOAPY_SDR_TX,   0, "A");
    this->setBandwidth(SOAPY_SDR_RX, 0, 30.72e6);
    this->setBandwidth(SOAPY_SDR_TX, 0, 30.72e6);

    /* TX/RX 2 */
    _cachedFreqValues[SOAPY_SDR_RX][1]["RF"] = 1e9;
    _cachedFreqValues[SOAPY_SDR_TX][1]["RF"] = 1e9;
    this->setAntenna(SOAPY_SDR_RX,   1, "A_BALANCED");
    this->setAntenna(SOAPY_SDR_TX,   1, "A");
    this->setBandwidth(SOAPY_SDR_RX, 1, 30.72e6);
    this->setBandwidth(SOAPY_SDR_TX, 1, 30.72e6);

    this->setGain(SOAPY_SDR_RX, 0, false);
    this->setGain(SOAPY_SDR_RX, 1, false);

    this->setFrequency(SOAPY_SDR_RX, 0, "BB", 0.0);
    this->setFrequency(SOAPY_SDR_RX, 0, "BB", 0.0);
    this->setFrequency(SOAPY_SDR_TX, 1, "BB", 0.0);
    this->setFrequency(SOAPY_SDR_TX, 1, "BB", 0.0);

    this->setIQBalance(SOAPY_SDR_RX, 0, 1.0);
    this->setIQBalance(SOAPY_SDR_RX, 1, 1.0);
    this->setIQBalance(SOAPY_SDR_TX, 0, 1.0);
    this->setIQBalance(SOAPY_SDR_TX, 1, 1.0);

    // set-up the DMA
    checked_ioctl(_fd, LITEPCIE_IOCTL_MMAP_DMA_INFO, &_dma_mmap_info);
    _dma_buf = NULL;

    SoapySDR::log(SOAPY_SDR_INFO, "SoapyLiteXM2SDR initialization complete");
}

SoapyLiteXM2SDR::~SoapyLiteXM2SDR(void) {
    SoapySDR::log(SOAPY_SDR_INFO, "Power down and cleanup");
    if (_rx_stream.opened) {
        litepcie_release_dma(_fd, 0, 1);

            munmap(_rx_stream.buf, _dma_mmap_info.dma_rx_buf_size *
                                    _dma_mmap_info.dma_rx_buf_count);
        _rx_stream.opened = false;
    }
    if (_tx_stream.opened) {
        // release the DMA engine
        litepcie_release_dma(_fd, 1, 0);

        munmap(_tx_stream.buf, _dma_mmap_info.dma_tx_buf_size *
                                   _dma_mmap_info.dma_tx_buf_count);
        _tx_stream.opened = false;
    }

    close(_fd);
}


/***************************************************************************************************
 *                                  Identification API
 **************************************************************************************************/

SoapySDR::Kwargs SoapyLiteXM2SDR::getHardwareInfo(void) const {
    SoapySDR::Kwargs args;
    args["identification"] = getLiteXM2SDRIdentification(_fd);
    return args;
}

/***************************************************************************************************
 *                                     Antenna API
 **************************************************************************************************/

std::vector<std::string> SoapyLiteXM2SDR::listAntennas(const int direction,
                                                 const size_t) const {
    std::vector<std::string> ants;
    if(direction == SOAPY_SDR_RX) ants.push_back( "A_BALANCED" );
    if(direction == SOAPY_SDR_TX) ants.push_back( "A" );
    return ants;
}

void SoapyLiteXM2SDR::setAntenna(const int direction, const size_t channel,
                           const std::string &name) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (direction == SOAPY_SDR_RX) {
    }
    if (direction == SOAPY_SDR_TX) {
    }
    _cachedAntValues[direction][channel] = name;
}

std::string SoapyLiteXM2SDR::getAntenna(const int direction,
                                  const size_t channel) const {
    printf("here1");
    return _cachedAntValues.at(direction).at(channel);
}


/***************************************************************************************************
 *                                 Frontend corrections API
 **************************************************************************************************/

bool SoapyLiteXM2SDR::hasDCOffsetMode(const int /*direction*/,
                                const size_t /*channel*/) const {
    return false;
}

/***************************************************************************************************
 *                                           Gain API
 **************************************************************************************************/

std::vector<std::string> SoapyLiteXM2SDR::listGains(const int /*direction*/,
                                              const size_t) const {
    std::vector<std::string> gains;
    gains.push_back("PGA");
    return gains;
}

bool SoapyLiteXM2SDR::hasGainMode(const int direction, const size_t /*channel*/) const
{
    if (direction == SOAPY_SDR_RX)
        return true;
    return false;
}

void SoapyLiteXM2SDR::setGain(int direction, size_t channel, const double value) {
    std::lock_guard<std::mutex> lock(_mutex);
    SoapySDR::logf(SOAPY_SDR_DEBUG, "SoapyLiteXM2SDR::setGain(%s, ch%d, %f dB)",
                   dir2Str(direction), channel, value);

    _cachedFreqValues[direction][channel]["PGA"] = value;

    //if (SOAPY_SDR_TX == direction) {
    //} else {
    //}
}

void SoapyLiteXM2SDR::setGain(const int direction, const size_t channel,
                        const std::string &name, const double value) {
    std::lock_guard<std::mutex> lock(_mutex);
    SoapySDR::logf(SOAPY_SDR_DEBUG, "SoapyLiteXM2SDR::setGain(%s, ch%d, %s, %f dB)",
                   dir2Str(direction), channel, name.c_str(), value);
    _cachedFreqValues[direction][channel][name] = value;
}

double SoapyLiteXM2SDR::getGain(const int direction, const size_t channel,
                          const std::string &name) const {

    return 0;
    printf("here2");
    //printf("%lf", _cachedGainValues.at(direction).at(channel).at(name));
    printf("here2.5");
    //return _cachedGainValues.at(direction).at(channel).at(name);
}

SoapySDR::Range SoapyLiteXM2SDR::getGainRange(const int direction,
                                        const size_t /*channel*/,
                                        const std::string &/*name*/) const {
	if(direction==SOAPY_SDR_RX)
        return(SoapySDR::Range(0, 73));
    return(SoapySDR::Range(0,89));
}


/***************************************************************************************************
 *                                     Frequency API
 **************************************************************************************************/

void SoapyLiteXM2SDR::setFrequency(int direction, size_t channel, double frequency,
                             const SoapySDR::Kwargs &args) {
    setFrequency(direction, channel, "RF", frequency, args);
}

void SoapyLiteXM2SDR::setFrequency(const int direction, const size_t channel,
                             const std::string &name, const double frequency,
                             const SoapySDR::Kwargs &/*args*/) {
    std::unique_lock<std::mutex> lock(_mutex);

    SoapySDR::logf(SOAPY_SDR_DEBUG,
                   "SoapyLiteXM2SDR::setFrequency(%s, ch%d, %s, %f MHz)",
                   dir2Str(direction), channel, name.c_str(), frequency / 1e6);
}

double SoapyLiteXM2SDR::getFrequency(const int direction, const size_t channel,
                               const std::string &name) const {
    printf("here3");
    return _cachedFreqValues.at(direction).at(channel).at(name);
}

std::vector<std::string> SoapyLiteXM2SDR::listFrequencies(const int /*direction*/,
                                                    const size_t /*channel*/) const {
    std::vector<std::string> opts;
    opts.push_back("RF");
    return opts;
}

SoapySDR::RangeList
SoapyLiteXM2SDR::getFrequencyRange(const int /*direction*/, const size_t /*channel*/,
                             const std::string &/*name*/) const
{
	return(SoapySDR::RangeList(1, SoapySDR::Range(70000000, 6000000000ull)));
}


/***************************************************************************************************
 *                                        Sample Rate API
 **************************************************************************************************/

void SoapyLiteXM2SDR::setSampleRate(const int direction, const size_t channel,
                              const double rate) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string dirName ((direction == SOAPY_SDR_RX) ? "Rx" : "Tx");
    SoapySDR::logf(SOAPY_SDR_DEBUG, "setSampleRate(%s, %ld, %g MHz)", dirName, channel, rate / 1e6);

    _cachedSampleRates[direction] = rate;
}

double SoapyLiteXM2SDR::getSampleRate(const int direction, const size_t) const {
    printf("here4");
    return _cachedSampleRates.at(direction);
}

std::vector<double> SoapyLiteXM2SDR::listSampleRates(const int /*direction*/,
		const size_t /*channel*/) const
{
    std::vector<double> options;

    options.push_back(65105);//25M/48/8+1
	for (int i = 0; i <= 10; i++)
    	options.push_back(static_cast<double>(i) * 1e6);
    return(options);
}

SoapySDR::RangeList SoapyLiteXM2SDR::getSampleRateRange(const int ,
                                               const size_t) const
{
	SoapySDR::RangeList results;
    results.push_back(SoapySDR::Range(25e6 / 96, 61440000));
	return results;
}

std::vector<std::string> SoapyLiteXM2SDR::getStreamFormats(const int /*direction*/,
                                                     const size_t /*channel*/) const
{
    std::vector<std::string> formats;
    formats.push_back(SOAPY_SDR_CS16);
    formats.push_back(SOAPY_SDR_CF32);
    return formats;
}

/***************************************************************************************************
 *                                        BW filter API
 **************************************************************************************************/

void SoapyLiteXM2SDR::setBandwidth(const int direction, const size_t channel,
                             const double bw) {
    if (bw == 0.0)
        return;
    double &actualBw = _cachedFilterBws[direction][channel];
    double lpf = bw;
    actualBw = lpf;
}

double SoapyLiteXM2SDR::getBandwidth(const int direction,
                               const size_t channel) const {
    printf("here5");
    return _cachedFilterBws.at(direction).at(channel);
}

std::vector<double> SoapyLiteXM2SDR::listBandwidths(const int /*direction*/,
                                              const size_t) const {
    std::vector<double> bws;
	bws.push_back(0.2e6);
	for (int i = 1; i < 11; i++)
		bws.push_back(static_cast<double>(i) * 1e6);

    return bws;
}


/***************************************************************************************************
 *                                        Clocking API
 **************************************************************************************************/

double SoapyLiteXM2SDR::getTSPRate(const int direction) const {
    return (direction == SOAPY_SDR_TX) ? _masterClockRate
                                       : _masterClockRate / 4;
}

void SoapyLiteXM2SDR::setMasterClockRate(const double rate) {
    std::lock_guard<std::mutex> lock(_mutex);

    _masterClockRate = rate;
    SoapySDR::logf(SOAPY_SDR_TRACE, "LMS7002M_set_data_clock(%f MHz) -> %f MHz",
                   rate / 1e6, _masterClockRate / 1e6);
}

double SoapyLiteXM2SDR::getMasterClockRate(void) const { return _masterClockRate; }

/*!
 * Set the reference clock rate of the device.
 * \param rate the clock rate in Hz
 */
void SoapyLiteXM2SDR::setReferenceClockRate(const double rate) {
    _refClockRate = rate;
}

/*!
 * Get the reference clock rate of the device.
 * \return the clock rate in Hz
 */
double SoapyLiteXM2SDR::getReferenceClockRate(void) const { return _refClockRate; }

/*!
 * Get the range of available reference clock rates.
 * \return a list of clock rate ranges in Hz
 */
SoapySDR::RangeList SoapyLiteXM2SDR::getReferenceClockRates(void) const {
    SoapySDR::RangeList ranges;
    // Really whatever you want to try...
    ranges.push_back(SoapySDR::Range(25e6, 27e6));
    return ranges;
}



/*!
 * Get the list of available clock sources.
 * \return a list of clock source names
 */
std::vector<std::string> SoapyLiteXM2SDR::listClockSources(void) const {
    std::vector<std::string> sources;
    sources.push_back("internal");
    sources.push_back("external");
    return sources;
}

/*!
 * Set the clock source on the device
 * \param source the name of a clock source
 */
void SoapyLiteXM2SDR::setClockSource(const std::string &source) {
	(void)source;
}

/*!
 * Get the clock source of the device
 * \return the name of a clock source
 */
std::string SoapyLiteXM2SDR::getClockSource(void) const {
	return "internal";
}

/***************************************************************************************************
 *                                  Sensors API
 **************************************************************************************************/

std::vector<std::string> SoapyLiteXM2SDR::listSensors(void) const {
    std::vector<std::string> sensors;
#ifdef CSR_XADC_BASE
    sensors.push_back("xadc_temp");
    sensors.push_back("xadc_vccint");
    sensors.push_back("xadc_vccaux");
    sensors.push_back("xadc_vccbram");
#endif
    return sensors;
}

SoapySDR::ArgInfo SoapyLiteXM2SDR::getSensorInfo(const std::string &key) const {
    SoapySDR::ArgInfo info;

    std::size_t dash = key.find("_");
    if (dash < std::string::npos) {
        std::string deviceStr = key.substr(0, dash);
        std::string sensorStr = key.substr(dash + 1);

#ifdef CSR_XADC_BASE
        if (deviceStr == "xadc") {
            /* Temp */
            if (sensorStr == "temp") {
                info.key         = "temp";
                info.value       = "0.0";
                info.units       = "C";
                info.description = "FPGA temperature";
                info.type        = SoapySDR::ArgInfo::FLOAT;
            /* VCCINT */
            } else if (sensorStr == "vccint") {
                info.key         = "vccint";
                info.value       = "0.0";
                info.units       = "V";
                info.description = "FPGA internal supply voltage";
                info.type        = SoapySDR::ArgInfo::FLOAT;
            /* VCCAUX */
            } else if (sensorStr == "vccaux") {
                info.key         = "vccaux";
                info.value       = "0.0";
                info.units       = "V";
                info.description = "FPGA auxiliary supply voltage";
                info.type        = SoapySDR::ArgInfo::FLOAT;
            /* VCCBRAM */
            } else if (sensorStr == "vccbram") {
                info.key         = "vccbram";
                info.value       = "0.0";
                info.units       = "V";
                info.description = "FPGA supply voltage for block RAM memories";
                info.type        = SoapySDR::ArgInfo::FLOAT;
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

std::string SoapyLiteXM2SDR::readSensor(const std::string &key) const {
    std::string sensorValue;

    std::size_t dash = key.find("_");
    if (dash < std::string::npos) {
        std::string deviceStr = key.substr(0, dash);
        std::string sensorStr = key.substr(dash + 1);

#ifdef CSR_XADC_BASE
        if (deviceStr == "xadc") {
            /* Temp */
            if (sensorStr == "temp") {
                sensorValue = std::to_string(
                    (double)litepcie_readl(_fd, CSR_XADC_TEMPERATURE_ADDR) * 503.975 / 4096 - 273.15
                );
            /* VCCINT */
            } else if (sensorStr == "vccint") {
                sensorValue = std::to_string(
                    (double)litepcie_readl(_fd, CSR_XADC_VCCINT_ADDR) / 4096 * 3
                );
            /* VCCAUX */
            } else if (sensorStr == "vccaux") {
                sensorValue = std::to_string(
                    (double)litepcie_readl(_fd, CSR_XADC_VCCAUX_ADDR) / 4096 * 3
                );
            /* VCCBRAM */
            } else if (sensorStr == "vccbram") {
                sensorValue = std::to_string(
                    (double)litepcie_readl(_fd, CSR_XADC_VCCBRAM_ADDR) / 4096 * 3
                );
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
