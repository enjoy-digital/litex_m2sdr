/* SPDX-License-Identifier: BSD-2-Clause
 *
 * M2SDR signal analysis: ADS-B, Mode S extended squitters (DF17, DF18) at 1090 MHz.
 *
 * This file is part of LiteX-M2SDR.
 *
 * Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
 *
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "m2sdr_analysis_wireless.h"

/* The samples and the products here are finite: complex products without the NaN and infinity
 * handling of C99 (__mulsc3). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("cx-limited-range")
#endif

#define HALF_US            0.5     /* A chip: half of a bit. */
#define PREAMBLE_CHIPS     16      /* 8 us. */
#define LONG_BITS          112
#define MESSAGE_US         120.0   /* Preamble and 112 bits. */
#define CRC_POLY           0xfff409u   /* Generator of the Mode S parity, x^24 term left out. */
#define MIN_PULSE_RATIO    4.0     /* Pulses of a preamble over the noise floor: 6 dB. */
#define MIN_CONTRAST       2.0     /* Pulses of a preamble over its quiet chips: 3 dB. */
#define PHASES             8       /* Steps of the timing per sample. */
#define FLOOR_SAMPLES      65536   /* Samples the median noise floor is taken from. */
#define CPR_PAIR_S         10.0    /* Most time between the even and the odd frame of a pair. */
#define CPR_LOCAL_S        300.0   /* Age of a position single frames are still decoded near. */
#define CPR_NZ             15

/* Chips of the preamble holding a pulse: 0, 1, 3.5 and 4.5 us. */
static const uint8_t pulse_chip[PREAMBLE_CHIPS] = { 1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0 };

/* Mode S parity */
/*---------------*/

uint32_t m2sdr_adsb_crc(const uint8_t *msg, int bits)
{
    uint32_t rem = 0, pi = 0;
    int i;

    if (!msg || (bits != 56 && bits != LONG_BITS))
        return 0xffffffffu;
    /* Division of the first bits - 24 bits by the generator, then the parity sent taken off. */
    for (i = 0; i < bits - 24; i++) {
        const uint32_t bit = (msg[i >> 3] >> (7 - (i & 7))) & 1u;
        const uint32_t top = ((rem >> 23) & 1u) ^ bit;

        rem = (rem << 1) & 0xffffffu;
        if (top)
            rem ^= CRC_POLY;
    }
    for (i = bits - 24; i < bits; i++)
        pi = (pi << 1) | ((msg[i >> 3] >> (7 - (i & 7))) & 1u);
    return rem ^ pi;
}

/* Field Decoding */
/*----------------*/

/* Bits first to last of a message, numbered from 1 as in the standard. */
static uint32_t bits_of(const uint8_t *msg, int first, int last)
{
    uint32_t v = 0;
    int i;

    for (i = first - 1; i < last; i++)
        v = (v << 1) | ((msg[i >> 3] >> (7 - (i & 7))) & 1u);
    return v;
}

