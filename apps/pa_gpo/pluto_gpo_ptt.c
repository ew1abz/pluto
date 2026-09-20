/*
 * pluto_gpo_ptt.c
 *
 * Drive the AD9363's GPO_[0:3] pins from the Rx/Tx state of the radio, so an
 * external power amplifier / T-R relay is keyed automatically instead of
 * needing a separate PTT line.
 *
 * Based on the C example from the Analog Devices wiki:
 *   https://wiki.analog.com/university/tools/pluto/hacking/power_amp
 * with error checking, command line options, and an FDD restore path added.
 *
 * ---------------------------------------------------------------------------
 * HOW IT WORKS
 * ---------------------------------------------------------------------------
 * The AD9363 Enable State Machine (ENSM) has an FDD state (Rx and Tx chains
 * both live) and separate TDD rx / tx states. The four GPO pins can be slaved
 * to that state machine: gpo0-slave-rx-enable makes GPO_0 assert whenever the
 * part is in Rx, gpo1-slave-tx-enable makes GPO_1 assert in Tx.
 *
 * Slaving only works in TDD mode, so the setup writes:
 *
 *   adi,frequency-division-duplex-mode-enable = 0   (0 = TDD, 1 = FDD)
 *   adi,gpo0-slave-rx-enable                  = 1
 *   adi,gpo1-slave-tx-enable                  = 1
 *   initialize                                = 1   (apply the above)
 *
 * After that, writing "rx" or "tx" to the ensm_mode attribute swaps the pins.
 *
 * ---------------------------------------------------------------------------
 * READ THIS BEFORE USING IT WITH THE DOWNCONVERTER
 * ---------------------------------------------------------------------------
 * TDD mode is mutually exclusive with the full-duplex RX->TX path that
 * pluto_downconverter uses. In TDD:
 *
 *   - capturing a buffer while in tx blocks until it times out,
 *   - pushing a buffer while in rx blocks the same way.
 *
 * So this is for half-duplex / push-to-talk style use (beacon, tone, keyed
 * transmit), not for the translator. Run --restore to put the part back into
 * FDD before starting the downconverter again.
 *
 * ---------------------------------------------------------------------------
 * ELECTRICAL
 * ---------------------------------------------------------------------------
 * VDD_GPO on Pluto is 1.3 V. A GPO pin drives roughly 1.04-1.30 V at about
 * 10 mA typical (32 ohm on, 15 ohm off). That is not enough to switch a relay
 * or most PA enable inputs directly -- level shift and buffer it. Also note
 * that because VDD_GPO sits at 1.3 V, AUX_ADC and AUX_DAC are non-functional
 * on this board.
 *
 * The GPO_[0:3] pins come out to test points; see the Pluto schematic.
 *
 * ---------------------------------------------------------------------------
 * TIMING (from the wiki, software control over SPI)
 * ---------------------------------------------------------------------------
 *   host  USB   shell     80 ms per swap
 *   host  USB   C code    0.6 - 1.3 ms
 *   pluto local shell     30 - 35 ms
 *   pluto local C code    0.2 - 0.6 ms
 *
 * Sub-30 us needs ENSM pin control from an FPGA state machine, not this.
 * Per-pin assertion delay is available via the gpo0-rx-delay-us /
 * gpo0-tx-delay-us debug attributes, not covered here.
 *
 * ---------------------------------------------------------------------------
 * BUILD
 * ---------------------------------------------------------------------------
 * Host:
 *
 *   gcc -O2 -o pluto_gpo_ptt pluto_gpo_ptt.c -liio
 *
 * Cross-compiling to run natively on Pluto (same toolchain and staging tree as
 * pluto_downconverter):
 *
 *   arm-linux-gnueabihf-gcc -O2 -mfpu=neon -mfloat-abi=hard \
 *       -I<fw>/buildroot/output/staging/usr/include \
 *       -L<fw>/buildroot/output/staging/usr/lib \
 *       -o pluto_gpo_ptt pluto_gpo_ptt.c -liio
 *
 *   scp pluto_gpo_ptt root@192.168.2.1:/tmp/
 *
 * ---------------------------------------------------------------------------
 * USAGE
 * ---------------------------------------------------------------------------
 *   pluto_gpo_ptt --setup                 configure TDD + GPO slaving, leave in rx
 *   pluto_gpo_ptt --state tx              key transmit (asserts the Tx GPO)
 *   pluto_gpo_ptt --state rx              back to receive
 *   pluto_gpo_ptt --bench                 toggle rx/tx until Ctrl-C, report rate
 *   pluto_gpo_ptt --restore               back to FDD for the downconverter
 *
 * --uri defaults to the local backend, which is what you want when running on
 * Pluto itself. From a host use --uri ip:192.168.2.1 or --uri usb:1.3.5.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <iio.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_sig(int s)
{
    (void)s;
    g_stop = 1;
}

/* ------------------------------------------------------------------------- */
/* IIO helpers                                                               */
/* ------------------------------------------------------------------------- */

