/* SPDX-License-Identifier: BSD-2-Clause */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

bool m2sdr_cli_finalize_device(struct m2sdr_cli_device *dev)
{
    if (!dev)
        return false;

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
