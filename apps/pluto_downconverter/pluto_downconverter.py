#!/usr/bin/env python3
"""
pluto_downconverter.py

Fixed-shift frequency translator using a single PlutoSDR, via gr-soapy.

Signal chain:
  10.386 GHz (dish) -> LNB (LO 9,750,154,910 Hz) -> IF ~635.845 MHz
    -> Pluto RX -> [optional narrow filter] -> Pluto TX -> 432.3 MHz -> FTX-1

OFFSET TUNING
-------------
Pluto has a DC offset spike at the RX center frequency and LO leakage at
the TX center frequency. Rather than fight either, both LOs are offset
low by the same amount (--if-offset, default 100 kHz):

    RX LO = (RF_target - LNB_LO) - offset
    TX LO = out_freq - offset

The wanted signal lands exactly at out_freq, while the RX DC spike and TX
LO leakage both land at (out_freq - offset), out of the way.

GAIN CONVENTIONS (gr-soapy, NOT raw libiio)
-------------------------------------------
  RX gain: 0..73 dB   higher = more sensitivity
  TX gain: 0..89 dB   higher = more output power
Note this is INVERTED from the raw IIO 'hardwaregain' attribute, where TX
is expressed as negative attenuation (0 = full power, -89.75 = minimum).

SAMPLE RATE
-----------
The AD9363's raw minimum is ~2.083 MSps, but SoapyPlutoSDR auto-loads
on-chip FIR decimator taps to reach lower rates (check filter_fir_en).
Default here is 600 kSps: full duplex at 2.5 MSps saturates the USB
transport (~20 MB/s total) and produces choppy audio, while 600 kSps is
still ~30x more bandwidth than an SSB signal needs.

Verify what the device actually accepted:
    iio_attr -o -c ad9361-phy voltage0 sampling_frequency
    iio_attr -o -c ad9361-phy voltage0 filter_fir_en

  python pluto_downconverter.py --tx-gain 60
  python pluto_downconverter.py --filter --filter-bw 3000 --tx-gain 60
"""

import argparse
import math
import os
import signal
import threading

from gnuradio import gr, blocks
from gnuradio import filter as gr_filter
from gnuradio.filter import firdes
from gnuradio import soapy

VERSION = "dc-v4"

LNB_LO_HZ = 9_750_154_910.0   # measured/calibrated LNB local oscillator


