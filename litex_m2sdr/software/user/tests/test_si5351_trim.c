/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Register-level tests for the SI5351 PLLB ppm trim, run with the real
 * m2sdr_si5351_i2c.c against an in-memory fake of the LiteI2C master and the
 * SI5351 register file. The fake applies every I2C byte to the register file
 * as it would land on the chip, so the tests can check not only the final
 * register state but every live intermediate P1/P2/P3 combination a retrim
 * passes through.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csr.h"
#include "etherbone.h"
#include "litepcie_helpers.h"
#include "m2sdr_si5351_i2c.h"
#include "m2sdr_config.h"

#define FB_BASE 0x22
#define FB_REGS 8

/* LiteI2C master fake ----------------------------------------------------- */

#define RXQ_DEPTH 16

struct eb_connection {
    /* CSRs. */
    uint32_t settings;
    uint32_t slave;
    uint32_t active;

    /* Open I2C write transaction. */
    int      txn_open;
    unsigned txn_bytes;
    uint8_t  reg_ptr;

    /* RX status queue (one entry per TX FIFO word, as in the gateware). */
    struct {
        uint32_t data;
        int      nack;
        int      unfinished_tx;
    } rxq[RXQ_DEPTH];
    unsigned rx_head, rx_count;

    /* SI5351 register file. */
    uint8_t regs[256];

    /* Transient tracking: worst multiplier deviation (ppm) outside the
     * [band_lo, band_hi] multiplier band, sampled after every applied byte
     * that lands in the feedback block. */
    int    band_armed;
    double band_lo, band_hi;
    double worst_dev_ppm;

    /* Bookkeeping. */
    unsigned txn_count;
    unsigned txn_bytes_max;
    unsigned fb_writes;      /* Bytes applied inside the feedback block. */
    unsigned reg27_writes;   /* Writes touching the P2[19:16]/P3[19:16] reg. */
    unsigned violations;
};

static double fb_multiplier(const uint8_t *fb)
{
    /* Independent decode, cross-checking the library's encoder. */
    uint32_t p3 = ((uint32_t)(fb[5] & 0xF0) << 12) | ((uint32_t)fb[0] << 8) | fb[1];
    uint32_t p1 = ((uint32_t)(fb[2] & 0x03) << 16) | ((uint32_t)fb[3] << 8) | fb[4];
    uint32_t p2 = ((uint32_t)(fb[5] & 0x0F) << 16) | ((uint32_t)fb[6] << 8) | fb[7];

    if (p3 == 0)
        return 0.0;
    return ((double)(p1 + 512) + (double)p2 / (double)p3) / 128.0;
}

static void fake_apply_byte(struct eb_connection *f, uint8_t reg, uint8_t val)
{
    f->regs[reg] = val;
    if (reg >= FB_BASE && reg < FB_BASE + FB_REGS) {
        f->fb_writes++;
        if (reg == 0x27)
            f->reg27_writes++;
        if (f->band_armed) {
            double m   = fb_multiplier(&f->regs[FB_BASE]);
            double dev = 0.0;
            if (m < f->band_lo)
                dev = (f->band_lo - m) / f->band_lo * 1e6;
            if (m > f->band_hi)
                dev = (m - f->band_hi) / f->band_hi * 1e6;
            if (dev > f->worst_dev_ppm)
                f->worst_dev_ppm = dev;
        }
    }
}

static void fake_rx_push(struct eb_connection *f, uint32_t data, int nack, int unfinished)
{
    if (f->rx_count == RXQ_DEPTH) {
        f->violations++;
        return;
    }
    unsigned tail = (f->rx_head + f->rx_count) % RXQ_DEPTH;
    f->rxq[tail].data          = data;
    f->rxq[tail].nack          = nack;
    f->rxq[tail].unfinished_tx = unfinished;
    f->rx_count++;
}

