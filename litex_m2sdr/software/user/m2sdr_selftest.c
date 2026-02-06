/* SPDX-License-Identifier: BSD-2-Clause
 *
 * LiteX-M2SDR self-test utility
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include "config.h"

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
    printf("Usage: %s [options] [device]\n", prog);
    printf("  options:\n");
    printf("    --time       check board time is monotonic\n");
    printf("    --loopback   toggle DMA loopback (PCIe only)\n");
    printf("    --stream-loopback  run a DMA streaming loopback test (PCIe only)\n");
    printf("    -h, --help   show this help\n");
    printf("  device: optional device identifier (pcie:/dev/m2sdr0 or eth:ip:port)\n");
}

int main(int argc, char **argv)
{
    const char *dev_id = NULL;
    bool do_time = false;
    bool do_loopback = false;
    bool do_stream_loopback = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--time")) {
            do_time = true;
        } else if (!strcmp(argv[i], "--loopback")) {
            do_loopback = true;
        } else if (!strcmp(argv[i], "--stream-loopback")) {
            do_stream_loopback = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else {
            dev_id = argv[i];
        }
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

    if (do_time) {
        uint64_t t0 = 0;
        uint64_t t1 = 0;
        rc = m2sdr_get_time(dev, &t0);
        if (rc == M2SDR_ERR_OK) {
            usleep(10000);
            rc = m2sdr_get_time(dev, &t1);
        }
        print_status("Board time monotonic", rc, &errors);
        if (rc == M2SDR_ERR_OK && t1 < t0) {
            printf("[WARN] time moved backwards (%llu -> %llu)\n",
                   (unsigned long long)t0, (unsigned long long)t1);
        }
    }

    if (do_loopback) {
        rc = m2sdr_set_dma_loopback(dev, true);
        print_status("DMA loopback enable", rc, &errors);
        if (rc == M2SDR_ERR_OK)
            rc = m2sdr_set_dma_loopback(dev, false);
        print_status("DMA loopback disable", rc, &errors);
    }

    if (do_stream_loopback) {
        rc = m2sdr_set_dma_loopback(dev, true);
        if (rc == M2SDR_ERR_UNSUPPORTED) {
            print_status("DMA streaming loopback", rc, &errors);
        } else if (rc != M2SDR_ERR_OK) {
            print_status("DMA loopback enable", rc, &errors);
        } else {
            enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;
            size_t sample_size = m2sdr_format_size(format);
            unsigned samples_per_buf = DMA_BUFFER_SIZE / sample_size;
            int16_t tx_buf[DMA_BUFFER_SIZE / 2];
            int16_t rx_buf[DMA_BUFFER_SIZE / 2];

            for (unsigned i = 0; i < DMA_BUFFER_SIZE / sizeof(int16_t); i++)
                tx_buf[i] = (int16_t)(i & 0x7fff);

            rc = m2sdr_sync_config(dev, M2SDR_RX, format, 0, samples_per_buf, 0, 1000);
            if (rc == M2SDR_ERR_OK)
                rc = m2sdr_sync_config(dev, M2SDR_TX, format, 0, samples_per_buf, 0, 1000);
            if (rc != M2SDR_ERR_OK) {
                print_status("DMA streaming loopback config", rc, &errors);
            } else {
                rc = m2sdr_sync_tx(dev, tx_buf, samples_per_buf, NULL, 1000);
                if (rc != M2SDR_ERR_OK) {
                    print_status("DMA streaming loopback TX", rc, &errors);
                } else {
                    rc = m2sdr_sync_rx(dev, rx_buf, samples_per_buf, NULL, 1000);
                    if (rc != M2SDR_ERR_OK) {
                        print_status("DMA streaming loopback RX", rc, &errors);
                    } else {
                        unsigned mismatches = 0;
                        for (unsigned i = 0; i < DMA_BUFFER_SIZE / sizeof(int16_t); i++) {
                            if (rx_buf[i] != tx_buf[i]) {
                                mismatches++;
                                if (mismatches < 4)
                                    printf("[WARN] mismatch @%u: tx=%d rx=%d\n",
                                           i, tx_buf[i], rx_buf[i]);
                            }
                        }
                        if (mismatches) {
                            printf("[FAIL] DMA streaming loopback (mismatches=%u)\n", mismatches);
                            errors++;
                        } else {
                            printf("[OK]   DMA streaming loopback\n");
                        }
                    }
                }
            }

            m2sdr_set_dma_loopback(dev, false);
        }
    }

    m2sdr_close(dev);

    if (errors) {
        printf("Self-test: %d error(s)\n", errors);
        return 1;
    }

    printf("Self-test: PASS\n");
    return 0;
}
