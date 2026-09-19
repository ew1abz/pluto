#!/usr/bin/env python3
"""
pluto_10ghz_xlator.py

Fixed-shift frequency translator using a single PlutoSDR.
RX: LNB IF output from a 10 GHz downconverter
TX: narrow SSB/CW-width signal re-centered into 70cm, for a Yaesu FTX-1

Chain:
  10.386 GHz (sky) -> LNB (LO = 9,750,154,910 Hz) -> IF ~635.85 MHz
    -> Pluto RX -> narrow filter/decimate -> resample back up -> Pluto TX
    -> 432.1 MHz -> FTX-1

RX and TX run at Pluto's native sample rate (600 kSps by default) since
direct source->sink passthrough requires matching rates; the actual
noise bandwidth is set by the xlating filter in the middle, not by the
Pluto sample rate itself.

Requires: GNU Radio 3.8/3.9/3.10 with gr-iio (PlutoSDR support).
Note: iio.pluto_source / iio.pluto_sink positional argument order has
shifted slightly between gr-iio releases. If construction throws a
TypeError, run `python3 -c "from gnuradio import iio; help(iio.pluto_source)"`
to check your installed signature, or build the equivalent flowgraph in
GRC (Pluto Source/Sink blocks expose the same parameters by name and
are more forgiving of version drift).
"""

import argparse
import signal
import time

from gnuradio import gr, filter as gr_filter, blocks
from gnuradio.filter import firdes
from gnuradio import iio


LNB_LO_HZ = 9_750_154_910  # your measured/calibrated LNB LO


def compute_rx_freq(rf_target_hz: float, lnb_lo_hz: float = LNB_LO_HZ) -> float:
    """RF target on the dish side -> Pluto RX frequency (LNB IF output)."""
    return rf_target_hz - lnb_lo_hz


class PlutoXlator(gr.top_block):
    def __init__(self, uri, rf_target_hz, tx_freq_hz, samp_rate,
                 rx_gain, tx_atten, mode):
        gr.top_block.__init__(self, "Pluto 10GHz -> 70cm Xlator")

        rx_freq = compute_rx_freq(rf_target_hz)

        # --- narrow filter width by mode ---
        # cutoff/transition in Hz, applied at the decimated rate
        if mode == "cw":
            cutoff, transition, decim = 500, 200, 20      # ~1 kHz noise BW
        else:  # ssb
            cutoff, transition, decim = 3000, 1000, 10    # ~3 kHz noise BW

        dec_rate = samp_rate / decim

        print(f"RF target      : {rf_target_hz/1e9:.9f} GHz")
        print(f"LNB LO         : {LNB_LO_HZ/1e9:.9f} GHz")
        print(f"Pluto RX freq  : {rx_freq/1e6:.6f} MHz")
        print(f"Pluto TX freq  : {tx_freq_hz/1e6:.6f} MHz")
        print(f"Pluto samp rate: {samp_rate/1e3:.1f} kSps (RX and TX)")
        print(f"Filter mode    : {mode}  (cutoff={cutoff} Hz, "
              f"transition={transition} Hz, decim={decim} -> {dec_rate/1e3:.2f} kSps)")

        # --- Pluto RX ---
        self.pluto_source = iio.pluto_source(
            uri,                # uri
            int(rx_freq),       # frequency (Hz)
            int(samp_rate),     # samplerate
            int(samp_rate * 2), # bandwidth (analog filter, Hz)
            0x8000,             # buffer size
            True,               # quadrature
            True,               # rf dc rejection
            True,               # bb dc rejection
            "manual",           # gain mode
            rx_gain,            # manual gain (dB)
            "",                 # filter (fir file, unused)
            True,               # filter auto
        )

        # --- narrow-band xlating filter, centered on 0 Hz offset ---
        # (Pluto RX is already tuned to the exact target, so no extra
        # frequency offset is needed here -- this just narrows the BW)
        taps = firdes.low_pass(1.0, samp_rate, cutoff, transition)
        self.xlating_filter = gr_filter.freq_xlating_fir_filter_ccc(
            decim, taps, 0, samp_rate
        )

        # --- resample back up to Pluto's TX rate ---
        self.resampler = gr_filter.rational_resampler_ccc(
            interpolation=decim, decimation=1, taps=None, fractional_bw=0
        )

        # --- Pluto TX ---
        self.pluto_sink = iio.pluto_sink(
            uri,                 # uri
            int(tx_freq_hz),     # frequency (Hz)
            int(samp_rate),      # samplerate
            int(samp_rate * 2),  # bandwidth
            0x8000,              # buffer size
            False,               # cyclic
            tx_atten,            # attenuation (dB, negative = less power)
            "",                  # filter
            True,                # filter auto
        )

        self.connect(self.pluto_source, self.xlating_filter,
                      self.resampler, self.pluto_sink)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--uri", default="ip:192.168.2.1",
                     help="Pluto IIO URI (default: ip:192.168.2.1)")
    ap.add_argument("--rf-target", type=float, default=10.386e9,
                     help="RF frequency on the dish/sky side, Hz (default: 10.386 GHz)")
    ap.add_argument("--tx-freq", type=float, default=432.1e6,
                     help="Pluto TX output frequency into the FTX-1, Hz (default: 432.1 MHz)")
    ap.add_argument("--samp-rate", type=float, default=600e3,
                     help="Pluto RX/TX sample rate, Sps (default: 600k, Pluto's practical floor)")
    ap.add_argument("--rx-gain", type=float, default=40.0,
                     help="Manual RX gain, dB (default: 40)")
    ap.add_argument("--tx-atten", type=float, default=-10.0,
                     help="TX attenuation, dB, negative value (default: -10)")
    ap.add_argument("--mode", choices=["ssb", "cw"], default="ssb",
                     help="Narrow filter width preset (default: ssb)")
    args = ap.parse_args()

    tb = PlutoXlator(args.uri, args.rf_target, args.tx_freq, args.samp_rate,
                      args.rx_gain, args.tx_atten, args.mode)

    def stop(sig, frame):
        print("\nStopping...")
        tb.stop()
        tb.wait()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    print("\nStarting flowgraph. Ctrl-C to stop.")
    tb.start()
    while True:
        time.sleep(1)


if __name__ == "__main__":
    main()
