/*
 * dsp_test.c -- numerical checks for the downconverter's FIR and NCO.
 *
 * The implementation under test is the real one: this includes
 * pluto_downconverter.c directly (renaming its main) so the test cannot
 * drift from the shipping code.
 *
 * The FIR is checked against a direct convolution y[k] = sum_j t[j]*x[k-j]
 * accumulated in double precision -- an unambiguous reference. Note the
 * original shift-register implementation would NOT pass this: it applied
 * taps[0] and taps[1] both to the newest sample, delaying everything else
 * by one sample.
 *
 * Build and run it on the target so the NEON path is the one exercised:
 *
 *   make dsp_test && scp dsp_test root@192.168.2.1:/tmp/ && \
 *       ssh root@192.168.2.1 /tmp/dsp_test
 */

#define main downconverter_main
#include "pluto_downconverter.c"
#undef main

/* ================================== tests ================================= */

#define BLK   4096
#define NBLK  24
#define NTOT  (BLK * NBLK)

static int fails = 0;
static void check(const char *what, double got, double limit)
{
    int ok = (got <= limit) && !isnan(got);
    printf("  %-44s %11.3e  (limit %8.1e)  %s\n",
           what, got, limit, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}
static void check_min(const char *what, double got, double limit)
{
    int ok = (got >= limit) && !isnan(got);
    printf("  %-44s %11.3e  (min   %8.1e)  %s\n",
           what, got, limit, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

static float  xi[NTOT], xq[NTOT];
static float  oi[BLK],  oq[BLK];
static double ri[NTOT / 2], rq[NTOT / 2];
static float  gi[NTOT / 2], gq[NTOT / 2];
static float  taps[MAX_TAPS];

int main(void)
{
    const double fs = 600000.0, cut = 20000.0;
    const double f_pass = 5000.0, f_stop = 120000.0;
    const int ntaps = 129, decim = 3;
    int b, k, j;
    long g;

    design_lowpass(taps, ntaps, cut, fs);

    /* passband tone + stopband interferer + a little noise */
    unsigned seed = 12345;
    for (k = 0; k < NTOT; k++) {
        double t = (double)k / fs;
        seed = seed * 1103515245u + 12345u;
        double nz = ((double)((seed >> 16) & 0x7fff) / 32768.0 - 0.5) * 0.02;
        xi[k] = (float)(0.5 * cos(2 * M_PI * f_pass * t)
                      + 0.3 * cos(2 * M_PI * f_stop * t) + nz);
        xq[k] = (float)(0.5 * sin(2 * M_PI * f_pass * t)
                      + 0.3 * sin(2 * M_PI * f_stop * t) + nz);
    }

    printf("\n== FIR vs direct convolution (double precision) ==\n");

    /* Outputs fire at input indices decim-1, 2*decim-1, ... */
    long nref = 0;
    for (g = decim - 1; g < NTOT; g += decim) {
        double ai = 0.0, aq = 0.0;
        for (j = 0; j < ntaps; j++) {
            long idx = g - j;
            if (idx < 0) break;
            ai += (double)taps[j] * xi[idx];
            aq += (double)taps[j] * xq[idx];
        }
        ri[nref] = ai; rq[nref] = aq; nref++;
    }

    cfir_t f;
    if (cfir_init(&f, taps, ntaps, decim, BLK) < 0) { printf("init failed\n"); return 1; }
    long ngot = 0;
    for (b = 0; b < NBLK; b++) {
        memcpy(cfir_in_i(&f), xi + (long)b * BLK, sizeof(float) * BLK);
        memcpy(cfir_in_q(&f), xq + (long)b * BLK, sizeof(float) * BLK);
        int n = cfir_run(&f, BLK, oi, oq);
        for (k = 0; k < n; k++) { gi[ngot] = oi[k]; gq[ngot] = oq[k]; ngot++; }
    }
    cfir_free(&f);

    printf("  reference outputs %ld, implementation %ld\n", nref, ngot);
    if (nref != ngot) { printf("  COUNT MISMATCH\n"); fails++; }

    /* skip the first ntaps outputs: startup transient differs by how the
     * reference truncates versus the zeroed history */
    long skip = ntaps, cmp = (nref < ngot ? nref : ngot);
    double maxd = 0.0, peak = 0.0;
    for (g = skip; g < cmp; g++) {
        double di = fabs((double)gi[g] - ri[g]), dq = fabs((double)gq[g] - rq[g]);
        if (di > maxd) maxd = di;
        if (dq > maxd) maxd = dq;
        if (fabs(ri[g]) > peak) peak = fabs(ri[g]);
    }
    printf("  compared %ld outputs, peak |out| %.4f\n", cmp - skip, peak);
    check("max abs error vs double convolution", maxd, 5e-6);
    check("relative to peak", maxd / peak, 1e-5);

    printf("\n== FIR response (functional) ==\n");
    /* correlate the output against each tone to recover its amplitude */
    double fso = fs / decim;
    double pr = 0, pi_ = 0, sr = 0, si_ = 0;
    long nn = 0;
    for (g = skip; g < cmp; g++) {
        double t = (double)(g * decim + decim - 1) / fs;
        double cp = cos(2 * M_PI * f_pass * t), sp = sin(2 * M_PI * f_pass * t);
        double cs = cos(2 * M_PI * f_stop * t), ss = sin(2 * M_PI * f_stop * t);
        pr += gi[g] * cp + gq[g] * sp;  pi_ += gq[g] * cp - gi[g] * sp;
        sr += gi[g] * cs + gq[g] * ss;  si_ += gq[g] * cs - gi[g] * ss;
        nn++;
    }
    double amp_pass = sqrt(pr * pr + pi_ * pi_) / nn;
    double amp_stop = sqrt(sr * sr + si_ * si_) / nn;
    printf("  decimated rate %.1f kSps, passband %.0f Hz, stopband %.0f Hz\n",
           fso / 1e3, f_pass, f_stop);
    printf("  recovered amplitudes: passband %.4f (in 0.500), "
           "stopband %.2e (in 0.300)\n", amp_pass, amp_stop);
    check_min("passband gain", amp_pass / 0.5, 0.98);
    check("stopband leakage", amp_stop / 0.3, 1e-3);

    printf("\n== NCO: recursive rotator vs exact cos/sin ==\n");
    nco_t n1;
    const double foff = 100000.0;
    nco_init(&n1, foff, fs);
    double w = 2.0 * M_PI * foff / fs;
    double maxphase = 0.0, maxmagerr = 0.0;
    long N = 2000000;
    for (k = 0; k < N; k++) {
        float a, c;
        nco_mix(&n1, 1.0f, 0.0f, &a, &c);   /* unit input -> the phasor */
        double want = w * k;
        double got = atan2((double)c, (double)a);
        double d = fmod(got - want, 2 * M_PI);
        if (d > M_PI) d -= 2 * M_PI;
        if (d < -M_PI) d += 2 * M_PI;
        if (fabs(d) > maxphase) maxphase = fabs(d);
        double m = sqrt((double)a * a + (double)c * c);
        if (fabs(m - 1.0) > maxmagerr) maxmagerr = fabs(m - 1.0);
    }
    printf("  over %ld samples (%.1f s at %.0f Sps)\n", N, N / fs, fs);
    check("max magnitude error abs(|p|-1)", maxmagerr, 1e-4);
    check("max phase error (rad)", maxphase, 1e-2);

    printf("\n%s\n\n", fails ? "*** FAILURES ***" : "all checks passed");
    return fails ? 1 : 0;
}