static void fake_rxtx_write(struct eb_connection *f, uint32_t word)
{
    unsigned len_tx = f->settings & 0x7;
    unsigned len_rx = (f->settings >> 8) & 0x7;
    unsigned chunk  = len_tx > 4 ? 4 : len_tx;
    int      more   = len_tx > 4;
    unsigned i;

    if (!f->active || len_tx == 0) {
        f->violations++;
        return;
    }

    if (len_rx > 0) {
        /* Register read: one address byte out, len_rx bytes back. */
        if (f->txn_open || len_tx != 1 || len_rx != 1) {
            f->violations++;
            return;
        }
        f->reg_ptr = word & 0xFF;
        fake_rx_push(f, f->regs[f->reg_ptr], 0, 0);
        return;
    }

    if (!f->txn_open) {
        f->txn_open  = 1;
        f->txn_bytes = 0;
        f->txn_count++;
    }
    for (i = 0; i < chunk; i++) {
        uint8_t byte = (word >> (8 * (chunk - 1 - i))) & 0xFF;
        if (f->txn_bytes == 0)
            f->reg_ptr = byte;
        else
            fake_apply_byte(f, f->reg_ptr++, byte);
        f->txn_bytes++;
    }
    if (f->txn_bytes > f->txn_bytes_max)
        f->txn_bytes_max = f->txn_bytes;
    if (more) {
        fake_rx_push(f, 0, 0, 1);
    } else {
        f->txn_open = 0;
        fake_rx_push(f, 0, 0, 0);
    }
}

void eb_write32(struct eb_connection *f, uint32_t val, uint32_t addr)
{
    switch (addr) {
    case CSR_SI5351_I2C_MASTER_SETTINGS_ADDR:
        f->settings = val;
        break;
    case CSR_SI5351_I2C_MASTER_ADDR_ADDR:
        f->slave = val;
        break;
    case CSR_SI5351_I2C_MASTER_ACTIVE_ADDR:
        /* Dropping active mid-transaction would abort it on the bus. */
        if (val == 0 && f->txn_open)
            f->violations++;
        f->active = val;
        break;
    case CSR_SI5351_I2C_MASTER_RXTX_ADDR:
        fake_rxtx_write(f, val);
        break;
    default:
        f->violations++;
        break;
    }
}

uint32_t eb_read32(struct eb_connection *f, uint32_t addr)
{
    switch (addr) {
    case CSR_SI5351_I2C_MASTER_STATUS_ADDR: {
        uint32_t status = 1 << CSR_SI5351_I2C_MASTER_STATUS_TX_READY_OFFSET;
        if (f->rx_count) {
            status |= 1 << CSR_SI5351_I2C_MASTER_STATUS_RX_READY_OFFSET;
            status |= f->rxq[f->rx_head].nack << CSR_SI5351_I2C_MASTER_STATUS_NACK_OFFSET;
            status |= f->rxq[f->rx_head].unfinished_tx << CSR_SI5351_I2C_MASTER_STATUS_TX_UNFINISHED_OFFSET;
        }
        return status;
    }
    case CSR_SI5351_I2C_MASTER_RXTX_ADDR: {
        uint32_t data;
        if (!f->rx_count)
            return 0;
        data = f->rxq[f->rx_head].data;
        f->rx_head = (f->rx_head + 1) % RXQ_DEPTH;
        f->rx_count--;
        return data;
    }
    case CSR_SI5351_BASE:
        return 0x4C495832; /* Anything but the pre-LiteI2C magic. */
    default:
        f->violations++;
        return 0;
    }
}

int eb_read32_checked(struct eb_connection *f, uint32_t addr, uint32_t *val)
{
    *val = eb_read32(f, addr);
    return 0;
}

int eb_get_last_error(struct eb_connection *f)
{
    (void)f;
    return EB_ERR_OK;
}

