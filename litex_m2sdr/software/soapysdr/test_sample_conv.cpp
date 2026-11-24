/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2026 Enjoy Digital.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sample conversions of LiteXM2SDRSampleConv.hpp: the block loops (SSE2 on x86-64) against the
 * scalar conversion of each sample, which must give the same bits. `--exhaustive` also runs the
 * CF32 conversions over every float (a minute).
 */

#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "LiteXM2SDRSampleConv.hpp"

using namespace litex_m2sdr;

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        if (failures++ < 20) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } \
    } \
} while (0)

/* Samples of the application: random ones, with the cases that matter mixed in. */
static void fill_cf32(std::vector<float> &x, std::mt19937 &g, float scale)
{
    std::uniform_real_distribution<float> u(-1.3f, 1.3f);
    std::uniform_int_distribution<int> kind(0, 15), step(-300, 300);

    for (auto &v : x) {
        switch (kind(g)) {
        case 0:  v = std::numeric_limits<float>::quiet_NaN(); break;
        case 1:  v = std::numeric_limits<float>::infinity() * (g() & 1 ? 1.0f : -1.0f); break;
        case 2:  v = (step(g) + 0.5f) / scale; break;                  /* Rounding ties. */
        case 3:  v = step(g) / scale; break;                           /* Exact steps. */
        case 4:  v = std::nextafter((step(g) + 0.5f) / scale, g() & 1 ? 2.0f : -2.0f); break;
        case 5:  v = u(g) * 1e-30f; break;                             /* Tiny, and denormal. */
        case 6:  v = u(g) * 1e30f; break;                              /* Far beyond full scale. */
        case 7:  v = g() & 1 ? 0.0f : -0.0f; break;
        default: v = u(g); break;
        }
    }
}

/* CF32 to the samples of the board, 16 or 8 bits. */
template <class T, class Block, class Sample>
static void test_cf32_tx(const char *name, Block block, Sample sample, float scale)
{
    static const int modes[] = { FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO };
    static const float gains[] = { 1.0f, 0.1f, 0.5f, 0.7071f, 2.0f, 16.0f };
    std::mt19937 g(1);
    unsigned long checked = 0;

    for (int mode : modes) {
        std::fesetround(mode);
        for (float gain : gains) {
            for (uint32_t stride = 2; stride <= 4; stride += 2) {
                for (int block_i = 0; block_i < 200; block_i++) {
                    const uint32_t len = 1 + g() % 67;
                    std::vector<float> src(2 * len);
                    std::vector<T> got(stride * len, 99), want(stride * len, 99);

                    fill_cf32(src, g, scale);
                    block(src.data(), got.data(), len, stride, gain, scale);
                    for (uint32_t i = 0; i < len; i++) {
                        want[stride * i + 0] = sample(src[2 * i + 0] * gain, static_cast<double>(scale));
                        want[stride * i + 1] = sample(src[2 * i + 1] * gain, static_cast<double>(scale));
                    }
                    /* The samples of the other channel are left as they were. */
                    CHECK(got == want, "%s: mode %d, gain %g, stride %u, %u samples", name, mode, gain, stride, len);
                    checked += 2 * len;
                }
            }
        }
    }
    std::fesetround(FE_TONEAREST);
    std::printf("%s: %lu values checked\n", name, checked);
}

/* Samples of the board to CF32: every value, every position in a block. */
static void test_rx_cf32(void)
{
    for (uint32_t stride = 2; stride <= 4; stride += 2) {
        for (uint32_t len : { 1u, 7u, 64u, 65536u / 2 }) {
            std::vector<int16_t> s16(stride * len);
            std::vector<int8_t> s8(stride * len);
            std::vector<float> got(2 * len), want(2 * len);

            for (uint32_t i = 0; i < stride * len; i++) {
                s16[i] = static_cast<int16_t>(i * 2053u + 17u);    /* All 16-bit patterns, 4 MSBs included. */
                s8[i]  = static_cast<int8_t>(i * 37u + 5u);
            }
            sc12_to_cf32_block(s16.data(), got.data(), len, stride, 2047.0f);
            for (uint32_t i = 0; i < len; i++) {
                want[2 * i + 0] = sc12_to_cf32(s16[stride * i + 0], 2047.0f);
                want[2 * i + 1] = sc12_to_cf32(s16[stride * i + 1], 2047.0f);
            }
            CHECK(std::memcmp(got.data(), want.data(), 2 * len * sizeof(float)) == 0,
                "sc12_to_cf32_block: stride %u, %u samples", stride, len);
            sc8_to_cf32_block(s8.data(), got.data(), len, stride, 127.0f);
            for (uint32_t i = 0; i < len; i++) {
                want[2 * i + 0] = static_cast<float>(s8[stride * i + 0]) / 127.0f;
                want[2 * i + 1] = static_cast<float>(s8[stride * i + 1]) / 127.0f;
            }
            CHECK(std::memcmp(got.data(), want.data(), 2 * len * sizeof(float)) == 0,
                "sc8_to_cf32_block: stride %u, %u samples", stride, len);
        }
    }
    std::printf("sc12_to_cf32_block, sc8_to_cf32_block: checked\n");
}

