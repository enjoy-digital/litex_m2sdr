/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Board Utility.
 *
 * This file is part of LiteX-M2SDR project.
 *
 * Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <math.h>

#include "ad9361/util.h"
#include "ad9361/ad9361.h"

#include "flags.h"

#include "liblitepcie.h"
#include "libm2sdr.h"

#include "m2sdr_config.h"

/* Parameters */
/*------------*/

#define DMA_CHECK_DATA   /* Enable Data Check when defined */
#define DMA_RANDOM_DATA  /* Enable Random Data when defined */

#define FLASH_WRITE /* Enable Flash Write when defined */

/* Variables */
/*-----------*/

static char litepcie_device[1024];
static int litepcie_device_num;

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    keep_running = 0;
}

#ifdef CSR_SI5351_BASE

/* SI5351 */
/*--------*/

static void test_si5351_scan(void)
{
    int fd;

    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    printf("\e[1m[> SI53512 I2C Bus Scan:\e[0m\n");
    printf("-----------------------------\n");
    m2sdr_si5351_i2c_scan(fd);
    printf("\n");

    close(fd);
}

static void test_si5351_init(void)
{
    int fd;

    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    printf("\e[1m[> SI53512 Init...\e[0m\n");
    m2sdr_si5351_i2c_config(fd, SI5351_I2C_ADDR, si5351_xo_config, sizeof(si5351_xo_config)/sizeof(si5351_xo_config[0]));
    printf("Done.\n");

    close(fd);
}

#endif

/* AD9361 Dump */
/*-------------*/

static void test_ad9361_dump(void)
{
    int i;
    int fd;

    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    /* AD9361 SPI Init */
    m2sdr_ad9361_spi_init(fd, 0);

    /* AD9361 SPI Dump of all the Registers */
    for (i=0; i<1024; i++)
        printf("Reg 0x%03x: 0x%04x\n", i, m2sdr_ad9361_spi_read(fd, i));

    printf("\n");

    close(fd);
}

/* Info */
/*------*/

static uint32_t icap_read(int fd, uint32_t reg)
{
    litepcie_writel(fd, CSR_ICAP_ADDR_ADDR, reg);
    litepcie_writel(fd, CSR_ICAP_READ_ADDR, 1);
    while (litepcie_readl(fd, CSR_ICAP_DONE_ADDR) == 0)
        usleep(1000);
    litepcie_writel(fd, CSR_ICAP_READ_ADDR, 0);
    return litepcie_readl(fd, CSR_ICAP_DATA_ADDR);
}

static void info(void)
{
    int fd;
    int i;
    unsigned char fpga_identifier[256];

    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }


    printf("\e[1m[> FPGA/SoC Info:\e[0m\n");
    printf("-----------------\n");

    for (i = 0; i < 256; i ++)
        fpga_identifier[i] = litepcie_readl(fd, CSR_IDENTIFIER_MEM_BASE + 4 * i);
    printf("SoC Identifier   : %s.\n", fpga_identifier);
#ifdef CSR_DNA_BASE
    printf("FPGA DNA         : 0x%08x%08x\n",
        litepcie_readl(fd, CSR_DNA_ID_ADDR + 4 * 0),
        litepcie_readl(fd, CSR_DNA_ID_ADDR + 4 * 1)
    );
#endif
#ifdef CSR_XADC_BASE
    printf("FPGA Temperature : %0.1f °C\n",
           (double)litepcie_readl(fd, CSR_XADC_TEMPERATURE_ADDR) * 503.975/4096 - 273.15);
    printf("FPGA VCC-INT     : %0.2f V\n",
           (double)litepcie_readl(fd, CSR_XADC_VCCINT_ADDR) / 4096 * 3);
    printf("FPGA VCC-AUX     : %0.2f V\n",
           (double)litepcie_readl(fd, CSR_XADC_VCCAUX_ADDR) / 4096 * 3);
    printf("FPGA VCC-BRAM    : %0.2f V\n",
           (double)litepcie_readl(fd, CSR_XADC_VCCBRAM_ADDR) / 4096 * 3);
#endif
#ifdef CSR_ICAP_BASE
    uint32_t status;
    status = icap_read(fd, ICAP_BOOTSTS_REG);
    printf("FPGA Status      : %s\n", (status & ICAP_BOOTSTS_FALLBACK)? "Fallback" : "Operational");
