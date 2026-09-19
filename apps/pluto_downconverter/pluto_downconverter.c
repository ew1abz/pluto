/*
 * pluto_downconverter.c
 *
 * Standalone frequency translator for the ADALM-Pluto, running natively on
 * Pluto's own ARM core via libiio. No host PC required.
 *
 * Signal chain:
 *   10.386 GHz (dish) -> LNB (LO 9,750,154,910 Hz) -> IF ~635.845 MHz
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
 * On a host, cross-compiling for Pluto:
 *
 *   arm-linux-gnueabihf-gcc -O2 -mfpu=neon -mfloat-abi=hard \
 *       -o pluto_downconverter pluto_downconverter.c -liio -lm
 *
 * You need libiio built for arm-linux-gnueabihf. If you have the Pluto
 * firmware build tree (plutosdr-fw), its staging directory already contains
 * a suitable libiio and headers:
 *
 *   arm-linux-gnueabihf-gcc -O2 -mfpu=neon -mfloat-abi=hard \
 *       -I<fw>/buildroot/output/staging/usr/include \
 *       -L<fw>/buildroot/output/staging/usr/lib \
 *       -o pluto_downconverter pluto_downconverter.c -liio -lm
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
#include <iio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LNB_LO_HZ        9750154910.0
#define DEFAULT_RF_TGT   10386000000.0
#define DEFAULT_OUT_FREQ 432300000.0
#define DEFAULT_OFFSET   100000.0
#define DEFAULT_SAMPRATE 600000.0

#define MAX_TAPS 512

static volatile sig_atomic_t g_stop = 0;

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
/* Complex decimating FIR with history                                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    float *taps;
    int    ntaps;
    int    decim;
    float *hist_i;   /* circular-free: simple shift buffer of ntaps-1 */
    float *hist_q;
    int    hist_len;
    int    phase;    /* decimation phase counter */
} cfir_t;

static int cfir_init(cfir_t *f, const float *taps, int ntaps, int decim)
{
    f->taps  = malloc(sizeof(float) * ntaps);
    f->ntaps = ntaps;
    f->decim = decim;
    f->hist_len = ntaps - 1;
    f->hist_i = calloc(f->hist_len ? f->hist_len : 1, sizeof(float));
    f->hist_q = calloc(f->hist_len ? f->hist_len : 1, sizeof(float));
    f->phase  = 0;
    if (!f->taps || !f->hist_i || !f->hist_q)
        return -1;
    memcpy(f->taps, taps, sizeof(float) * ntaps);
    return 0;
}

static void cfir_free(cfir_t *f)
{
    free(f->taps);
    free(f->hist_i);
    free(f->hist_q);
}

/*
 * Filter n input samples, writing decimated output.
 * Returns number of output samples produced.
 *
 * Straightforward direct-form implementation -- readable rather than fast.
 * If this proves too slow on the A9, the obvious optimizations are NEON
 * intrinsics and a polyphase decomposition (which avoids computing outputs
 * that get thrown away by the decimator).
 */
static int cfir_run(cfir_t *f, const float *in_i, const float *in_q, int n,
                    float *out_i, float *out_q)
{
    int nout = 0;
    int hl = f->hist_len;
    int k, j;

    for (k = 0; k < n; k++) {
        /* shift history */
        if (hl > 0) {
            memmove(f->hist_i, f->hist_i + 1, sizeof(float) * (hl - 1));
            memmove(f->hist_q, f->hist_q + 1, sizeof(float) * (hl - 1));
            f->hist_i[hl - 1] = in_i[k];
            f->hist_q[hl - 1] = in_q[k];
        }

        if (++f->phase >= f->decim) {
            f->phase = 0;
            float acc_i = 0.0f, acc_q = 0.0f;
            /* newest sample is in_i[k], older ones in history (newest last) */
            acc_i += f->taps[0] * in_i[k];
            acc_q += f->taps[0] * in_q[k];
            for (j = 1; j < f->ntaps; j++) {
                int hidx = hl - j;
                if (hidx < 0) break;
                acc_i += f->taps[j] * f->hist_i[hidx];
                acc_q += f->taps[j] * f->hist_q[hidx];
            }
            out_i[nout] = acc_i;
            out_q[nout] = acc_q;
            nout++;
        }
    }
    return nout;
}

