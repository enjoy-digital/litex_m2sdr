/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR self-test utility
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "m2sdr.h"

static void print_status(const char *label, int rc, int *errors)
{
    if (rc == M2SDR_ERR_OK) {
        printf("[OK]   %s\n", label);
    } else if (rc == M2SDR_ERR_UNSUPPORTED) {
        printf("[SKIP] %s (unsupported)\n", label);
    } else {
        printf("[FAIL] %s (rc=%d)\n", label, rc);
        if (errors)
            (*errors)++;
    }
}

static void usage(const char *prog)
{
    printf("Usage: %s [device]\n", prog);
    printf("  device: optional device identifier (pcie:/dev/m2sdr0 or eth:ip:port)\n");
}

int main(int argc, char **argv)
{
    const char *dev_id = NULL;
    if (argc > 1) {
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            usage(argv[0]);
            return 0;
        }
        dev_id = argv[1];
    }

    struct m2sdr_dev *dev = NULL;
    int rc = m2sdr_open(&dev, dev_id);
    if (rc != M2SDR_ERR_OK) {
        fprintf(stderr, "Failed to open device (rc=%d)\n", rc);
        return 1;
    }

    int errors = 0;

    struct m2sdr_version ver = {0};
    m2sdr_get_version(&ver);
    printf("libm2sdr: api=0x%08x abi=0x%08x version=%s\n",
           ver.api, ver.abi, ver.version_str ? ver.version_str : "unknown");

    char ident[256] = {0};
    rc = m2sdr_get_identifier(dev, ident, sizeof(ident));
    print_status("SoC identifier", rc, &errors);
    if (rc == M2SDR_ERR_OK) {
        printf("       %s\n", ident);
    }

    struct m2sdr_capabilities caps;
    rc = m2sdr_get_capabilities(dev, &caps);
    print_status("Capabilities", rc, &errors);
    if (rc == M2SDR_ERR_OK) {
        int major = (int)(caps.api_version >> 16);
        int minor = (int)(caps.api_version & 0xffff);
        printf("       api=%d.%d features=0x%08x\n", major, minor, caps.features);
    }

    uint64_t dna = 0;
    rc = m2sdr_get_fpga_dna(dev, &dna);
    print_status("FPGA DNA", rc, &errors);
    if (rc == M2SDR_ERR_OK) {
        printf("       0x%016llx\n", (unsigned long long)dna);
    }

    struct m2sdr_fpga_sensors sensors;
    rc = m2sdr_get_fpga_sensors(dev, &sensors);
    print_status("FPGA sensors", rc, &errors);
    if (rc == M2SDR_ERR_OK) {
        printf("       temp=%.1fC vccint=%.2fV vccaux=%.2fV vccbram=%.2fV\n",
               sensors.temperature_c, sensors.vccint_v,
               sensors.vccaux_v, sensors.vccbram_v);
    }

    m2sdr_close(dev);

    if (errors) {
        printf("Self-test: %d error(s)\n", errors);
        return 1;
    }

    printf("Self-test: PASS\n");
    return 0;
}