/* The fake runs on the Etherbone branch; the PCIe one must stay unused. */
void litepcie_writel(int fd, uint32_t addr, uint32_t val)
{
    (void)fd;
    (void)addr;
    (void)val;
    fprintf(stderr, "unexpected litepcie_writel\n");
    exit(1);
}

uint32_t litepcie_readl(int fd, uint32_t addr)
{
    (void)fd;
    (void)addr;
    fprintf(stderr, "unexpected litepcie_readl\n");
    exit(1);
}

/* Test helpers ------------------------------------------------------------ */

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

struct table {
    const char *name;
    const uint8_t (*config)[2];
    size_t length;
};

#define TABLE(t) { #t, t, sizeof(t) / sizeof(t[0]) }

static const struct table tables[] = {
    TABLE(si5351_xo_38p4m_config),
    TABLE(si5351_clkin_10m_38p4m_config),
    TABLE(si5351_xo_40m_config),
    TABLE(si5351_clkin_10m_40m_config),
};

static struct eb_connection fake;
static void *conn = &fake;

/* 128*m_nom of a table as an exact rational, for bit-exactness checks. */
static void table_nominal(const struct table *t, uint64_t *num, uint64_t *den, uint8_t fb[FB_REGS])
{
    size_t i;
    uint32_t p1, p2, p3;

    for (i = 0; i < t->length; i++)
        if (t->config[i][0] >= FB_BASE && t->config[i][0] < FB_BASE + FB_REGS)
            fb[t->config[i][0] - FB_BASE] = t->config[i][1];
    p3 = ((uint32_t)(fb[5] & 0xF0) << 12) | ((uint32_t)fb[0] << 8) | fb[1];
    p1 = ((uint32_t)(fb[2] & 0x03) << 16) | ((uint32_t)fb[3] << 8) | fb[4];
    p2 = ((uint32_t)(fb[5] & 0x0F) << 16) | ((uint32_t)fb[6] << 8) | fb[7];
    *num = (uint64_t)(p1 + 512) * p3 + p2;
    *den = p3;
}

/* Apply the table to the fake and seed the trim state, as the RF init does. */
static void table_reset(const struct table *t, struct m2sdr_si5351_pllb_state *state)
{
    size_t i;

    memset(&fake, 0, sizeof(fake));
    for (i = 0; i < t->length; i++)
        fake.regs[t->config[i][0]] = t->config[i][1];
    CHECK(m2sdr_si5351_pllb_state_from_config(state, t->config, t->length));
}

static double target_multiplier(const struct table *t, double ppm)
{
    uint64_t num, den;
    uint8_t fb[FB_REGS];

    table_nominal(t, &num, &den, fb);
    return (double)num / (double)den / 128.0 / (1.0 + ppm * 1e-6);
}

static double residual_ppm(const struct table *t, double ppm)
{
    double m_t   = target_multiplier(t, ppm);
    double m_ach = fb_multiplier(&fake.regs[FB_BASE]);

    return fabs((m_ach - m_t) / m_t) * 1e6;
}

static uint32_t rng = 0x5351;

static double rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return (double)(rng >> 8) / (double)(1u << 24);
}

/* Tests -------------------------------------------------------------------- */

static void test_config_applies_table(void)
{
    const struct table *t = &tables[0];
    size_t i;

    memset(&fake, 0, sizeof(fake));
    CHECK(m2sdr_si5351_i2c_config_checked(conn, SI5351_I2C_ADDR, t->config, t->length));
    for (i = 0; i < t->length; i++)
        CHECK(fake.regs[t->config[i][0]] == t->config[i][1]);
    /* The config sequencer path stays strictly single-register: two bytes
     * (address + value) per transaction. */
    CHECK(fake.txn_bytes_max == 2);
    CHECK(fake.violations == 0);
}

