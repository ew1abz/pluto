/*
 * pluto_downconverter.c
 *
 * Standalone frequency translator for the ADALM-Pluto, running natively on
 * Pluto's own ARM core via libiio. No host PC required.
 *
 * Signal chain:
 *   10.368 GHz (dish) -> LNB (LO 9,750,154,910 Hz) -> IF ~617.845 MHz
 *     -> Pluto RX -> [optional narrow filter] -> Pluto TX -> 432.3 MHz -> FTX-1
 *
 * This is the C equivalent of pluto_downconverter.py. Same frequency plan,
 * same offset-tuning scheme, no GNU Radio.
 *
 * ---------------------------------------------------------------------------
 * OFFSET TUNING
 * ---------------------------------------------------------------------------
 * Pluto has a DC offset spike at the RX center frequency and LO leakage at the
 * TX center frequency. Both LOs are tuned low by the same amount (--if-offset,
 * default 100 kHz):
 *
 *     RX LO = (RF_target - LNB_LO) - offset
 *     TX LO = out_freq - offset
 *
 * The wanted signal lands at out_freq; RX DC spike and TX LO leakage both land
 * at (out_freq - offset), out of the way.
 *
 * In passthrough mode the offset needs no DSP at all -- it falls out of the LO
 * placement. In filter mode we mix down by the offset, filter, and mix back up.
 *
 * ---------------------------------------------------------------------------
 * GAIN CONVENTION -- IMPORTANT
 * ---------------------------------------------------------------------------
 * This talks to libiio directly, so TX gain is the raw 'hardwaregain'
 * attribute: ATTENUATION in dB, where 0 = full power and -89.75 = minimum.
 * This is the OPPOSITE of the SoapySDR convention (0..89, higher = louder)
 * used by the Python version. Do not copy gain values between them.
 *
 *   --tx-atten 0     full power
 *   --tx-atten -30   moderate
 *   --tx-atten -89   essentially off
 *
 * RX gain is straightforward: higher = more sensitive, roughly 0..73 dB.
 *
 * ---------------------------------------------------------------------------
 * BUILD
 * ---------------------------------------------------------------------------
 * The Makefile next to this file cross-builds against the plutosdr-fw
 * staging tree:
 *
 *   make                 # the app
 *   make install         # stripped copy into apps/bin
 *   make dsp_test        # numerical checks; run it ON the target, where
 *                        # the NEON path is the one being exercised
 *
 * By hand, cross-compiling for Pluto:
 *
 *   arm-linux-gnueabihf-gcc -O2 -mfpu=neon -mfloat-abi=hard \
 *       -o pluto_downconverter pluto_downconverter.c -liio -lad9361 -lm
 *
 * You need libiio and libad9361 built for arm-linux-gnueabihf. If you have
 * the Pluto firmware build tree (plutosdr-fw), its staging directory already
 * contains both plus headers:
 *
 *   arm-linux-gnueabihf-gcc -O2 -mfpu=neon -mfloat-abi=hard \
 *       -I<fw>/buildroot/output/staging/usr/include \
 *       -L<fw>/buildroot/output/staging/usr/lib \
 *       -o pluto_downconverter pluto_downconverter.c -liio -lad9361 -lm
 *
 * libad9361.so.0 ships in the stock Pluto rootfs, so nothing extra needs
 * copying to the target.
 *
 * Then copy over and run:
 *
 *   scp pluto_downconverter root@192.168.2.1:/tmp/
 *   ssh root@192.168.2.1 '/tmp/pluto_downconverter'
 *
 * Note /tmp is volatile. Pluto's rootfs is not straightforwardly writable, so
 * for a persistent install you either re-copy each boot or rebuild the
 * firmware image with the binary included.
 *
 * It can also be built and run on a host against a networked Pluto for
 * testing -- pass --uri ip:192.168.2.1. Running on Pluto itself uses the
 * local backend automatically.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <getopt.h>
#include <time.h>
#include <iio.h>
#include <ad9361.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LNB_LO_HZ        9750154910.0
#define DEFAULT_RF_TGT   10368000000.0
#define DEFAULT_OUT_FREQ 432300000.0
#define DEFAULT_OFFSET   100000.0
#define DEFAULT_SAMPRATE 600000.0

#define MAX_TAPS 512

/*
 * AD9361 baseband rate limits. The ADC/DAC clock floor is 25 MHz and the
 * digital chain divides it by 12, so with the programmable FIR bypassed the
 * lowest usable rate is 25e6/12 = 2.0834 MSPS. Loading the FIR at
 * decimate/interpolate-by-4 drops the floor to 25e6/48 = 520.834 kSPS.
 *
 * A plain write to 'sampling_frequency' never touches the FIR, so anything
 * under 2.0834 MSPS comes back -EINVAL and the device silently stays at
 * whatever it was (30.72 MSPS out of reset). We go through
 * ad9361_set_bb_rate() instead, which designs and loads the FIR when the
 * requested rate needs it.
 */
