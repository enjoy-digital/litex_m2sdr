/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2025 Enjoy Digital.
 * Copyright (c) 2021 Julia Computing.
 * Copyright (c) 2015-2015 Fairwaves, Inc.
 * Copyright (c) 2015-2015 Rice University
 * SPDX-License-Identifier: Apache-2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <fcntl.h>
#include <unistd.h>

#include "LiteXM2SDRDevice.hpp"
#include "etherbone.h"

#include <SoapySDR/Registry.hpp>

/***********************************************************************
 * Find available devices
 **********************************************************************/

#define MAX_DEVICES 8
#define LITEX_IDENTIFIER_SIZE 256
#define LITEX_IDENTIFIER      "LiteX-M2SDR"

std::string readFPGAData(
    litex_m2sdr_device_desc_t fd,
    unsigned int baseAddr,
    size_t size) {
    std::string data(size, 0);
    for (size_t i = 0; i < size; i++)
        data[i] = static_cast<char>(litex_m2sdr_readl(fd, baseAddr + 4 * i));
    return data;
}

std::string getLiteXM2SDRIdentification(litex_m2sdr_device_desc_t fd) {
    return readFPGAData(fd, CSR_IDENTIFIER_MEM_BASE, LITEX_IDENTIFIER_SIZE);
}

std::string getLiteXM2SDRSerial(litex_m2sdr_device_desc_t fd) {
    unsigned int high = litex_m2sdr_readl(fd, CSR_DNA_ID_ADDR);
    unsigned int low  = litex_m2sdr_readl(fd, CSR_DNA_ID_ADDR + 4);
    char serial[32];
    snprintf(serial, sizeof(serial), "%x%08x", high, low);
    return std::string(serial);
}

std::string generateDeviceLabel(
    const SoapySDR::Kwargs &dev,
    const std::string &path) {
    std::string serialTrimmed = dev.at("serial").substr(dev.at("serial").find_first_not_of('0'));
    return dev.at("device") + " " + path + " " + serialTrimmed + " " + dev.at("identification");
}

SoapySDR::Kwargs createDeviceKwargs(
    litex_m2sdr_device_desc_t fd,
    const std::string &path,
    const std::string &eth_ip) {
    SoapySDR::Kwargs dev = {
        {"device",         "LiteX-M2SDR"},
#if USE_LITEETH
        {"eth_ip",         eth_ip},
#endif
        {"path",           path},
        {"serial",         getLiteXM2SDRSerial(fd)},
        {"identification", getLiteXM2SDRIdentification(fd)},
        {"version",        "1234"},
        {"label",          ""},
        {"bitmode",        "16"},
        {"oversampling",   "0"},
    };
    dev["label"] = generateDeviceLabel(dev, path);
    return dev;
}

std::vector<SoapySDR::Kwargs> findLiteXM2SDR(
    const SoapySDR::Kwargs &args) {
    std::vector<SoapySDR::Kwargs> discovered;

    std::string eth_ip = "192.168.1.50";
    std::string path   = "/dev/m2sdr0";

#if USE_LITEETH
    if (args.count("eth_ip") == 0) {
        std::cout << "When using ethernet mode eth_ip parameter is required\n";
        //throw std::runtime_error("plop");
    } else {
        eth_ip = args.at("eth_ip");
    }
#endif

#if USE_LITEPCIE
    auto attemptToAddDevice = [&](const std::string &path, const std::string &eth_ip) {
        int fd = open(path.c_str(), O_RDWR);
        if (fd < 0) return false;
        auto dev = createDeviceKwargs(fd, path, eth_ip);
        close(fd);

        if (dev["identification"].find(LITEX_IDENTIFIER) != std::string::npos) {
            discovered.push_back(std::move(dev));
            return true;
        }
        return false;
    };

    if (args.count("path") != 0) {
        attemptToAddDevice(args.at("path"), eth_ip);
    } else {
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (!attemptToAddDevice("/dev/m2sdr" + std::to_string(i), eth_ip))
                break; // Stop trying if a device fails to open, assuming sequential device numbering
        }
    }
#elif USE_LITEETH
    auto attemptToAddDevice = [&](const std::string &path, const std::string &eth_ip) {
        struct eb_connection *fd = eb_connect(eth_ip.c_str(), "1234", 1);
        if (!fd) {
            std::cerr << "Can't connect to EtherBone!\n";
            return false;
        }
        auto dev = createDeviceKwargs(fd, path, eth_ip);
        eb_disconnect(&fd);

        if (dev["identification"].find(LITEX_IDENTIFIER) != std::string::npos) {
            discovered.push_back(std::move(dev));
            return true;
        }
        return false;
    };

    attemptToAddDevice(path, eth_ip);
#endif
    return discovered;
}

/***********************************************************************
 * Make device instance
 **********************************************************************/

SoapySDR::Device *makeLiteXM2SDR(
    const SoapySDR::Kwargs &args) {
    return new SoapyLiteXM2SDR(args);
}

/***********************************************************************
 * Registration
 **********************************************************************/

static SoapySDR::Registry registerLiteXM2SDR(
    "LiteXM2SDR",
    &findLiteXM2SDR,
    &makeLiteXM2SDR,
    SOAPY_SDR_ABI_VERSION);