static void test_window(void)
{
    double ppm_min, ppm_max;

    /* XO 25 MHz table, m = 33.792: about +87 / -144 ppm. */
    CHECK(m2sdr_si5351_pllb_trim_window(tables[0].config, tables[0].length, &ppm_min, &ppm_max));
    CHECK(fabs(ppm_max - 86.9) < 0.5);
    CHECK(fabs(ppm_min + 144.2) < 0.5);

    /* CLKIN 10 MHz table, m = 84.48: about +41 / -52 ppm. */
    CHECK(m2sdr_si5351_pllb_trim_window(tables[1].config, tables[1].length, &ppm_min, &ppm_max));
    CHECK(fabs(ppm_max - 40.7) < 0.5);
    CHECK(fabs(ppm_min + 51.8) < 0.5);

    /* Integer tables sit on the window edge: positive trims re-centre. */
    CHECK(m2sdr_si5351_pllb_trim_window(tables[2].config, tables[2].length, &ppm_min, &ppm_max));
    CHECK(ppm_max == 0.0);
    CHECK(ppm_min < -200.0);
    CHECK(m2sdr_si5351_pllb_trim_window(tables[3].config, tables[3].length, &ppm_min, &ppm_max));
    CHECK(ppm_max == 0.0);
    CHECK(ppm_min < -90.0);
}

static void test_zero_ppm_round_trip(void)
{
    struct m2sdr_si5351_pllb_state state;
    struct m2sdr_si5351_pllb_trim_report report;
    uint64_t num, den;
    uint8_t fb[FB_REGS];
    size_t i;

    /* The XO 38.4MHz table already uses P3 = 1e6 with the trim's P1: a
     * 0 ppm trim must leave the shipped registers untouched, bit-exactly. */
    table_reset(&tables[0], &state);
    table_nominal(&tables[0], &num, &den, fb);
    CHECK(m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
        tables[0].config, tables[0].length, 0.0, &state, &report));
    CHECK(report.bytes_written == 0);
    CHECK(report.glitch_free);
    CHECK(!report.recentred);
    CHECK(memcmp(&fake.regs[FB_BASE], fb, FB_REGS) == 0);

    /* The other tables ship a different (P2, P3) encoding (e.g. 11/25 on
     * CLKIN), so the first trim normalizes P3 to 1e6: same exact multiplier,
     * different registers, and that one rewrite is not glitch-free. */
    for (i = 1; i < 4; i++) {
        uint32_t p1n, p2n, p3n, p1t, p2t, p3t;
        uint8_t fbt[FB_REGS];

        table_reset(&tables[i], &state);
        table_nominal(&tables[i], &num, &den, fb);
        CHECK(m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
            tables[i].config, tables[i].length, 0.0, &state, &report));
        CHECK(report.bytes_written > 0);
        CHECK(!report.glitch_free);
        CHECK(report.recentred);
        memcpy(fbt, &fake.regs[FB_BASE], FB_REGS);
        p3n = ((uint32_t)(fb[5] & 0xF0) << 12) | ((uint32_t)fb[0] << 8) | fb[1];
        p1n = ((uint32_t)(fb[2] & 0x03) << 16) | ((uint32_t)fb[3] << 8) | fb[4];
        p2n = ((uint32_t)(fb[5] & 0x0F) << 16) | ((uint32_t)fb[6] << 8) | fb[7];
        p3t = ((uint32_t)(fbt[5] & 0xF0) << 12) | ((uint32_t)fbt[0] << 8) | fbt[1];
        p1t = ((uint32_t)(fbt[2] & 0x03) << 16) | ((uint32_t)fbt[3] << 8) | fbt[4];
        p2t = ((uint32_t)(fbt[5] & 0x0F) << 16) | ((uint32_t)fbt[6] << 8) | fbt[7];
        /* P1 is unchanged; only the fractional encoding is normalized. */
        CHECK(p1t == p1n);
        CHECK(p3t == 1000000);
        /* Exact rational round trip: (128m)*p3n == (128m')*p3n. */
        CHECK(((uint64_t)(p1n + 512) * p3n + p2n) * p3t ==
              ((uint64_t)(p1t + 512) * p3t + p2t) * p3n);
    }
    CHECK(fake.violations == 0);
}