int m2sdr_adsb_decode(const uint8_t msg[14], struct m2sdr_adsb_message *m)
{
    static const char charset[65] =
        "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
    int tc, i;

    if (!msg || !m)
        return -1;
    memcpy(m->data, msg, 14);
    m->df   = (int)bits_of(msg, 1, 5);
    m->ca   = (int)bits_of(msg, 6, 8);
    m->icao = bits_of(msg, 9, 32);
    m->tc   = 0;
    m->callsign[0]  = '\0';
    m->has_altitude = m->has_cpr = m->has_position = m->has_velocity = false;
    m->has_track    = m->has_vrate = m->airspeed_true = false;
    m->lat_deg      = m->lon_deg = 0.0;
    m->cpr_odd      = m->cpr_lat = m->cpr_lon = 0;
    if (m->df != 17 && m->df != 18)
        return -1;
    /* DF18: the ME field has the format of DF17 for CF 0 (ICAO address), 1 (other address) and
     * 6 (ADS-R); TIS-B (CF 2, 3, 5) and the others are not read. */
    if (m->df == 18 && m->ca != 0 && m->ca != 1 && m->ca != 6)
        return 0;
    tc = (int)bits_of(msg, 33, 37);
    m->tc = tc;

    if (tc >= 1 && tc <= 4) {
        m->category = (int)bits_of(msg, 38, 40);
        for (i = 0; i < 8; i++)
            m->callsign[i] = charset[bits_of(msg, 41 + 6 * i, 46 + 6 * i)];
        m->callsign[8] = '\0';
        for (i = 7; i >= 0 && (m->callsign[i] == ' ' || m->callsign[i] == '#'); i--)
            m->callsign[i] = '\0';
    } else if (tc >= 9 && tc <= 18) {
        /* ALT, 12 bits: Q (its 8th bit) set, the 11 others count 25 ft from -1000 ft. */
        const uint32_t alt = bits_of(msg, 41, 52);

        if (alt != 0 && (alt & 0x10u)) {
            const uint32_t n = ((alt >> 5) << 4) | (alt & 0xfu);

            m->has_altitude = true;
            m->altitude_ft  = (int)n * 25 - 1000;
        }
        m->has_cpr = true;
        m->cpr_odd = (int)bits_of(msg, 54, 54);
        m->cpr_lat = (int)bits_of(msg, 55, 71);
        m->cpr_lon = (int)bits_of(msg, 72, 88);
    } else if (tc == 19) {
        const int st = (int)bits_of(msg, 38, 40);
        const int vr = (int)bits_of(msg, 70, 78);

        m->velocity_subtype = st;
        if (st == 1 || st == 2) {
            const int vew = (int)bits_of(msg, 47, 56), vns = (int)bits_of(msg, 58, 67);
            const int scale = st == 2 ? 4 : 1;

            if (vew != 0 && vns != 0) {
                const double ve = (bits_of(msg, 46, 46) ? -1.0 : 1.0) * (double)((vew - 1) * scale);
                const double vn = (bits_of(msg, 57, 57) ? -1.0 : 1.0) * (double)((vns - 1) * scale);
                double trk = atan2(ve, vn) * 180.0 / M_PI;

                if (trk < 0.0)
                    trk += 360.0;
                m->has_velocity = m->has_track = true;
                m->speed_kt     = hypot(ve, vn);
                m->track_deg    = trk;
            }
        } else if (st == 3 || st == 4) {
            const int as = (int)bits_of(msg, 58, 67);

            if (bits_of(msg, 46, 46)) {
                m->has_track = true;
                m->track_deg = (double)bits_of(msg, 47, 56) * 360.0 / 1024.0;
            }
            if (as != 0) {
                m->has_velocity  = true;
                m->speed_kt      = (double)((as - 1) * (st == 4 ? 4 : 1));
                m->airspeed_true = bits_of(msg, 57, 57) != 0;
            }
        }
        if ((st >= 1 && st <= 4) && vr != 0) {
            m->has_vrate = true;
            m->vrate_fpm = (bits_of(msg, 69, 69) ? -1 : 1) * (vr - 1) * 64;
        }
    }
    return 0;
}

/* CPR Positions */
/*---------------*/

static double cpr_mod(double a, double b)
{
    return a - b * floor(a / b);
}

/* Longitude zones at a latitude. */
static int cpr_nl(double lat)
{
    const double a = 1.0 - cos(M_PI / (2.0 * CPR_NZ));
    const double c = cos(M_PI / 180.0 * fabs(lat));

    if (fabs(lat) >= 87.0)
        return fabs(lat) > 87.0 ? 1 : 2;
    return (int)floor(2.0 * M_PI / acos(1.0 - a / (c * c)));
}