/* 16 bits to 8 bits: every 16-bit value, at every phase of the vector loop, both directions
 * (to the board: contiguous source; from the board: contiguous destination). */
static void test_sc16_to_sc8(void)
{
    static const uint32_t strides[][2] = { { 2, 2 }, { 2, 4 }, { 4, 2 } };

    for (const auto &st : strides) {
        for (uint32_t phase = 0; phase < 9; phase++) {
            const uint32_t len = 32768 + phase;                    /* 65536 values, and a tail. */
            std::vector<int16_t> src(st[0] * len, 1234);
            std::vector<int8_t> got(st[1] * len, 99), want(st[1] * len, 99);

            for (uint32_t i = 0; i < len; i++) {
                src[st[0] * i + 0] = static_cast<int16_t>(2 * i + phase);
                src[st[0] * i + 1] = static_cast<int16_t>(2 * i + 1 + phase);
            }
            sc16_q11_to_sc8_block(src.data(), got.data(), len, st[0], st[1]);
            for (uint32_t i = 0; i < len; i++) {
                want[st[1] * i + 0] = sc16_q11_to_sc8(src[st[0] * i + 0]);
                want[st[1] * i + 1] = sc16_q11_to_sc8(src[st[0] * i + 1]);
            }
            CHECK(got == want, "sc16_q11_to_sc8_block: strides %u %u, phase %u", st[0], st[1], phase);
        }
    }
    /* The 8-bit copy, with the same strides. */
    for (const auto &st : strides) {
        const uint32_t len = 1001;
        std::vector<int8_t> src(st[0] * len), got(st[1] * len, 99), want(st[1] * len, 99);

        for (uint32_t i = 0; i < st[0] * len; i++)
            src[i] = static_cast<int8_t>(i * 37u + 5u);
        sc8_copy_block(src.data(), got.data(), len, st[0], st[1]);
        for (uint32_t i = 0; i < len; i++) {
            want[st[1] * i + 0] = src[st[0] * i + 0];
            want[st[1] * i + 1] = src[st[0] * i + 1];
        }
        CHECK(got == want, "sc8_copy_block: strides %u %u", st[0], st[1]);
    }
    /* The scalar conversion itself: x / 16 rounded to nearest, halves away from zero, saturated. */
    for (int x = -32768; x <= 32767; x++) {
        int want = static_cast<int>(std::lround(x / 16.0));

        want = want > 127 ? 127 : want < -128 ? -128 : want;
        CHECK(sc16_q11_to_sc8(static_cast<int16_t>(x)) == want, "sc16_q11_to_sc8(%d)", x);
    }
    std::printf("sc16_q11_to_sc8_block: every 16-bit value checked\n");
}

/* Every float through the CF32 conversions, at gain 1. */
static void test_exhaustive(void)
{
    const uint32_t n = 4096;
    std::vector<float> src(2 * n);
    std::vector<int16_t> got16(2 * n);
    std::vector<int8_t> got8(2 * n);
    uint64_t bad = 0;

    for (uint64_t base = 0; base < (1ull << 32); base += 2 * n) {
        for (uint32_t i = 0; i < 2 * n; i++) {
            const uint32_t bits = static_cast<uint32_t>(base + i);
            std::memcpy(&src[i], &bits, 4);
        }
        cf32_to_sc12_block(src.data(), got16.data(), n, 2, 1.0f, 2047.0f);
        cf32_to_sc8_block(src.data(), got8.data(), n, 2, 1.0f, 127.0f);
        for (uint32_t i = 0; i < 2 * n; i++) {
            bad += got16[i] != cf32_to_sc12(src[i], 2047.0);
            bad += got8[i]  != cf32_to_sc8(src[i], 127.0);
        }
    }
    CHECK(bad == 0, "exhaustive: %llu values differ", static_cast<unsigned long long>(bad));
    std::printf("cf32_to_sc12_block, cf32_to_sc8_block: every float checked\n");
}

int main(int argc, char **argv)
{
    test_cf32_tx<int16_t>("cf32_to_sc12_block", cf32_to_sc12_block, cf32_to_sc12, 2047.0f);
    test_cf32_tx<int8_t>("cf32_to_sc8_block", cf32_to_sc8_block, cf32_to_sc8, 127.0f);
    test_rx_cf32();
    test_sc16_to_sc8();
    if (argc > 1 && std::strcmp(argv[1], "--exhaustive") == 0)
        test_exhaustive();
    if (failures) {
        std::printf("test_sample_conv: %d FAILED\n", failures);
        return 1;
    }
    std::printf("test_sample_conv: all tests passed\n");
    return 0;
}