static void test_accuracy(void)
{
    size_t i;
    int j;
    double worst = 0.0;

    for (i = 0; i < 4; i++) {
        struct m2sdr_si5351_pllb_state state;
        struct m2sdr_si5351_pllb_trim_report report;
        double ppm_min, ppm_max;

        CHECK(m2sdr_si5351_pllb_trim_window(tables[i].config, tables[i].length, &ppm_min, &ppm_max));
        if (ppm_min < -100.0)
            ppm_min = -100.0;
        if (ppm_max > 100.0)
            ppm_max = 100.0;

        table_reset(&tables[i], &state);
        for (j = 0; j <= 100; j++) {
            double ppm = ppm_min + (ppm_max - ppm_min) * j / 100.0;
            double res;

            CHECK(m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
                tables[i].config, tables[i].length, ppm, &state, &report));
            res = residual_ppm(&tables[i], ppm);
            if (res > worst)
                worst = res;
            CHECK(res <= 0.001);
            /* After the first write normalized P3, in-window retrims keep
             * P1/P3 and stay glitch-free. */
            if (j > 0)
                CHECK(report.glitch_free);
        }
    }
    printf("  accuracy: worst residual across windows %.6f ppm\n", worst);
    CHECK(fake.violations == 0);
}

static void test_transient_walk(void)
{
    size_t i;
    double worst_p2only = 0.0, worst_cross = 0.0, worst_res = 0.0;
    unsigned crossings = 0, steps = 0;

    for (i = 0; i < 4; i++) {
        struct m2sdr_si5351_pllb_state state;
        struct m2sdr_si5351_pllb_trim_report report;
        double ppm_min, ppm_max, ppm = 0.0;
        int j;

        CHECK(m2sdr_si5351_pllb_trim_window(tables[i].config, tables[i].length, &ppm_min, &ppm_max));
        ppm_min = (ppm_min < -100.0 ? -100.0 : ppm_min) * 0.98;
        ppm_max = (ppm_max > 100.0 ? 100.0 : ppm_max) * 0.98;

        table_reset(&tables[i], &state);
        /* Normalize the encoding first, as the RF init's initial trim does. */
        CHECK(m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
            tables[i].config, tables[i].length, 0.0, &state, &report));

        for (j = 0; j < 10000; j++) {
            double m_before = fb_multiplier(&fake.regs[FB_BASE]);
            double m_target, res;
            unsigned reg27_before = fake.reg27_writes;

            ppm += (rnd() - 0.5); /* |delta| < 0.5 ppm. */
            if (ppm < ppm_min)
                ppm = ppm_min;
            if (ppm > ppm_max)
                ppm = ppm_max;
            m_target = target_multiplier(&tables[i], ppm);

            /* Arm the per-byte transient check on the band spanned by the
             * previous and the target multiplier. */
            fake.band_armed    = 1;
            fake.band_lo       = m_before < m_target ? m_before : m_target;
            fake.band_hi       = m_before < m_target ? m_target : m_before;
            fake.worst_dev_ppm = 0.0;

            CHECK(m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
                tables[i].config, tables[i].length, ppm, &state, &report));
            fake.band_armed = 0;

            CHECK(report.glitch_free);
            res = residual_ppm(&tables[i], ppm);
            if (res > worst_res)
                worst_res = res;
            CHECK(res <= 0.001);

            /* Each in-window retrim is a single I2C write transaction, so
             * with the burst only the final state counts. Byte-wise, an
             * update that keeps the P2 high nibble must stay within 0.1 ppm
             * of the commanded band; one that carries into it (rare, and
             * atomic within the same burst) may step a 65536 P2 count. */
            if (fake.reg27_writes != reg27_before) {
                crossings++;
                if (fake.worst_dev_ppm > worst_cross)
                    worst_cross = fake.worst_dev_ppm;
            } else {
                if (fake.worst_dev_ppm > worst_p2only)
                    worst_p2only = fake.worst_dev_ppm;
                CHECK(fake.worst_dev_ppm <= 0.1);
            }
            steps++;
        }
    }
    printf("  transient: %u retrims, worst residual %.6f ppm,\n"
           "             byte-wise worst deviation %.4f ppm (P2 low bytes only),\n"
           "             %u P2 nibble carries, worst %.2f ppm within their burst\n",
           steps, worst_res, worst_p2only, crossings, worst_cross);
    CHECK(fake.violations == 0);
}