/* ------------------------------------------------------------------------- */
/* NCO / rotator                                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    double phase;
    double inc;
} nco_t;

static void nco_init(nco_t *n, double freq_hz, double fs)
{
    n->phase = 0.0;
    n->inc = 2.0 * M_PI * freq_hz / fs;
}

/* multiply (i,q) by exp(j*phase), advancing phase */
static inline void nco_mix(nco_t *n, float ii, float qq, float *oi, float *oq)
{
    double c = cos(n->phase);
    double s = sin(n->phase);
    *oi = (float)(ii * c - qq * s);
    *oq = (float)(ii * s + qq * c);
    n->phase += n->inc;
    if (n->phase > 2.0 * M_PI)  n->phase -= 2.0 * M_PI;
    if (n->phase < -2.0 * M_PI) n->phase += 2.0 * M_PI;
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
"  --rf-target HZ      target frequency at the dish (default 10.386e9)\n"
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

    iio_channel_attr_write(rx_phy_chn, "rf_port_select", "A_BALANCED");
    chn_wr_ll(rx_phy_chn, "rf_bandwidth", (long long)o.rx_bw);
    chn_wr_ll(rx_phy_chn, "sampling_frequency", (long long)o.samp_rate);
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
    chn_wr_ll(tx_phy_chn, "sampling_frequency", (long long)o.samp_rate);
    chn_wr_ll(tx_lo_chn, "frequency", (long long)tx_lo);
    chn_wr_d(tx_phy_chn, "hardwaregain", o.tx_atten);

    /* Verify what the hardware actually accepted. A silently clamped rate
     * shifts every frequency in the chain proportionally. */
    long long actual_fs = chn_rd_ll(tx_phy_chn, "sampling_frequency");
    printf("Sample rate    : requested %.0f, device reports %lld\n",
           o.samp_rate, actual_fs);
    if (actual_fs > 0 && fabs((double)actual_fs - o.samp_rate) > 1.0) {
        printf("  *** WARNING: device did not accept the requested rate.\n");
        printf("  *** All frequencies will be off by a factor of %.4f\n",
               (double)actual_fs / o.samp_rate);
    }
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
    cfir_t fir;
    nco_t nco_down, nco_up;
    float *wi = NULL, *wq = NULL, *fi = NULL, *fq = NULL;

    if (o.use_filter) {
        ntaps = 129;
        if (ntaps > MAX_TAPS) ntaps = MAX_TAPS;
        design_lowpass(taps, ntaps, o.filter_bw, o.samp_rate);
        if (cfir_init(&fir, taps, ntaps, decim) < 0) {
            fprintf(stderr, "error: FIR init failed\n");
            return 1;
        }
        /* mix the wanted signal from +offset down to DC before filtering */
        nco_init(&nco_down, -o.if_offset, o.samp_rate);
        /* and back up to +offset after, so it clears TX LO leakage */
        nco_init(&nco_up, o.if_offset, o.samp_rate);

        wi = malloc(sizeof(float) * o.bufsize);
        wq = malloc(sizeof(float) * o.bufsize);
        fi = malloc(sizeof(float) * o.bufsize);
        fq = malloc(sizeof(float) * o.bufsize);
        if (!wi || !wq || !fi || !fq) {
            fprintf(stderr, "error: allocation failed\n");
            return 1;
        }
    }

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    printf("Running. Ctrl-C to stop.\n");

    ptrdiff_t rx_step = iio_buffer_step(rxbuf);
    ptrdiff_t tx_step = iio_buffer_step(txbuf);

    unsigned long long nblocks = 0;

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
            /* unpack int16 -> float, mixing down by the offset */
            int n = 0;
            char *s = p_rx;
            while (s < p_rx_end && n < (int)o.bufsize) {
                float ii = (float)((int16_t *)s)[0] / 2048.0f;
                float qq = (float)((int16_t *)s)[1] / 2048.0f;
                nco_mix(&nco_down, ii, qq, &wi[n], &wq[n]);
                s += rx_step;
                n++;
            }

            /* filter + decimate */
            int nout = cfir_run(&fir, wi, wq, n, fi, fq);

            /*
             * Interpolate back up by simple sample-and-hold repetition, then
             * mix back up to +offset. Zero-order hold is crude -- it puts
             * images at multiples of the decimated rate. The filter_bw is
             * narrow relative to the sample rate so those images land far
             * out, and the FTX-1's front end rejects them, but a proper
             * polyphase interpolator would be cleaner if this proves audible.
             */
            char *d = p_tx;
            int k = 0;
            for (k = 0; k < nout; k++) {
                int rep;
                for (rep = 0; rep < decim; rep++) {
                    float oi, oq;
                    nco_mix(&nco_up, fi[k], fq[k], &oi, &oq);
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
        }

        ssize_t pushed = iio_buffer_push(txbuf);
        if (pushed < 0) {
            fprintf(stderr, "error: buffer push failed (%zd)\n", pushed);
            break;
        }

        if ((++nblocks % 200) == 0) {
            printf("\r%llu blocks", nblocks);
            fflush(stdout);
        }
    }

    printf("\nStopping...\n");

    if (o.use_filter) {
        cfir_free(&fir);
        free(wi); free(wq); free(fi); free(fq);
    }
    iio_buffer_destroy(txbuf);
    iio_buffer_destroy(rxbuf);
    iio_context_destroy(ctx);

    printf("Stopped.\n");
    return 0;
}