#endif
    printf("\n");

    printf("\e[1m[> AD9361 Info:\e[0m\n");
    printf("---------------\n");
    m2sdr_ad9361_spi_init(fd, 0);
    printf("AD9361 Product ID  : %04x \n", m2sdr_ad9361_spi_read(fd, REG_PRODUCT_ID));
    printf("AD9361 Temperature : %0.1f °C\n",
        (double)DIV_ROUND_CLOSEST(m2sdr_ad9361_spi_read(fd, REG_TEMPERATURE) * 1000000, 1140)/1000);

    close(fd);
}

/* Scratch */
/*---------*/

void scratch_test(void)
{
    int fd;

    printf("\e[1m[> Scratch register test:\e[0m\n");
    printf("-------------------------\n");

    /* Open LitePCIe device. */
    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    /* Write to scratch register. */
    printf("Write 0x12345678 to Scratch register:\n");
    litepcie_writel(fd, CSR_CTRL_SCRATCH_ADDR, 0x12345678);
    printf("Read: 0x%08x\n", litepcie_readl(fd, CSR_CTRL_SCRATCH_ADDR));

    /* Read from scratch register. */
    printf("Write 0xdeadbeef to Scratch register:\n");
    litepcie_writel(fd, CSR_CTRL_SCRATCH_ADDR, 0xdeadbeef);
    printf("Read: 0x%08x\n", litepcie_readl(fd, CSR_CTRL_SCRATCH_ADDR));

    /* Close LitePCIe device. */
    close(fd);
}

/* SPI Flash */
/*-----------*/

#ifdef CSR_FLASH_BASE

#ifdef FLASH_WRITE

static void flash_progress(void *opaque, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    fflush(stdout);
    va_end(ap);
}

static void flash_program(uint32_t base, const uint8_t *buf1, int size1)
{
    int fd;

    uint32_t size;
    uint8_t *buf;
    int sector_size;
    int errors;

    /* Open LitePCIe device. */
    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    /* Get flash sector size and pad size to it. */
    sector_size = litepcie_flash_get_erase_block_size(fd);
    size = ((size1 + sector_size - 1) / sector_size) * sector_size;

    /* Alloc buffer and copy data to it. */
    buf = calloc(1, size);
    if (!buf) {
        fprintf(stderr, "%d: alloc failed\n", __LINE__);
        exit(1);
    }
    memcpy(buf, buf1, size1);

    /* Program flash. */
    printf("Programming (%d bytes at 0x%08x)...\n", size, base);
    errors = litepcie_flash_write(fd, buf, base, size, flash_progress, NULL);
    if (errors) {
        printf("Failed %d errors.\n", errors);
        exit(1);
    } else {
        printf("Success.\n");
    }

    /* Free buffer and close LitePCIe device. */
    free(buf);
    close(fd);
}

static void flash_write(const char *filename, uint32_t offset)
{
    uint8_t *data;
    int size;
    FILE * f;

    /* Open data source file. */
    f = fopen(filename, "rb");
    if (!f) {
        perror(filename);
        exit(1);
    }

    /* Get size, alloc buffer and copy data to it. */
    fseek(f, 0L, SEEK_END);
    size = ftell(f);
    fseek(f, 0L, SEEK_SET);
    data = malloc(size);
    if (!data) {
        fprintf(stderr, "%d: malloc failed\n", __LINE__);
        exit(1);
    }
    ssize_t ret = fread(data, size, 1, f);
    fclose(f);

    /* Program file to flash */
    if (ret != 1)
        perror(filename);
    else
        flash_program(offset, data, size);

    /* Free buffer */
    free(data);
}

#endif

