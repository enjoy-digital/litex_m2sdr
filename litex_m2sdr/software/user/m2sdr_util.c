/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR Board Utility.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
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
#include <stdint.h>
#include <stdbool.h>

#include "ad9361/util.h"
#include "ad9361/ad9361.h"

#include "liblitepcie.h"
#include "libm2sdr.h"

#include "m2sdr_config.h"

/* Parameters */
/*------------*/

#define DMA_CHECK_DATA   /* Enable Data Check when defined */
#define DMA_RANDOM_DATA  /* Enable Random Data when defined */

#define FLASH_WRITE      /* Enable Flash Write when defined */

/* Variables */
/*-----------*/

#ifdef USE_LITEPCIE
static char m2sdr_device[1024];
static int m2sdr_device_num = 0;
#elif defined(USE_LITEETH)
static char m2sdr_ip_address[1024] = "192.168.1.50";
static char m2sdr_port[16] = "1234";
#endif

sig_atomic_t keep_running = 1;

void intHandler(int dummy) {
    keep_running = 0;
}

static bool confirm_flash_write(void)
{
    char buf[8];
    fprintf(stderr, "WARNING: flash_write can overwrite the FPGA image.\n");
    fprintf(stderr, "Type 'YES' to continue: ");
    if (!fgets(buf, sizeof(buf), stdin))
        return false;
    return (strncmp(buf, "YES", 3) == 0);
}

/* Connection Functions */
/*----------------------*/

static void * m2sdr_open(void) {
#ifdef USE_LITEPCIE
    int fd = open(m2sdr_device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Could not init driver\n");
        exit(1);
    }
    return (void *)(intptr_t)fd;
#elif USE_LITEETH
    struct eb_connection *eb = eb_connect(m2sdr_ip_address, m2sdr_port, 1);
    if (!eb) {
        fprintf(stderr, "Failed to connect to %s:%s\n", m2sdr_ip_address, m2sdr_port);
        exit(1);
    }
    return eb;
#endif
}

static void m2sdr_close(void *conn) {
#ifdef USE_LITEPCIE
    close((int)(intptr_t)conn);
#elif USE_LITEETH
    eb_disconnect((struct eb_connection **)&conn);
#endif
}

/* SI5351 */
/*--------*/

#ifdef CSR_SI5351_BASE

static void test_si5351_init(void)
{
    void *conn = m2sdr_open();

    printf("\e[1m[> SI5351 Init...\e[0m\n");
    m2sdr_si5351_i2c_config(conn, SI5351_I2C_ADDR, si5351_xo_38p4m_config, sizeof(si5351_xo_38p4m_config)/sizeof(si5351_xo_38p4m_config[0]));
    printf("Done.\n");

    m2sdr_close(conn);
}

static void test_si5351_dump(void)
{
    uint8_t value;
    int i;

    void *conn = m2sdr_open();

    printf("\e[1m[> SI5351 Registers Dump:\e[0m\n");
    printf("--------------------------\n");

    for (i = 0; i < 256; i++) {
        if (m2sdr_si5351_i2c_read(conn, SI5351_I2C_ADDR, i, &value, 1, true)) {
            printf("Reg 0x%02x: 0x%02x\n", i, value);
        } else {
            fprintf(stderr, "Failed to read reg 0x%02x\n", i);
        }
    }

    printf("\n");
    m2sdr_close(conn);
}

static void test_si5351_write(uint8_t reg, uint8_t value)
{
    void *conn = m2sdr_open();

    if (m2sdr_si5351_i2c_write(conn, SI5351_I2C_ADDR, reg, &value, 1)) {
        printf("Wrote 0x%02x to SI5351 reg 0x%02x\n", value, reg);
    } else {
        fprintf(stderr, "Failed to write to SI5351 reg 0x%02x\n", reg);
    }

    m2sdr_close(conn);
}

static void test_si5351_read(uint8_t reg)
{
    uint8_t value;

    void *conn = m2sdr_open();

    if (m2sdr_si5351_i2c_read(conn, SI5351_I2C_ADDR, reg, &value, 1, true)) {
        printf("SI5351 reg 0x%02x: 0x%02x\n", reg, value);
    } else {
        fprintf(stderr, "Failed to read SI5351 reg 0x%02x\n", reg);
    }

    m2sdr_close(conn);
}

#endif

/* AD9361 Dump */
/*-------------*/

static void test_ad9361_dump(void)
{
    int i;

    void *conn = m2sdr_open();

    /* AD9361 SPI Init */
    m2sdr_ad9361_spi_init(conn, 0);

    /* AD9361 SPI Dump of all the Registers */
    for (i=0; i<1024; i++)
        printf("Reg 0x%03x: 0x%04x\n", i, m2sdr_ad9361_spi_read(conn, i));

    printf("\n");

    m2sdr_close(conn);
}

static void test_ad9361_write(uint16_t reg, uint16_t value)
{
    void *conn = m2sdr_open();

    /* AD9361 SPI Init */
    m2sdr_ad9361_spi_init(conn, 0);

    m2sdr_ad9361_spi_write(conn, reg, value);
    printf("Wrote 0x%04x to AD9361 reg 0x%03x\n", value, reg);

    m2sdr_close(conn);
}

static void test_ad9361_read(uint16_t reg)
{
    uint16_t value;

    void *conn = m2sdr_open();

    /* AD9361 SPI Init */
    m2sdr_ad9361_spi_init(conn, 0);

    value = m2sdr_ad9361_spi_read(conn, reg);
    printf("AD9361 reg 0x%03x: 0x%04x\n", reg, value);

    m2sdr_close(conn);
}

/* AD9361 Dump Utilities */
/*-----------------------*/

static void print_table_row(const char *c1, const char *c2, const char *c3, const char *c4, const char *c5, const char *c6)
{
    printf("|%-9s|%-6s|%-5s|%-35s|%-8s|%-35s|\n",
           c1 ? c1 : "", c2 ? c2 : "", c3 ? c3 : "", c4 ? c4 : "", c5 ? c5 : "", c6 ? c6 : "");
}

static void print_separator(void)
{
    printf("+---------+------+-----+-----------------------------------+--------+-----------------------------------+\n");
}

