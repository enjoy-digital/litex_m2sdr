/* M2SDR cable-loopback self-test engine.
 *
 * Verifies, for one RF configuration, the properties the DMA/timestamp stack
 * must hold so that "when we get samples, they are correct":
 *
 *  T1  RX DMA headers: every buffer carries the sync word + a hardware
 *      timestamp with uniform cadence (one frame per 8KiB DMA buffer).
 *  T2  Transmission correctness over the loopback cable: per-channel tones
 *      with the expected SNR, channel mapping (no swap), and sample-stream
 *      continuity proven by tone phase advancing exactly one buffer period
 *      per buffer (a single dropped/duplicated sample shifts the phase by
 *      2*pi*f_rel and fails the check).
 *  T3  Host-drop recovery: the reader thread stalls on purpose; the library
 *      must report the overflow (zero-copy mode) and the first buffer after
 *      the gap must carry a timestamp an exact integer number of buffer
 *      periods after the last one before it.
 *  T4  TX headers: the FPGA extractor's last-header/last-timestamp CSRs must
 *      track the sync word and the synthetic timestamps submitted by the
 *      host (misalignment would show payload bytes here).
 *  T5  Burst timing determinism: gated bursts on the TX sample timeline must
 *      arrive with uniform spacing on the RX hardware timeline.
 *  T6  Stream restart alignment: stopping/restarting one direction while the
 *      other keeps running must not slip the header framing (this is the
 *      historical FDX header-FSM bug).
 *
 * Build/run via scripts/loopback_selftest.sh, which links against the
 * in-repo libm2sdr (never the installed copy) and sweeps configurations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#include "m2sdr.h"

#define MAGIC 0x5aa55aa55aa55aa5ull

/* ------------------------------------------------------------------ */
/* Options / globals                                                   */
/* ------------------------------------------------------------------ */

static struct {
    double      rate;
    int         layout2;     /* 1 = 2T2R */
    int         chan;        /* cabled port pair: 1, 2, or 3 = both */
    double      freq;
    int         rx_gain;
    int         tx_att;
    int         sc8;
    int         no_tx;
    int         nbufs;       /* buffers analyzed in the cadence phase */
    int         stall_ms;
    int         zero_copy;
    int         no_restart;
    int         no_rf;       /* skip apply_config: RF configured externally */
    const char *dev_id;
} opt = {
    .rate = 61.44e6, .layout2 = 0, .chan = 2, .freq = 3.6e9,
    .rx_gain = 40, .tx_att = 10, .sc8 = 0, .no_tx = 0,
    .nbufs = 6000, .stall_ms = 50, .zero_copy = 1, .no_restart = 0,
    .no_rf = 0,
    .dev_id = "pcie:/dev/m2sdr0",
};

static struct m2sdr_dev *dev;
static unsigned sample_sz;       /* bytes per channel-sample (I+Q)       */
static unsigned nch;             /* stream channels                      */
static unsigned spb;             /* channel-samples per DMA payload      */
static unsigned frames;          /* frames (per-channel samples) per buf */
static double   dt_nom;          /* nominal buffer period in ns          */
static double   f_rel[2];        /* per-channel tone, cycles per frame   */
static int      chan_cabled[2];

static int n_pass, n_fail, n_skip;