static void flash_read(const char *filename, uint32_t size, uint32_t offset)
{
    int fd;
    FILE * f;
    uint32_t base;
    uint32_t sector_size;
    uint8_t byte;
    int i;

    /* Open data destination file. */
    f = fopen(filename, "wb");
    if (!f) {
        perror(filename);
        exit(1);
    }

    /* Open LitePCIe device. */
    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    /* Get flash sector size. */
    sector_size = litepcie_flash_get_erase_block_size(fd);

    /* Read flash and write to destination file. */
    base = offset;
    for (i = 0; i < size; i++) {
        if ((i % sector_size) == 0) {
            printf("Reading 0x%08x\r", base + i);
            fflush(stdout);
        }
        byte = litepcie_flash_read(fd, base + i);
        fwrite(&byte, 1, 1, f);
    }

    /* Close destination file and LitePCIe device. */
    fclose(f);
    close(fd);
}

static void flash_reload(void)
{
    int fd;

    /* Open LitePCIe device. */
    fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    /* Reload FPGA through ICAP.*/
    litepcie_reload(fd);

    /* Notice user to reboot/rescan the hardware.*/
    printf("===========================================================================\n");
    printf("= PLEASE REBOOT YOUR HARDWARE OR RESCAN PCIe BUS TO USE NEW FPGA GATEWARE =\n");
    printf("===========================================================================\n");

    /* Close LitePCIe device. */
    close(fd);
}
#endif

/* DMA */
/*-----*/

#if (DMA_BUFFER_SIZE & (DMA_BUFFER_SIZE - 1)) == 0
static inline int64_t add_mod_int(int64_t a, int64_t b, int64_t m)
{
    /* Optimized for power of 2 */
    int64_t result;
    result = a + b;
    return result & (m - 1);
}
#else
static inline int64_t add_mod_int(int64_t a, int64_t b, int64_t m)
{
    /* Generic */
    a += b;
    if (a >= m)
        a -= m;
    return a;
}
#endif

static int get_next_pow2(int data_width)
{
    int x = 1;
    while (x < data_width)
        x <<= 1;
    return x;
}

#ifdef DMA_CHECK_DATA

static inline uint32_t seed_to_data(uint32_t seed)
{
#ifdef DMA_RANDOM_DATA
    /* Return pseudo random data from seed. */
    return seed * 69069 + 1;
#else
    /* Return seed. */
    return seed;
#endif
}

static uint32_t get_data_mask(int data_width)
{
    int i;
    uint32_t mask;
    mask = 0;
    for (i = 0; i < 32/get_next_pow2(data_width); i++) {
        mask <<= get_next_pow2(data_width);
        mask |= ((uint64_t) 1 << data_width) - 1;
    }
    return mask;
}

static void write_pn_data(uint32_t *buf, int count, uint32_t *pseed, int data_width)
{
    int i;
    uint32_t seed;
    uint32_t mask = get_data_mask(data_width);

    seed = *pseed;
    for(i = 0; i < count; i++) {
        buf[i] = (seed_to_data(seed) & mask);
        seed = add_mod_int(seed, 1, DMA_BUFFER_SIZE / sizeof(uint32_t));
    }
    *pseed = seed;
}

static int check_pn_data(const uint32_t *buf, int count, uint32_t *pseed, int data_width)
{
    int i, errors;
    uint32_t seed;
    uint32_t mask = get_data_mask(data_width);

    errors = 0;
    seed = *pseed;
    for (i = 0; i < count; i++) {
        if ((buf[i] & mask) != (seed_to_data(seed) & mask)) {
            errors ++;
        }
        seed = add_mod_int(seed, 1, DMA_BUFFER_SIZE / sizeof(uint32_t));
    }
    *pseed = seed;
    return errors;
}
#endif

