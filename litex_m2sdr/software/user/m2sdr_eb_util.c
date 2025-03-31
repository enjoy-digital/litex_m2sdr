/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Etherbone Utility
 *
 * Remote flash update utility for M2SDR boards over Etherbone.
 *
 * Copyright (c) 2024-2025 Enjoy-Digital <enjoy-digital.fr>
 *
 * Compilation: gcc -o m2sdr_eb_util m2sdr_eb_util.c libliteeth/etherbone.c -I../kernel -Ilibliteeth
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <inttypes.h>

#include "csr.h"
#include "soc.h"
#include "flags.h"
#include "libliteeth/etherbone.h"

/* Parameters */
/*------------*/

#define FLASH_WRITE /* Enable Flash Write when defined */

/* Variables */
/*-----------*/

#define litex_m2sdr_writel(_fd, _addr, _val) eb_write32(_fd, _val, _addr)
#define litex_m2sdr_readl(_fd, _addr) eb_read32(_fd, _addr)
typedef struct eb_connection *litex_m2sdr_device_desc_t;

static char ip_address[1024];
static char port[16] = "1234";

#define FLASH_READ_ID_REG 0x9F

#define FLASH_READ    0x03
#define FLASH_WREN    0x06
#define FLASH_WRDI    0x04
#define FLASH_PP      0x02
#define FLASH_SE      0xD8
#define FLASH_BE      0xC7
#define FLASH_RDSR    0x05
#define FLASH_WRSR    0x01
#define FLASH_WIP     0x01

#define FLASH_SECTOR_SIZE (1 << 16)
#define FLASH_RETRIES     16
#define SPI_TIMEOUT       100000 /* in us */

/* SPI Flash */
/*-----------*/

static void eb_flash_spi_cs(litex_m2sdr_device_desc_t conn, uint8_t cs_n)
{
    litex_m2sdr_writel(conn, CSR_FLASH_CS_N_OUT_ADDR, cs_n);
}

static uint32_t eb_flash_spi(litex_m2sdr_device_desc_t conn, int tx_len, uint8_t cmd, uint32_t tx_data)
{
    int i;
    uint64_t tx = ((uint64_t)cmd << 32) | tx_data;
    uint64_t rx_data;

    if (tx_len < 8 || tx_len > 40) {
        fprintf(stderr, "Invalid SPI transaction length: %d\n", tx_len);
        return 0;
    }

    eb_flash_spi_cs(conn, 0);
    litex_m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR, (tx >> 32) & 0xFFFFFFFF);
    litex_m2sdr_writel(conn, CSR_FLASH_SPI_MOSI_ADDR + 4, tx & 0xFFFFFFFF);
    litex_m2sdr_writel(conn, CSR_FLASH_SPI_CONTROL_ADDR, SPI_CTRL_START | (tx_len * SPI_CTRL_LENGTH));
    for (i = 0; i < SPI_TIMEOUT; i++) {
        if (litex_m2sdr_readl(conn, CSR_FLASH_SPI_STATUS_ADDR) & SPI_STATUS_DONE)
            break;
        usleep(1);
    }
    if (i >= SPI_TIMEOUT) {
        fprintf(stderr, "SPI transaction timed out\n");
        eb_flash_spi_cs(conn, 1);
        return 0;
    }
    rx_data = ((uint64_t)litex_m2sdr_readl(conn, CSR_FLASH_SPI_MISO_ADDR) << 32) |
              litex_m2sdr_readl(conn, CSR_FLASH_SPI_MISO_ADDR + 4);
    eb_flash_spi_cs(conn, 1);

    return (uint32_t)(rx_data & 0xFFFFFFFF);
}

static void eb_flash_write_enable(litex_m2sdr_device_desc_t conn)
{
    eb_flash_spi(conn, 8, FLASH_WREN, 0);
}

static void eb_flash_write_disable(litex_m2sdr_device_desc_t conn)
{
    eb_flash_spi(conn, 8, FLASH_WRDI, 0);
}

static uint8_t eb_flash_read_status(litex_m2sdr_device_desc_t conn)
{
    return eb_flash_spi(conn, 16, FLASH_RDSR, 0) & 0xFF;
}

static void eb_flash_erase_sector(litex_m2sdr_device_desc_t conn, uint32_t addr)
{
    eb_flash_spi(conn, 32, FLASH_SE, addr << 8);
}

static void eb_flash_write(litex_m2sdr_device_desc_t conn, uint32_t addr, uint8_t byte)
{
    eb_flash_spi(conn, 40, FLASH_PP, (addr << 8) | byte);
}

static uint8_t eb_flash_read(litex_m2sdr_device_desc_t conn, uint32_t addr)
{
    return eb_flash_spi(conn, 40, FLASH_READ, addr << 8) & 0xFF;
}