#define AD9361_MIN_RATE     520834.0    /* with FIR dec/int 4 */
#define AD9361_MIN_RATE_FIR 2083334.0   /* FIR bypassed */
#define AD9361_MAX_RATE     61440000.0

/* Accept the rounding the divider chain forces on us (ppm), reject a clamp
 * (a factor of several). */
#define RATE_TOL            0.001

static volatile sig_atomic_t g_stop = 0;

static double now_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void handle_sig(int s)
{
    (void)s;
    g_stop = 1;
}

/* ------------------------------------------------------------------------- */
/* IIO helpers                                                               */
/* ------------------------------------------------------------------------- */

static struct iio_device *get_phy(struct iio_context *ctx)
{
    struct iio_device *d = iio_context_find_device(ctx, "ad9361-phy");
    if (!d)
        fprintf(stderr, "error: ad9361-phy not found\n");
    return d;
}

/* Write a long long attribute on a channel, reporting failure. */
static int chn_wr_ll(struct iio_channel *chn, const char *attr, long long val)
{
    int ret = iio_channel_attr_write_longlong(chn, attr, val);
    if (ret < 0)
        fprintf(stderr, "warn: write %s = %lld failed (%d)\n", attr, val, ret);
    return ret;
}

static int chn_wr_d(struct iio_channel *chn, const char *attr, double val)
{
    int ret = iio_channel_attr_write_double(chn, attr, val);
    if (ret < 0)
        fprintf(stderr, "warn: write %s = %f failed (%d)\n", attr, val, ret);
    return ret;
}

/* Read back a long long so we can verify what the hardware actually accepted
 * rather than what we asked for -- a silently clamped sample rate shifts every
 * frequency in the chain. */
static long long chn_rd_ll(struct iio_channel *chn, const char *attr)
{
    long long v = -1;
    if (iio_channel_attr_read_longlong(chn, attr, &v) < 0)
        return -1;
    return v;
}

/* ------------------------------------------------------------------------- */
/* Low-pass FIR design (windowed sinc, Hamming)                              */
/* ------------------------------------------------------------------------- */

static int design_lowpass(float *taps, int ntaps, double cutoff_hz, double fs)
{
    double fc = cutoff_hz / fs;      /* normalized cutoff, cycles/sample */
    int m = ntaps - 1;
    double sum = 0.0;
    int i;

    for (i = 0; i < ntaps; i++) {
        double n = (double)i - (double)m / 2.0;
        double sinc = (fabs(n) < 1e-9)
                    ? 2.0 * fc
                    : sin(2.0 * M_PI * fc * n) / (M_PI * n);
        /* Hamming window */
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * (double)i / (double)m);
        taps[i] = (float)(sinc * w);
        sum += taps[i];
    }
    /* normalize for unity DC gain */
    for (i = 0; i < ntaps; i++)
        taps[i] /= (float)sum;

    return ntaps;
}

/* ------------------------------------------------------------------------- */
/* Complex decimating FIR                                                     */
/* ------------------------------------------------------------------------- */
/*
 * History is kept as a contiguous window rather than a shift register. Each
 * block is written at bi[hist..hist+n-1] with the previous block's last
 * (ntaps-1) samples already sitting at bi[0..hist-1], so an output at input
 * index k is a straight dot product over bi[k..k+ntaps-1] -- no per-sample
 * shuffling. Only one memmove per block carries the tail forward.
 *
 * The earlier version memmoved the whole history for every input sample:
 * ~1 KB of traffic 600k times a second, which is what kept the A9 from
 * running filter mode in real time.
 *
 * Taps are stored time-reversed (so index 0 pairs with the oldest sample)
 * and zero-padded to a multiple of 4 for the NEON path.
 */