static void dma_test(uint8_t zero_copy, uint8_t external_loopback, int data_width, int auto_rx_delay, int duration)
{
    static struct litepcie_dma_ctrl dma = {.use_reader = 1, .use_writer = 1};
    dma.loopback = external_loopback ? 0 : 1;

    if (data_width > 32 || data_width < 1) {
        fprintf(stderr, "Invalid data width %d\n", data_width);
        exit(1);
    }

    /* Statistics */
    int i = 0;
    int64_t reader_sw_count_last = 0;
    int64_t last_time;
    uint32_t errors = 0;
    int64_t end_time = (duration > 0) ? get_time_ms() + duration * 1000 : 0;

#ifdef DMA_CHECK_DATA
    uint32_t seed_wr = 0;
    uint32_t seed_rd = 0;
    uint8_t  run = (auto_rx_delay == 0);
#else
    uint8_t run = 1;
#endif

    signal(SIGINT, intHandler);

    printf("\e[1m[> DMA loopback test:\e[0m\n");
    printf("---------------------\n");

    if (litepcie_dma_init(&dma, litepcie_device, zero_copy))
        exit(1);

    dma.reader_enable = 1;
    dma.writer_enable = 1;

    /* Test loop. */
    last_time = get_time_ms();
    for (;;) {
        /* Exit loop on CTRL+C or when the duration is over. */
        if (!keep_running || (duration > 0 && get_time_ms() >= end_time))
            break;

        /* Update DMA status. */
        litepcie_dma_process(&dma);

#ifdef DMA_CHECK_DATA
        char *buf_wr;
        char *buf_rd;

        /* DMA-TX Write. */
        while (1) {
            /* Get Write buffer. */
            buf_wr = litepcie_dma_next_write_buffer(&dma);
            /* Break when no buffer available for Write. */
            if (!buf_wr)
                break;
            /* Write data to buffer. */
            write_pn_data((uint32_t *) buf_wr, DMA_BUFFER_SIZE / sizeof(uint32_t), &seed_wr, data_width);
        }

        /* DMA-RX Read/Check */
        while (1) {
            /* Get Read buffer. */
            buf_rd = litepcie_dma_next_read_buffer(&dma);
            /* Break when no buffer available for Read. */
            if (!buf_rd)
                break;
            /* Skip the first 128 DMA loops. */
            if (dma.writer_hw_count < 128*DMA_BUFFER_COUNT)
                break;
            /* When running... */
            if (run) {
                /* Check data in Read buffer. */
                errors += check_pn_data((uint32_t *) buf_rd, DMA_BUFFER_SIZE / sizeof(uint32_t), &seed_rd, data_width);
                /* Clear Read buffer */
                memset(buf_rd, 0, DMA_BUFFER_SIZE);
            } else {
                /* Find initial Delay/Seed (Useful when loopback is introducing delay). */
                uint32_t errors_min = 0xffffffff;
                for (int delay = 0; delay < DMA_BUFFER_SIZE / sizeof(uint32_t); delay++) {
                    seed_rd = delay;
                    errors = check_pn_data((uint32_t *) buf_rd, DMA_BUFFER_SIZE / sizeof(uint32_t), &seed_rd, data_width);
                    //printf("delay: %d / errors: %d\n", delay, errors);
                    if (errors < errors_min)
                        errors_min = errors;
                    if (errors < (DMA_BUFFER_SIZE / sizeof(uint32_t)) / 2) {
                        printf("RX_DELAY: %d (errors: %d)\n", delay, errors);
                        run = 1;
                        break;
                    }
                }
                if (!run) {
                    printf("Unable to find DMA RX_DELAY (min errors: %d/%ld), exiting.\n",
                        errors_min,
                        DMA_BUFFER_SIZE / sizeof(uint32_t));
                    goto end;
                }
            }

        }
#endif

        /* Statistics every 200ms. */
        int64_t duration_ms = get_time_ms() - last_time;
        if (run & (duration_ms > 200)) {
            /* Print banner every 10 lines. */
            if (i % 10 == 0)
                printf("\e[1mDMA_SPEED(Gbps)\tTX_BUFFERS\tRX_BUFFERS\tDIFF\tERRORS\e[0m\n");
            i++;
            /* Print statistics. */
            printf("%14.2f\t%10" PRIu64 "\t%10" PRIu64 "\t%4" PRIu64 "\t%6u\n",
                   (double)(dma.reader_sw_count - reader_sw_count_last) * DMA_BUFFER_SIZE * 8 * data_width / (get_next_pow2(data_width) * (double)duration_ms * 1e6),
                   dma.reader_sw_count,
                   dma.writer_sw_count,
                   (uint64_t) abs(dma.reader_sw_count - dma.writer_sw_count),
                   errors);
            /* Update errors/time/count. */
            errors = 0;
            last_time = get_time_ms();
            reader_sw_count_last = dma.reader_sw_count;
        }
    }

    /* Cleanup DMA. */
#ifdef DMA_CHECK_DATA
end:
#endif
    litepcie_dma_cleanup(&dma);
}

/* Clk Measurement */
/*-----------------*/

