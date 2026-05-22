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

#include <fcntl.h>
#include <unistd.h>
#include <cctype>

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
    struct m2sdr_dev *dev,
    unsigned int baseAddr,
    size_t size) {
    std::string data(size, 0);
    for (size_t i = 0; i < size; i++)
        data[i] = static_cast<char>(litex_m2sdr_readl(dev, baseAddr + 4 * i));
    return data;
}


std::string getLiteXM2SDRIdentification(struct m2sdr_dev *dev)
{
    /* Read up to LITEX_IDENTIFIER_SIZE bytes from the FPGA */
    std::string data = readFPGAData(dev, CSR_IDENTIFIER_MEM_BASE, LITEX_IDENTIFIER_SIZE);

    /* Truncate at the first null terminator if present */
    size_t nullPos = data.find('\0');
    if (nullPos != std::string::npos)
    {
        data.resize(nullPos);
    }

    return data;
}

std::string getLiteXM2SDRSerial(struct m2sdr_dev *dev) {
    unsigned int high = litex_m2sdr_readl(dev, CSR_DNA_ID_ADDR + 0);
    unsigned int low  = litex_m2sdr_readl(dev, CSR_DNA_ID_ADDR + 4);
    char serial[32];
    snprintf(serial, sizeof(serial), "%x%08x", high, low);
    return std::string(serial);
}

std::string generateDeviceLabel(
    const SoapySDR::Kwargs &dev,
    const std::string &path) {
    std::string serialTrimmed = dev.at("serial").substr(dev.at("serial").find_first_not_of('0'));
    return dev.at("device") + " " + path + " " + serialTrimmed;
}

static bool isDecimalIndex(const std::string &value)
{
    if (value.empty())
        return false;
    for (char c : value) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

static std::string transportName(enum m2sdr_transport_kind transport)
{
    if (transport == M2SDR_TRANSPORT_KIND_LITEPCIE)
        return "pcie";
    if (transport == M2SDR_TRANSPORT_KIND_LITEETH)
        return "ethernet";
    return "unknown";
}

static std::string displayPath(const struct m2sdr_device_addr &addr)
{
    if (addr.transport == M2SDR_TRANSPORT_KIND_LITEPCIE)
        return addr.path;
    if (addr.transport == M2SDR_TRANSPORT_KIND_LITEETH)
        return addr.ip;
    return addr.identifier;
}

static bool resolveIdentifier(const std::string &candidate, struct m2sdr_device_addr &addr)
{
    return m2sdr_resolve_device_identifier(candidate.c_str(), &addr) == M2SDR_ERR_OK;
}

SoapySDR::Kwargs createDeviceKwargs(
    struct m2sdr_dev *m2sdr_dev,
    const struct m2sdr_device_addr &addr) {
    const std::string path = displayPath(addr);
    SoapySDR::Kwargs dev = {
        {"device",         "LiteX-M2SDR"},
        {"transport",      transportName(addr.transport)},
        {"dev_id",         addr.identifier},
        {"path",           path},
        {"serial",         getLiteXM2SDRSerial(m2sdr_dev)},
        {"identification", getLiteXM2SDRIdentification(m2sdr_dev)},
        {"version",        "1234"},
        {"label",          ""},
        {"oversampling",   "0"},
    };
    if (addr.transport == M2SDR_TRANSPORT_KIND_LITEETH)
        dev["eth_ip"] = addr.ip;
    dev["label"] = generateDeviceLabel(dev, path);
    return dev;
}

std::vector<SoapySDR::Kwargs> findLiteXM2SDR(
    const SoapySDR::Kwargs &args) {
    std::vector<SoapySDR::Kwargs> discovered;

    std::string eth_ip = "192.168.1.50";
    if (args.count("eth_ip") != 0)
        eth_ip = args.at("eth_ip");

    auto attemptToAddDevice = [&](const std::string &candidate) {
        struct m2sdr_dev *dev = nullptr;
        struct m2sdr_device_addr addr;

        if (!resolveIdentifier(candidate, addr))
            return false;

        if (m2sdr_open(&dev, addr.identifier) != 0) {
            return false;
        }
        auto dev_args = createDeviceKwargs(dev, addr);
        m2sdr_close(dev);

        if (dev_args["identification"].find(LITEX_IDENTIFIER) != std::string::npos) {
            discovered.push_back(std::move(dev_args));
            return true;
        }
        return false;
    };

    if (args.count("dev_id") != 0) {
        attemptToAddDevice(args.at("dev_id"));
        return discovered;
    }

    if (args.count("device") != 0) {
        std::string device = args.at("device");

        if (isDecimalIndex(device))
            device = "/dev/m2sdr" + device;
        attemptToAddDevice(device);
        return discovered;
    }

    if (args.count("eth_ip") != 0) {
        attemptToAddDevice("eth:" + eth_ip + ":1234");
    } else if (args.count("path") != 0) {
        attemptToAddDevice(args.at("path"));
    } else {
        for (int i = 0; i < MAX_DEVICES; i++) {
            const std::string path = "/dev/m2sdr" + std::to_string(i);
            if (!attemptToAddDevice(path))
                break;
        }
    }
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
