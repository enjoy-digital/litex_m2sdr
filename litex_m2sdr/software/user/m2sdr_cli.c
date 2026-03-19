/* SPDX-License-Identifier: BSD-2-Clause */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "m2sdr_cli.h"

void m2sdr_cli_device_init(struct m2sdr_cli_device *dev)
{
    if (!dev)
        return;

    memset(dev, 0, sizeof(*dev));
#ifdef USE_LITEPCIE
    dev->device_num = 0;
#elif defined(USE_LITEETH)
    strncpy(dev->ip_address, "192.168.1.50", sizeof(dev->ip_address) - 1);
    strncpy(dev->port, "1234", sizeof(dev->port) - 1);
#endif
}

int m2sdr_cli_handle_device_option(struct m2sdr_cli_device *dev, int opt, const char *optarg)
{
    if (!dev || !optarg)
        return -1;

    if (opt == 'd')
        return m2sdr_cli_set_device_id(dev, optarg);

#ifdef USE_LITEPCIE
    if (opt == 'c') {
        dev->device_num = atoi(optarg);
        return 0;
    }
#elif defined(USE_LITEETH)
    if (opt == 'i') {
        strncpy(dev->ip_address, optarg, sizeof(dev->ip_address) - 1);
        dev->ip_address[sizeof(dev->ip_address) - 1] = '\0';
        return 0;
    }
    if (opt == 'p') {
        strncpy(dev->port, optarg, sizeof(dev->port) - 1);
        dev->port[sizeof(dev->port) - 1] = '\0';
        return 0;
    }
#endif
    return -1;
}

int m2sdr_cli_set_device_id(struct m2sdr_cli_device *dev, const char *device_id)
{
    if (!dev || !device_id)
        return -1;

    if (snprintf(dev->device_id, sizeof(dev->device_id), "%s", device_id) >= (int)sizeof(dev->device_id)) {
        fprintf(stderr, "Device identifier too long\n");
        return -1;
    }
    dev->use_explicit_device_id = true;

#ifdef USE_LITEPCIE
    if (strncmp(device_id, "pcie:", 5) == 0) {
        if (snprintf(dev->pcie_path, sizeof(dev->pcie_path), "%s", device_id + 5) >= (int)sizeof(dev->pcie_path)) {
            fprintf(stderr, "PCIe device path too long\n");
            return -1;
        }
    } else if (strncmp(device_id, "/dev/m2sdr", 10) == 0) {
        if (snprintf(dev->pcie_path, sizeof(dev->pcie_path), "%s", device_id) >= (int)sizeof(dev->pcie_path)) {
            fprintf(stderr, "PCIe device path too long\n");
            return -1;
        }
        if (snprintf(dev->device_id, sizeof(dev->device_id), "pcie:%s", dev->pcie_path) >= (int)sizeof(dev->device_id)) {
            fprintf(stderr, "Device identifier too long\n");
            return -1;
        }
    } else {
        dev->pcie_path[0] = '\0';
    }
#endif
    return 0;
}

bool m2sdr_cli_finalize_device(struct m2sdr_cli_device *dev)
{
    if (!dev)
        return false;

    if (dev->use_explicit_device_id)
        return true;

#ifdef USE_LITEPCIE
    snprintf(dev->pcie_path, sizeof(dev->pcie_path), "/dev/m2sdr%d", dev->device_num);
    if (snprintf(dev->device_id, sizeof(dev->device_id), "pcie:%s", dev->pcie_path) >= (int)sizeof(dev->device_id)) {
        fprintf(stderr, "Device path too long\n");
        return false;
    }
#elif defined(USE_LITEETH)
    if (snprintf(dev->device_id, sizeof(dev->device_id), "eth:%s:%s", dev->ip_address, dev->port) >= (int)sizeof(dev->device_id)) {
        fprintf(stderr, "Device address too long\n");
        return false;
    }
#endif
    return true;
}

const char *m2sdr_cli_device_id(const struct m2sdr_cli_device *dev)
{
    return dev ? dev->device_id : NULL;
}

const char *m2sdr_cli_pcie_path(const struct m2sdr_cli_device *dev)
{
#ifdef USE_LITEPCIE
    return dev ? dev->pcie_path : NULL;
#else
    (void)dev;
    return NULL;
#endif
}

void m2sdr_cli_print_device_help(void)
{
    fputs("Device options:\n", stdout);
    fputs("  -d, --device DEV      Use explicit device id.\n", stdout);
#ifdef USE_LITEPCIE
    fputs("  -c, --device-num N    Use /dev/m2sdrN (default: 0).\n", stdout);
#elif defined(USE_LITEETH)
    fputs("  -i, --ip ADDR         Target IP address (default: 192.168.1.50).\n", stdout);
    fputs("  -p, --port PORT       Target port (default: 1234).\n", stdout);
#endif
}

void m2sdr_cli_error(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void m2sdr_cli_invalid_choice(const char *what, const char *value, const char *expected)
{
    m2sdr_cli_error("invalid %s '%s' (expected %s)",
                    what ? what : "value",
                    value ? value : "(null)",
                    expected ? expected : "a supported value");
}

void m2sdr_cli_unknown_option(const char *opt)
{
    m2sdr_cli_error("unknown option: %s", opt ? opt : "(null)");
}