/* The GPO slaving bits live in the debug attributes, which only exist when the
 * kernel is built with IIO debugfs support. Report loudly if they are missing
 * rather than silently doing nothing. */
static int dbg_wr_bool(struct iio_device *dev, const char *attr, bool val)
{
    ssize_t ret = iio_device_debug_attr_write_bool(dev, attr, val);
    if (ret < 0) {
        fprintf(stderr, "error: debug attr %s = %d failed (%zd)\n",
                attr, (int)val, ret);
        return -1;
    }
    return 0;
}

static int dev_wr(struct iio_device *dev, const char *attr, const char *val)
{
    ssize_t ret = iio_device_attr_write(dev, attr, val);
    if (ret < 0) {
        fprintf(stderr, "error: write %s = %s failed (%zd)\n", attr, val, ret);
        return -1;
    }
    return 0;
}

static int dev_rd(struct iio_device *dev, const char *attr,
                  char *buf, size_t len)
{
    ssize_t ret = iio_device_attr_read(dev, attr, buf, len);
    if (ret < 0) {
        fprintf(stderr, "error: read %s failed (%zd)\n", attr, ret);
        buf[0] = '\0';
        return -1;
    }
    return 0;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------------- */

/* 0 = TDD, 1 = FDD. Slaving the GPO pins to Rx/Tx only makes sense in TDD,
 * since in FDD both chains are enabled at once. */
static int set_duplex(struct iio_device *phy, bool fdd, int gpo_rx, int gpo_tx)
{
    char attr[64];

    if (dbg_wr_bool(phy, "adi,frequency-division-duplex-mode-enable", fdd))
        return -1;

    if (!fdd) {
        snprintf(attr, sizeof(attr), "adi,gpo%d-slave-rx-enable", gpo_rx);
        if (dbg_wr_bool(phy, attr, 1))
            return -1;
        snprintf(attr, sizeof(attr), "adi,gpo%d-slave-tx-enable", gpo_tx);
        if (dbg_wr_bool(phy, attr, 1))
            return -1;
    }

    /* Nothing above takes effect until the part is re-initialized. */
    return dbg_wr_bool(phy, "initialize", 1);
}

/* Confirm the part actually landed in the mode we asked for. ensm_mode_available
 * lists "rx tx" in TDD and "fdd" in FDD, so it is the honest check. */
static int verify_modes(struct iio_device *phy, bool want_fdd)
{
    char buf[256];

    if (dev_rd(phy, "ensm_mode_available", buf, sizeof(buf)))
        return -1;
    printf("ensm_mode_available: %s\n", buf);

    if (want_fdd != (strstr(buf, "fdd") != NULL)) {
        fprintf(stderr, "error: expected %s mode, device reports otherwise\n",
                want_fdd ? "FDD" : "TDD");
        return -1;
    }
    return 0;
}

static int bench(struct iio_device *phy)
{
    double t0, dt;
    long long iters = 0;

    printf("toggling rx/tx, Ctrl-C to stop\n");
    t0 = now_s();
    while (!g_stop) {
        iters++;
        if (dev_wr(phy, "ensm_mode", "rx"))
            return -1;
        if (dev_wr(phy, "ensm_mode", "tx"))
            return -1;
    }
    dt = now_s() - t0;

    printf("iterations = %lld in %.3f s\n", iters, dt);
    if (iters)
        printf("%.3f ms per rx/tx slot\n", dt * 1000.0 / (double)(iters * 2));

    /* Do not leave the PA keyed on the way out. */
    return dev_wr(phy, "ensm_mode", "rx");
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --uri URI        libiio URI (default: local/default context)\n"
        "  --setup          put part in TDD, slave GPOs, leave in rx\n"
        "  --state MODE     write ensm_mode (rx, tx, alert, wait, sleep)\n"
        "  --bench          toggle rx/tx until Ctrl-C and report timing\n"
        "  --restore        return the part to FDD (needed by the downconverter)\n"
        "  --gpo-rx N       GPO pin slaved to Rx (default 0)\n"
        "  --gpo-tx N       GPO pin slaved to Tx (default 1)\n"
        "  --help\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *uri = NULL;
    const char *state = NULL;
    bool do_setup = false, do_bench = false, do_restore = false;
    int gpo_rx = 0, gpo_tx = 1;
    struct iio_context *ctx;
    struct iio_device *phy;
    int rc = EXIT_FAILURE;

    static struct option opts[] = {
        { "uri",     required_argument, 0, 'u' },
        { "setup",   no_argument,       0, 's' },
        { "state",   required_argument, 0, 'S' },
        { "bench",   no_argument,       0, 'b' },
        { "restore", no_argument,       0, 'r' },
        { "gpo-rx",  required_argument, 0, 'R' },
        { "gpo-tx",  required_argument, 0, 'T' },
        { "help",    no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    for (;;) {
        int c = getopt_long(argc, argv, "u:sS:brR:T:h", opts, NULL);
        if (c == -1)
            break;
        switch (c) {
        case 'u': uri = optarg; break;
        case 's': do_setup = true; break;
        case 'S': state = optarg; break;
        case 'b': do_bench = true; break;
        case 'r': do_restore = true; break;
        case 'R': gpo_rx = atoi(optarg); break;
        case 'T': gpo_tx = atoi(optarg); break;
        case 'h': usage(argv[0]); return EXIT_SUCCESS;
        default:  usage(argv[0]); return EXIT_FAILURE;
        }
    }

    if (!do_setup && !do_bench && !do_restore && !state) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (gpo_rx < 0 || gpo_rx > 3 || gpo_tx < 0 || gpo_tx > 3) {
        fprintf(stderr, "error: GPO pin must be 0..3\n");
        return EXIT_FAILURE;
    }
    if (do_restore && (do_setup || do_bench)) {
        fprintf(stderr, "error: --restore cannot be combined with --setup/--bench\n");
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    ctx = uri ? iio_create_context_from_uri(uri) : iio_create_default_context();
    if (!ctx) {
        fprintf(stderr, "error: no IIO context (%s)\n", uri ? uri : "default");
        return EXIT_FAILURE;
    }

    phy = iio_context_find_device(ctx, "ad9361-phy");
    if (!phy) {
        fprintf(stderr, "error: ad9361-phy not found\n");
        goto out;
    }

    if (do_restore) {
        if (set_duplex(phy, true, gpo_rx, gpo_tx))
            goto out;
        if (verify_modes(phy, true))
            goto out;
        printf("restored to FDD\n");
        rc = EXIT_SUCCESS;
        goto out;
    }

    /* --bench without --setup would toggle a part still in FDD, where the
     * rx/tx states do not exist, so do the setup implicitly. */
    if (do_setup || do_bench) {
        if (set_duplex(phy, false, gpo_rx, gpo_tx))
            goto out;
        if (verify_modes(phy, false))
            goto out;
        printf("TDD mode, GPO_%d slaved to Rx, GPO_%d slaved to Tx\n",
               gpo_rx, gpo_tx);
        if (dev_wr(phy, "ensm_mode", "rx"))
            goto out;
    }

    if (state) {
        if (dev_wr(phy, "ensm_mode", state))
            goto out;
        printf("ensm_mode = %s\n", state);
    }

    if (do_bench && bench(phy))
        goto out;

    rc = EXIT_SUCCESS;
out:
    iio_context_destroy(ctx);
    return rc;
}
