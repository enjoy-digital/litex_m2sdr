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

static bool startsWith(const std::string &value, const char *prefix)
{
    return value.rfind(prefix, 0) == 0;
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

static std::string ethIpFromIdentifier(const std::string &dev_id, const std::string &fallback)
{
    if (!startsWith(dev_id, "eth:"))
        return fallback;

    const size_t start = 4;
    const size_t end = dev_id.find(':', start);
    if (end == std::string::npos || end <= start)
        return fallback;

    return dev_id.substr(start, end - start);
}

SoapySDR::Kwargs createDeviceKwargs(
    struct m2sdr_dev *m2sdr_dev,
    const std::string &path,
    const std::string &eth_ip,
    const std::string &dev_id,
    enum m2sdr_transport_kind transport) {
    SoapySDR::Kwargs dev = {
        {"device",         "LiteX-M2SDR"},
        {"transport",      transportName(transport)},
        {"dev_id",         dev_id},
        {"path",           path},
        {"serial",         getLiteXM2SDRSerial(m2sdr_dev)},
        {"identification", getLiteXM2SDRIdentification(m2sdr_dev)},
        {"version",        "1234"},
        {"label",          ""},
        {"oversampling",   "0"},
    };
    if (transport == M2SDR_TRANSPORT_KIND_LITEETH)
        dev["eth_ip"] = eth_ip;
    dev["label"] = generateDeviceLabel(dev, path);
    return dev;
}

std::vector<SoapySDR::Kwargs> findLiteXM2SDR(
    const SoapySDR::Kwargs &args) {
    std::vector<SoapySDR::Kwargs> discovered;

    std::string eth_ip = "192.168.1.50";
    if (args.count("eth_ip") != 0)
        eth_ip = args.at("eth_ip");

    auto attemptToAddDevice = [&](const std::string &dev_id,
                                  const std::string &path,
                                  const std::string &eth_ip) {
        struct m2sdr_dev *dev = nullptr;
        enum m2sdr_transport_kind transport = M2SDR_TRANSPORT_KIND_UNKNOWN;

        if (m2sdr_open(&dev, dev_id.c_str()) != 0) {
            return false;
        }
        if (m2sdr_get_transport(dev, &transport) != 0)
            transport = M2SDR_TRANSPORT_KIND_UNKNOWN;
        auto dev_args = createDeviceKwargs(dev, path, eth_ip, dev_id, transport);
        m2sdr_close(dev);

        if (dev_args["identification"].find(LITEX_IDENTIFIER) != std::string::npos) {
            discovered.push_back(std::move(dev_args));
            return true;
        }
        return false;
    };

    if (args.count("dev_id") != 0) {
        std::string dev_id = args.at("dev_id");

        if (startsWith(dev_id, "pcie:")) {
            attemptToAddDevice(dev_id, dev_id.substr(5), eth_ip);
        } else if (startsWith(dev_id, "eth:")) {
            const std::string parsed_eth_ip = ethIpFromIdentifier(dev_id, eth_ip);
            attemptToAddDevice(dev_id, parsed_eth_ip, parsed_eth_ip);
        }
        return discovered;
    }

    if (args.count("device") != 0) {
        std::string device = args.at("device");

        if (startsWith(device, "pcie:")) {
            attemptToAddDevice(device, device.substr(5), eth_ip);
        } else if (startsWith(device, "eth:")) {
            const std::string parsed_eth_ip = ethIpFromIdentifier(device, eth_ip);
            attemptToAddDevice(device, parsed_eth_ip, parsed_eth_ip);
        } else if (startsWith(device, "/dev/m2sdr")) {
            attemptToAddDevice("pcie:" + device, device, eth_ip);
        } else if (isDecimalIndex(device)) {
            std::string path = "/dev/m2sdr" + device;
            attemptToAddDevice("pcie:" + path, path, eth_ip);
        }
        return discovered;
    }

    if (args.count("eth_ip") != 0) {
        attemptToAddDevice("eth:" + eth_ip + ":1234", eth_ip, eth_ip);
    } else if (args.count("path") != 0) {
        const std::string path = args.at("path");
        attemptToAddDevice("pcie:" + path, path, eth_ip);
    } else {
        for (int i = 0; i < MAX_DEVICES; i++) {
            const std::string path = "/dev/m2sdr" + std::to_string(i);
            if (!attemptToAddDevice("pcie:" + path, path, eth_ip))
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