/* AD9361 Port Dump */
/*------------------*/
static void test_ad9361_port_dump(void)
{
    void *conn = m2sdr_open();
    /* AD9361 SPI Init */
    m2sdr_ad9361_spi_init(conn, 0);
    uint8_t reg010 = m2sdr_ad9361_spi_read(conn, 0x010);
    uint8_t reg011 = m2sdr_ad9361_spi_read(conn, 0x011);
    uint8_t reg012 = m2sdr_ad9361_spi_read(conn, 0x012);
    printf("\e[1m[> AD9361 Parallel Port Configuration Dump:\e[0m\n");
    printf("-------------------------------------------\n");

    /* Separator */
    print_separator();

    /* Table Header */
    print_table_row("Register", "Hex", "Bits", "Field Name", "Value", "Decoding");

    /* Separator */
    print_separator();

    /* Register 0x010 */
    {
        char hex010[7];
        snprintf(hex010, sizeof(hex010), "0x%02X", reg010);
        print_table_row("0x010", hex010, "", "", "", "");
        print_table_row("", "", "D7", "PP Tx Swap IQ",       ((reg010 >> 7) & 0x01) ? "1" : "0", ((reg010 >> 7) & 0x01) ? "No Swap"          : "Swap Enabled (Spectral Inversion)");
        print_table_row("", "", "D6", "PP Rx Swap IQ",       ((reg010 >> 6) & 0x01) ? "1" : "0", ((reg010 >> 6) & 0x01) ? "No Swap"          : "Swap Enabled (Spectral Inversion)");
        print_table_row("", "", "D5", "Tx Channel Swap",     ((reg010 >> 5) & 0x01) ? "1" : "0", ((reg010 >> 5) & 0x01) ? "Swap Enabled"     : "No Swap");
        print_table_row("", "", "D4", "Rx Channel Swap",     ((reg010 >> 4) & 0x01) ? "1" : "0", ((reg010 >> 4) & 0x01) ? "Swap Enabled"     : "No Swap");
        print_table_row("", "", "D3", "Rx Frame Pulse Mode", ((reg010 >> 3) & 0x01) ? "1" : "0", ((reg010 >> 3) & 0x01) ? "Pulse (50% duty)" : "Level (stays high)");
        print_table_row("", "", "D2", "2R2T Timing",         ((reg010 >> 2) & 0x01) ? "1" : "0", ((reg010 >> 2) & 0x01) ? "Always 2R2T"      : "Auto (based on paths)");
        print_table_row("", "", "D1", "Invert Data Bus",     ((reg010 >> 1) & 0x01) ? "1" : "0", ((reg010 >> 1) & 0x01) ? "Enabled ([0:11])" : "Disabled ([11:0])");
        print_table_row("", "", "D0", "Invert DATA CLK",     (reg010 & 0x01)        ? "1" : "0", (reg010 & 0x01)        ? "Enabled"          : "Disabled");
    }

    /* Separator */
    print_separator();

    /* Register 0x011 */
    {
        char hex011[7];
        snprintf(hex011, sizeof(hex011), "0x%02X", reg011);
        print_table_row("0x011", hex011, "", "", "", "");
        print_table_row("", "", "D7", "FDD Alt Word Order", ((reg011 >> 7) & 0x01) ? "1" : "0", ((reg011 >> 7) & 0x01) ? "Enabled (6-bit split)" : "Disabled");
        {
            char mustbe_val[6];
            snprintf(mustbe_val, sizeof(mustbe_val), "0x%X", (reg011 >> 5) & 0x03);
            char mustbe_desc[50];
            snprintf(mustbe_desc, sizeof(mustbe_desc), "%s", ((reg011 >> 5) & 0x03) ? "Warning: Should be 0x0" : "Clear");
            print_table_row("", "", "D6:5", "Must be 0", mustbe_val, mustbe_desc);
        }
        print_table_row("", "", "D4", "Invert Tx1",      ((reg011 >> 4) & 0x01) ? "1" : "0", ((reg011 >> 4) & 0x01) ? "Enabled (Multiply by -1)" : "Normal");
        print_table_row("", "", "D3", "Invert Tx2",      ((reg011 >> 3) & 0x01) ? "1" : "0", ((reg011 >> 3) & 0x01) ? "Enabled (Multiply by -1)" : "Normal");
        print_table_row("", "", "D2", "Invert Rx Frame", ((reg011 >> 2) & 0x01) ? "1" : "0", ((reg011 >> 2) & 0x01) ? "Enabled"                  : "Disabled");
        {
            char delay_val[6];
            snprintf(delay_val, sizeof(delay_val), "0x%X", reg011 & 0x03);
            char delay_desc[64];
            snprintf(delay_desc, sizeof(delay_desc), "%u (1/4 clk cycles for DDR)", reg011 & 0x03);
            print_table_row("", "", "D1:0", "Delay Rx Data", delay_val, delay_desc);
        }
    }

    /* Separator */
    print_separator();

    /* Register 0x012 */
    {
        char hex012[7];
        snprintf(hex012, sizeof(hex012), "0x%02X", reg012);
        print_table_row("0x012", hex012, "", "", "", "");
        print_table_row("", "", "D7", "FDD Rx Rate = 2*Tx Rate", ((reg012 >> 7) & 0x01) ? "1" : "0", ((reg012 >> 7) & 0x01) ? "Enabled (Rx 2x Tx)"           : "Disabled (Rx = Tx)");
        print_table_row("", "", "D6", "Swap Ports",              ((reg012 >> 6) & 0x01) ? "1" : "0", ((reg012 >> 6) & 0x01) ? "Enabled (P0 <-> P1)"          : "Disabled");
        print_table_row("", "", "D5", "Single Data Rate",        ((reg012 >> 5) & 0x01) ? "1" : "0", ((reg012 >> 5) & 0x01) ? "SDR (one edge)"               : "DDR (both edges)");
        print_table_row("", "", "D4", "LVDS Mode",               ((reg012 >> 4) & 0x01) ? "1" : "0", ((reg012 >> 4) & 0x01) ? "Enabled (LVDS)"               : "Disabled (CMOS)");
        print_table_row("", "", "D3", "Half-Duplex Mode",        ((reg012 >> 3) & 0x01) ? "1" : "0", ((reg012 >> 3) & 0x01) ? "Enabled (TDD)"                : "Disabled (FDD)");
        print_table_row("", "", "D2", "Single Port Mode",        ((reg012 >> 2) & 0x01) ? "1" : "0", ((reg012 >> 2) & 0x01) ? "Enabled (1 port)"             : "Disabled (2 ports)");
        print_table_row("", "", "D1", "Full Port",               ((reg012 >> 1) & 0x01) ? "1" : "0", ((reg012 >> 1) & 0x01) ? "Enabled (Rx/Tx separated)"    : "Disabled (Mixed)");
        print_table_row("", "", "D0", "Full Duplex Swap Bit",    (reg012 & 0x01)        ? "1" : "0", (reg012 & 0x01)        ? "Enabled (Toggle Rx/Tx bits)"  : "Disabled");
    }

    /* Final Separator */
    print_separator();
    printf("\n");

    m2sdr_close(conn);
}

/* AD9361 ENSM Dump */
/*-----------------*/

static const char* decode_cal_state(uint8_t state)
{
    switch (state) {
        case 0x0: return "Calibrations Done";
        case 0x1: return "Baseband DC Offset Cal";
        case 0x2: return "RF DC Offset Cal";
        case 0x3: return "Tx1 Quadrature Cal";
        case 0x4: return "Tx2 Quadrature Cal";
        case 0x5: return "Receiver Gain Step Cal";
        case 0x9: return "Baseband Cal Flush";
        case 0xA: return "RF Cal Flush";
        case 0xB: return "Tx Quad Cal Flush";
        case 0xC: return "Tx Power Detector Cal Flush";
        case 0xE: return "Rx Gain Step Cal Flush";
        case 0xF: return "Unknown";
        default: return "Reserved";
    }
}