static void check(const char *name, int pass, const char *fmt, ...)
{
    char detail[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    printf("CHECK %-28s %s  %s\n", name, pass ? "PASS" : "FAIL", detail);
    if (pass) n_pass++; else n_fail++;
}

static void skip(const char *name, const char *why)
{
    printf("CHECK %-28s SKIP  %s\n", name, why);
    n_skip++;
}

static void info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("INFO  ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* TX thread: continuous per-channel tones, optional burst gating      */
/* ------------------------------------------------------------------ */

enum { TX_TONE, TX_BURST, TX_PAUSE, TX_QUIT };

#define BURST_PERIOD 64 /* buffers */
#define BURST_ON      8 /* buffers */

static atomic_int    tx_mode    = TX_TONE;
static atomic_int    tx_paused  = 0;     /* ack that the thread is idle   */
static atomic_long   tx_bufs    = 0;
static atomic_long   tx_underflows = 0;
static pthread_t     tx_thread;
static int           tx_running = 0;

#define TX_TS_BASE 0x4000000000000000ull /* synthetic, never 0 */

static void tx_fill(void *buf, long bufidx, int gated_off)
{
    /* Per-channel rotators persist across buffers so the tone phase is
     * continuous on the TX sample timeline. */
    static double cr[2] = {1, 1}, ci[2] = {0, 0};
    double wr[2], wi[2];
    int amp_on[2];

    for (int c = 0; c < 2; c++) {
        wr[c] = cos(2 * M_PI * f_rel[c]);
        wi[c] = sin(2 * M_PI * f_rel[c]);
        amp_on[c] = chan_cabled[c] && !gated_off;
    }
    (void)bufidx;

    int16_t *s16 = buf;
    int8_t  *s8  = buf;
    const int a16 = 1200, a8 = 75;

    for (unsigned n = 0; n < frames; n++) {
        for (unsigned c = 0; c < nch; c++) {
            int cc = (nch == 2) ? (int)c : (opt.chan == 2 ? 1 : 0);
            int i, q;
            if (amp_on[cc]) {
                i = (int)lrint(cr[cc] * (opt.sc8 ? a8 : a16));
                q = (int)lrint(ci[cc] * (opt.sc8 ? a8 : a16));
            } else {
                i = q = 0;
            }
            if (opt.sc8) { *s8++ = (int8_t)i; *s8++ = (int8_t)q; }
            else         { *s16++ = (int16_t)i; *s16++ = (int16_t)q; }
            /* advance the rotator even while gated off: burst edges must
             * land on the continuous timeline, not restart the phase */
            double nr = cr[cc] * wr[cc] - ci[cc] * wi[cc];
            double ni = cr[cc] * wi[cc] + ci[cc] * wr[cc];
            cr[cc] = nr; ci[cc] = ni;
        }
        if ((n & 0xff) == 0xff)
            for (int c = 0; c < 2; c++) {
                double m = sqrt(cr[c] * cr[c] + ci[c] * ci[c]);
                cr[c] /= m; ci[c] /= m;
            }
    }
}

static void *tx_main(void *arg)
{
    (void)arg;
    void *buf = m2sdr_alloc_buffer(opt.sc8 ? M2SDR_FORMAT_SC8_Q7
                                           : M2SDR_FORMAT_SC16_Q11, spb);

    for (;;) {
        int mode = atomic_load(&tx_mode);
        if (mode == TX_QUIT)
            break;
        if (mode == TX_PAUSE) {
            atomic_store(&tx_paused, 1);
            usleep(2000);
            continue;
        }
        atomic_store(&tx_paused, 0);

        long idx = atomic_load(&tx_bufs);
        int gated_off = (mode == TX_BURST) &&
                        ((idx % BURST_PERIOD) >= BURST_ON);
        tx_fill(buf, idx, gated_off);

        struct m2sdr_metadata meta = {
            .timestamp = TX_TS_BASE + (uint64_t)idx,
            .flags     = M2SDR_META_FLAG_HAS_TIME,
        };
        int rc = m2sdr_sync_tx(dev, buf, spb, &meta, 1000);
        if (rc == M2SDR_ERR_UNDERFLOW) {
            atomic_fetch_add(&tx_underflows, 1);
        } else if (rc != M2SDR_ERR_OK && rc != M2SDR_ERR_TIMEOUT) {
            usleep(1000);
            continue;
        }
        atomic_fetch_add(&tx_bufs, 1);
    }
    m2sdr_free_buffer(buf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* RX analysis                                                         */
/* ------------------------------------------------------------------ */

/* One RX DMA buffer, analyzed. */
struct bufstat {
    uint64_t ts;
    int      has_ts;
    int      zero;
    float    amp[2];     /* tone magnitude per channel, normalized      */
    float    phase[2];   /* tone phase per channel (local-index ref)    */
    float    rms[2];
};

static float *tw_cos[2], *tw_sin[2]; /* per-channel local-index twiddles */

static void analysis_init(void)
{
    for (int c = 0; c < 2; c++) {
        tw_cos[c] = malloc(frames * sizeof(float));
        tw_sin[c] = malloc(frames * sizeof(float));
        for (unsigned n = 0; n < frames; n++) {
            tw_cos[c][n] = (float)cos(2 * M_PI * f_rel[c] * n);
            tw_sin[c][n] = (float)sin(2 * M_PI * f_rel[c] * n);
        }
    }
}

static void analyze(const void *payload, struct bufstat *st)
{
    const int16_t *s16 = payload;
    const int8_t  *s8  = payload;
    const float fs = opt.sc8 ? 127.0f : 2047.0f;

    if (opt.no_tx) {
        /* No signal expected: only the all-zero (DMA start gap) check, on a
         * sparse stride so the reader keeps up at 983 MB/s. */
        const uint64_t *w = payload;
        unsigned nw = spb * sample_sz / 8;
        st->zero = 1;
        for (unsigned k = 0; k < nw; k += 16)
            if (w[k]) {
                st->zero = 0;
                break;
            }
        memset(st->amp, 0, sizeof(st->amp));
        memset(st->phase, 0, sizeof(st->phase));
        memset(st->rms, 0, sizeof(st->rms));
        return;
    }

    st->zero = 1;
    for (unsigned c = 0; c < nch; c++) {
        int cc = (nch == 2) ? (int)c : (opt.chan == 2 ? 1 : 0);
        float ar = 0, ai = 0;
        double p = 0;
        for (unsigned n = 0; n < frames; n++) {
            float xi, xq;
            unsigned k = (n * nch + c) * 2;
            if (opt.sc8) { xi = s8[k] / fs;  xq = s8[k + 1] / fs; }
            else         { xi = s16[k] / fs; xq = s16[k + 1] / fs; }
            if (xi != 0 || xq != 0)
                st->zero = 0;
            /* x * e^{-j 2 pi f n} accumulated over the buffer */
            ar += xi * tw_cos[cc][n] + xq * tw_sin[cc][n];
            ai += xq * tw_cos[cc][n] - xi * tw_sin[cc][n];
            p  += xi * xi + xq * xq;
        }
        st->amp[cc]   = sqrtf(ar * ar + ai * ai) / frames;
        st->phase[cc] = atan2f(ai, ar);
        st->rms[cc]   = (float)sqrt(p / frames);
    }
}

static int read_buf(struct bufstat *st, void *payload, int *overflowed,
                    unsigned timeout_ms)
{
    struct m2sdr_metadata meta;
    int rc = m2sdr_sync_rx(dev, payload, spb, &meta, timeout_ms);
    if (rc == M2SDR_ERR_OVERFLOW) {
        if (overflowed)
            (*overflowed)++;
        rc = m2sdr_sync_rx(dev, payload, spb, &meta, timeout_ms);
    }
    if (rc != M2SDR_ERR_OK)
        return rc;
    st->has_ts = !!(meta.flags & M2SDR_META_FLAG_HAS_TIME);
    st->ts     = meta.timestamp;
    return 0;
}

static double wrap_pi(double x)
{
    while (x >  M_PI) x -= 2 * M_PI;
    while (x < -M_PI) x += 2 * M_PI;
    return x;
}

static int cmp_dbl(const void *a, const void *b)
{
    double d = *(const double *)a - *(const double *)b;
    return (d > 0) - (d < 0);
}

static double median(double *v, int n)
{
    qsort(v, n, sizeof(double), cmp_dbl);
    return n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* ------------------------------------------------------------------ */
/* CSR helpers                                                         */
/* ------------------------------------------------------------------ */

static uint64_t csr_read64(uint32_t addr)
{
    uint32_t hi = 0, lo = 0;
    m2sdr_reg_read(dev, addr, &hi);
    m2sdr_reg_read(dev, addr + 4, &lo);
    return ((uint64_t)hi << 32) | lo;
}

/* ------------------------------------------------------------------ */
/* Phases                                                              */
/* ------------------------------------------------------------------ */

static struct bufstat *stats;
static double steady_amp[2];

/* Read and discard until the stream looks live: headers present and, with
 * TX running, the tone visible. Returns leading all-zero buffer count. */
static int warmup(void *payload, int want_tone, int max_ms)
{
    struct bufstat st;
    int zeros = 0, got = 0;
    long deadline = max_ms;

    while (deadline > 0) {
        int rc = read_buf(&st, payload, NULL, 1000);
        if (getenv("LBT_DEBUG") && (rc != 0 || got < 3))
            fprintf(stderr, "warmup: rc=%d got=%d zeros=%d\n", rc, got, zeros);
        if (rc == M2SDR_ERR_TIMEOUT) {
            deadline -= 1000;
            continue;
        }
        if (rc != 0)
            return -1;
        analyze(payload, &st);
        if (st.zero && !got) {
            zeros++;
            continue;
        }
        got++;
        if (!want_tone && got >= 100)
            return zeros;
        if (want_tone) {
            int ok = 1;
            for (int c = 0; c < 2; c++)
                if (chan_cabled[c] && st.amp[c] < 0.01)
                    ok = 0;
            if (ok && got >= 100)
                return zeros;
        }
        if (got > 60000) /* ~1-2 s of buffers without the tone */
            return zeros;
    }
    return -1;
}

static void phase_cadence(const char *tag, int nbufs, int with_tone_checks)
{
    char name[64];
    int hdr_ok = 0, zero_mid = 0, ovf = 0;
    int n = 0;

    /* Decouple acquisition from analysis. At 2T2R@122.88 full-duplex the RX
     * stream is 983 MB/s; the single-bin DFT in analyze() (per-sample trig over
     * a whole buffer) is far too slow to run inline between reads. Doing so
     * stalls the single reader thread long enough for the free-running FPGA to
     * lap the finite DMA ring -> intermittent, spurious ring overflows that look
     * like a full-duplex board fault but are pure host-side analysis latency.
     * So first drain every buffer out of the ring (wait+copy+release only, which
     * easily keeps up), then analyze the captured copies. */
    size_t bufbytes = (size_t)spb * sample_sz;
    uint8_t *capture = malloc((size_t)nbufs * bufbytes);
    if (!capture) {
        check(tag, 0, "capture alloc (%zu bytes) failed",
              (size_t)nbufs * bufbytes);
        return;
    }
    for (n = 0; n < nbufs; n++) {
        if (read_buf(&stats[n], capture + (size_t)n * bufbytes, &ovf, 1000) != 0)
            break;
    }
    for (int m = 0; m < n; m++) {
        analyze(capture + (size_t)m * bufbytes, &stats[m]);
        if (stats[m].has_ts)
            hdr_ok++;
        if (stats[m].zero && m > 0 && !stats[m - 1].zero)
            zero_mid++;
    }
    free(capture);

    snprintf(name, sizeof(name), "%s.headers", tag);
    check(name, n == nbufs && hdr_ok == n,
          "%d/%d buffers read, %d with valid sync word", n, nbufs, hdr_ok);
    if (opt.zero_copy) {
        snprintf(name, sizeof(name), "%s.no-overflow", tag);
        check(name, ovf == 0, "%d unexpected ring overflows", ovf);
    }
    if (n < 2)
        return;

    /* Cadence: per-buffer timestamp delta uniformity. */
    double *dts = malloc((n - 1) * sizeof(double));
    int mono = 1;
    for (int k = 1; k < n; k++) {
        dts[k - 1] = (double)(int64_t)(stats[k].ts - stats[k - 1].ts);
        if (dts[k - 1] <= 0)
            mono = 0;
    }
    /* Mean over the whole span averages out the 10ns timestamp
     * quantization (per-buffer deltas snap to the 10ns time-clock grid). */
    double dt_mean = (double)(int64_t)(stats[n - 1].ts - stats[0].ts) / (n - 1);
    double worst = 0;
    for (int k = 0; k < n - 1; k++) {
        double d = fabs(dts[k] - dt_mean);
        if (d > worst)
            worst = d;
    }
    double ppm = (dt_mean / dt_nom - 1) * 1e6;

    snprintf(name, sizeof(name), "%s.cadence", tag);
    check(name, mono && worst <= 40.0 && fabs(ppm) < 100.0,
          "dt=%.1fns (nom %.1f, %+.1fppm), max dev %.1fns, monotonic=%d",
          dt_mean, dt_nom, ppm, worst, mono);
    free(dts);

    snprintf(name, sizeof(name), "%s.zeros-midstream", tag);
    check(name, zero_mid == 0, "%d all-zero buffers after stream start",
          zero_mid);

    if (!with_tone_checks)
        return;

    /* Tone level + channel mapping. */
    for (int c = 0; c < 2; c++) {
        if (!chan_cabled[c]) {
            if (nch == 2) {
                /* Un-driven channel must not show the other channel's tone
                 * at ITS OWN tone bin beyond crosstalk. */
                double a = 0;
                for (int k = 0; k < n; k++)
                    a += stats[k].amp[c];
                a /= n;
                snprintf(name, sizeof(name), "%s.ch%d-quiet", tag, c + 1);
                check(name, a < 0.02, "undriven channel amp %.4f", a);
            }
            continue;
        }
        double a = 0, r = 0;
        for (int k = 0; k < n; k++) {
            a += stats[k].amp[c];
            r += stats[k].rms[c];
        }
        a /= n; r /= n;
        steady_amp[c] = a;
        /* rms^2 = mean(I^2+Q^2) carries the full tone power a^2. */
        double rest = fmax(r * r - a * a, 1e-12);
        double snr = 10 * log10(a * a / rest);
        snprintf(name, sizeof(name), "%s.ch%d-tone", tag, c + 1);
        check(name, a > 0.01 && snr > 15.0,
              "amp %.4f (%.1f dBFS rms), tone/rest %.1f dB",
              a, 20 * log10(fmax(r, 1e-9)), snr);

        /* Sample-stream continuity: tone phase must advance by the same
         * amount every buffer. A single dropped/repeated frame shifts it
         * by 2*pi*f_rel (~0.3-0.5 rad), well above the noise. */
        double exp_adv = 0;
        double sr = 0, si = 0;
        for (int k = 1; k < n; k++) {
            double d = wrap_pi(stats[k].phase[c] - stats[k - 1].phase[c]);
            sr += cos(d); si += sin(d);
        }
        exp_adv = atan2(si, sr); /* circular mean advance */
        double worst_ph = 0;
        int bad = 0;
        for (int k = 1; k < n; k++) {
            double d = fabs(wrap_pi(stats[k].phase[c]
                                    - stats[k - 1].phase[c] - exp_adv));
            if (d > worst_ph)
                worst_ph = d;
            if (d > 0.15)
                bad++;
        }
        snprintf(name, sizeof(name), "%s.ch%d-continuity", tag, c + 1);
        check(name, bad == 0,
              "phase step %.4f rad/buf, worst dev %.3f rad, %d/%d over 0.15",
              exp_adv, worst_ph, bad, n - 1);
    }
}

static void phase_stall(void *payload)
{
    /* Deliberately stop reading, then verify the stream account stays exact:
     * the discontinuity must land on the buffer grid (lost data = an integer
     * number of whole DMA buffers, headers still aligned) and its size must
     * match the stall. In zero-copy mode the loss surfaces at the first read
     * after the stall; the buffered helper path may serve retained ring
     * contents first, so the whole post-stall window is scanned. */
    struct bufstat anchor, st;
    int overflows = 0;

    if (read_buf(&anchor, payload, &overflows, 1000) != 0 || !anchor.has_ts) {
        check("stall.anchor", 0, "could not read anchor buffer");
        return;
    }

    usleep((useconds_t)opt.stall_ms * 1000);

    overflows = 0;
    int nscan = 600, ngaps = 0, offgrid = 0, hdr_bad = 0;
    double missing = 0, worst_frac = 0;
    uint64_t prev = anchor.ts;
    int i;
    for (i = 0; i < nscan; i++) {
        if (read_buf(&st, payload, &overflows, 2000) != 0)
            break;
        if (!st.has_ts) {
            hdr_bad++;
            continue;
        }
        double dt = (double)(int64_t)(st.ts - prev);
        prev = st.ts;
        double bufs = dt / dt_nom;
        double frac = fabs(bufs - round(bufs)) * dt_nom; /* ns off the grid */
        if (frac > worst_frac)
            worst_frac = frac;
        if (frac > 60.0)
            offgrid++;
        if (round(bufs) > 1) {
            ngaps++;
            missing += dt - dt_nom;
        }
    }

    check("stall.resume", i == nscan && hdr_bad == 0,
          "%d/%d valid headered buffers after %d ms host stall",
          i - hdr_bad, nscan, opt.stall_ms);

    if (opt.zero_copy)
        check("stall.overflow-reported", overflows > 0,
              "library returned M2SDR_ERR_OVERFLOW %d time(s)", overflows);
    else
        skip("stall.overflow-reported", "no explicit signaling without zero-copy");

    check("stall.gap-on-buffer-grid", ngaps >= 1 && offgrid == 0,
          "%d gap(s), worst %.1fns off the %.1fns buffer grid",
          ngaps, worst_frac, dt_nom);
    check("stall.gap-duration",
          missing > opt.stall_ms * 0.3e6 && missing < opt.stall_ms * 1e6 + 100e6,
          "%.1fms missing vs %dms stall (ring holds %.1fms)",
          missing / 1e6, opt.stall_ms, 256 * dt_nom / 1e6);

    /* Post-recovery re-verification. */
    phase_cadence("recovery", opt.nbufs / 2, !opt.no_tx);
}

static void phase_tx_csrs(void)
{
    if (opt.no_tx) {
        skip("txhdr.magic", "TX disabled in this configuration");
        return;
    }
    uint64_t hdr = csr_read64(CSR_HEADER_LAST_TX_HEADER_ADDR);
    uint64_t ts  = csr_read64(CSR_HEADER_LAST_TX_TIMESTAMP_ADDR);
    long idx_now = atomic_load(&tx_bufs);

    check("txhdr.magic", hdr == MAGIC,
          "extractor last header %016llx", (unsigned long long)hdr);

    long idx = (long)(ts - TX_TS_BASE);
    check("txhdr.timestamp-tracks", ts >= TX_TS_BASE &&
          idx <= idx_now && idx >= idx_now - 512,
          "extracted ts -> buffer %ld, host at %ld (lag %ld)",
          idx, idx_now, idx_now - idx);

    usleep(50 * 1000);
    uint64_t ts2 = csr_read64(CSR_HEADER_LAST_TX_TIMESTAMP_ADDR);
    check("txhdr.timestamp-advances", ts2 > ts,
          "%llu -> %llu over 50ms",
          (unsigned long long)(ts - TX_TS_BASE),
          (unsigned long long)(ts2 - TX_TS_BASE));

    uint64_t rxh = csr_read64(CSR_HEADER_LAST_RX_HEADER_ADDR);
    check("rxhdr.magic-csr", rxh == MAGIC,
          "inserter last header %016llx", (unsigned long long)rxh);
}

static void phase_burst(void)
{
    if (opt.no_tx) {
        skip("burst.spacing", "TX disabled in this configuration");
        return;
    }
    int c = chan_cabled[1] ? 1 : 0; /* measure on one cabled channel */
    double thr_lo = 0.1 * steady_amp[c];
    if (steady_amp[c] < 0.01) {
        skip("burst.spacing", "no steady tone amplitude to gate against");
        return;
    }

    /* settle two periods, then collect 16 bursts (bounded read budget) */
    int budget = BURST_PERIOD * (16 + 4);

    /* Same decoupling as phase_cadence: at 983 MB/s the inline analyze() stalls
     * the reader and the ring overflows, which read_buf recovers by SKIPPING a
     * buffer - and a skip at a burst boundary drops one rising edge, reading as
     * a single burst interval of ~2x the period. Capture the whole budget fast,
     * then detect burst edges offline from the copies. */
    size_t bufbytes = (size_t)spb * sample_sz;
    uint8_t *capture = malloc((size_t)budget * bufbytes);
    struct bufstat *bts = malloc((size_t)budget * sizeof(*bts));
    if (!capture || !bts) {
        check("burst.spacing", 0, "capture alloc failed");
        free(capture); free(bts);
        return;
    }

    atomic_store(&tx_mode, TX_BURST);
    int got = 0;
    for (; got < budget; got++) {
        if (read_buf(&bts[got], capture + (size_t)got * bufbytes, NULL, 1000) != 0)
            break;
    }
    atomic_store(&tx_mode, TX_TONE);

    double arrivals[24];
    int n_arr = 0, low_run = 0, settled = 0;
    const float fs = opt.sc8 ? 127.0f : 2047.0f;
    float p_thr = (float)(0.25 * steady_amp[c] * steady_amp[c] * 2);

    for (int b = 0; b < got && n_arr < 16; b++) {
        void *payload = capture + (size_t)b * bufbytes;
        struct bufstat st;
        analyze(payload, &st);
        st.ts = bts[b].ts;
        st.has_ts = bts[b].has_ts;
        if (settled < 2 * BURST_PERIOD) {
            settled++;
            low_run = (st.amp[c] < thr_lo) ? low_run + 1 : 0;
            continue;
        }
        if (st.amp[c] < thr_lo) {
            low_run++;
            continue;
        }
        if (low_run >= 3 && st.amp[c] > thr_lo) {
            /* burst rising edge inside this buffer: find the first frame
             * above the power threshold */
            const int16_t *s16 = payload;
            const int8_t  *s8  = payload;
            int edge = -1;
            for (unsigned nf = 0; nf < frames; nf++) {
                unsigned k = (nf * nch + (nch == 2 ? (unsigned)c : 0)) * 2;
                float xi, xq;
                if (opt.sc8) { xi = s8[k] / fs;  xq = s8[k + 1] / fs; }
                else         { xi = s16[k] / fs; xq = s16[k + 1] / fs; }
                if (xi * xi + xq * xq > p_thr) {
                    edge = (int)nf;
                    break;
                }
            }
            if (edge >= 0 && st.has_ts)
                arrivals[n_arr++] = (double)st.ts + edge / opt.rate * 1e9;
        }
        low_run = 0;
    }

    free(capture);
    free(bts);

    if (n_arr < 6) {
        check("burst.spacing", 0, "only %d burst edges detected", n_arr);
        return;
    }
    double sp[23];
    for (int k = 1; k < n_arr; k++)
        sp[k - 1] = arrivals[k] - arrivals[k - 1];
    double *tmp = malloc((n_arr - 1) * sizeof(double));
    memcpy(tmp, sp, (n_arr - 1) * sizeof(double));
    double sp_med = median(tmp, n_arr - 1);
    free(tmp);
    double worst = 0;
    for (int k = 0; k < n_arr - 1; k++) {
        double d = fabs(sp[k] - sp_med);
        if (d > worst)
            worst = d;
    }
    double sp_nom = BURST_PERIOD * dt_nom;
    check("burst.spacing", fabs(sp_med - sp_nom) < 1000.0 && worst < 300.0,
          "%d bursts, spacing %.0fns (nom %.0f), max jitter %.0fns",
          n_arr, sp_med, sp_nom, worst);
}

static void phase_restart(void *payload)
{
    if (opt.no_restart) {
        skip("restart.rx", "disabled");
        return;
    }

    /* RX restarts while TX keeps running: the historical FDX header-FSM
     * bug made the inserter free-run mid-frame here, shifting every
     * header by a constant offset for the whole run. */
    int ok = 1;
    for (int it = 0; it < 3 && ok; it++) {
        if (m2sdr_stream_deactivate(dev, M2SDR_RX) != 0 ||
            (usleep(20000), m2sdr_stream_activate(dev, M2SDR_RX)) != 0) {
            ok = 0;
            break;
        }
        if (warmup(payload, !opt.no_tx, 4000) < 0) {
            ok = 0;
            break;
        }
        struct bufstat st;
        for (int k = 0; k < 300; k++) {
            if (read_buf(&st, payload, NULL, 1000) != 0 || !st.has_ts) {
                ok = 0;
                break;
            }
        }
    }
    check("restart.rx", ok,
          "3x RX stop/start under running TX, 300 headered buffers each");

    if (opt.no_tx) {
        skip("restart.tx", "TX disabled in this configuration");
        return;
    }

    /* TX restarts while RX keeps running. */
    ok = 1;
    for (int it = 0; it < 3 && ok; it++) {
        atomic_store(&tx_mode, TX_PAUSE);
        for (int w = 0; w < 500 && !atomic_load(&tx_paused); w++)
            usleep(1000);
        if (m2sdr_stream_deactivate(dev, M2SDR_TX) != 0 ||
            (usleep(20000), m2sdr_stream_activate(dev, M2SDR_TX)) != 0) {
            ok = 0;
            atomic_store(&tx_mode, TX_TONE);
            break;
        }
        atomic_store(&tx_mode, TX_TONE);
        /* tone must come back and headers stay aligned */
        struct bufstat st;
        int back = 0;
        for (int k = 0; k < 60000 && !back; k++) {
            if (read_buf(&st, payload, NULL, 1000) != 0)
                break;
            analyze(payload, &st);
            if (!st.has_ts) {
                ok = 0;
                break;
            }
            int c = chan_cabled[1] ? 1 : 0;
            if (st.amp[c] > 0.5 * steady_amp[c])
                back = 1;
        }
        if (!back)
            ok = 0;
        if (ok) {
            uint64_t hdr = csr_read64(CSR_HEADER_LAST_TX_HEADER_ADDR);
            if (hdr != MAGIC)
                ok = 0;
        }
    }
    check("restart.tx", ok,
          "3x TX stop/start under running RX, tone + header magic recover");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--rate HZ] [--layout 1t1r|2t2r] [--chan 1|2|both]\n"
        "          [--freq HZ] [--rx-gain dB] [--tx-att dB] [--sc8]\n"
        "          [--no-tx] [--nbufs N] [--stall-ms MS] [--no-zerocopy]\n"
        "          [--no-restart] [--dev ID]\n", argv0);
    exit(2);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--rate")     && i + 1 < argc) opt.rate = atof(argv[++i]);
        else if (!strcmp(a, "--layout")   && i + 1 < argc) opt.layout2 = !strcmp(argv[++i], "2t2r");
        else if (!strcmp(a, "--chan")     && i + 1 < argc) {
            const char *v = argv[++i];
            opt.chan = !strcmp(v, "both") ? 3 : atoi(v);
        }
        else if (!strcmp(a, "--freq")     && i + 1 < argc) opt.freq = atof(argv[++i]);
        else if (!strcmp(a, "--rx-gain")  && i + 1 < argc) opt.rx_gain = atoi(argv[++i]);
        else if (!strcmp(a, "--tx-att")   && i + 1 < argc) opt.tx_att = atoi(argv[++i]);
        else if (!strcmp(a, "--sc8"))                      opt.sc8 = 1;
        else if (!strcmp(a, "--no-tx"))                    opt.no_tx = 1;
        else if (!strcmp(a, "--nbufs")    && i + 1 < argc) opt.nbufs = atoi(argv[++i]);
        else if (!strcmp(a, "--stall-ms") && i + 1 < argc) opt.stall_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--no-zerocopy"))              opt.zero_copy = 0;
        else if (!strcmp(a, "--no-restart"))               opt.no_restart = 1;
        else if (!strcmp(a, "--no-rf"))                    opt.no_rf = 1;
        else if (!strcmp(a, "--dev")      && i + 1 < argc) opt.dev_id = argv[++i];
        else usage(argv[0]);
    }

    nch       = opt.layout2 ? 2 : 1;
    sample_sz = opt.sc8 ? 2 : 4;
    spb       = (M2SDR_BUFFER_BYTES - M2SDR_DMA_HEADER_SIZE) / sample_sz;
    frames    = spb / nch;
    dt_nom    = frames / opt.rate * 1e9;
    f_rel[0]  = 0.0537;
    f_rel[1]  = 0.0773;
    chan_cabled[0] = (opt.chan & 1) != 0;
    chan_cabled[1] = (opt.chan & 2) != 0;

    info("config: %s %.2f MSPS %s chan=%s%s%s, %u frames/buf, dt %.1f ns",
         opt.layout2 ? "2T2R" : "1T1R", opt.rate / 1e6,
         opt.sc8 ? "sc8" : "sc16",
         chan_cabled[0] ? "1" : "", chan_cabled[1] ? "2" : "",
         opt.no_tx ? " (RX-only)" : "", frames, dt_nom);

    /* In 1T1R the second physical port pair is selected in the RFIC. */
    if (!opt.layout2 && opt.chan == 2)
        setenv("M2SDR_RF_CHANNEL", "2", 1);

    if (m2sdr_open(&dev, opt.dev_id) != 0) {
        fprintf(stderr, "device open failed\n");
        return 1;
    }

    if (!opt.no_rf) {
    struct m2sdr_config cfg;
    m2sdr_config_init(&cfg);
    cfg.sample_rate      = (int64_t)opt.rate;
    cfg.bandwidth        = (int64_t)fmin(opt.rate * 0.8, 56e6);
    cfg.tx_freq          = (int64_t)opt.freq;
    cfg.rx_freq          = (int64_t)opt.freq;
    cfg.tx_att           = opt.tx_att;
    cfg.rx_gain1         = opt.rx_gain;
    cfg.rx_gain2         = opt.rx_gain;
    cfg.program_rx_gains = true;
    cfg.enable_8bit_mode = opt.sc8;
    cfg.channel_layout   = opt.layout2 ? M2SDR_CHANNEL_LAYOUT_2T2R
                                       : M2SDR_CHANNEL_LAYOUT_1T1R;
    cfg.clock_source     = M2SDR_CLOCK_SOURCE_INTERNAL;
    if (m2sdr_apply_config(dev, &cfg) != 0) {
        fprintf(stderr, "apply_config failed\n");
        return 1;
    }
    }

    enum m2sdr_format fmt = opt.sc8 ? M2SDR_FORMAT_SC8_Q7
                                    : M2SDR_FORMAT_SC16_Q11;
    struct m2sdr_sync_params sp;
    if (!opt.no_tx) {
        m2sdr_sync_params_init(&sp);
        sp.direction        = M2SDR_TX;
        sp.format           = fmt;
        sp.buffer_size      = spb;
        sp.zero_copy        = opt.zero_copy;
        sp.tx_header_enable = true;
        if (m2sdr_sync_config_ex(dev, &sp) != 0) {
            fprintf(stderr, "TX stream config failed\n");
            return 1;
        }
    }
    m2sdr_sync_params_init(&sp);
    sp.direction        = M2SDR_RX;
    sp.format           = fmt;
    sp.buffer_size      = spb;
    sp.zero_copy        = opt.zero_copy;
    sp.rx_header_enable = true;
    sp.rx_strip_header  = true;
    if (m2sdr_sync_config_ex(dev, &sp) != 0) {
        fprintf(stderr, "RX stream config failed\n");
        return 1;
    }

    /* Route TX DMA -> RFIC and RFIC -> RX DMA through the FPGA crossbar. */
    m2sdr_reg_write(dev, CSR_CROSSBAR_MUX_SEL_ADDR, 0);
    m2sdr_reg_write(dev, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);

    analysis_init();
    stats = malloc((size_t)opt.nbufs * sizeof(*stats));
    void *payload = m2sdr_alloc_buffer(fmt, spb);

    if (!opt.no_tx) {
        pthread_create(&tx_thread, NULL, tx_main, NULL);
        tx_running = 1;
        /* Let the TX side prefill the kernel ring before RX starts so the
         * free-running FPGA reader never outruns the host writes. */
        for (int w = 0; w < 300 && atomic_load(&tx_bufs) < 128; w++)
            usleep(10000);
    }

    int zeros = warmup(payload, !opt.no_tx, 6000);
    check("stream.starts", zeros >= 0, "first samples within timeout");
    if (zeros < 0)
        goto out;
    info("leading all-zero buffers at start: %d (%.2f ms)",
         zeros, zeros * dt_nom / 1e6);

    /* Board time vs first header timestamp. */
    {
        struct bufstat st;
        uint64_t t_board = 0;
        if (read_buf(&st, payload, NULL, 1000) == 0 && st.has_ts &&
            m2sdr_get_time(dev, &t_board) == 0) {
            double d = ((double)t_board - (double)st.ts) / 1e6;
            check("timestamp.absolute", fabs(d) < 200.0,
                  "header vs board time: %.2f ms apart", d);
        } else {
            check("timestamp.absolute", 0, "could not read time/header");
        }
    }

    phase_cadence("run", opt.nbufs, !opt.no_tx);
    phase_stall(payload);
    phase_tx_csrs();
    phase_burst();

    if (!opt.no_tx) {
        long uf = atomic_load(&tx_underflows);
        check("tx.no-underflows", uf == 0, "%ld TX ring underflows", uf);
    }

    phase_restart(payload);

out:
    if (tx_running) {
        atomic_store(&tx_mode, TX_QUIT);
        pthread_join(tx_thread, NULL);
    }
    m2sdr_close(dev);

    printf("RESULT %s pass=%d fail=%d skip=%d\n",
           n_fail == 0 ? "PASS" : "FAIL", n_pass, n_fail, n_skip);
    return n_fail == 0 ? 0 : 1;
}
