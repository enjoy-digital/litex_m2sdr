/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2026 Enjoy Digital.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sample conversions of the streaming path, between the formats of the application (CF32, CS16,
 * CS8) and the samples of the board (12 bits in 16, or 8 bits). The loops that cost have an SSE2
 * version that gives the same results, bit for bit, as the scalar code (test_sample_conv.cpp).
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

/* CF32 to the 8-bit samples of the 8-bit mode: saturated, rounded half away from zero. */
static inline int8_t cf32_to_sc8(float sample, double scale)
{
    if (!std::isfinite(sample))
        return 0;
    const double scaled = sample * scale;
    if (scaled > 127.0)
        return 127;
    if (scaled < -128.0)
        return -128;
    return static_cast<int8_t>(std::lround(scaled));
}

/* 12-bit sample (Q11, in 16 bits) to 8 bits: divided by 16, rounded to nearest (halves away
 * from zero), saturated. */
static inline int8_t sc16_q11_to_sc8(int16_t sample)
{
    /* An arithmetic shift, written without relying on >> of a negative value. */
    const int32_t value = static_cast<int32_t>(sample) + (sample < 0 ? 7 : 8);
    const int32_t shifted = value >= 0 ? value >> 4 : -(((-value) + 15) >> 4);

    if (shifted > 127)
        return 127;
    if (shifted < -128)
        return -128;
    return static_cast<int8_t>(shifted);
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

/* `len` complex samples from `src` (I/Q floats, contiguous) multiplied by `gain`, to `dst`
 * (I/Q int8), `dst_stride` int8 from one complex sample to the next. */
static inline void cf32_to_sc8_block(const float *src, int8_t *dst, uint32_t len,
    uint32_t dst_stride, float gain, float scale)
{
    const double dscale = static_cast<double>(scale);
    uint32_t i = 0;
#if defined(__SSE2__)
    /* Same steps as the scalar code: float product, then double, scale, clamp, round half away
     * from zero (as lround), and 0 for a non-finite product. The scaled value is exact in double
     * (24 bits times a small integer scale), so adding 0.5 of its sign and truncating is lround. */
    const __m128  vgain = _mm_set1_ps(gain);
    const __m128d vs    = _mm_set1_pd(dscale);
    const __m128d vmax  = _mm_set1_pd(127.0);
    const __m128d vmin  = _mm_set1_pd(-128.0);
    const __m128d vhalf = _mm_set1_pd(0.5);
    const __m128d vsign = _mm_set1_pd(-0.0);
    const __m128d vzero = _mm_setzero_pd();
    for (; i + 2 <= len; i += 2) {
        __m128  p  = _mm_mul_ps(_mm_loadu_ps(src + 2 * i), vgain);    /* I0 Q0 I1 Q1. */
        __m128d d0 = _mm_cvtps_pd(p);
        __m128d d1 = _mm_cvtps_pd(_mm_movehl_ps(p, p));
        /* Finite: x - x == 0 (NaN for NaN and +-inf). */
        __m128d f0 = _mm_cmpeq_pd(_mm_sub_pd(d0, d0), vzero);
        __m128d f1 = _mm_cmpeq_pd(_mm_sub_pd(d1, d1), vzero);
        d0 = _mm_min_pd(_mm_max_pd(_mm_mul_pd(d0, vs), vmin), vmax);
        d1 = _mm_min_pd(_mm_max_pd(_mm_mul_pd(d1, vs), vmin), vmax);
        d0 = _mm_and_pd(_mm_add_pd(d0, _mm_or_pd(_mm_and_pd(d0, vsign), vhalf)), f0);
        d1 = _mm_and_pd(_mm_add_pd(d1, _mm_or_pd(_mm_and_pd(d1, vsign), vhalf)), f1);
        __m128i w = _mm_unpacklo_epi64(_mm_cvttpd_epi32(d0), _mm_cvttpd_epi32(d1));
        w = _mm_packs_epi32(w, w);
        w = _mm_packs_epi16(w, w);                                    /* 4 x int8 in the low 32 bits. */
        uint32_t iq = static_cast<uint32_t>(_mm_cvtsi128_si32(w));
        if (dst_stride == 2) {
            std::memcpy(dst + 2 * i, &iq, 4);
        } else {
            uint16_t iq0 = static_cast<uint16_t>(iq), iq1 = static_cast<uint16_t>(iq >> 16);
            std::memcpy(dst + dst_stride * i,       &iq0, 2);
            std::memcpy(dst + dst_stride * (i + 1), &iq1, 2);
        }
    }
#endif
    for (; i < len; i++) {
        dst[dst_stride * i + 0] = cf32_to_sc8(src[2 * i + 0] * gain, dscale); /* I. */
        dst[dst_stride * i + 1] = cf32_to_sc8(src[2 * i + 1] * gain, dscale); /* Q. */
    }
}

/* `len` complex samples from `src` (I/Q int8), `src_stride` int8 from one complex sample to the
 * next, to `dst` (I/Q floats, contiguous). */
static inline void sc8_to_cf32_block(const int8_t *src, float *dst, uint32_t len,
    uint32_t src_stride, float scale)
{
    /* Plain loop: with the parameters passed by value, the compiler vectorizes it. */
    for (uint32_t i = 0; i < len; i++) {
        dst[0] = static_cast<float>(src[0]) / scale; /* I. */
        dst[1] = static_cast<float>(src[1]) / scale; /* Q. */
        dst += 2;
        src += src_stride;
    }
}

/* `len` complex samples from `src` (I/Q int16, Q11), `src_stride` int16 from one complex sample
 * to the next, to `dst` (I/Q int8), `dst_stride` int8 from one to the next. One of the strides
 * is 2: the buffer of the application is contiguous. */
static inline void sc16_q11_to_sc8_block(const int16_t *src, int8_t *dst, uint32_t len,
    uint32_t src_stride, uint32_t dst_stride)
{
    uint32_t i = 0;
#if defined(__SSE2__)
    /* (x + (x < 0 ? 7 : 8)) >> 4, saturated to 8 bits. The saturating add stands for the 32-bit
     * sum of the scalar code: a sum beyond 16 bits is beyond 8 bits after the shift too. */
    if (src_stride == 2) {
        const __m128i veight = _mm_set1_epi16(8);
        for (; i + 8 <= len; i += 8) {
            __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + 2 * i));
            __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + 2 * i + 8));
            a = _mm_srai_epi16(_mm_adds_epi16(a, _mm_add_epi16(veight, _mm_srai_epi16(a, 15))), 4);
            b = _mm_srai_epi16(_mm_adds_epi16(b, _mm_add_epi16(veight, _mm_srai_epi16(b, 15))), 4);
            __m128i w = _mm_packs_epi16(a, b);                        /* 8 x (I, Q) int8. */
            if (dst_stride == 2) {
                _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + 2 * i), w);
            } else {
                /* One (I, Q) pair of int8 at a time, from the 16-bit lanes. */
                int8_t *d = dst + dst_stride * i;
                uint16_t iq;
                iq = static_cast<uint16_t>(_mm_extract_epi16(w, 0)); std::memcpy(d + 0 * dst_stride, &iq, 2);
                iq = static_cast<uint16_t>(_mm_extract_epi16(w, 1)); std::memcpy(d + 1 * dst_stride, &iq, 2);
                iq = static_cast<uint16_t>(_mm_extract_epi16(w, 2)); std::memcpy(d + 2 * dst_stride, &iq, 2);
                iq = static_cast<uint16_t>(_mm_extract_epi16(w, 3)); std::memcpy(d + 3 * dst_stride, &iq, 2);
                iq = static_cast<uint16_t>(_mm_extract_epi16(w, 4)); std::memcpy(d + 4 * dst_stride, &iq, 2);
                iq = static_cast<uint16_t>(_mm_extract_epi16(w, 5)); std::memcpy(d + 5 * dst_stride, &iq, 2);
                iq = static_cast<uint16_t>(_mm_extract_epi16(w, 6)); std::memcpy(d + 6 * dst_stride, &iq, 2);
                iq = static_cast<uint16_t>(_mm_extract_epi16(w, 7)); std::memcpy(d + 7 * dst_stride, &iq, 2);
            }
        }
    } else if (src_stride == 4 && dst_stride == 2) {
        /* Two channels in the samples of the board: every other (I, Q) pair. */
        const __m128i veight = _mm_set1_epi16(8);
        for (; i + 4 <= len; i += 4) {
            __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + 4 * i));
            __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + 4 * i + 8));
            a = _mm_unpacklo_epi64(_mm_shuffle_epi32(a, _MM_SHUFFLE(2, 0, 2, 0)),
                                   _mm_shuffle_epi32(b, _MM_SHUFFLE(2, 0, 2, 0)));
            a = _mm_srai_epi16(_mm_adds_epi16(a, _mm_add_epi16(veight, _mm_srai_epi16(a, 15))), 4);
            _mm_storel_epi64(reinterpret_cast<__m128i *>(dst + 2 * i), _mm_packs_epi16(a, a));
        }
    }
#endif
    for (; i < len; i++) {
        dst[dst_stride * i + 0] = sc16_q11_to_sc8(src[src_stride * i + 0]); /* I. */
        dst[dst_stride * i + 1] = sc16_q11_to_sc8(src[src_stride * i + 1]); /* Q. */
    }
}

/* `len` complex samples (I/Q int8) copied from `src` to `dst`, `src_stride` and `dst_stride`
 * int8 from one complex sample to the next. */
static inline void sc8_copy_block(const int8_t *src, int8_t *dst, uint32_t len,
    uint32_t src_stride, uint32_t dst_stride)
{
    /* The I/Q pair as one 16-bit move, the parameters by value: nothing is read again from
     * the object for each sample, as it is when int8 stores may alias its members. */
    for (uint32_t i = 0; i < len; i++) {
        uint16_t iq;
        std::memcpy(&iq, src, 2);
        std::memcpy(dst, &iq, 2);
        src += src_stride;
        dst += dst_stride;
    }
}

} /* namespace litex_m2sdr */