typedef struct {
    float *taps;     /* time-reversed, zero-padded to tlen */
    int    ntaps;
    int    tlen;     /* ntaps rounded up to a multiple of 4 */
    int    decim;
    float *bi;       /* hist tail + current block, contiguous */
    float *bq;
    int    hist;     /* ntaps - 1 */
    int    phase;    /* decimation phase carried across blocks */
} cfir_t;

static int cfir_init(cfir_t *f, const float *taps, int ntaps, int decim,
                     int maxblock)
{
    int m;

    memset(f, 0, sizeof(*f));
    f->ntaps = ntaps;
    f->tlen  = (ntaps + 3) & ~3;
    f->decim = decim;
    f->hist  = ntaps - 1;
    f->phase = 0;

    /* The padded taps are zero, but they still index up to tlen-1 past the
     * window start, so the buffer needs that much slack beyond the block.
     * calloc keeps the slack at zero -- garbage there could be a NaN, and
     * 0 * NaN is NaN, which would poison the accumulator. */
    int cap = f->hist + maxblock + f->tlen;

    f->taps = calloc(f->tlen, sizeof(float));
    f->bi   = calloc(cap, sizeof(float));
    f->bq   = calloc(cap, sizeof(float));
    if (!f->taps || !f->bi || !f->bq)
        return -1;

    /* Reverse into the FRONT of the array, leaving the pad at the end: the
     * newest sample of an output's window sits at b[k + ntaps - 1], so tap
     * index ntaps-1 must be the one that pairs with it. Padding at the front
     * instead would shift the whole window by tlen - ntaps. The trailing
     * zero taps then multiply samples newer than x[k], which contribute
     * nothing. */
    for (m = 0; m < ntaps; m++)
        f->taps[ntaps - 1 - m] = taps[m];

    return 0;
}

static void cfir_free(cfir_t *f)
{
    free(f->taps);
    free(f->bi);
    free(f->bq);
}

/* Where the caller writes the n new samples for the next cfir_run(). */
static inline float *cfir_in_i(cfir_t *f) { return f->bi + f->hist; }
static inline float *cfir_in_q(cfir_t *f) { return f->bq + f->hist; }

/* Input index, within the next block, at which the first output lands.
 * Call before cfir_run(); the interpolator needs it to line the held
 * samples up with the input sample grid. */
static inline int cfir_first_out(const cfir_t *f)
{
    return f->decim - 1 - f->phase;
}

/*
 * One complex output: dot(taps, window) for I and Q against shared taps.
 *
 * NEON reassociates the sum into four partial accumulators, so results
 * differ from the scalar path in the last bit or two. That is fine for this
 * -- compare with a tolerance, not for equality.
 */
static inline void cdot(const float *t, int tlen,
                        const float *bi, const float *bq,
                        float *oi, float *oq)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    float32x4_t ai = vdupq_n_f32(0.0f), aq = vdupq_n_f32(0.0f);
    float32x2_t si, sq;
    int m;

    for (m = 0; m < tlen; m += 4) {
        float32x4_t tv = vld1q_f32(t + m);
        ai = vmlaq_f32(ai, tv, vld1q_f32(bi + m));
        aq = vmlaq_f32(aq, tv, vld1q_f32(bq + m));
    }
    si = vadd_f32(vget_low_f32(ai), vget_high_f32(ai));
    sq = vadd_f32(vget_low_f32(aq), vget_high_f32(aq));
    *oi = vget_lane_f32(vpadd_f32(si, si), 0);
    *oq = vget_lane_f32(vpadd_f32(sq, sq), 0);
#else
    float acc_i = 0.0f, acc_q = 0.0f;
    int m;

    for (m = 0; m < tlen; m++) {
        acc_i += t[m] * bi[m];
        acc_q += t[m] * bq[m];
    }
    *oi = acc_i;
    *oq = acc_q;
#endif
}

/*
 * Filter and decimate n samples already written at cfir_in_i/q().
 * Returns the number of output samples produced.
 *
 * Outputs land on the input indices where the phase counter wraps, which is
 * every decim'th sample starting at (decim - 1 - phase) -- the same sample
 * grid the per-sample version produced, just computed directly instead of by
 * stepping a counter through every input.
 */
static int cfir_run(cfir_t *f, int n, float *out_i, float *out_q)
{
    int nout = 0;
    int k;

    for (k = f->decim - 1 - f->phase; k < n; k += f->decim) {
        cdot(f->taps, f->tlen, f->bi + k, f->bq + k,
             &out_i[nout], &out_q[nout]);
        nout++;
    }
    f->phase = (f->phase + n) % f->decim;

    /* carry the last (ntaps-1) samples forward as the next block's history */
    memmove(f->bi, f->bi + n, sizeof(float) * f->hist);
    memmove(f->bq, f->bq + n, sizeof(float) * f->hist);

    return nout;
}