/* Airborne position from an even (0) and an odd (1) frame; the newest one gives it. */
static int cpr_global(const int lat_cpr[2], const int lon_cpr[2], int newest, double *lat, double *lon)
{
    const double lat0 = lat_cpr[0] / 131072.0, lat1 = lat_cpr[1] / 131072.0;
    const double lon0 = lon_cpr[0] / 131072.0, lon1 = lon_cpr[1] / 131072.0;
    const double j = floor(59.0 * lat0 - 60.0 * lat1 + 0.5);
    double rlat0 = 360.0 / 60.0 * (cpr_mod(j, 60.0) + lat0);
    double rlat1 = 360.0 / 59.0 * (cpr_mod(j, 59.0) + lat1);
    double rlat, rlon, m;
    int nl, ni;

    if (rlat0 >= 270.0)
        rlat0 -= 360.0;
    if (rlat1 >= 270.0)
        rlat1 -= 360.0;
    if (fabs(rlat0) > 90.0 || fabs(rlat1) > 90.0 || cpr_nl(rlat0) != cpr_nl(rlat1))
        return -1;      /* The two frames straddle a zone: wait for another pair. */
    rlat = newest ? rlat1 : rlat0;
    nl   = cpr_nl(rlat);
    ni   = nl - newest > 1 ? nl - newest : 1;
    m    = floor(lon0 * (nl - 1) - lon1 * nl + 0.5);
    rlon = 360.0 / ni * (cpr_mod(m, ni) + (newest ? lon1 : lon0));
    if (rlon >= 180.0)
        rlon -= 360.0;
    *lat = rlat;
    *lon = rlon;
    return 0;
}

/* Airborne position of one frame near a reference (within 180 NM). */
static void cpr_local(int odd, int lat_cpr, int lon_cpr, double ref_lat, double ref_lon, double *lat, double *lon)
{
    const double dlat = 360.0 / (60.0 - odd);
    const double yl   = lat_cpr / 131072.0, xl = lon_cpr / 131072.0;
    const double j    = floor(ref_lat / dlat) + floor(0.5 + cpr_mod(ref_lat, dlat) / dlat - yl);
    double rlat = dlat * (j + yl), dlon, m, rlon;
    const int nl = cpr_nl(rlat);

    dlon = 360.0 / (nl - odd > 1 ? nl - odd : 1);
    m    = floor(ref_lon / dlon) + floor(0.5 + cpr_mod(ref_lon, dlon) / dlon - xl);
    rlon = dlon * (m + xl);
    if (rlon >= 180.0)
        rlon -= 360.0;
    *lat = rlat;
    *lon = rlon;
}

/* Aircraft */
/*----------*/

static struct m2sdr_adsb_aircraft *aircraft_of(struct m2sdr_adsb_state *st, uint32_t icao)
{
    struct m2sdr_adsb_aircraft *ac;
    int i, oldest = 0;

    for (i = 0; i < st->aircraft; i++)
        if (st->ac[i].icao == icao)
            return &st->ac[i];
    if (st->aircraft < M2SDR_ADSB_MAX_AIRCRAFT) {
        ac = &st->ac[st->aircraft++];
    } else {
        /* Full: the one heard last the longest ago makes room. */
        for (i = 1; i < st->aircraft; i++)
            if (st->ac[i].last_s < st->ac[oldest].last_s)
                oldest = i;
        ac = &st->ac[oldest];
    }
    memset(ac, 0, sizeof(*ac));
    ac->icao     = icao;
    ac->cpr_s[0] = ac->cpr_s[1] = -1e300;
    return ac;
}

static void aircraft_update(struct m2sdr_adsb_state *st, const struct m2sdr_adsb_config *cfg,
                            struct m2sdr_adsb_message *m)
{
    struct m2sdr_adsb_aircraft *ac = aircraft_of(st, m->icao);