#ifdef FLASH_WRITE

static void flash_progress(void *opaque, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    fflush(stdout);
    va_end(ap);
}

static int eb_flash_write_buffer(litex_m2sdr_device_desc_t conn, uint32_t base, const uint8_t *buf, uint32_t size);

static void eb_flash_program(litex_m2sdr_device_desc_t conn, uint32_t base, const uint8_t *buf, int size)
{
    uint32_t padded_size;
    uint8_t *padded_buf;
    int sector_size;
    int errors;

    sector_size = FLASH_SECTOR_SIZE;
    padded_size = ((size + sector_size - 1) / sector_size) * sector_size;
    padded_buf = calloc(1, padded_size);
    if (!padded_buf) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(1);
    }
    memcpy(padded_buf, buf, size);

    printf("Programming (%d bytes at 0x%08x)...\n", size, base);
    errors = eb_flash_write_buffer(conn, base, padded_buf, padded_size);
    if (errors)
        printf("Failed with %d errors.\n", errors);
    else
        printf("Success.\n");

    free(padded_buf);
}

static int eb_flash_write_buffer(litex_m2sdr_device_desc_t conn, uint32_t base, const uint8_t *buf, uint32_t size)
{
    int i, retries = 0;
    uint8_t cmp_buf[256];
    uint16_t program_size = 256;

    for (i = 0; i < size; i += program_size) {
        uint16_t chunk_size = (size - i < program_size) ? size - i : program_size;
        if ((i % FLASH_SECTOR_SIZE) == 0) {
            flash_progress(NULL, "Erasing @%08x\r", base + i);
            eb_flash_write_enable(conn);
            eb_flash_erase_sector(conn, base + i);
            while (eb_flash_read_status(conn) & FLASH_WIP)
                usleep(1000);
        }
        flash_progress(NULL, "Writing @%08x\r", base + i);
        eb_flash_write_enable(conn);
        for (int j = 0; j < chunk_size; j++)
            eb_flash_write(conn, base + i + j, buf[i + j]);
        eb_flash_write_disable(conn);
        while (eb_flash_read_status(conn) & FLASH_WIP)
            usleep(100);
        for (int j = 0; j < chunk_size; j++)
            cmp_buf[j] = eb_flash_read(conn, base + i + j);
        if (memcmp(buf + i, cmp_buf, chunk_size) != 0) {
            retries++;
            if (retries > FLASH_RETRIES) {
                printf("Failed to write page at 0x%08x after %d retries\n", base + i, FLASH_RETRIES);
                return 1;
            }
            i -= chunk_size;
        } else {
            retries = 0;
        }
    }
    flash_progress(NULL, "\n");
    return 0;
}

static void flash_write(const char *filename, uint32_t offset)
{
    uint8_t *data;
    int size;
    FILE *f;
    litex_m2sdr_device_desc_t conn;

    /* Open Etherbone connection. */
    conn = eb_connect(ip_address, port, 1);
    if (!conn) {
        fprintf(stderr, "Failed to connect to %s:%s\n", ip_address, port);
        exit(1);
    }

    /* Open data source file. */
    f = fopen(filename, "rb");
    if (!f) {
        perror(filename);
        eb_disconnect(&conn);
        exit(1);
    }

    /* Get size, alloc buffer and copy data to it. */
    fseek(f, 0L, SEEK_END);
    size = ftell(f);
    fseek(f, 0L, SEEK_SET);
    data = malloc(size);
    if (!data) {
        fprintf(stderr, "malloc failed\n");
        fclose(f);
        eb_disconnect(&conn);
        exit(1);
    }
    if (fread(data, size, 1, f) != 1) {
        perror(filename);
        free(data);
        fclose(f);
        eb_disconnect(&conn);
        exit(1);
    }
    fclose(f);

    /* Program file to flash. */
    eb_flash_program(conn, offset, data, size);

    /* Free buffer and close connection. */
    free(data);
    eb_disconnect(&conn);
}

#endif

static void flash_read(const char *filename, uint32_t size, uint32_t offset)
{
    litex_m2sdr_device_desc_t conn;
    FILE *f;
    uint32_t i;
    uint8_t byte;

    /* Open data destination file. */
    f = fopen(filename, "wb");
    if (!f) {
        perror(filename);
        exit(1);
    }

    /* Open Etherbone connection. */
    conn = eb_connect(ip_address, port, 1);
    if (!conn) {
        fprintf(stderr, "Failed to connect to %s:%s\n", ip_address, port);
        fclose(f);
        exit(1);
    }

    /* Read flash and write to destination file. */
    for (i = 0; i < size; i++) {
        if ((i % FLASH_SECTOR_SIZE) == 0) {
            printf("Reading 0x%08x\r", offset + i);
            fflush(stdout);
        }
        byte = eb_flash_read(conn, offset + i);
        fwrite(&byte, 1, 1, f);
    }
    printf("\n");

    /* Close destination file and connection. */
    fclose(f);
    eb_disconnect(&conn);
}