#define N_CLKS 5

static const uint32_t latch_addrs[N_CLKS] =
{
    CSR_CLK_MEASUREMENT_CLK0_LATCH_ADDR,
    CSR_CLK_MEASUREMENT_CLK1_LATCH_ADDR,
    CSR_CLK_MEASUREMENT_CLK2_LATCH_ADDR,
    CSR_CLK_MEASUREMENT_CLK3_LATCH_ADDR,
    CSR_CLK_MEASUREMENT_CLK4_LATCH_ADDR,
};

static const uint32_t value_addrs[N_CLKS] =
{
    CSR_CLK_MEASUREMENT_CLK0_VALUE_ADDR,
    CSR_CLK_MEASUREMENT_CLK1_VALUE_ADDR,
    CSR_CLK_MEASUREMENT_CLK2_VALUE_ADDR,
    CSR_CLK_MEASUREMENT_CLK3_VALUE_ADDR,
    CSR_CLK_MEASUREMENT_CLK4_VALUE_ADDR,
};

static const char* clk_names[N_CLKS] = {
    "       Sys Clk",
    "      PCIe Clk",
    "AD9361 Ref Clk",
    "AD9361 Dat Clk",
    "  Time Ref Clk",
};

static uint64_t read_64bit_register(int fd, uint32_t addr)
{
    uint32_t lower = litepcie_readl(fd, addr + 4);
    uint32_t upper = litepcie_readl(fd, addr + 0);
    return ((uint64_t)upper << 32) | lower;
}

static void latch_all_clocks(int fd)
{
    for (int i = 0; i < N_CLKS; i++) {
        litepcie_writel(fd, latch_addrs[i], 1);
    }
}

static void read_all_clocks(int fd, uint64_t *values)
{
    for (int i = 0; i < N_CLKS; i++) {
        values[i] = read_64bit_register(fd, value_addrs[i]);
    }
}