static void test_window_crossing(void)
{
    const struct table *t = &tables[0]; /* +86.9 ppm window edge. */
    struct m2sdr_si5351_pllb_state state;
    struct m2sdr_si5351_pllb_trim_report report;

    table_reset(t, &state);

    /* Outside the nominal window: detected, re-centred, still accurate. */
    CHECK(m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
        t->config, t->length, 95.0, &state, &report));
    CHECK(report.recentred);
    CHECK(!report.glitch_free);
    CHECK(residual_ppm(t, 95.0) <= 0.001);

    /* The re-centred P1 opens a window around the new operating point. */
    CHECK(m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
        t->config, t->length, 95.1, &state, &report));
    CHECK(report.glitch_free);
    CHECK(residual_ppm(t, 95.1) <= 0.001);

    /* Jumping back to nominal re-centres again and lands on the shipped
     * encoding. */
    CHECK(m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
        t->config, t->length, 0.0, &state, &report));
    CHECK(report.recentred);
    CHECK(residual_ppm(t, 0.0) <= 1e-6);
    CHECK(fake.violations == 0);
}

static void test_burst_write(void)
{
    const struct table *t = &tables[1]; /* First trim rewrites P3: full span. */
    struct m2sdr_si5351_pllb_state state;
    struct m2sdr_si5351_pllb_trim_report report;
    unsigned txn_before;

    table_reset(t, &state);
    txn_before = fake.txn_count;
    CHECK(m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
        t->config, t->length, 0.5, &state, &report));
    /* Whole-block rewrite: one transaction of address + 8 data bytes. */
    CHECK(fake.txn_count == txn_before + 1);
    CHECK(fake.txn_bytes_max == 9);
    CHECK(residual_ppm(t, 0.5) <= 0.001);

    /* The follow-up retrim moves only the P2 low bytes: one transaction,
     * address + 2 data bytes. */
    txn_before = fake.txn_count;
    fake.txn_bytes_max = 0;
    CHECK(m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
        t->config, t->length, 0.6, &state, &report));
    CHECK(report.glitch_free);
    CHECK(fake.txn_count == txn_before + 1);
    CHECK(fake.txn_bytes_max == 3);
    CHECK(residual_ppm(t, 0.6) <= 0.001);
    CHECK(fake.violations == 0);
}

static void test_stateless_compat(void)
{
    const struct table *t = &tables[0];
    struct m2sdr_si5351_pllb_state state;

    table_reset(t, &state);
    /* Without device state the full block is rewritten, as before. */
    CHECK(m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
        t->config, t->length, -5.75, NULL, NULL));
    CHECK(residual_ppm(t, -5.75) <= 0.001);

    /* Requests leaving the valid multiplier range are rejected before
     * touching the device (the public +-100 ppm limit is the callers'). */
    CHECK(!m2sdr_si5351_i2c_trim_pllb_ppm(conn, SI5351_I2C_ADDR,
        t->config, t->length, -9e5, &state, NULL));
    CHECK(fake.violations == 0);
}

int main(void)
{
    test_config_applies_table();
    test_window();
    test_zero_ppm_round_trip();
    test_accuracy();
    test_transient_walk();
    test_window_crossing();
    test_burst_write();
    test_stateless_compat();
    printf("test_si5351_trim: ok\n");
    return 0;
}