static const char* decode_ensm_state(uint8_t state)
{
    switch (state) {
        case 0x0: return "Sleep (Clocks/BB PLL disabled)";
        case 0x1: return "Wait";
        case 0x5: return "Alert (Synthesizers enabled)";
        case 0x6: return "Tx (Tx signal chain enabled)";
        case 0x7: return "Tx Flush";
        case 0x8: return "Rx (Rx signal chain enabled)";
        case 0x9: return "Rx Flush";
        case 0xA: return "FDD (Tx and Rx enabled)";
        case 0xB: return "FDD Flush";
        default: return "Unknown";
    }
}



static void test_ad9361_ensm_dump(void)
{
    void *conn = m2sdr_open();
    /* AD9361 SPI Init */
    m2sdr_ad9361_spi_init(conn, 0);
    uint8_t reg013 = m2sdr_ad9361_spi_read(conn, 0x013);
    uint8_t reg014 = m2sdr_ad9361_spi_read(conn, 0x014);
    uint8_t reg015 = m2sdr_ad9361_spi_read(conn, 0x015);
    uint8_t reg016 = m2sdr_ad9361_spi_read(conn, 0x016);
    uint8_t reg017 = m2sdr_ad9361_spi_read(conn, 0x017);
    printf("\e[1m[> AD9361 ENSM Dump:\e[0m\n");
    printf("--------------------\n");
    /* Separator */
    print_separator();
    /* Table Header */
    print_table_row("Register", "Hex", "Bits", "Field Name", "Value", "Decoding");
    /* Separator */
    print_separator();

    /* Register 0x013 */
    {
        char hex013[7];
        snprintf(hex013, sizeof(hex013), "0x%02X", reg013);
        print_table_row("0x013", hex013, "", "", "", "");
        print_table_row("", "", "D7", "Open",     ((reg013 >> 7) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D6", "Reserved", ((reg013 >> 6) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D5", "Reserved", ((reg013 >> 5) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D4", "Reserved", ((reg013 >> 4) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D3", "Reserved", ((reg013 >> 3) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D2", "Reserved", ((reg013 >> 2) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D1", "Reserved", ((reg013 >> 1) & 0x01) ? "1" : "0", "Reserved");
        print_table_row("", "", "D0", "FDD Mode", ((reg013 >> 0) & 0x01) ? "1" : "0", ((reg013 >> 0) & 0x01) ? "FDD" : "TDD");
    }
    /* Separator */
    print_separator();

    /* Register 0x014 */
    {
        char hex014[7];
        snprintf(hex014, sizeof(hex014), "0x%02X", reg014);
        print_table_row("0x014", hex014, "", "", "", "");
        print_table_row("", "", "D7", "Enable Rx Data Port for Cal", ((reg014 >> 7) & 0x01) ? "1" : "0", ((reg014 >> 7) & 0x01) ? "Enabled"             : "Disabled");
        print_table_row("", "", "D6", "Force Rx On",                 ((reg014 >> 6) & 0x01) ? "1" : "0", ((reg014 >> 6) & 0x01) ? "Force Rx State"      : "Normal");
        print_table_row("", "", "D5", "Force Tx On",                 ((reg014 >> 5) & 0x01) ? "1" : "0", ((reg014 >> 5) & 0x01) ? "Force Tx/FDD State"  : "Normal");
        print_table_row("", "", "D4", "ENSM Pin Control",            ((reg014 >> 4) & 0x01) ? "1" : "0", ((reg014 >> 4) & 0x01) ? "Pin Controlled"      : "SPI Controlled");
        print_table_row("", "", "D3", "Level Mode",                  ((reg014 >> 3) & 0x01) ? "1" : "0", ((reg014 >> 3) & 0x01) ? "Level"               : "Pulse");
        print_table_row("", "", "D2", "Force Alert State",           ((reg014 >> 2) & 0x01) ? "1" : "0", ((reg014 >> 2) & 0x01) ? "Force to Alert/Wait" : "Normal");
        print_table_row("", "", "D1", "Auto Gain Lock",              ((reg014 >> 1) & 0x01) ? "1" : "0", ((reg014 >> 1) & 0x01) ? "Enabled"             : "Disabled");
        print_table_row("", "", "D0", "To Alert",                    ((reg014 >> 0) & 0x01) ? "1" : "0", ((reg014 >> 0) & 0x01) ? "To Alert"            : "To Wait");
    }
    /* Separator */
    print_separator();

    /* Register 0x015 */
    {
        char hex015[7];
        snprintf(hex015, sizeof(hex015), "0x%02X", reg015);
        print_table_row("0x015", hex015, "", "", "", "");
        print_table_row("", "", "D7", "FDD External Control Enable", ((reg015 >> 7) & 0x01) ? "1" : "0", ((reg015 >> 7) & 0x01) ? "Enabled (Independent)" : "Disabled");
        print_table_row("", "", "D6", "Power Down Rx Synth",         ((reg015 >> 6) & 0x01) ? "1" : "0", ((reg015 >> 6) & 0x01) ? "Powered Down"          : "Normal");
        print_table_row("", "", "D5", "Power Down Tx Synth",         ((reg015 >> 5) & 0x01) ? "1" : "0", ((reg015 >> 5) & 0x01) ? "Powered Down"          : "Normal");
        print_table_row("", "", "D4", "TXNRX SPI Control",           ((reg015 >> 4) & 0x01) ? "1" : "0", ((reg015 >> 4) & 0x01) ? "TXNRX/ENRX Control"    : "ENRX/ENTX Control");
        print_table_row("", "", "D3", "Synth Pin Control Mode",      ((reg015 >> 3) & 0x01) ? "1" : "0", ((reg015 >> 3) & 0x01) ? "TXNRX Controls Synth"  : "Bit D4 Controls");
        print_table_row("", "", "D2", "Dual Synth Mode",             ((reg015 >> 2) & 0x01) ? "1" : "0", ((reg015 >> 2) & 0x01) ? "Both Synths Always On" : "Single Synth");
        print_table_row("", "", "D1", "Rx Synth Ready Mask",         ((reg015 >> 1) & 0x01) ? "1" : "0", ((reg015 >> 1) & 0x01) ? "Ignore VCO Cal"        : "Wait for Lock");
        print_table_row("", "", "D0", "Tx Synth Ready Mask",         ((reg015 >> 0) & 0x01) ? "1" : "0", ((reg015 >> 0) & 0x01) ? "Ignore VCO Cal"        : "Wait for Lock");
    }
    /* Separator */
    print_separator();

    /* Register 0x016 */
    {
        char hex016[7];
        snprintf(hex016, sizeof(hex016), "0x%02X", reg016);
        print_table_row("0x016", hex016, "", "", "", "");
        print_table_row("", "", "D7", "Rx BB Tune",       ((reg016 >> 7) & 0x01) ? "1" : "0", ((reg016 >> 7) & 0x01) ? "Start Rx BB Filter Cal" : "Idle");
        print_table_row("", "", "D6", "Tx BB Tune",       ((reg016 >> 6) & 0x01) ? "1" : "0", ((reg016 >> 6) & 0x01) ? "Start Tx BB Filter Cal" : "Idle");
        print_table_row("", "", "D5", "Must be 0",        ((reg016 >> 5) & 0x01) ? "1" : "0", ((reg016 >> 5) & 0x01) ? "Warning: Should be 0"   : "Clear");
        print_table_row("", "", "D4", "Tx Quad Cal",      ((reg016 >> 4) & 0x01) ? "1" : "0", ((reg016 >> 4) & 0x01) ? "Start Tx Quad Cal"      : "Idle");
        print_table_row("", "", "D3", "Rx Gain Step Cal", ((reg016 >> 3) & 0x01) ? "1" : "0", ((reg016 >> 3) & 0x01) ? "Start Rx Gain Step Cal" : "Idle");
        print_table_row("", "", "D2", "Must be 0",        ((reg016 >> 2) & 0x01) ? "1" : "0", ((reg016 >> 2) & 0x01) ? "Warning: Should be 0"   : "Clear");
        print_table_row("", "", "D1", "DC Cal RF Start",  ((reg016 >> 1) & 0x01) ? "1" : "0", ((reg016 >> 1) & 0x01) ? "Start RF DC Cal"        : "Idle");
        print_table_row("", "", "D0", "DC Cal BB Start",  ((reg016 >> 0) & 0x01) ? "1" : "0", ((reg016 >> 0) & 0x01) ? "Start BB DC Cal"        : "Idle");
    }
    /* Separator */
    print_separator();

    /* Register 0x017 */
    {
        char hex017[7];
        snprintf(hex017, sizeof(hex017), "0x%02X", reg017);
        print_table_row("0x017", hex017, "", "", "", "");
        char cal_val[6];
        snprintf(cal_val, sizeof(cal_val), "0x%X", (reg017 >> 4) & 0x0F);
        print_table_row("", "", "D7:4", "Cal Sequence State", cal_val, decode_cal_state((reg017 >> 4) & 0x0F));
        char ensm_val[6];
        snprintf(ensm_val, sizeof(ensm_val), "0x%X", reg017 & 0x0F);
        print_table_row("", "", "D3:0", "ENSM State", ensm_val, decode_ensm_state(reg017 & 0x0F));
    }

    /* Final Separator */
    print_separator();
    printf("\n");

    m2sdr_close(conn);
}

/* Info */
/*------*/

static uint32_t icap_read(void *conn, uint32_t reg)
{
    m2sdr_writel(conn, CSR_ICAP_ADDR_ADDR, reg);
    m2sdr_writel(conn, CSR_ICAP_READ_ADDR, 1);
    while (m2sdr_readl(conn, CSR_ICAP_DONE_ADDR) == 0)
        usleep(1000);
    m2sdr_writel(conn, CSR_ICAP_READ_ADDR, 0);
    return m2sdr_readl(conn, CSR_ICAP_DATA_ADDR);
}

static void info(void)
{
    int i;
    unsigned char soc_identifier[256];

    void *conn = m2sdr_open();

    printf("\e[1m[> SoC Info:\e[0m\n");
    printf("------------\n");

    for (i = 0; i < 256; i ++)
        soc_identifier[i] = m2sdr_readl(conn, CSR_IDENTIFIER_MEM_BASE + 4 * i);
    printf("SoC Identifier   : %s.\n", soc_identifier);

#ifdef CSR_CAPABILITY_BASE
    uint32_t api_version = m2sdr_readl(conn, CSR_CAPABILITY_API_VERSION_ADDR);
    int major = api_version >> 16;
    int minor = api_version & 0xffff;
    printf("API Version      : %d.%d\n", major, minor);

    uint32_t features = m2sdr_readl(conn, CSR_CAPABILITY_FEATURES_ADDR);
    bool pcie_enabled = (features >> CSR_CAPABILITY_FEATURES_PCIE_OFFSET) & ((1 << CSR_CAPABILITY_FEATURES_PCIE_SIZE) - 1);
    bool eth_enabled  = (features >> CSR_CAPABILITY_FEATURES_ETH_OFFSET)  & ((1 << CSR_CAPABILITY_FEATURES_ETH_SIZE)  - 1);
    bool sata_enabled = (features >> CSR_CAPABILITY_FEATURES_SATA_OFFSET) & ((1 << CSR_CAPABILITY_FEATURES_SATA_SIZE) - 1);
    bool gpio_enabled = (features >> CSR_CAPABILITY_FEATURES_GPIO_OFFSET) & ((1 << CSR_CAPABILITY_FEATURES_GPIO_SIZE) - 1);
    bool wr_enabled   = (features >> CSR_CAPABILITY_FEATURES_WR_OFFSET)   & ((1 << CSR_CAPABILITY_FEATURES_WR_SIZE)   - 1);
    bool jtagbone_enabled = (features >> CSR_CAPABILITY_FEATURES_JTAGBONE_OFFSET) & ((1 << CSR_CAPABILITY_FEATURES_JTAGBONE_SIZE) - 1);

    {
        uint32_t board_info = m2sdr_readl(conn, CSR_CAPABILITY_BOARD_INFO_ADDR);
        int variant = (board_info >> CSR_CAPABILITY_BOARD_INFO_VARIANT_OFFSET) & ((1 << CSR_CAPABILITY_BOARD_INFO_VARIANT_SIZE) - 1);
        const char *variant_str[] = {"M.2", "Baseboard", "Reserved", "Reserved"};
        const char *variant_name  = (variant < 4) ? variant_str[variant] : "Unknown";
        printf("Board:\n");
        printf("  Variant        : %s\n", variant_name);
    }

    printf("Features:\n");
    printf("  PCIe           : %s\n", pcie_enabled ? "Yes" : "No");
    printf("  Ethernet       : %s\n", eth_enabled  ? "Yes" : "No");
    printf("  SATA           : %s\n", sata_enabled ? "Yes" : "No");
    printf("  GPIO           : %s\n", gpio_enabled ? "Yes" : "No");
    printf("  White Rabbit   : %s\n", wr_enabled   ? "Yes" : "No");
    printf("  JTAGBone       : %s\n", jtagbone_enabled ? "Yes" : "No");

    if (pcie_enabled) {
        uint32_t pcie_config = m2sdr_readl(conn, CSR_CAPABILITY_PCIE_CONFIG_ADDR);
        int pcie_speed = (pcie_config >> CSR_CAPABILITY_PCIE_CONFIG_SPEED_OFFSET) & ((1 << CSR_CAPABILITY_PCIE_CONFIG_SPEED_SIZE) - 1);
        int pcie_lanes = (pcie_config >> CSR_CAPABILITY_PCIE_CONFIG_LANES_OFFSET) & ((1 << CSR_CAPABILITY_PCIE_CONFIG_LANES_SIZE) - 1);
        bool pcie_ptm  = (pcie_config >> CSR_CAPABILITY_PCIE_CONFIG_PTM_OFFSET)   & ((1 << CSR_CAPABILITY_PCIE_CONFIG_PTM_SIZE) - 1);
        const char *pcie_speed_str[] = {"Gen1", "Gen2"};
        const char *pcie_lanes_str[] = {"x1", "x2", "x4"};
        printf("  PCIe Speed     : %s\n", pcie_speed_str[pcie_speed]);
        printf("  PCIe Lanes     : %s\n", pcie_lanes_str[pcie_lanes]);
        printf("  PCIe PTM       : %s\n", pcie_ptm ? "Enabled" : "Disabled");
    }

    if (eth_enabled) {
        uint32_t eth_config = m2sdr_readl(conn, CSR_CAPABILITY_ETH_CONFIG_ADDR);
        int eth_speed = (eth_config >> CSR_CAPABILITY_ETH_CONFIG_SPEED_OFFSET) & ((1 << CSR_CAPABILITY_ETH_CONFIG_SPEED_SIZE) - 1);
        const char *eth_speed_str[] = {"1Gbps", "2.5Gbps"};
        printf("  Ethernet Speed : %s\n", eth_speed_str[eth_speed]);
        {
            uint32_t board_info = m2sdr_readl(conn, CSR_CAPABILITY_BOARD_INFO_ADDR);
            int eth_sfp  = (board_info >> CSR_CAPABILITY_BOARD_INFO_ETH_SFP_OFFSET) & ((1 << CSR_CAPABILITY_BOARD_INFO_ETH_SFP_SIZE) - 1);
            printf("  Ethernet SFP   : %d\n", eth_sfp);
        }
    }

    if (sata_enabled) {
        uint32_t sata_config = m2sdr_readl(conn, CSR_CAPABILITY_SATA_CONFIG_ADDR);
        int sata_gen = (sata_config >> CSR_CAPABILITY_SATA_CONFIG_GEN_OFFSET) & ((1 << CSR_CAPABILITY_SATA_CONFIG_GEN_SIZE) - 1);
        const char *sata_gen_str[] = {"Gen1", "Gen2", "Gen3"};
        printf("  SATA Gen       : %s\n", sata_gen_str[sata_gen]);
        int sata_mode = (sata_config >> CSR_CAPABILITY_SATA_CONFIG_MODE_OFFSET) & ((1 << CSR_CAPABILITY_SATA_CONFIG_MODE_SIZE) - 1);
        const char *sata_mode_str[] = {"Read-only", "Write-only", "Read+Write", "Reserved"};
        const char *sata_mode_name = (sata_mode < 4) ? sata_mode_str[sata_mode] : "Unknown";
        printf("  SATA Mode      : %s\n", sata_mode_name);
    }

    if (wr_enabled) {
        uint32_t board_info = m2sdr_readl(conn, CSR_CAPABILITY_BOARD_INFO_ADDR);
        int wr_sfp   = (board_info >> CSR_CAPABILITY_BOARD_INFO_WR_SFP_OFFSET)  & ((1 << CSR_CAPABILITY_BOARD_INFO_WR_SFP_SIZE)  - 1);
        printf("  WR SFP         : %d\n", wr_sfp);
    }
#endif
    printf("\n");

    printf("\e[1m[> FPGA Info:\e[0m\n");
    printf("-------------\n");

#ifdef CSR_DNA_BASE
    printf("FPGA DNA         : 0x%08x%08x\n",
        m2sdr_readl(conn, CSR_DNA_ID_ADDR + 4 * 0),
        m2sdr_readl(conn, CSR_DNA_ID_ADDR + 4 * 1)
    );
#endif
#ifdef CSR_XADC_BASE
    printf("FPGA Temperature : %0.1f °C\n",
           (double)m2sdr_readl(conn, CSR_XADC_TEMPERATURE_ADDR) * 503.975/4096 - 273.15);
    printf("FPGA VCC-INT     : %0.2f V\n",
           (double)m2sdr_readl(conn, CSR_XADC_VCCINT_ADDR) / 4096 * 3);
    printf("FPGA VCC-AUX     : %0.2f V\n",
           (double)m2sdr_readl(conn, CSR_XADC_VCCAUX_ADDR) / 4096 * 3);
    printf("FPGA VCC-BRAM    : %0.2f V\n",
           (double)m2sdr_readl(conn, CSR_XADC_VCCBRAM_ADDR) / 4096 * 3);
#endif
#ifdef CSR_ICAP_BASE
    uint32_t status;
    status = icap_read(conn, ICAP_BOOTSTS_REG);
    printf("FPGA Status      : %s\n", (status & ICAP_BOOTSTS_FALLBACK)? "Fallback" : "Operational");
#endif
    printf("\n");

#ifdef CSR_SI5351_BASE
    printf("\e[1m[> SI5351 Info:\e[0m\n");
    printf("---------------\n");
    if (m2sdr_si5351_i2c_check_litei2c(conn)) {
        bool si5351_present = m2sdr_si5351_i2c_poll(conn, SI5351_I2C_ADDR);
        printf("SI5351 Presence  : %s\n", si5351_present ? "Yes" : "No");
        if (si5351_present) {
            uint8_t status;
            m2sdr_si5351_i2c_read(conn, SI5351_I2C_ADDR, 0x00, &status, 1, true);
            printf("Device Status    : 0x%02x\n", status);
            printf("  SYS_INIT       : %s\n", (status & 0x80) ? "Initializing"   : "Ready");
            printf("  LOL_B          : %s\n", (status & 0x40) ? "Unlocked"       : "Locked");
            printf("  LOL_A          : %s\n", (status & 0x20) ? "Unlocked"       : "Locked");
            printf("  LOS            : %s\n", (status & 0x10) ? "Loss of Signal" : "Valid Signal");
            printf("  REVID          : 0x%01x\n", status & 0x03);

            uint8_t rev;
            m2sdr_si5351_i2c_read(conn, SI5351_I2C_ADDR, 0x0F, &rev, 1, true);
            printf("PLL Input Source : 0x%02x\n", rev);
            printf("  PLLB_SRC       : %s\n", (rev & 0x08) ? "CLKIN" : "XTAL");
            printf("  PLLA_SRC       : %s\n", (rev & 0x04) ? "CLKIN" : "XTAL");
        }
    } else {
        printf("Old gateware detected: SI5351 Software I2C access is not supported. Please update gateware.\n");
    }
    printf("\n");
#endif

    printf("\e[1m[> AD9361 Info:\e[0m\n");
    printf("---------------\n");
    m2sdr_ad9361_spi_init(conn, 0);
    uint16_t product_id = m2sdr_ad9361_spi_read(conn, REG_PRODUCT_ID);
    bool ad9361_present = (product_id == 0xa);
    printf("AD9361 Presence    : %s\n", ad9361_present ? "Yes" : "No");
    if (ad9361_present) {
        printf("AD9361 Product ID  : %04x \n", product_id);
        printf("AD9361 Temperature : %0.1f °C\n",
            (double)DIV_ROUND_CLOSEST(m2sdr_ad9361_spi_read(conn, REG_TEMPERATURE) * 1000000, 1140)/1000);
    }

    printf("\n\e[1m[> Board Time:\e[0m\n");
    printf("--------------\n");
    uint32_t ctrl = m2sdr_readl(conn, CSR_TIME_GEN_CONTROL_ADDR);
    m2sdr_writel(conn, CSR_TIME_GEN_CONTROL_ADDR, ctrl | 0x2);
    m2sdr_writel(conn, CSR_TIME_GEN_CONTROL_ADDR, ctrl & ~0x2);
    uint64_t ts    = (
        ((uint64_t) m2sdr_readl(conn, CSR_TIME_GEN_READ_TIME_ADDR + 0)) << 32 |
        ((uint64_t) m2sdr_readl(conn, CSR_TIME_GEN_READ_TIME_ADDR + 4)) <<  0
    );
    time_t seconds = ts / 1000000000ULL;
    uint32_t ms    = (ts % 1000000000ULL) / 1000000;
    struct tm tm;
    localtime_r(&seconds, &tm);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
    printf("Board Time : %s.%03u\n", time_str, ms);

    m2sdr_close(conn);
}


/* FPGA Reg Access */
/*-----------------*/

static void test_reg_write(uint32_t offset, uint32_t value)
{
    void *conn = m2sdr_open();

    m2sdr_writel(conn, offset, value);
    printf("Wrote 0x%08x to reg 0x%08x\n", value, offset);

    m2sdr_close(conn);
}

static void test_reg_read(uint32_t offset)
{
    uint32_t value;

    void *conn = m2sdr_open();

    value = m2sdr_readl(conn, offset);
    printf("Reg 0x%08x: 0x%08x\n", offset, value);

    m2sdr_close(conn);
}

/* Scratch */
/*---------*/

void scratch_test(void)
{
    printf("\e[1m[> Scratch register test:\e[0m\n");
    printf("-------------------------\n");

    void *conn = m2sdr_open();

    /* Write to scratch register. */
    printf("Write 0x12345678 to Scratch register:\n");
    m2sdr_writel(conn, CSR_CTRL_SCRATCH_ADDR, 0x12345678);
    printf("Read: 0x%08x\n", m2sdr_readl(conn, CSR_CTRL_SCRATCH_ADDR));

    /* Read from scratch register. */
    printf("Write 0xdeadbeef to Scratch register:\n");
    m2sdr_writel(conn, CSR_CTRL_SCRATCH_ADDR, 0xdeadbeef);
    printf("Read: 0x%08x\n", m2sdr_readl(conn, CSR_CTRL_SCRATCH_ADDR));

    m2sdr_close(conn);
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
    void *conn = m2sdr_open();
    uint32_t size;
    uint8_t *buf;
    int sector_size;
    int errors;

    /* Get flash sector size and pad size to it. */
    sector_size = m2sdr_flash_get_erase_block_size(conn);
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
    errors = m2sdr_flash_write(conn, buf, base, size, flash_progress, NULL);
    if (errors) {
        printf("Failed %d errors.\n", errors);
        exit(1);
    } else {
        printf("Success.\n");
    }

    /* Free buffer and close connection. */
    free(buf);
    m2sdr_close(conn);
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

#endif /* FLASH_WRITE */

static void flash_read(const char *filename, uint32_t size, uint32_t offset)
{
    void *conn = m2sdr_open();
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

    /* Get flash sector size. */
    sector_size = m2sdr_flash_get_erase_block_size(conn);

    /* Read flash and write to destination file. */
    base = offset;
    for (i = 0; i < size; i++) {
        if ((i % sector_size) == 0) {
            printf("Reading 0x%08x\r", base + i);
            fflush(stdout);
        }
        byte = m2sdr_flash_read(conn, base + i);
        fwrite(&byte, 1, 1, f);
    }

    /* Close destination file and connection. */
    fclose(f);
    m2sdr_close(conn);
}

static void flash_reload(void)
{
    void *conn = m2sdr_open();

    /* Reload FPGA through ICAP.*/
    m2sdr_writel(conn, CSR_ICAP_ADDR_ADDR, ICAP_CMD_REG);
    m2sdr_writel(conn, CSR_ICAP_DATA_ADDR, ICAP_CMD_IPROG);
    m2sdr_writel(conn, CSR_ICAP_WRITE_ADDR, 1);

    /* Notice user to reboot/rescan the hardware.*/
    printf("===========================================================================\n");
    printf("= PLEASE REBOOT YOUR HARDWARE OR RESCAN PCIe BUS TO USE NEW FPGA GATEWARE =\n");
    printf("===========================================================================\n");

    m2sdr_close(conn);
}

#endif /* CSR_FLASH_BASE */

/* DMA */
/*-----*/

#ifdef USE_LITEPCIE

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

    if (litepcie_dma_init(&dma, m2sdr_device, zero_copy))
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

#endif

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

static uint64_t read_64bit_register(void *conn, uint32_t addr)
{
    uint32_t lower = m2sdr_readl(conn, addr + 4);
    uint32_t upper = m2sdr_readl(conn, addr + 0);
    return ((uint64_t)upper << 32) | lower;
}

static void latch_all_clocks(void *conn)
{
    for (int i = 0; i < N_CLKS; i++) {
        m2sdr_writel(conn, latch_addrs[i], 1);
    }
}

static void read_all_clocks(void *conn, uint64_t *values)
{
    for (int i = 0; i < N_CLKS; i++) {
        values[i] = read_64bit_register(conn, value_addrs[i]);
    }
}

static void clk_test(int num_measurements, int delay_between_tests)
{
    void *conn = m2sdr_open();

    printf("\e[1m[> Clk Measurement Test:\e[0m\n");
    printf("-------------------------\n");

    uint64_t previous_values[N_CLKS], current_values[N_CLKS];
    struct timespec start_time, current_time;
    double elapsed_time;

    printf("\e[1m%-8s", "Meas.");
    for (int i = 0; i < N_CLKS; i++) {
        printf("  %-15s", clk_names[i]);
    }
    printf(" (MHz)\e[0m\n");
    printf("--------");
    for (int i = 0; i < N_CLKS; i++) {
        printf("  ---------------");
    }
    printf("\n");

    latch_all_clocks(conn);
    read_all_clocks(conn, previous_values);
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (int i = 0; i < num_measurements; i++) {
        sleep(delay_between_tests);

        latch_all_clocks(conn);
        read_all_clocks(conn, current_values);
        clock_gettime(CLOCK_MONOTONIC, &current_time);

        elapsed_time = (current_time.tv_sec - start_time.tv_sec) +
                       (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
        start_time = current_time;

        printf("%-8d", i + 1);
        for (int clk_index = 0; clk_index < N_CLKS; clk_index++) {
            uint64_t delta_value = current_values[clk_index] - previous_values[clk_index];
            double frequency_mhz = delta_value / (elapsed_time * 1e6);
            printf("  %15.2f", frequency_mhz);
            previous_values[clk_index] = current_values[clk_index];
        }
        printf("\n");
    }

    m2sdr_close(conn);
}

/* VCXO Test */
/*-----------*/

#ifdef CSR_SI5351_BASE

#define VCXO_TEST_PWM_PERIOD             4096   /* PWM period */
#define VCXO_TEST_MEASUREMENT_SAMPLES    10     /* Number of samples for averaging frequency measurements */
#define VCXO_TEST_STABILIZATION_DELAY_MS 100    /* Milliseconds to wait for stabilization */
#define VCXO_TEST_MEASUREMENT_DELAY_MS   100    /* Milliseconds for measurement period */
#define VCXO_TEST_DETECTION_THRESHOLD_HZ 1000.0 /* Threshold for detecting VCXO presence (SI5351B) */

static double measure_frequency(void *conn, int clk_index)
{
    uint64_t previous_value, current_value;
    struct timespec start_time, current_time;
    double elapsed_time;
    double freq_sum = 0.0;

    for (int sample = 0; sample < VCXO_TEST_MEASUREMENT_SAMPLES; sample++) {
        latch_all_clocks(conn);
        previous_value = read_64bit_register(conn, value_addrs[clk_index]);
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        struct timespec ts;
        ts.tv_sec = VCXO_TEST_MEASUREMENT_DELAY_MS / 1000;
        ts.tv_nsec = (VCXO_TEST_MEASUREMENT_DELAY_MS % 1000) * 1000000L;
        nanosleep(&ts, NULL);

        latch_all_clocks(conn);
        current_value = read_64bit_register(conn, value_addrs[clk_index]);
        clock_gettime(CLOCK_MONOTONIC, &current_time);

        elapsed_time = (current_time.tv_sec - start_time.tv_sec) +
                       (current_time.tv_nsec - start_time.tv_nsec) / 1e9;

        uint64_t delta_value = current_value - previous_value;
        freq_sum += delta_value / elapsed_time;
    }

    return freq_sum / VCXO_TEST_MEASUREMENT_SAMPLES;
}

static void vcxo_test(void)
{
    void *conn = m2sdr_open();

    printf("\e[1m[> VCXO Test:\e[0m\n");
    printf("-------------\n");

    /* Find clock index for AD9361 Ref Clk. */
    int clk_index = -1;
    for (int i = 0; i < N_CLKS; i++) {
        if (strcmp(clk_names[i], "AD9361 Ref Clk") == 0) {
            clk_index = i;
            break;
        }
    }
    if (clk_index == -1) {
        fprintf(stderr, "Error: Clock 'AD9361 Ref Clk' not found in clk_names\n");
        m2sdr_close(conn);
        exit(1);
    }

    /* Set PWM period. */
    m2sdr_writel(conn, CSR_SI5351_PWM_PERIOD_ADDR, VCXO_TEST_PWM_PERIOD);
    /* Enable PWM. */
    m2sdr_writel(conn, CSR_SI5351_PWM_ENABLE_ADDR, 1);

    /* Set PWM to 0% and wait for stabilization. */
    m2sdr_writel(conn, CSR_SI5351_PWM_WIDTH_ADDR, 0);
    struct timespec ts_stab;
    ts_stab.tv_sec = VCXO_TEST_STABILIZATION_DELAY_MS / 1000;
    ts_stab.tv_nsec = (VCXO_TEST_STABILIZATION_DELAY_MS % 1000) * 1000000L;
    nanosleep(&ts_stab, NULL);

    /* Detection phase for SI5351B (VCXO) vs SI5351C. */
    double freq_0 = measure_frequency(conn, clk_index);
    m2sdr_writel(conn, CSR_SI5351_PWM_WIDTH_ADDR, VCXO_TEST_PWM_PERIOD / 2);  /* 50% */
    nanosleep(&ts_stab, NULL);
    double freq_50 = measure_frequency(conn, clk_index);
    m2sdr_writel(conn, CSR_SI5351_PWM_WIDTH_ADDR, VCXO_TEST_PWM_PERIOD);  /* 100% */
    nanosleep(&ts_stab, NULL);
    double freq_100 = measure_frequency(conn, clk_index);

    double max_diff = fabs(freq_0 - freq_50) + fabs(freq_100 - freq_50);
    int is_vcxo = (max_diff >= VCXO_TEST_DETECTION_THRESHOLD_HZ);

    if (!is_vcxo) {
        printf("Detected SI5351C (no VCXO), exiting.\n");
        /* Set back PWM to nominal width. */
        m2sdr_writel(conn, CSR_SI5351_PWM_WIDTH_ADDR, VCXO_TEST_PWM_PERIOD / 2);
        m2sdr_close(conn);
        return;
    }

    printf("Detected SI5351B (with VCXO): Max frequency variation %.2f Hz >= threshold %.2f Hz.\n\n", max_diff, VCXO_TEST_DETECTION_THRESHOLD_HZ);

    /* Full test: Reset to 0% and stabilize again. */
    m2sdr_writel(conn, CSR_SI5351_PWM_WIDTH_ADDR, 0);
    nanosleep(&ts_stab, NULL);

    double nominal_frequency_hz = 0.0;
    double min_frequency = 1e12;
    double max_frequency = 0.0;
    double previous_frequency_hz = 0.0;

    /* Print table header. */
    printf("\e[1m%-12s  %-15s  %-15s\e[0m\n", "PWM Width (%)", "Frequency (MHz)", "Variation (Hz)");
    printf("------------  ---------------  ---------------\n");

    for (double pwm_width_percent = 0.0; pwm_width_percent <= 100.0; pwm_width_percent += 10.0) {
        uint32_t pwm_width = (uint32_t)((pwm_width_percent / 100.0) * VCXO_TEST_PWM_PERIOD);

        /* Set PWM width. */
        m2sdr_writel(conn, CSR_SI5351_PWM_WIDTH_ADDR, pwm_width);
        nanosleep(&ts_stab, NULL);

        double frequency_hz = measure_frequency(conn, clk_index);

        if (pwm_width == VCXO_TEST_PWM_PERIOD / 2) {
            nominal_frequency_hz = frequency_hz;
        }

        double variation_hz = (pwm_width_percent != 0.0) ? frequency_hz - previous_frequency_hz : 0.0;

        printf("%-12.2f  %15.6f  %c%14.2f\n",
               pwm_width_percent, frequency_hz / 1e6,
               (variation_hz >= 0 ? '+' : '-'), fabs(variation_hz));

        if (frequency_hz < min_frequency) min_frequency = frequency_hz;
        if (frequency_hz > max_frequency) max_frequency = frequency_hz;

        previous_frequency_hz = frequency_hz;
    }

    /* Set back PWM to nominal width. */
    m2sdr_writel(conn, CSR_SI5351_PWM_WIDTH_ADDR, VCXO_TEST_PWM_PERIOD / 2);

    m2sdr_close(conn);

    /* Calculate PPM and Hz variation from nominal at 50% PWM. */
    double hz_variation_from_nominal_max = max_frequency - nominal_frequency_hz;
    double hz_variation_from_nominal_min = nominal_frequency_hz - min_frequency;
    double ppm_variation_from_nominal_max = (hz_variation_from_nominal_max / nominal_frequency_hz) * 1e6;
    double ppm_variation_from_nominal_min = (hz_variation_from_nominal_min / nominal_frequency_hz) * 1e6;

    printf("\n\e[1m[> Report:\e[0m\n");
    printf("----------\n");
    printf(" Hz Variation from Nominal (50%% PWM): -%10.2f Hz / +%10.2f Hz\n",
           hz_variation_from_nominal_min, hz_variation_from_nominal_max);
    printf("PPM Variation from Nominal (50%% PWM): -%10.2f PPM / +%10.2f PPM\n",
           ppm_variation_from_nominal_min, ppm_variation_from_nominal_max);
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
#ifdef USE_LITEPCIE
           "-c device_num                     Select the device (default = 0).\n"
           "-z                                Enable zero-copy DMA mode.\n"
           "-e                                Use external loopback (default = internal).\n"
           "-w data_width                     Width of data bus (default = 32).\n"
           "-a                                Automatic DMA RX-Delay calibration.\n"
           "-t duration                       Duration of the test in seconds (default = 0, infinite).\n"
#elif USE_LITEETH
           "-i ip_address                     Target IP address for Etherbone (required).\n"
           "-p port                           Port number (default = 1234).\n"
#endif
           "\n"
           "available commands:\n"
           "info                              Get Board information.\n"
           "reg_write offset value            Write to FPGA register.\n"
           "reg_read offset                   Read from FPGA register.\n"
           "\n"
#ifdef USE_LITEPCIE
           "dma_test                          Test DMA.\n"
#endif
           "scratch_test                      Test Scratch register.\n"
           "clk_test                          Test Clks frequencies.\n"
#ifdef  CSR_SI5351_BASE
           "vcxo_test                         Test VCXO frequency variation.\n"
#endif
           "\n"
#ifdef  CSR_SI5351_BASE
           "si5351_init                       Init SI5351.\n"
           "si5351_dump                       Dump SI5351 Registers.\n"
           "si5351_write reg value            Write to SI5351 register.\n"
           "si5351_read reg                   Read from SI5351 register.\n"
           "\n"
#endif
           "ad9361_dump                       Dump AD9361 Registers.\n"
           "ad9361_write reg value            Write to AD9361 register.\n"
           "ad9361_read reg                   Read from AD9361 register.\n"
           "ad9361_port_dump                  Dump AD9361 Port Configuration.\n"
           "ad9361_ensm_dump                  Dump AD9361 ENSM Configuration.\n"
           "\n"
#ifdef CSR_FLASH_BASE
#ifdef FLASH_WRITE
           "flash_write filename [offset]     Write file to SPI Flash.\n"
#endif
           "flash_read filename size [offset] Read from SPI Flash to file.\n"
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
#ifdef USE_LITEPCIE
    static uint8_t m2sdr_device_zero_copy = 0;
    static uint8_t m2sdr_device_external_loopback = 0;
    static int litepcie_data_width = 32;
    static int litepcie_auto_rx_delay = 0;
    static int test_duration = 0; /* Default to 0 for infinite duration.*/
#endif

    /* Parameters. */
    for (;;) {
        #ifdef USE_LITEPCIE
        c = getopt(argc, argv, "hc:w:zeat:");
        #elif USE_LITEETH
        c = getopt(argc, argv, "hi:p:");
        #endif
        if (c == -1)
            break;
        switch(c) {
        case 'h':
            help();
            break;
        #ifdef USE_LITEETH
        case 'i':
            strncpy(m2sdr_ip_address, optarg, sizeof(m2sdr_ip_address) - 1);
            m2sdr_ip_address[sizeof(m2sdr_ip_address) - 1] = '\0';
            break;
        case 'p':
            strncpy(m2sdr_port, optarg, sizeof(m2sdr_port) - 1);
            m2sdr_port[sizeof(m2sdr_port) - 1] = '\0';
            break;
        #endif
        #ifdef USE_LITEPCIE
        case 'c':
            m2sdr_device_num = atoi(optarg);
            break;
        case 'w':
            litepcie_data_width = atoi(optarg);
            break;
        case 'z':
            m2sdr_device_zero_copy = 1;
            break;
        case 'e':
            m2sdr_device_external_loopback = 1;
            break;
        case 'a':
            litepcie_auto_rx_delay = 1;
            break;
        case 't':
            test_duration = atoi(optarg);
            break;
        #endif
        default:
            exit(1);
        }
    }

    /* Show help when too much args. */
    if (optind >= argc)
        help();

    /* Select device. */
    #ifdef USE_LITEPCIE
    snprintf(m2sdr_device, sizeof(m2sdr_device), "/dev/m2sdr%d", m2sdr_device_num);
    #endif

    cmd = argv[optind++];

    /* Info cmds. */
    if (!strcmp(cmd, "info"))
        info();

    /* Reg cmds. */
    else if (!strcmp(cmd, "reg_write")) {
        if (optind + 2 > argc) goto show_help;
        uint32_t offset = strtoul(argv[optind++], NULL, 0);
        uint32_t value = strtoul(argv[optind++], NULL, 0);
        test_reg_write(offset, value);
    }
    else if (!strcmp(cmd, "reg_read")) {
        if (optind + 1 > argc) goto show_help;
        uint32_t offset = strtoul(argv[optind++], NULL, 0);
        test_reg_read(offset);
    }

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
    else if (!strcmp(cmd, "si5351_init"))
        test_si5351_init();
    else if (!strcmp(cmd, "si5351_dump"))
        test_si5351_dump();
    else if (!strcmp(cmd, "si5351_write")) {
        if (optind + 2 > argc) goto show_help;
        uint8_t reg = strtoul(argv[optind++], NULL, 0);
        uint8_t value = strtoul(argv[optind++], NULL, 0);
        test_si5351_write(reg, value);
    }
    else if (!strcmp(cmd, "si5351_read")) {
        if (optind + 1 > argc) goto show_help;
        uint8_t reg = strtoul(argv[optind++], NULL, 0);
        test_si5351_read(reg);
    }
#endif

    /* AD9361 cmds. */
    else if (!strcmp(cmd, "ad9361_dump"))
        test_ad9361_dump();
    else if (!strcmp(cmd, "ad9361_write")) {
        if (optind + 2 > argc) goto show_help;
        uint16_t reg = strtoul(argv[optind++], NULL, 0);
        uint16_t value = strtoul(argv[optind++], NULL, 0);
        test_ad9361_write(reg, value);
    }
    else if (!strcmp(cmd, "ad9361_read")) {
        if (optind + 1 > argc) goto show_help;
        uint16_t reg = strtoul(argv[optind++], NULL, 0);
        test_ad9361_read(reg);
    }
    else if (!strcmp(cmd, "ad9361_port_dump"))
        test_ad9361_port_dump();
    else if (!strcmp(cmd, "ad9361_ensm_dump"))
        test_ad9361_ensm_dump();

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
        if (!confirm_flash_write()) {
            fprintf(stderr, "Aborted.\n");
            exit(1);
        }
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

#ifdef USE_LITEPCIE
    /* DMA cmds. */
    else if (!strcmp(cmd, "dma_test"))
        dma_test(
            m2sdr_device_zero_copy,
            m2sdr_device_external_loopback,
            litepcie_data_width,
            litepcie_auto_rx_delay,
            test_duration);
#endif

    /* Show help otherwise. */
    else
        goto show_help;

    return 0;

show_help:
    help();

    return 0;
}