static void flash_reload(void)
{
    litex_m2sdr_device_desc_t conn;

    /* Open Etherbone connection. */
    conn = eb_connect(ip_address, port, 1);
    if (!conn) {
        fprintf(stderr, "Failed to connect to %s:%s\n", ip_address, port);
        exit(1);
    }

    /* Reload FPGA through ICAP. */
    litex_m2sdr_writel(conn, CSR_ICAP_ADDR_ADDR, ICAP_CMD_REG);
    litex_m2sdr_writel(conn, CSR_ICAP_DATA_ADDR, ICAP_CMD_IPROG);
    litex_m2sdr_writel(conn, CSR_ICAP_WRITE_ADDR, 1);

    /* Notify user of update. */
    printf("===========================================================================\n");
    printf("= HARDWARE HAS BEEN UPDATED AND IS NOW RUNNING UPDATED FPGA GATEWARE      =\n");
    printf("===========================================================================\n");

    /* Close connection. */
    eb_disconnect(&conn);
}

/* Scratch */
/*---------*/

static void scratch_test(void)
{
    litex_m2sdr_device_desc_t conn;

    printf("\e[1m[> Scratch register test:\e[0m\n");
    printf("-------------------------\n");

    /* Open Etherbone connection. */
    conn = eb_connect(ip_address, port, 1);
    if (!conn) {
        fprintf(stderr, "Failed to connect to %s:%s\n", ip_address, port);
        exit(1);
    }

    /* Write to scratch register. */
    printf("Write 0x12345678 to Scratch register:\n");
    litex_m2sdr_writel(conn, CSR_CTRL_SCRATCH_ADDR, 0x12345678);
    printf("Read: 0x%08x\n", litex_m2sdr_readl(conn, CSR_CTRL_SCRATCH_ADDR));

    /* Read from scratch register. */
    printf("Write 0xdeadbeef to Scratch register:\n");
    litex_m2sdr_writel(conn, CSR_CTRL_SCRATCH_ADDR, 0xdeadbeef);
    printf("Read: 0x%08x\n", litex_m2sdr_readl(conn, CSR_CTRL_SCRATCH_ADDR));

    /* Close connection. */
    eb_disconnect(&conn);
}

/* Help */
/*------*/

static void help(void)
{
    printf("M2SDR Etherbone Utility\n"
           "usage: m2sdr_eb_util [options] cmd [args...]\n"
           "\n"
           "options:\n"
           "-h                                Help.\n"
           "-i ip_address                     Target IP address of the board.\n"
           "-p port                           Port number (default = 1234).\n"
           "\n"
           "available commands:\n"
           "scratch_test                      Test Scratch register.\n"
#ifdef FLASH_WRITE
           "flash_write filename [offset]     Write file contents to SPI Flash.\n"
#endif
           "flash_read filename size [offset] Read from SPI Flash and write contents to file.\n"
           "flash_reload                      Reload FPGA Image.\n");
    exit(1);
}

/* Main */
/*------*/

int main(int argc, char **argv)
{
    const char *cmd;
    int c;

    /* Parameters. */
    ip_address[0] = '\0'; /* Initialize to empty string */
    for (;;) {
        c = getopt(argc, argv, "hi:p:");
        if (c == -1)
            break;
        switch (c) {
        case 'h':
            help();
            break;
        case 'i':
            strncpy(ip_address, optarg, sizeof(ip_address) - 1);
            ip_address[sizeof(ip_address) - 1] = '\0';
            break;
        case 'p':
            strncpy(port, optarg, sizeof(port) - 1);
            port[sizeof(port) - 1] = '\0';
            break;
        default:
            exit(1);
        }
    }

    /* Show help if no args or no IP. */
    if (optind >= argc || ip_address[0] == '\0')
        help();

    cmd = argv[optind++];

    /* Scratch cmds. */
    if (!strcmp(cmd, "scratch_test"))
        scratch_test();

    /* SPI Flash cmds. */
#ifdef FLASH_WRITE
    else if (!strcmp(cmd, "flash_write")) {
        const char *filename;
        uint32_t offset = 0x1000000; /* Default offset */
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
        uint32_t offset = 0x1000000; /* Default offset */
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

    /* Show help otherwise. */
    else
        goto show_help;

    return 0;

show_help:
    help();
    return 0;
}