class Downconverter(gr.top_block):
    def __init__(self, uri, rx_lo, tx_lo, samp_rate, if_offset,
                 rx_gain, tx_gain, rx_agc, rx_bw, tx_bw,
                 use_filter, filter_bw, decim, bufflen=None):
        gr.top_block.__init__(self, "Pluto Downconverter")

        # bufflen is a DEVICE argument in SoapyPlutoSDR, not a stream argument.
        dev = f"driver=plutosdr,uri={uri}"
        if bufflen:
            dev += f",bufflen={bufflen}"

        # ---------------- RX ----------------
        self.source = soapy.source(dev, "fc32", 1, "", "", [""], [""])
        self.source.set_sample_rate(0, samp_rate)
        self.source.set_frequency(0, rx_lo)
        self.source.set_bandwidth(0, rx_bw)
        self.source.set_gain_mode(0, rx_agc)      # True = AGC
        if not rx_agc:
            self.source.set_gain(0, rx_gain)

        # ---------------- TX ----------------
        self.sink = soapy.sink(dev, "fc32", 1, "", "", [""], [""])
        self.sink.set_sample_rate(0, samp_rate)
        self.sink.set_frequency(0, tx_lo)
        self.sink.set_bandwidth(0, tx_bw)
        self.sink.set_gain(0, tx_gain)

        if not use_filter:
            # Straight passthrough. The whole RX passband (including the DC
            # spike at RX center) gets retransmitted, shifted as a block.
            self.connect(self.source, self.sink)
            return

        # ------------- narrow filter path -------------
        # freq_xlating_fir_filter shifts by -if_offset, moving the wanted
        # signal from +if_offset down to DC, and filters out everything else
        # (including the DC spike, which is now at -if_offset).
        taps = firdes.low_pass(
            1.0, samp_rate, filter_bw, filter_bw * 0.35
        )
        self.xlate = gr_filter.freq_xlating_fir_filter_ccc(
            decim, taps, if_offset, samp_rate
        )

        # back up to the Pluto TX rate
        # NOTE: taps must be [] (empty list), not None, in GR 3.10+
        self.resamp = gr_filter.rational_resampler_ccc(
            interpolation=decim, decimation=1, taps=[], fractional_bw=0.0
        )

        # re-apply the offset so the signal sits at +if_offset again, which
        # puts it at tx_lo + if_offset = out_freq, clear of TX LO leakage.
        self.rotator = blocks.rotator_cc(2.0 * math.pi * if_offset / samp_rate)

        self.connect(self.source, self.xlate, self.resamp,
                     self.rotator, self.sink)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)

    ap.add_argument("--rf-target", type=float, default=10.386e9,
                    help="Target frequency at the dish, Hz (default: 10.386e9)")
    ap.add_argument("--lnb-lo", type=float, default=LNB_LO_HZ,
                    help=f"LNB local oscillator, Hz (default: {LNB_LO_HZ:.0f})")
    ap.add_argument("--out-freq", type=float, default=432.3e6,
                    help="Where the signal should land for the FTX-1, Hz "
                         "(default: 432.3e6)")
    ap.add_argument("--if-offset", type=float, default=100e3,
                    help="Offset-tuning amount, Hz. Moves the RX DC spike and "
                         "TX LO leakage away from the wanted signal "
                         "(default: 100 kHz)")

    ap.add_argument("--samp-rate", type=float, default=600e3,
                    help="RX/TX sample rate, Sps (default: 600e3). The AD9363's "
                         "raw floor is ~2.083 MSps, but SoapyPlutoSDR auto-loads "
                         "on-chip FIR decimator taps for lower rates. 600 kSps "
                         "keeps full-duplex data rate well inside what the USB "
                         "transport handles -- 2.5 MSps duplex saturates it and "
                         "produces choppy audio. Verify with: "
                         "iio_attr -o -c ad9361-phy voltage0 sampling_frequency")
    ap.add_argument("--rx-bw", type=float, default=None,
                    help="RX analog bandwidth, Hz (default: sample rate)")
    ap.add_argument("--tx-bw", type=float, default=None,
                    help="TX analog bandwidth, Hz (default: sample rate)")

    ap.add_argument("--rx-gain", type=float, default=50.0,
                    help="RX gain, 0..73 dB, higher = more sensitive (default: 50)")
    ap.add_argument("--rx-agc", action="store_true",
                    help="Use automatic gain control instead of --rx-gain")
    ap.add_argument("--tx-gain", type=float, default=60.0,
                    help="TX gain, 0..89 dB, higher = more output power. "
                         "NOTE: 0 is minimum power, not maximum (default: 60)")

    ap.add_argument("--filter", action="store_true", dest="use_filter",
                    help="Enable the narrow filter path (removes DC spike, "
                         "cuts noise bandwidth). Without this it is a straight "
                         "wideband passthrough.")
    ap.add_argument("--filter-bw", type=float, default=20000.0,
                    help="Filter cutoff, Hz (default: 20000). The filter's job "
                         "here is killing the DC spike and limiting broadband "
                         "noise -- the FTX-1's own crystal filter does the final "
                         "SSB/CW shaping far better. Narrow values (3000, 500) "
                         "work but make the resampler much more expensive.")
    ap.add_argument("--decim", type=int, default=None,
                    help="Filter decimation factor. Default: chosen so the "
                         "decimated rate is roughly 8x the filter bandwidth.")

    ap.add_argument("--bufflen", type=int, default=None,
                    help="SoapyPlutoSDR buffer length in samples (device arg). "
                         "Driver default is 4096, which at 2.5 MSps is only "
                         "~1.6 ms per buffer. Try 32768 or 65536 if audio is "
                         "choppy.")
    ap.add_argument("--uri", default="ip:192.168.2.1",
                    help="Pluto URI (default: ip:192.168.2.1)")
    args = ap.parse_args()

    if not (0.0 <= args.tx_gain <= 89.0):
        ap.error(f"--tx-gain must be 0..89 (got {args.tx_gain}). "
                 "Higher = more output power.")
    if not (0.0 <= args.rx_gain <= 73.0):
        ap.error(f"--rx-gain must be 0..73 (got {args.rx_gain}).")

    if_center = args.rf_target - args.lnb_lo
    rx_lo = if_center - args.if_offset
    tx_lo = args.out_freq - args.if_offset

    rx_bw = args.rx_bw if args.rx_bw else args.samp_rate
    tx_bw = args.tx_bw if args.tx_bw else args.samp_rate

    # pick a decimation that leaves comfortable margin around the filter
    decim = args.decim
    if decim is None:
        decim = max(1, int(args.samp_rate / (args.filter_bw * 8)))

    print(f"--- pluto_downconverter {VERSION} ---")
    print(f"RF target      : {args.rf_target/1e9:.9f} GHz")
    print(f"LNB LO         : {args.lnb_lo/1e9:.9f} GHz")
    print(f"IF center      : {if_center/1e6:.6f} MHz")
    print(f"Offset tuning  : {args.if_offset/1e3:.1f} kHz")
    print()
    print(f"Pluto RX LO    : {rx_lo/1e6:.6f} MHz")
    print(f"Pluto TX LO    : {tx_lo/1e6:.6f} MHz")
    print(f"--> LISTEN AT  : {args.out_freq/1e6:.6f} MHz")
    print(f"    (artifacts park at {(args.out_freq - args.if_offset)/1e6:.6f} MHz)")
    print()
    print(f"Sample rate    : {args.samp_rate/1e6:.4f} MSps")
    print(f"RX gain        : {'AGC' if args.rx_agc else f'{args.rx_gain} dB'}")
    print(f"TX gain        : {args.tx_gain} dB  (0 = min power, 89 = max)")
    if args.use_filter:
        print(f"Filter         : ON, {args.filter_bw:.0f} Hz cutoff, "
              f"decim {decim} -> {args.samp_rate/decim/1e3:.1f} kSps")
    else:
        print(f"Filter         : OFF (wideband passthrough)")
    print(f"bufflen        : {args.bufflen or '(driver default, 4096)'}")
    print()
    print("Verify the device accepted the sample rate:")
    print("  iio_attr -o -c ad9361-phy voltage0 sampling_frequency")
    print()

    tb = Downconverter(args.uri, rx_lo, tx_lo, args.samp_rate, args.if_offset,
                       args.rx_gain, args.tx_gain, args.rx_agc, rx_bw, tx_bw,
                       args.use_filter, args.filter_bw, decim, args.bufflen)

    stop_event = threading.Event()

    def stop(sig, frame):
        if stop_event.is_set():
            print("Forcing exit.")
            os._exit(1)
        print("\nStopping...")
        stop_event.set()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    print("Running. Ctrl-C to stop.")
    tb.start()

    try:
        while not stop_event.wait(0.2):
            pass
    except KeyboardInterrupt:
        stop_event.set()

    def graceful():
        tb.stop()
        tb.wait()

    t = threading.Thread(target=graceful, daemon=True)
    t.start()
    t.join(timeout=3.0)
    if t.is_alive():
        print("Did not shut down cleanly; forcing exit.")
        os._exit(1)

    print("Stopped.")


if __name__ == "__main__":
    main()