static void clk_test(int num_measurements, int delay_between_tests)
{
    int fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    printf("\e[1m[> Clk Measurement Test:\e[0m\n");
    printf("-------------------------\n");

    uint64_t previous_values[N_CLKS], current_values[N_CLKS];
    struct timespec start_time, current_time;
    double elapsed_time;

    latch_all_clocks(fd);
    read_all_clocks(fd, previous_values);
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (int i = 0; i < num_measurements; i++) {
        sleep(delay_between_tests);

        latch_all_clocks(fd);
        read_all_clocks(fd, current_values);
        clock_gettime(CLOCK_MONOTONIC, &current_time);

        elapsed_time = (current_time.tv_sec - start_time.tv_sec) +
                       (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
        start_time = current_time;

        for (int clk_index = 0; clk_index < N_CLKS; clk_index++) {
            uint64_t delta_value = current_values[clk_index] - previous_values[clk_index];
            double frequency_mhz = delta_value / (elapsed_time * 1e6);
            printf("Measurement %d, %s: Frequency: %3.2f MHz\n", i + 1, clk_names[clk_index], frequency_mhz);
            previous_values[clk_index] = current_values[clk_index];
        }
    }

    close(fd);
}

/* VCXO Test  */
/*------------*/

#ifdef CSR_SI5351_BASE

#define PWM_PERIOD 4096  // Define the PWM period

static void vcxo_test() {
    int fd = open(litepcie_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }

    printf("\e[1m[> VCXO Test:\e[0m\n");
    printf("-------------\n");

    uint32_t nominal_pwm_width = (uint32_t)((50.0 / 100.0) * PWM_PERIOD);
    double nominal_frequency_hz = 0;

    uint64_t previous_value, current_value;
    struct timespec start_time, current_time;
    double elapsed_time, min_frequency = 1e12, max_frequency = 0, previous_frequency_hz = 0;

    /* Set PWM period. */
    litepcie_writel(fd, CSR_SI5351_PWM_PERIOD_ADDR, PWM_PERIOD);
    /* Enable PWM. */
    litepcie_writel(fd, CSR_SI5351_PWM_ENABLE_ADDR, 1);

    for (double pwm_width_percent = 0.0; pwm_width_percent <= 100.0; pwm_width_percent += 10.0) {
        uint32_t pwm_width = (uint32_t)((pwm_width_percent / 100.0) * PWM_PERIOD);

        /* Set PWM width. */
        litepcie_writel(fd, CSR_SI5351_PWM_WIDTH_ADDR, pwm_width);

        latch_all_clocks(fd);
        previous_value = read_64bit_register(fd, CSR_CLK_MEASUREMENT_CLK0_VALUE_ADDR);
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        sleep(1);

        latch_all_clocks(fd);
        current_value = read_64bit_register(fd, CSR_CLK_MEASUREMENT_CLK0_VALUE_ADDR);
        clock_gettime(CLOCK_MONOTONIC, &current_time);

        elapsed_time = (current_time.tv_sec - start_time.tv_sec) +
                       (current_time.tv_nsec - start_time.tv_nsec) / 1e9;

        uint64_t delta_value = current_value - previous_value;
        double frequency_hz = delta_value / elapsed_time;

        if (pwm_width == nominal_pwm_width) {
            nominal_frequency_hz = frequency_hz;
        }

        double variation_hz = (pwm_width_percent != 0.0) ? frequency_hz - previous_frequency_hz : 0;

        printf("PWM Width: %6.2f%%, Frequency: %10.6f MHz, Variation: %c%7.2f Hz\n",
               pwm_width_percent, frequency_hz / 1e6, (variation_hz >= 0 ? '+' : '-'), fabs(variation_hz));

        if (frequency_hz < min_frequency) min_frequency = frequency_hz;
        if (frequency_hz > max_frequency) max_frequency = frequency_hz;

        previous_frequency_hz = frequency_hz;
    }

    /* Set back PWM to nominal width. */
    litepcie_writel(fd, CSR_SI5351_PWM_WIDTH_ADDR, nominal_pwm_width);

    close(fd);

    /* Calculate PPM and Hz variation from nominal at 50% PWM. */
    double hz_variation_from_nominal_max = max_frequency - nominal_frequency_hz;
    double hz_variation_from_nominal_min = nominal_frequency_hz - min_frequency;
    double ppm_variation_from_nominal_max = (hz_variation_from_nominal_max / nominal_frequency_hz) * 1e6;
    double ppm_variation_from_nominal_min = (hz_variation_from_nominal_min / nominal_frequency_hz) * 1e6;

    printf("\e[1m[>Report:\e[0m\n");
    printf("-------------------------\n");
    printf(" Hz Variation from Nominal (50%% PWM): -%10.2f  Hz / +%10.2f  Hz\n", hz_variation_from_nominal_min, hz_variation_from_nominal_max);
    printf("PPM Variation from Nominal (50%% PWM): -%10.2f PPM / +%10.2f PPM\n", ppm_variation_from_nominal_min, ppm_variation_from_nominal_max);
}

#endif

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR Board Utility\n"
           "usage: m2sdr_util [options] cmd [args...]\n"
           "\n"
           "options:\n"
           "-h                                Help.\n"
           "-c device_num                     Select the device (default = 0).\n"
           "-z                                Enable zero-copy DMA mode.\n"
           "-e                                Use external loopback (default = internal).\n"
           "-w data_width                     Width of data bus (default = 32).\n"
           "-a                                Automatic DMA RX-Delay calibration.\n"
           "-t duration                       Duration of the test in seconds (default = 0, infinite).\n"
           "\n"
           "available commands:\n"
           "info                              Get Board information.\n"
           "\n"
           "dma_test                          Test DMA.\n"
           "scratch_test                      Test Scratch register.\n"
           "clk_test                          Test Clks frequencies.\n"
#ifdef  CSR_SI5351_BASE
           "vcxo_test                         Test VCXO frequency variation.\n"
#endif
           "\n"
#ifdef  CSR_SI5351_BASE
           "si5351_scan                       Scan SI5351 I2C Bus.\n"
           "si5351_init                       Init SI5351.\n"
           "\n"
#endif
           "ad9361_dump                       Dump AD9361 Registers.\n"
           "\n"
#ifdef CSR_FLASH_BASE
#ifdef FLASH_WRITE
           "flash_write filename [offset]     Write file contents to SPI Flash.\n"
#endif
           "flash_read filename size [offset] Read from SPI Flash and write contents to file.\n"
           "flash_reload                      Reload FPGA Image.\n"
#endif
           );
    exit(1);
}