    ac->messages++;
    ac->last_s     = m->time_s;
    ac->level_dbfs = m->level_dbfs;
    if (m->callsign[0])
        memcpy(ac->callsign, m->callsign, sizeof(ac->callsign));
    if (m->has_altitude) {
        ac->has_altitude = true;
        ac->altitude_ft  = m->altitude_ft;
    }
    if (m->has_velocity && m->has_track) {
        ac->has_velocity = true;
        ac->speed_kt     = m->speed_kt;
        ac->track_deg    = m->track_deg;
    }
    if (m->has_vrate)
        ac->vrate_fpm = m->vrate_fpm;
    if (m->has_cpr) {
        const int f = m->cpr_odd;
        double lat = 0, lon = 0;

        ac->cpr_lat[f] = m->cpr_lat;
        ac->cpr_lon[f] = m->cpr_lon;
        ac->cpr_s[f]   = m->time_s;
        if (fabs(ac->cpr_s[0] - ac->cpr_s[1]) < CPR_PAIR_S &&
            cpr_global(ac->cpr_lat, ac->cpr_lon, f, &lat, &lon) == 0) {
            m->has_position = true;
        } else if (ac->has_position && m->time_s - ac->position_s < CPR_LOCAL_S) {
            cpr_local(f, m->cpr_lat, m->cpr_lon, ac->lat_deg, ac->lon_deg, &lat, &lon);
            m->has_position = true;
        } else if (cfg && cfg->has_reference) {
            cpr_local(f, m->cpr_lat, m->cpr_lon, cfg->ref_lat_deg, cfg->ref_lon_deg, &lat, &lon);
            m->has_position = true;
        }
        if (m->has_position) {
            m->lat_deg = ac->lat_deg = lat;
            m->lon_deg = ac->lon_deg = lon;
            ac->has_position = true;
            ac->position_s   = m->time_s;
        }
    }
}

/* Demodulation */
/*--------------*/

/* A running sum: s[n] is the sum of v before n, so that the mean of v over any span of time,
 * fractional ends included, is two reads. */
struct running {
    const float  *v;
    const double *s;
};

