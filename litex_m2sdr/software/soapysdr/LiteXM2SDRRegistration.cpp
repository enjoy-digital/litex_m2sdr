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

#include <cctype>
#include <cstdlib>
#include <stdexcept>

#include "LiteXM2SDRDevice.hpp"

#include <SoapySDR/Registry.hpp>

/***********************************************************************
 * Find available devices
 **********************************************************************/

#define LITEX_IDENTIFIER      "LiteX-M2SDR"

std::string getLiteXM2SDRIdentification(struct m2sdr_dev *dev)
{
    struct m2sdr_devinfo info = {};

    if (m2sdr_get_device_info(dev, &info) != M2SDR_ERR_OK)
        return "";

    return info.identification;
}

std::string getLiteXM2SDRSerial(struct m2sdr_dev *dev) {
    struct m2sdr_devinfo info = {};

    if (m2sdr_get_device_info(dev, &info) != M2SDR_ERR_OK)
        return "";

    return info.serial;
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

static std::string makeEthernetIdentifier(const std::string &target,
                                          const std::string &default_port)
{
    if (target.rfind("eth:", 0) == 0)
        return target;
    if (target.find(':') != std::string::npos)
        return "eth:" + target;
    return "eth:" + target + ":" + default_port;
}

static std::string ethernetDiscoveryTargets(const SoapySDR::Kwargs &args)
{
    const char *env_targets = std::getenv("LITEXM2SDR_ETH_IPS");

    if (args.count("eth_ips") != 0)
        return args.at("eth_ips");
    else if (env_targets && env_targets[0] != '\0')
        return env_targets;
    return "192.168.1.50";
}

static uint16_t parsePort(const std::string &value)
{
    size_t end = 0;
    unsigned long port = std::stoul(value, &end, 10);

    if (end != value.size() || port == 0 || port > 65535)
        throw std::runtime_error("Invalid Ethernet port: " + value);
    return static_cast<uint16_t>(port);
}

SoapySDR::Kwargs createDeviceKwargs(
    struct m2sdr_dev *m2sdr_dev,
    const struct m2sdr_device_addr &addr) {
    const std::string path = displayPath(addr);
    struct m2sdr_devinfo info = {};
    (void)m2sdr_get_device_info(m2sdr_dev, &info);
    SoapySDR::Kwargs dev = {
        {"device",         "LiteX-M2SDR"},
        {"transport",      transportName(addr.transport)},
        {"dev_id",         addr.identifier},
        {"path",           path},
        {"serial",         info.serial},
        {"identification", info.identification},
        {"version",        "1234"},
        {"label",          ""},
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
        struct m2sdr_discovery_config discovery_config;
        struct m2sdr_device_addr targets[64];
        size_t target_count = 0;
        std::string target_list = ethernetDiscoveryTargets(args);

        m2sdr_discovery_config_init(&discovery_config);
        discovery_config.enable_liteeth =
            args.count("eth_discovery") == 0 || stringFlagEnabled(args.at("eth_discovery"));
        discovery_config.liteeth_targets = target_list.c_str();
        discovery_config.liteeth_port = parsePort(eth_port);

        if (m2sdr_get_discovery_targets(&discovery_config,
                                        targets,
                                        sizeof(targets) / sizeof(targets[0]),
                                        &target_count) == M2SDR_ERR_OK) {
            for (size_t i = 0; i < target_count; i++)
                attemptToAddDevice(targets[i].identifier);
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
