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
#include <cstdlib>
#include <sstream>

#include "LiteXM2SDRDevice.hpp"

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

static bool stringFlagEnabled(const std::string &value)
{
    return !(value == "0" || value == "false" || value == "off" || value == "none");
}

static std::vector<std::string> splitList(const std::string &value)
{
    std::vector<std::string> items;
    std::string normalized = value;
    std::stringstream ss;
    std::string item;

    for (char &c : normalized) {
        if (c == ';')
            c = ',';
    }
    ss.str(normalized);

    while (std::getline(ss, item, ',')) {
        size_t first = item.find_first_not_of(" \t\r\n");
        size_t last = item.find_last_not_of(" \t\r\n");

        if (first == std::string::npos)
            continue;
        items.push_back(item.substr(first, last - first + 1));
    }

    return items;
}

static std::string makeEthernetIdentifier(const std::string &target,
                                          const std::string &default_port)
{
    if (target.rfind("eth:", 0) == 0)
        return target;
    if (target.find(':') != std::string::npos)
        return "eth:" + target;
    return "eth:" + target + ":" + default_port;
}

static std::vector<std::string> ethernetDiscoveryTargets(const SoapySDR::Kwargs &args)
{
    std::string targets;
    const char *env_targets = std::getenv("LITEXM2SDR_ETH_IPS");

    if (args.count("eth_ips") != 0)
        targets = args.at("eth_ips");
    else if (env_targets && env_targets[0] != '\0')
        targets = env_targets;
    else
        targets = "192.168.1.50";

    return splitList(targets);
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
    std::string eth_port = "1234";
    if (args.count("eth_port") != 0)
        eth_port = args.at("eth_port");
    else if (args.count("port") != 0)
        eth_port = args.at("port");

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
            for (const auto &existing : discovered) {
                if (existing.count("dev_id") && existing.at("dev_id") == dev_args["dev_id"])
                    return true;
            }
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
        attemptToAddDevice(makeEthernetIdentifier(eth_ip, eth_port));
    } else if (args.count("path") != 0) {
        attemptToAddDevice(args.at("path"));
    } else {
        for (int i = 0; i < MAX_DEVICES; i++) {
            const std::string path = "/dev/m2sdr" + std::to_string(i);
            if (!attemptToAddDevice(path))
                break;
        }
        if (args.count("eth_discovery") == 0 || stringFlagEnabled(args.at("eth_discovery"))) {
            for (const auto &target : ethernetDiscoveryTargets(args))
                attemptToAddDevice(makeEthernetIdentifier(target, eth_port));
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