static int cmp_float(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

/* Mean of v over [t, t + w) samples. */
static double chip_mean(const struct running *r, double t, double w)
{
    const size_t a = (size_t)t, b = (size_t)(t + w);
    const double ca = r->s[a] + (t - (double)a) * r->v[a];
    const double cb = r->s[b] + (t + w - (double)b) * r->v[b];

    return (cb - ca) / w;
}

/* Pulses and quiet chips of a preamble starting at t: mean of each. */
static void preamble_levels(const struct running *r, double t, double w, double *hi, double *lo)
{
    double sh = 0, sl = 0;
    int k;

    for (k = 0; k < PREAMBLE_CHIPS; k++) {
        const double e = chip_mean(r, t + k * w, w);

        if (pulse_chip[k])
            sh += e;
        else
            sl += e;
    }
    *hi = sh / 4.0;
    *lo = sl / 12.0;
}

/* Expected magnitude of a chip from what the preamble shows: a chip on (`on`) or off, its left
 * and right neighbours on or off. Off a whole sample, a pulse leaks into the chips next to it
 * (at 2 MSPS, half a sample off, half of it into each): the preamble has isolated pulses, quiet
 * chips on the right of one (3, 10), on the left of one (6), between two (1, 8) and alone,
 * which measure each case. */
struct chip_model {
    double on, from_left, from_right, both, quiet;
};

static double chip_expected(const struct chip_model *cm, int on, int left, int right)
{
    if (on)
        return cm->on + left * (cm->from_left - cm->quiet) + right * (cm->from_right - cm->quiet);
    if (left && right)
        return cm->both;
    return left ? cm->from_left : right ? cm->from_right : cm->quiet;
}

/* The 112 bits from the magnitude, the most likely sequence given the chip model: bit k puts a
 * pulse in chip 2k (1) or 2k + 1 (0), so that chip 2k + 1 depends on bit k + 1 through its
 * neighbour; two states (the last bit) carry that. */
static void detect_bits(const struct running *mag, double t, double w, const struct chip_model *cm,
                        uint8_t data[14], double *conf_min)
{
    double e[2 * LONG_BITS], cost[2], next[2];
    uint8_t from[LONG_BITS][2];
    int k, b, a, s;

    for (k = 0; k < 2 * LONG_BITS; k++)
        e[k] = chip_mean(mag, t + k * w, w);
    /* Bit 0: chip 0 has the quiet last chip of the preamble on its left, chip 1 on its right. */
    for (b = 0; b < 2; b++) {
        const double d = e[0] - chip_expected(cm, b, 0, !b);

        cost[b] = d * d;
    }
    for (k = 1; k < LONG_BITS; k++) {
        for (b = 0; b < 2; b++) {
            next[b] = 1e300;
            for (a = 0; a < 2; a++) {
                /* Chip 2k - 1 (bit k - 1 = a: on when a = 0) between chip 2k - 2 (a) and chip
                 * 2k (b); chip 2k (b) between chip 2k - 1 (!a) and chip 2k + 1 (!b). */
                const double d1 = e[2 * k - 1] - chip_expected(cm, !a, a, b);
                const double d2 = e[2 * k] - chip_expected(cm, b, !a, !b);
                const double c  = cost[a] + d1 * d1 + d2 * d2;

                if (c < next[b]) {
                    next[b]    = c;
                    from[k][b] = (uint8_t)a;
                }
            }
        }
        cost[0] = next[0];
        cost[1] = next[1];
    }
    /* The last chip has nothing after it. */
    for (b = 0; b < 2; b++) {
        const double d = e[2 * LONG_BITS - 1] - chip_expected(cm, !b, b, 0);

        cost[b] += d * d;
    }
    s = cost[1] < cost[0];
    memset(data, 0, 14);
    *conf_min = 1.0;
    for (k = LONG_BITS - 1; k >= 0; k--) {
        /* Confidence: the contrast of the chip holding the pulse over the other one. */
        const double on = e[2 * k + !s], off = e[2 * k + s];
        const double conf = (on - off) / (on + off + 1e-30);

        if (s)
            data[k >> 3] |= (uint8_t)(0x80u >> (k & 7));
        if (conf < *conf_min)
            *conf_min = conf;
        if (k > 0)
            s = from[k][s];
    }
}

/* Chip model of the preamble at t. */
static void preamble_model(const struct running *mag, double t, double w, struct chip_model *cm)
{
    double e[PREAMBLE_CHIPS];
    int k;

    for (k = 0; k < PREAMBLE_CHIPS; k++)
        e[k] = chip_mean(mag, t + k * w, w);
    cm->on    = (e[0] + e[2] + e[7] + e[9]) / 4.0;
    cm->from_left  = (e[3] + e[10]) / 2.0;
    cm->from_right = e[6];
    cm->both       = (e[1] + e[8]) / 2.0;
    cm->quiet = (e[4] + e[5] + e[11] + e[12] + e[13] + e[14]) / 6.0;
}

/* Remainder of the CRC of a message with only bit k set, for the bits after the DF field. */
static void make_syndromes(uint32_t syndrome[LONG_BITS])
{
    int k;

    for (k = 0; k < LONG_BITS; k++) {
        uint8_t e[14] = { 0 };

        e[k >> 3]   = (uint8_t)(0x80u >> (k & 7));
        syndrome[k] = m2sdr_adsb_crc(e, LONG_BITS);
    }
}

/* Bits by the chip holding more magnitude: best with the pulses on the samples. */
static void compare_bits(const struct running *mag, double t, double w, uint8_t data[14], double *conf_min)
{
    int k;

    memset(data, 0, 14);
    *conf_min = 1.0;
    for (k = 0; k < LONG_BITS; k++) {
        const double e1 = chip_mean(mag, t + (2 * k) * w, w), e2 = chip_mean(mag, t + (2 * k + 1) * w, w);
        const double conf = fabs(e1 - e2) / (e1 + e2 + 1e-30);

        if (e1 > e2)
            data[k >> 3] |= (uint8_t)(0x80u >> (k & 7));
        if (conf < *conf_min)
            *conf_min = conf;
    }
}

/* Remainder of a squitter's CRC, or -1 for another format. */
static long squitter_crc(const uint8_t data[14])
{
    if ((data[0] >> 3) != 17 && (data[0] >> 3) != 18)
        return -1;
    return (long)m2sdr_adsb_crc(data, LONG_BITS);
}

/* One wrong bit leaves the remainder of that bit alone: its syndrome. The DF field is left out,
 * the format being already that of a squitter. Returns 0 when corrected. */
static int correct_bit(uint8_t data[14], uint32_t rem, uint32_t syndrome[LONG_BITS], bool *have_syndromes)
{
    int k;

    if (!*have_syndromes) {
        make_syndromes(syndrome);
        *have_syndromes = true;
    }
    for (k = 5; k < LONG_BITS && syndrome[k] != rem; k++)
        ;
    if (k == LONG_BITS)
        return -1;
    data[k >> 3] ^= (uint8_t)(0x80u >> (k & 7));
    return 0;
}

/* A message at t: its bits, right or with one bit corrected. The comparison of the two chips of
 * each bit first (best on the samples), then the sequence of the chip model (pulses across
 * samples); a bit corrected in either, last. Returns 0, or -1. */
static int read_message(const struct running *mag, double t, double w, bool correct, uint32_t syndrome[LONG_BITS],
                        bool *have_syndromes, uint8_t data[14], bool *corrected, double *conf_min)
{
    struct chip_model cm;
    uint8_t seq[14];
    double conf_seq;
    long r1, r2;

    *corrected = false;
    compare_bits(mag, t + PREAMBLE_CHIPS * w, w, data, conf_min);
    r1 = squitter_crc(data);
    if (r1 == 0)
        return 0;
    preamble_model(mag, t, w, &cm);
    detect_bits(mag, t + PREAMBLE_CHIPS * w, w, &cm, seq, &conf_seq);
    r2 = squitter_crc(seq);
    if (r2 == 0) {
        memcpy(data, seq, 14);
        *conf_min = conf_seq;
        return 0;
    }
    if (!correct)
        return -1;
    *corrected = true;
    *conf_min  = 0.0;
    if (r1 > 0 && correct_bit(data, (uint32_t)r1, syndrome, have_syndromes) == 0)
        return 0;
    if (r2 > 0 && correct_bit(seq, (uint32_t)r2, syndrome, have_syndromes) == 0) {
        memcpy(data, seq, 14);
        return 0;
    }
    return -1;
}

int m2sdr_adsb_analyse(const float complex *x, size_t count, double sample_rate, double time_s,
                       const struct m2sdr_adsb_config *cfg, struct m2sdr_adsb_state *state,
                       struct m2sdr_adsb_result *res)
{
    /* Timings tried when the best one does not give a right message, in eighths of a sample. */
    static const int retry[] = { 0, -2, 2, -4, 4 };
    struct m2sdr_adsb_state *tmp = NULL;
    struct running pw, mag;
    uint32_t syndrome[LONG_BITS];
    bool have_syndromes = false;
    float *p = NULL, *a = NULL, *sub = NULL;
    double *cp = NULL, *ca = NULL;
    double noise, w, span;
    size_t i, nsub, step, last;
    int rc = -1;

    if (!x || !res || sample_rate < M2SDR_ADSB_MIN_RATE * (1.0 - 1e-6))
        return -1;
    w    = HALF_US * 1e-6 * sample_rate;
    span = MESSAGE_US * 1e-6 * sample_rate;
    if ((double)count < span + 4.0 * w)
        return -1;
    memset(res, 0, sizeof(*res));
    if (!state) {
        /* Without a state, the pairs of CPR frames of this capture still give positions. */
        tmp = calloc(1, sizeof(*tmp));
        if (!tmp)
            return -1;
        state = tmp;
    }
    p   = malloc(count * sizeof(*p));
    a   = malloc(count * sizeof(*a));
    cp  = malloc((count + 1) * sizeof(*cp));
    ca  = malloc((count + 1) * sizeof(*ca));
    sub = malloc(FLOOR_SAMPLES * sizeof(*sub));
    if (!p || !a || !cp || !ca || !sub)
        goto out;
    cp[0] = ca[0] = 0;
    for (i = 0; i < count; i++) {
        p[i]      = crealf(x[i]) * crealf(x[i]) + cimagf(x[i]) * cimagf(x[i]);
        a[i]      = sqrtf(p[i]);
        cp[i + 1] = cp[i] + p[i];
        ca[i + 1] = ca[i] + a[i];
    }
    pw.v  = p;
    pw.s  = cp;
    mag.v = a;
    mag.s = ca;
    res->power_dbfs = m2sdr_power_db(cp[count] / (double)count);

    /* Noise floor: the median power, the messages taking a small part of the time. */
    step = count / FLOOR_SAMPLES + 1;
    for (nsub = 0, i = 0; i < count && nsub < FLOOR_SAMPLES; i += step)
        sub[nsub++] = p[i];
    qsort(sub, nsub, sizeof(*sub), cmp_float);
    /* The median of the power of complex Gaussian noise is ln 2 of its mean. */
    noise           = sub[nsub / 2] / M_LN2 + 1e-30;
    res->noise_dbfs = m2sdr_power_db(noise);

    last = count - (size_t)ceil(span + 2.0 * w);
    for (i = 1; i < last; i++) {
        struct m2sdr_adsb_message msg;
        uint8_t data[14];
        double hi, lo, best = -1, t0 = (double)i, on = 0, off = 0, conf_min = 1.0;
        bool corrected = false;
        int k, ph, r, ok = -1;

        /* Cheap test first: the first pulse over the floor. */
        if (p[i] < MIN_PULSE_RATIO * noise && p[i + 1] < MIN_PULSE_RATIO * noise)
            continue;
        preamble_levels(&pw, (double)i, w, &hi, &lo);
        if (hi < MIN_PULSE_RATIO * noise || hi < MIN_CONTRAST * lo)
            continue;
        res->preambles++;
        /* Timing: the pulses best out of the quiet chips, a sample around, an eighth apart. */
        for (ph = -PHASES; ph <= PHASES; ph++) {
            const double tt = (double)i + (double)ph / PHASES;

            if (tt < 0.0)
                continue;
            preamble_levels(&pw, tt, w, &hi, &lo);
            if (hi - lo > best) {
                best = hi - lo;
                t0   = tt;
            }
        }
        for (r = 0; r < (int)(sizeof(retry) / sizeof(retry[0])) && ok != 0; r++) {
            const double tt = t0 + (double)retry[r] / PHASES;

            if (tt < 0.0 || tt + span + w >= (double)count)
                continue;
            ok = read_message(&mag, tt, w, !(cfg && cfg->no_correction), syndrome, &have_syndromes,
                              data, &corrected, &conf_min);
            if (ok == 0)
                t0 = tt;
        }
        if (ok != 0)
            continue;
        m2sdr_adsb_decode(data, &msg);
        msg.corrected = corrected;
        /* Levels, in power: the pulses of the message with those of the preamble, and the noise
         * of the chips between. */
        for (k = 0; k < LONG_BITS; k++) {
            const int bit = (data[k >> 3] >> (7 - (k & 7))) & 1;
            const double t = t0 + (PREAMBLE_CHIPS + 2 * k) * w;

            on  += chip_mean(&pw, bit ? t : t + w, w);
            off += chip_mean(&pw, bit ? t + w : t, w);
        }
        preamble_levels(&pw, t0, w, &hi, &lo);
        on  = (on + 4.0 * hi) / (LONG_BITS + 4);
        off = (off + 12.0 * lo) / (LONG_BITS + 12);
        msg.time_s     = time_s + t0 / sample_rate;
        msg.level_dbfs = m2sdr_power_db(on - off > 0.0 ? on - off : 0.0);
        msg.snr_db     = 10.0 * log10((on - off > 0.0 ? on - off : 1e-30) / (off + 1e-30));
        msg.confidence = conf_min;
        aircraft_update(state, cfg, &msg);
        if (msg.corrected)
            res->corrected++;
        res->messages++;
        if (res->nmessages < M2SDR_ADSB_MAX_MESSAGES)
            res->message[res->nmessages++] = msg;
        /* The next message starts after this one. */
        i = (size_t)(t0 + span);
    }
    res->message_rate = res->messages / ((double)count / sample_rate);
    rc = 0;
out:
    free(p);
    free(a);
    free(cp);
    free(ca);
    free(sub);
    free(tmp);
    return rc;
}