/* Main */
/*------*/

int main(int argc, char **argv)
{
    const char *cmd;
    int c;
    static uint8_t litepcie_device_zero_copy;
    static uint8_t litepcie_device_external_loopback;
    static int litepcie_data_width;
    static int litepcie_auto_rx_delay;
    static int test_duration = 0; /* Default to 0 for infinite duration.*/

    litepcie_device_num = 0;
    litepcie_data_width = 32;
    litepcie_auto_rx_delay = 0;
    litepcie_device_zero_copy = 0;
    litepcie_device_external_loopback = 0;

    /* Parameters. */
    for (;;) {
        c = getopt(argc, argv, "hc:w:zeat:");
        if (c == -1)
            break;
        switch(c) {
        case 'h':
            help();
            break;
        case 'c':
            litepcie_device_num = atoi(optarg);
            break;
        case 'w':
            litepcie_data_width = atoi(optarg);
            break;
        case 'z':
            litepcie_device_zero_copy = 1;
            break;
        case 'e':
            litepcie_device_external_loopback = 1;
            break;
        case 'a':
            litepcie_auto_rx_delay = 1;
            break;
        case 't':
            test_duration = atoi(optarg);
            break;
        default:
            exit(1);
        }
    }

    /* Show help when too much args. */
    if (optind >= argc)
        help();

    /* Select device. */
    snprintf(litepcie_device, sizeof(litepcie_device), "/dev/m2sdr%d", litepcie_device_num);

    cmd = argv[optind++];

    /* Info cmds. */
    if (!strcmp(cmd, "info"))
        info();

    /* Scratch cmds. */
    else if (!strcmp(cmd, "scratch_test"))
        scratch_test();

    /* Clk cmds. */
    else if (!strcmp(cmd, "clk_test")) {
        int num_measurements = 10;
        int delay_between_tests = 1;

        if (optind < argc)
            num_measurements = atoi(argv[optind++]);
        if (optind < argc)
            delay_between_tests = atoi(argv[optind++]);

        clk_test(num_measurements, delay_between_tests);
    }

#ifdef  CSR_SI5351_BASE
    /* VCXO test cmd. */
    else if (!strcmp(cmd, "vcxo_test")) {
        vcxo_test();
    }
#endif

    /* SI5351 cmds. */
#ifdef CSR_SI5351_BASE
    else if (!strcmp(cmd, "si5351_scan"))
        test_si5351_scan();
    else if (!strcmp(cmd, "si5351_init"))
        test_si5351_init();
#endif

    /* AD9361 cmds. */
    else if (!strcmp(cmd, "ad9361_dump"))
        test_ad9361_dump();

    /* SPI Flash cmds. */
#if CSR_FLASH_BASE
#ifdef FLASH_WRITE
    else if (!strcmp(cmd, "flash_write")) {
        const char *filename;
        uint32_t offset = CONFIG_FLASH_IMAGE_SIZE;  /* Operational */
        if (optind + 1 > argc)
            goto show_help;
        filename = argv[optind++];
        if (optind < argc)
            offset = strtoul(argv[optind++], NULL, 0);
        flash_write(filename, offset);
    }
#endif
    else if (!strcmp(cmd, "flash_read")) {
        const char *filename;
        uint32_t size = 0;
        uint32_t offset = CONFIG_FLASH_IMAGE_SIZE; /* Operational */
        if (optind + 2 > argc)
            goto show_help;
        filename = argv[optind++];
        size = strtoul(argv[optind++], NULL, 0);
        if (optind < argc)
            offset = strtoul(argv[optind++], NULL, 0);
        flash_read(filename, size, offset);
    }
    else if (!strcmp(cmd, "flash_reload"))
        flash_reload();
#endif

    /* DMA cmds. */
    else if (!strcmp(cmd, "dma_test"))
        dma_test(
            litepcie_device_zero_copy,
            litepcie_device_external_loopback,
            litepcie_data_width,
            litepcie_auto_rx_delay,
            test_duration);

    /* Show help otherwise. */
    else
        goto show_help;

    return 0;

show_help:
        help();

    return 0;
}
