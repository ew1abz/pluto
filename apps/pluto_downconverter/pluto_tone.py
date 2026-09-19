#!/usr/bin/env python3
"""
pluto_tone.py

Dead-simple continuous carrier generator for testing the PlutoSDR TX chain.

Emits an unmodulated tone at (freq + tone_offset). No Morse, no keying,
no big sample buffers -- just a continuous complex sinusoid straight into
the Soapy Pluto sink. This is the minimal "does TX work at all" test.

  python pluto_tone.py --freq 432.3e6 --tx-gain 10

Listen on a 70cm receiver in CW or USB mode at (freq + tone_offset),
which the script prints on startup.

Transmit into a dummy load or through attenuation, not an antenna.
"""

import argparse
import os
import signal
import threading

from gnuradio import gr, analog
from gnuradio import soapy

VERSION = "tone-v3"


class ToneTx(gr.top_block):
    def __init__(self, uri, center_freq, samp_rate, tone_offset,
                 amplitude, tx_gain, bandwidth, bufflen=None):
        gr.top_block.__init__(self, "Pluto Tone TX")

        # Continuous complex sinusoid -- generated on the fly, no buffers.
        self.src = analog.sig_source_c(
            samp_rate,              # sampling frequency
            analog.GR_COS_WAVE,     # waveform
            tone_offset,            # frequency (offset from center)
            amplitude,              # amplitude
            0,                      # DC offset
        )

        # NOTE: SoapyPlutoSDR takes bufflen as a DEVICE argument (part of the
        # device string), not as a stream argument.
        dev = f"driver=plutosdr,uri={uri}"
        if bufflen:
            dev += f",bufflen={bufflen}"

        self.sink = soapy.sink(
            dev, "fc32", 1, "", "", [""], [""],
        )
        self.sink.set_sample_rate(0, samp_rate)
        self.sink.set_frequency(0, center_freq)
        self.sink.set_bandwidth(0, bandwidth)
        self.sink.set_gain(0, tx_gain)

        self.connect(self.src, self.sink)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--freq", type=float, default=432.3e6,
                    help="TX center frequency, Hz (default: 432.3e6)")
    ap.add_argument("--tone-offset", type=float, default=10e3,
                    help="Tone offset from center, Hz. Keeps the signal clear of "
                         "Pluto's LO leakage at exactly center (default: 10 kHz)")
    ap.add_argument("--samp-rate", type=float, default=1e6,
                    help="Sample rate, Sps. Pluto floor is ~521k (default: 1e6)")
    ap.add_argument("--amplitude", type=float, default=0.5,
                    help="IQ amplitude, 0..1. Below 1.0 to avoid clipping (default: 0.5)")
    ap.add_argument("--tx-gain", type=float, default=10.0,
                    help="Soapy TX gain, 0..89 dB. Higher = more output (default: 10)")
    ap.add_argument("--bandwidth", type=float, default=None,
                    help="Analog filter bandwidth, Hz (default: same as sample rate)")
    ap.add_argument("--bufflen", type=int, default=None,
                    help="SoapyPlutoSDR buffer length in samples. Default in the "
                         "driver is 4096, which at 1 MSps is only ~4 ms per buffer "
                         "-- try 32768 or 65536 if you see TX underruns")
    ap.add_argument("--uri", default="ip:192.168.2.1",
                    help="Pluto URI (default: ip:192.168.2.1)")
    args = ap.parse_args()

    if not (0.0 <= args.tx_gain <= 89.0):
        ap.error(f"--tx-gain must be 0..89 (got {args.tx_gain}). "
                 "Higher = more output power.")

    bandwidth = args.bandwidth if args.bandwidth else args.samp_rate

    print(f"--- pluto_tone {VERSION} ---")
    print(f"TX center freq : {args.freq/1e6:.6f} MHz")
    print(f"Tone offset    : {args.tone_offset/1e3:.3f} kHz")
    print(f"--> LISTEN AT  : {(args.freq + args.tone_offset)/1e6:.6f} MHz")
    print(f"Sample rate    : {args.samp_rate/1e6:.3f} MSps")
    print(f"Amplitude      : {args.amplitude}")
    print(f"TX gain        : {args.tx_gain} dB  (0 = MINIMUM power, 89 = maximum)")
    print(f"bufflen        : {args.bufflen or '(driver default, 4096)'}")

    tb = ToneTx(args.uri, args.freq, args.samp_rate, args.tone_offset,
                args.amplitude, args.tx_gain, bandwidth, args.bufflen)

    stop_event = threading.Event()

    def stop(sig, frame):
        if stop_event.is_set():
            print("Forcing exit.")
            os._exit(1)
        print("\nStopping transmit...")
        stop_event.set()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    print("\nTransmitting continuous tone. Ctrl-C to stop.")
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
        print("Flowgraph did not shut down cleanly; forcing exit.")
        os._exit(1)

    print("Transmit stopped.")


if __name__ == "__main__":
    main()
