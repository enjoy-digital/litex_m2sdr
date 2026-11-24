/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2026 Enjoy Digital.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CF32 <-> 12-bit sample conversion of the streaming path. The CF32 to 12-bit loop has an SSE2
 * version that gives the same results, bit for bit, as the scalar code.
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace litex_m2sdr {

/* CF32 to the 12-bit samples of the RFIC (in 16 bits): saturated. A sample beyond full scale
 * would otherwise overflow the 12 bits and come out with the opposite sign. */
static inline int16_t cf32_to_sc12(float sample, double scale)
{
    if (!std::isfinite(sample))
        return 0;
    double v = std::nearbyint(static_cast<double>(sample) * scale);
    if (v > scale)
        v = scale;
    if (v < -scale)
        v = -scale;
    return static_cast<int16_t>(v);
}

/* 12-bit sample (in the 12 LSBs of 16 bits) to CF32. */
static inline float sc12_to_cf32(int16_t sample, float scale)
{
    /* Mask 12 LSB and sign-extend */
    int16_t v = static_cast<int16_t>(sample << 4) >> 4;
    return static_cast<float>(v) / scale;
}

/* `len` complex samples from `src` (I/Q floats, contiguous) multiplied by `gain`, to `dst`
 * (I/Q int16), `dst_stride` int16 from one complex sample to the next. */
static inline void cf32_to_sc12_block(const float *src, int16_t *dst, uint32_t len,
    uint32_t dst_stride, float gain, float scale)
{
    const double dscale = static_cast<double>(scale);
    uint32_t i = 0;
#if defined(__SSE2__)
    /* Same steps as the scalar code: float product, then double, scale, round to nearest
     * (current rounding mode, as nearbyint), clamp, and 0 for a non-finite product. */
    const __m128  vgain = _mm_set1_ps(gain);
    const __m128d vs    = _mm_set1_pd(dscale);
    const __m128d vmin  = _mm_set1_pd(-dscale);
    const __m128d vzero = _mm_setzero_pd();
    for (; i + 2 <= len; i += 2) {
        __m128  p  = _mm_mul_ps(_mm_loadu_ps(src + 2 * i), vgain);    /* I0 Q0 I1 Q1. */
        __m128d d0 = _mm_cvtps_pd(p);
        __m128d d1 = _mm_cvtps_pd(_mm_movehl_ps(p, p));
        /* Finite: x - x == 0 (NaN for NaN and +-inf). */
        __m128d f0 = _mm_cmpeq_pd(_mm_sub_pd(d0, d0), vzero);
        __m128d f1 = _mm_cmpeq_pd(_mm_sub_pd(d1, d1), vzero);
        /* The bounds are integers, so clamping before rounding is the same as after. */
        d0 = _mm_and_pd(_mm_min_pd(_mm_max_pd(_mm_mul_pd(d0, vs), vmin), vs), f0);
        d1 = _mm_and_pd(_mm_min_pd(_mm_max_pd(_mm_mul_pd(d1, vs), vmin), vs), f1);
        __m128i w = _mm_unpacklo_epi64(_mm_cvtpd_epi32(d0), _mm_cvtpd_epi32(d1));
        w = _mm_packs_epi32(w, w);                                    /* 4 x int16 in the low half. */
        if (dst_stride == 2) {
            _mm_storel_epi64(reinterpret_cast<__m128i *>(dst + 2 * i), w);
        } else {
            uint32_t iq0 = static_cast<uint32_t>(_mm_cvtsi128_si32(w));
            uint32_t iq1 = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_srli_si128(w, 4)));
            std::memcpy(dst + dst_stride * i,       &iq0, 4);
            std::memcpy(dst + dst_stride * (i + 1), &iq1, 4);
        }
    }
#endif
    for (; i < len; i++) {
        dst[dst_stride * i + 0] = cf32_to_sc12(src[2 * i + 0] * gain, dscale); /* I. */
        dst[dst_stride * i + 1] = cf32_to_sc12(src[2 * i + 1] * gain, dscale); /* Q. */
    }
}

/* `len` complex samples from `src` (I/Q int16, 12-bit), `src_stride` int16 from one complex
 * sample to the next, to `dst` (I/Q floats, contiguous). */
static inline void sc12_to_cf32_block(const int16_t *src, float *dst, uint32_t len,
    uint32_t src_stride, float scale)
{
    /* Plain loop: with the parameters passed by value, the compiler vectorizes it. */
    for (uint32_t i = 0; i < len; i++) {
        dst[0] = sc12_to_cf32(src[0], scale); /* I. */
        dst[1] = sc12_to_cf32(src[1], scale); /* Q. */
        dst += 2;
        src += src_stride;
    }
}

} /* namespace litex_m2sdr */