/* ------------------------------------------------------------------------- */
/* NCO / rotator                                                              */
/* ------------------------------------------------------------------------- */
/*
 * Recursive complex rotator: the phasor is advanced by one complex multiply
 * per sample instead of calling cos() and sin(). The old version made two
 * double-precision libm calls per sample on each of the down- and up-mixes,
 * four per input sample at 600 kSps, which the A9 has no hardware for.
 *
 * Repeated multiplication lets |p| drift, so it is pulled back to the unit
 * circle periodically.
 */

#define NCO_RENORM 1024

typedef struct {
    float    pi, pq;   /* running phasor */
    float    si, sq;   /* per-sample step, exp(j*w) */
    unsigned n;        /* samples since the last renormalize */
} nco_t;

static void nco_init(nco_t *n, double freq_hz, double fs)
{
    double w = 2.0 * M_PI * freq_hz / fs;

    n->pi = 1.0f;
    n->pq = 0.0f;
    n->si = (float)cos(w);
    n->sq = (float)sin(w);
    n->n  = 0;
}

/* multiply (i,q) by the current phasor, then advance it */
static inline void nco_mix(nco_t *n, float ii, float qq, float *oi, float *oq)
{
    float pi = n->pi, pq = n->pq;
    float ni, nq;

    *oi = ii * pi - qq * pq;
    *oq = ii * pq + qq * pi;

    ni = pi * n->si - pq * n->sq;
    nq = pi * n->sq + pq * n->si;
    n->pi = ni;
    n->pq = nq;

    if (++n->n >= NCO_RENORM) {
        /* |p| is within a few ppm of 1, so one Newton step on 1/sqrt(m)
         * is plenty and costs no divide or square root. */
        float m = ni * ni + nq * nq;
        float g = 1.5f - 0.5f * m;
        n->pi = ni * g;
        n->pq = nq * g;
        n->n  = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Options                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *uri;
    double rf_target;
    double lnb_lo;
    double out_freq;
    double if_offset;
    double samp_rate;
    double rx_bw;
    double tx_bw;
    double rx_gain;
    double tx_atten;
    bool   rx_agc;
    bool   use_filter;
    double filter_bw;
    int    decim;
    size_t bufsize;
} opts_t;

static void usage(const char *prog)
{
    printf(
"Usage: %s [options]\n"
"\n"
"  --uri URI           libiio URI. Omit to use the local backend when\n"
"                      running on Pluto itself; use ip:192.168.2.1 from a host\n"
"  --rf-target HZ      target frequency at the dish (default 10.368e9)\n"
"  --lnb-lo HZ         LNB local oscillator (default 9750154910)\n"
"  --out-freq HZ       where the signal should land (default 432.3e6)\n"
"  --if-offset HZ      offset tuning amount (default 100e3)\n"
"  --samp-rate HZ      RX/TX sample rate (default 600e3)\n"
"  --rx-gain DB        RX gain, higher = more sensitive (default 50)\n"
"  --rx-agc            use slow_attack AGC instead of manual RX gain\n"
"  --tx-atten DB       TX ATTENUATION, 0 = full power, -89.75 = min\n"
"                      (default -30). NOTE: opposite sign from the Soapy\n"
"                      convention used by the Python version.\n"
"  --filter            enable narrowband filtering (removes DC spike)\n"
"  --filter-bw HZ      filter cutoff (default 20000)\n"
"  --decim N           decimation factor (default: auto)\n"
"  --bufsize N         IIO buffer size in samples (default 16384)\n"
"  --help\n"
"\n", prog);
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    opts_t o = {
        .uri        = NULL,
        .rf_target  = DEFAULT_RF_TGT,
        .lnb_lo     = LNB_LO_HZ,
        .out_freq   = DEFAULT_OUT_FREQ,
        .if_offset  = DEFAULT_OFFSET,
        .samp_rate  = DEFAULT_SAMPRATE,
        .rx_bw      = 0,
        .tx_bw      = 0,
        .rx_gain    = 50.0,
        .tx_atten   = -30.0,
        .rx_agc     = false,
        .use_filter = false,
        .filter_bw  = 20000.0,
        .decim      = 0,
        .bufsize    = 16384,
    };

    static struct option lo[] = {
        {"uri",       required_argument, 0, 'u'},
        {"rf-target", required_argument, 0, 'r'},
        {"lnb-lo",    required_argument, 0, 'l'},
        {"out-freq",  required_argument, 0, 'o'},
        {"if-offset", required_argument, 0, 'O'},
        {"samp-rate", required_argument, 0, 's'},
        {"rx-gain",   required_argument, 0, 'g'},
        {"rx-agc",    no_argument,       0, 'A'},
        {"tx-atten",  required_argument, 0, 'a'},
        {"filter",    no_argument,       0, 'f'},
        {"filter-bw", required_argument, 0, 'b'},
        {"decim",     required_argument, 0, 'd'},
        {"bufsize",   required_argument, 0, 'B'},
        {"help",      no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int c, idx;
    while ((c = getopt_long(argc, argv, "", lo, &idx)) != -1) {
        switch (c) {
        case 'u': o.uri = optarg; break;
        case 'r': o.rf_target = atof(optarg); break;
        case 'l': o.lnb_lo = atof(optarg); break;
        case 'o': o.out_freq = atof(optarg); break;
        case 'O': o.if_offset = atof(optarg); break;
        case 's': o.samp_rate = atof(optarg); break;
        case 'g': o.rx_gain = atof(optarg); break;
        case 'A': o.rx_agc = true; break;
        case 'a': o.tx_atten = atof(optarg); break;
        case 'f': o.use_filter = true; break;
        case 'b': o.filter_bw = atof(optarg); break;
        case 'd': o.decim = atoi(optarg); break;
        case 'B': o.bufsize = (size_t)atol(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (o.tx_atten > 0.0 || o.tx_atten < -89.75) {
        fprintf(stderr,
            "error: --tx-atten must be 0..-89.75 (got %f).\n"
            "       This is ATTENUATION: 0 = full power, -89.75 = minimum.\n"
            "       If you were thinking of the Soapy 0..89 scale, negate it.\n",
            o.tx_atten);
        return 1;
    }

    if (o.samp_rate < AD9361_MIN_RATE || o.samp_rate > AD9361_MAX_RATE) {
        fprintf(stderr,
            "error: --samp-rate %.0f is outside the AD9361's range.\n"
            "       Valid: %.0f .. %.0f Hz (below %.0f Hz the programmable\n"
            "       FIR is loaded at dec/int 4 to reach the rate).\n",
            o.samp_rate, AD9361_MIN_RATE, AD9361_MAX_RATE, AD9361_MIN_RATE_FIR);
        return 1;
    }

    double if_center = o.rf_target - o.lnb_lo;
    double rx_lo = if_center - o.if_offset;
    double tx_lo = o.out_freq - o.if_offset;
    if (o.rx_bw <= 0) o.rx_bw = o.samp_rate;
    if (o.tx_bw <= 0) o.tx_bw = o.samp_rate;

    int decim = o.decim;
    if (decim <= 0)
        decim = (int)(o.samp_rate / (o.filter_bw * 8.0));
    if (decim < 1) decim = 1;

    printf("--- pluto_downconverter (native C) ---\n");
    printf("RF target      : %.6f MHz\n", o.rf_target / 1e6);
    printf("LNB LO         : %.6f MHz\n", o.lnb_lo / 1e6);
    printf("IF center      : %.6f MHz\n", if_center / 1e6);
    printf("Offset tuning  : %.1f kHz\n", o.if_offset / 1e3);
    printf("\n");
    printf("Pluto RX LO    : %.6f MHz\n", rx_lo / 1e6);
    printf("Pluto TX LO    : %.6f MHz\n", tx_lo / 1e6);
    printf("--> LISTEN AT  : %.6f MHz\n", o.out_freq / 1e6);
    printf("    (artifacts at %.6f MHz)\n", (o.out_freq - o.if_offset) / 1e6);
    printf("\n");

    /* ---------------- context ---------------- */
    struct iio_context *ctx = o.uri ? iio_create_context_from_uri(o.uri)
                                    : iio_create_default_context();
    if (!ctx) {
        fprintf(stderr, "error: could not create IIO context\n");
        return 1;
    }

    struct iio_device *phy = get_phy(ctx);
    struct iio_device *rxdev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    struct iio_device *txdev = iio_context_find_device(ctx,
                                        "cf-ad9361-dds-core-lpc");
    if (!phy || !rxdev || !txdev) {
        fprintf(stderr, "error: required IIO devices not found\n");
        iio_context_destroy(ctx);
        return 1;
    }

    /* ---------------- configure RX ---------------- */
    struct iio_channel *rx_lo_chn  = iio_device_find_channel(phy, "altvoltage0", true);
    struct iio_channel *tx_lo_chn  = iio_device_find_channel(phy, "altvoltage1", true);
    struct iio_channel *rx_phy_chn = iio_device_find_channel(phy, "voltage0", false);
    struct iio_channel *tx_phy_chn = iio_device_find_channel(phy, "voltage0", true);

    if (!rx_lo_chn || !tx_lo_chn || !rx_phy_chn || !tx_phy_chn) {
        fprintf(stderr, "error: could not find phy channels\n");
        iio_context_destroy(ctx);
        return 1;
    }

    /* ---------------- sample rate ----------------
     * Set this first: it reconfigures the whole digital chain (BBPLL, the
     * halfband stages and the programmable FIR), and doing so resets the
     * analog filter corners, so rf_bandwidth has to be written after it.
     *
     * ad9361_set_bb_rate() covers RX and TX in one call and loads a
     * decimate/interpolate-by-4 FIR when the rate is below the FIR-bypassed
     * floor. Writing 'sampling_frequency' by hand does not, which is why the
     * default 600 kSPS used to fail with -EINVAL and leave the device at
     * 30.72 MSPS. */
    int rate_ret = ad9361_set_bb_rate(phy, (unsigned long)o.samp_rate);
    if (rate_ret < 0) {
        fprintf(stderr,
            "error: ad9361_set_bb_rate(%.0f) failed (%d)\n",
            o.samp_rate, rate_ret);
        iio_context_destroy(ctx);
        return 1;
    }

    iio_channel_attr_write(rx_phy_chn, "rf_port_select", "A_BALANCED");
    chn_wr_ll(rx_phy_chn, "rf_bandwidth", (long long)o.rx_bw);
    chn_wr_ll(rx_lo_chn, "frequency", (long long)rx_lo);

    if (o.rx_agc) {
        iio_channel_attr_write(rx_phy_chn, "gain_control_mode", "slow_attack");
    } else {
        iio_channel_attr_write(rx_phy_chn, "gain_control_mode", "manual");
        chn_wr_d(rx_phy_chn, "hardwaregain", o.rx_gain);
    }

    /* ---------------- configure TX ---------------- */
    iio_channel_attr_write(tx_phy_chn, "rf_port_select", "A");
    chn_wr_ll(tx_phy_chn, "rf_bandwidth", (long long)o.tx_bw);
    chn_wr_ll(tx_lo_chn, "frequency", (long long)tx_lo);
    chn_wr_d(tx_phy_chn, "hardwaregain", o.tx_atten);

    /* Verify what the hardware actually accepted, on both sides. The LOs are
     * absolute, so a wrong rate does not move the frequency plan in
     * passthrough -- but it does scale the filter-mode NCO and FIR cutoff by
     * the same ratio, and it multiplies the sample throughput the ARM core
     * has to shovel. A clamped rate is unusable either way, so refuse to run.
     *
     * The tolerance is relative, not absolute: the BBPLL and divider chain
     * can only synthesize discrete rates, so asking for 2.5 MSPS genuinely
     * lands on 2499998 Hz. That 0.8 ppm is harmless; a rate that got clamped
     * to a limit is out by a factor of several, which RATE_TOL catches. */
    long long rx_fs = chn_rd_ll(rx_phy_chn, "sampling_frequency");
    long long tx_fs = chn_rd_ll(tx_phy_chn, "sampling_frequency");
    printf("Sample rate    : requested %.0f, device reports RX %lld / TX %lld\n",
           o.samp_rate, rx_fs, tx_fs);
    if (rx_fs <= 0 || tx_fs <= 0 ||
        fabs((double)rx_fs - o.samp_rate) / o.samp_rate > RATE_TOL ||
        fabs((double)tx_fs - o.samp_rate) / o.samp_rate > RATE_TOL) {
        fprintf(stderr,
            "error: device did not accept the requested sample rate\n"
            "       (asked %.0f, got RX %lld / TX %lld -- off by %.4fx).\n"
            "       Refusing to run: the ARM core cannot keep up with the\n"
            "       actual rate, and filter-mode DSP would be off by the\n"
            "       same ratio.\n",
            o.samp_rate, rx_fs, tx_fs, (double)rx_fs / o.samp_rate);
        iio_context_destroy(ctx);
        return 1;
    }

    /* Within tolerance, but use what the hardware is really clocking so the
     * NCO and FIR cutoff are designed against the true rate. */
    o.samp_rate = (double)rx_fs;
    printf("RX gain        : %s\n", o.rx_agc ? "slow_attack AGC" : "manual");
    if (!o.rx_agc) printf("                 %.1f dB\n", o.rx_gain);
    printf("TX attenuation : %.2f dB (0 = full power)\n", o.tx_atten);
    printf("Filter         : %s\n", o.use_filter ? "ON" : "OFF (passthrough)");
    if (o.use_filter)
        printf("                 %.0f Hz cutoff, decim %d -> %.1f kSps\n",
               o.filter_bw, decim, o.samp_rate / decim / 1e3);
    printf("Buffer size    : %zu samples\n", o.bufsize);
    printf("\n");

    /* ---------------- streaming channels ---------------- */
    struct iio_channel *rx0i = iio_device_find_channel(rxdev, "voltage0", false);
    struct iio_channel *rx0q = iio_device_find_channel(rxdev, "voltage1", false);
    struct iio_channel *tx0i = iio_device_find_channel(txdev, "voltage0", true);
    struct iio_channel *tx0q = iio_device_find_channel(txdev, "voltage1", true);

    if (!rx0i || !rx0q || !tx0i || !tx0q) {
        fprintf(stderr, "error: could not find streaming channels\n");
        iio_context_destroy(ctx);
        return 1;
    }

    iio_channel_enable(rx0i);
    iio_channel_enable(rx0q);
    iio_channel_enable(tx0i);
    iio_channel_enable(tx0q);

    struct iio_buffer *rxbuf = iio_device_create_buffer(rxdev, o.bufsize, false);
    if (!rxbuf) {
        fprintf(stderr, "error: could not create RX buffer\n");
        iio_context_destroy(ctx);
        return 1;
    }
    struct iio_buffer *txbuf = iio_device_create_buffer(txdev, o.bufsize, false);
    if (!txbuf) {
        fprintf(stderr, "error: could not create TX buffer\n");
        iio_buffer_destroy(rxbuf);
        iio_context_destroy(ctx);
        return 1;
    }

    /* ---------------- DSP setup ---------------- */
    float taps[MAX_TAPS];
    int ntaps = 0;
    /* zero-init: these are only touched under use_filter, but the compiler
     * cannot see that and warns */
    cfir_t fir = {0};
    nco_t nco_down = {0}, nco_up = {0};
    float *fi = NULL, *fq = NULL;

    if (o.use_filter) {
        ntaps = 129;
        if (ntaps > MAX_TAPS) ntaps = MAX_TAPS;
        design_lowpass(taps, ntaps, o.filter_bw, o.samp_rate);
        if (cfir_init(&fir, taps, ntaps, decim, (int)o.bufsize) < 0) {
            fprintf(stderr, "error: FIR init failed\n");
            return 1;
        }
        /* mix the wanted signal from +offset down to DC before filtering */
        nco_init(&nco_down, -o.if_offset, o.samp_rate);
        /* and back up to +offset after, so it clears TX LO leakage */
        nco_init(&nco_up, o.if_offset, o.samp_rate);

        fi = malloc(sizeof(float) * o.bufsize);
        fq = malloc(sizeof(float) * o.bufsize);
        if (!fi || !fq) {
            fprintf(stderr, "error: allocation failed\n");
            return 1;
        }
    }

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    printf("Running. Ctrl-C to stop.\n");

    ptrdiff_t rx_step = iio_buffer_step(rxbuf);
    ptrdiff_t tx_step = iio_buffer_step(txbuf);

    /* the interpolator's held sample carries across block boundaries */
    float hold_i = 0.0f, hold_q = 0.0f;

    unsigned long long nblocks = 0;
    unsigned long long nsamp = 0, nsamp_mark = 0;
    double t_mark = now_mono();

    while (!g_stop) {
        ssize_t nbytes = iio_buffer_refill(rxbuf);
        if (nbytes < 0) {
            fprintf(stderr, "error: buffer refill failed (%zd)\n", nbytes);
            break;
        }

        char *p_rx = (char *)iio_buffer_first(rxbuf, rx0i);
        char *p_tx = (char *)iio_buffer_first(txbuf, tx0i);
        char *p_rx_end = (char *)iio_buffer_end(rxbuf);

        if (!o.use_filter) {
            /*
             * Passthrough. The frequency shift is entirely in the LO
             * placement -- the samples just get copied across unchanged.
             */
            char *s = p_rx, *d = p_tx;
            while (s < p_rx_end) {
                ((int16_t *)d)[0] = ((int16_t *)s)[0];  /* I */
                ((int16_t *)d)[1] = ((int16_t *)s)[1];  /* Q */
                s += rx_step;
                d += tx_step;
            }
        } else {
            /* unpack int16 -> float, mixing down by the offset, straight
             * into the filter's window so nothing is copied twice */
            float *win_i = cfir_in_i(&fir);
            float *win_q = cfir_in_q(&fir);
            int n = 0;
            char *s = p_rx;
            while (s < p_rx_end && n < (int)o.bufsize) {
                float ii = (float)((int16_t *)s)[0] / 2048.0f;
                float qq = (float)((int16_t *)s)[1] / 2048.0f;
                nco_mix(&nco_down, ii, qq, &win_i[n], &win_q[n]);
                s += rx_step;
                n++;
            }

            /* filter + decimate */
            int first_out = cfir_first_out(&fir);
            int nout = cfir_run(&fir, n, fi, fq);

            /*
             * Hold back up to the input rate, then mix up to +offset.
             *
             * One output per INPUT sample, walking the input grid -- not
             * decim copies per filtered sample. Those are not the same
             * count: nout * decim only equals n when decim divides the
             * block. At the default 16384-sample buffer and decim 3 it
             * cycled 16383 / 16383 / 16386, so two blocks in three left the
             * last TX sample stale and every third wrote two samples past
             * the end of the buffer. That repeated every 3 blocks, an
             * audible 12 Hz chop, and it jittered the up-mixer's phase
             * because the NCO advanced a different number of times per
             * block. Walking the input grid fills the buffer exactly and
             * keeps the NCO locked to the sample clock.
             *
             * The hold itself is still zero-order, which puts images at
             * multiples of the decimated rate. They land far out and the
             * FTX-1's front end rejects them; a polyphase interpolator
             * would be cleaner if they ever prove audible.
             */
            char *d = p_tx;
            int oidx = 0;
            int next = first_out;
            int k;
            for (k = 0; k < n; k++) {
                if (oidx < nout && k == next) {
                    hold_i = fi[oidx];
                    hold_q = fq[oidx];
                    oidx++;
                    next += decim;
                }
                float oi, oq;
                /* mix even before the first output arrives, so the NCO
                 * advances exactly once per input sample */
                nco_mix(&nco_up, hold_i, hold_q, &oi, &oq);
                int si = (int)(oi * 2048.0f);
                int sq = (int)(oq * 2048.0f);
                if (si >  2047) si =  2047;
                if (si < -2048) si = -2048;
                if (sq >  2047) sq =  2047;
                if (sq < -2048) sq = -2048;
                ((int16_t *)d)[0] = (int16_t)si;
                ((int16_t *)d)[1] = (int16_t)sq;
                d += tx_step;
            }
        }

        ssize_t pushed = iio_buffer_push(txbuf);
        if (pushed < 0) {
            fprintf(stderr, "error: buffer push failed (%zd)\n", pushed);
            break;
        }

        /* Report achieved throughput, not just a block count. The RX refill
         * blocks, so we can never run fast -- anything short of ~100% means
         * samples are being dropped on the floor. */
        nblocks++;
        nsamp += (unsigned long long)(nbytes / rx_step);

        double t_now = now_mono();
        if (t_now - t_mark >= 2.0) {
            double ksps = (double)(nsamp - nsamp_mark) / (t_now - t_mark) / 1e3;
            printf("\r%llu blocks   %.1f kSps   %.0f%% of real time    ",
                   nblocks, ksps, 100.0 * ksps * 1e3 / o.samp_rate);
            fflush(stdout);
            t_mark = t_now;
            nsamp_mark = nsamp;
        }
    }

    printf("\nStopping...\n");

    if (o.use_filter) {
        cfir_free(&fir);
        free(fi); free(fq);
    }
    iio_buffer_destroy(txbuf);
    iio_buffer_destroy(rxbuf);
    iio_context_destroy(ctx);

    printf("Stopped.\n");
    return 0;
}
