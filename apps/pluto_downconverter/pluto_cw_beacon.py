#!/usr/bin/env python3
"""
pluto_cw_beacon.py

Simple CW (Morse) beacon generator for testing the PlutoSDR transmit chain
using GNU Radio's Soapy blocks.

Generates a keyed carrier at (center_freq + tone_offset), with raised-cosine
envelope shaping to avoid key clicks, and loops the message continuously.

Typical test: transmit into a dummy load (or through attenuation) and listen
on a 70cm receiver in CW mode to confirm the TX chain works end to end.

  python3 pluto_cw_beacon.py --text "VVV DE KM6RNJ" --freq 432.3e6 --wpm 15

Requires: GNU Radio 3.10+ with gr-soapy and SoapyPlutoSDR.

Note: the soapy.sink() constructor signature has drifted between GNU Radio
releases. If you get a TypeError, run:
    python -c "from gnuradio import soapy; help(soapy.sink)"
and adjust, or build the equivalent flowgraph in GRC using the
"Soapy PlutoSDR Sink" block, which exposes these parameters by name.
"""

import argparse
import os
import signal
import threading
import time

import numpy as np
from gnuradio import gr, blocks
from gnuradio import soapy


VERSION = "v3"


# ---------------------------------------------------------------------------
# Morse code table
# ---------------------------------------------------------------------------
MORSE = {
    'A': '.-',     'B': '-...',   'C': '-.-.',   'D': '-..',    'E': '.',
    'F': '..-.',   'G': '--.',    'H': '....',   'I': '..',     'J': '.---',
    'K': '-.-',    'L': '.-..',   'M': '--',     'N': '-.',     'O': '---',
    'P': '.--.',   'Q': '--.-',   'R': '.-.',    'S': '...',    'T': '-',
    'U': '..-',    'V': '...-',   'W': '.--',    'X': '-..-',   'Y': '-.--',
    'Z': '--..',
    '0': '-----',  '1': '.----',  '2': '..---',  '3': '...--',  '4': '....-',
    '5': '.....',  '6': '-....',  '7': '--...',  '8': '---..',  '9': '----.',
    '.': '.-.-.-', ',': '--..--', '?': '..--..', '/': '-..-.',  '=': '-...-',
    '+': '.-.-.',  '-': '-....-', ':': '---...', "'": '.----.', '@': '.--.-.',
}


def morse_key_pattern(text):
    """
    Convert text to a list of (on/off, duration_in_dit_units) tuples
    following standard CW timing:
      dit = 1 unit, dah = 3 units
      intra-character gap = 1 unit
      inter-character gap = 3 units
      word gap = 7 units
    """
    pattern = []
    words = text.upper().strip().split()

    for wi, word in enumerate(words):
        for ci, char in enumerate(word):
            symbols = MORSE.get(char)
            if symbols is None:
                continue  # silently skip unsupported characters
            for si, sym in enumerate(symbols):
                pattern.append((1, 3 if sym == '-' else 1))
                if si < len(symbols) - 1:
                    pattern.append((0, 1))       # intra-character gap
            if ci < len(word) - 1:
                pattern.append((0, 3))           # inter-character gap
        if wi < len(words) - 1:
            pattern.append((0, 7))               # word gap

    return pattern


def build_envelope(text, wpm, samp_rate, rise_ms=5.0, tail_units=7):
    """
    Build a float32 keying envelope with raised-cosine rise/fall edges.

    rise_ms of ~5 ms is the usual compromise: fast enough to sound crisp,
    slow enough to keep key clicks out of the adjacent spectrum.
    """
    unit_sec = 1.2 / wpm                      # standard: dit length = 1.2/WPM
    unit_samples = int(round(unit_sec * samp_rate))
    rise_samples = int(round((rise_ms / 1000.0) * samp_rate))
    rise_samples = min(rise_samples, unit_samples // 2)

    # raised-cosine (half-cycle) rising edge, and its mirror for falling
    n = np.arange(rise_samples)
    rise = 0.5 * (1.0 - np.cos(np.pi * n / max(rise_samples, 1)))
    fall = rise[::-1]

    segments = []
    for state, units in morse_key_pattern(text):
        length = unit_samples * units
        if state == 1:
            seg = np.ones(length, dtype=np.float32)
            if rise_samples > 0 and length >= 2 * rise_samples:
                seg[:rise_samples] = rise
                seg[-rise_samples:] = fall
            segments.append(seg.astype(np.float32))
        else:
            segments.append(np.zeros(length, dtype=np.float32))

    # trailing silence so the looped message has a clean gap before repeating
    segments.append(np.zeros(unit_samples * tail_units, dtype=np.float32))

    return np.concatenate(segments).astype(np.float32)


def build_iq(text, wpm, samp_rate, tone_offset, amplitude, rise_ms):
    """Keyed complex carrier: envelope * exp(j*2*pi*tone_offset*t)."""
    env = build_envelope(text, wpm, samp_rate, rise_ms)
    t = np.arange(len(env)) / samp_rate
    carrier = np.exp(2j * np.pi * tone_offset * t)
    return (amplitude * env * carrier).astype(np.complex64)


class CWBeacon(gr.top_block):
    def __init__(self, iq, uri, center_freq, samp_rate, tx_gain, bandwidth):
        gr.top_block.__init__(self, "Pluto CW Beacon")

        self.src = blocks.vector_source_c(iq.tolist(), repeat=True)

        self.sink = soapy.sink(
            f"driver=plutosdr,uri={uri}",  # device string
            "fc32",                        # sample format
            1,                             # number of channels
            "",                            # device args
            "",                            # stream args
            [""],                          # tune args
            [""],                          # other settings
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
    ap.add_argument("--text", default="VVV DE KM6RNJ",
                    help='Message to send (default: "VVV DE KM6RNJ")')
    ap.add_argument("--freq", type=float, default=432.3e6,
                    help="TX center frequency in Hz (default: 432.3e6)")
    ap.add_argument("--tone-offset", type=float, default=10e3,
                    help="Tone offset from center, Hz. Keeps the signal clear "
                         "of Pluto's LO leakage at exactly center (default: 10 kHz)")
    ap.add_argument("--wpm", type=float, default=15.0,
                    help="Morse speed in words per minute (default: 15)")
    ap.add_argument("--samp-rate", type=float, default=1e6,
                    help="Sample rate, Sps. Pluto's practical floor is ~521k "
                         "(default: 1e6)")
    ap.add_argument("--amplitude", type=float, default=0.5,
                    help="Peak IQ amplitude, 0..1. Keep below 1.0 to avoid "
                         "clipping the DAC (default: 0.5)")
    ap.add_argument("--tx-gain", type=float, default=10.0,
                    help="Soapy TX gain in dB. SoapyPlutoSDR range is 0..89, "
                         "where HIGHER means MORE output power. Start low and "
                         "work up (default: 10)")
    ap.add_argument("--rise-ms", type=float, default=5.0,
                    help="Raised-cosine keying edge time in ms. Lower values "
                         "sound crisper but generate key clicks (default: 5)")
    ap.add_argument("--bandwidth", type=float, default=None,
                    help="Analog filter bandwidth, Hz (default: same as sample rate)")
    ap.add_argument("--uri", default="ip:192.168.2.1",
                    help="Pluto URI (default: ip:192.168.2.1)")
    args = ap.parse_args()

    if not (0.0 <= args.tx_gain <= 89.0):
        ap.error(f"--tx-gain must be between 0 and 89 (got {args.tx_gain}). "
                 "SoapyPlutoSDR uses 0..89 dB where higher = more output power.")

    bandwidth = args.bandwidth if args.bandwidth else args.samp_rate

    iq = build_iq(args.text, args.wpm, args.samp_rate,
                  args.tone_offset, args.amplitude, args.rise_ms)

    duration = len(iq) / args.samp_rate

    print(f"--- pluto_cw_beacon {VERSION} ---")
    print(f"Message        : {args.text}")
    print(f"Speed          : {args.wpm} WPM")
    print(f"Loop length    : {duration:.2f} s ({len(iq):,} samples, "
          f"{iq.nbytes/1e6:.1f} MB)")
    print(f"TX center freq : {args.freq/1e6:.6f} MHz")
    print(f"Tone offset    : {args.tone_offset/1e3:.3f} kHz")
    print(f"--> Emitted at : {(args.freq + args.tone_offset)/1e6:.6f} MHz")
    print(f"Sample rate    : {args.samp_rate/1e6:.3f} MSps")
    print(f"TX gain        : {args.tx_gain} dB")
    print(f"Keying edges   : {args.rise_ms} ms raised cosine")

    tb = CWBeacon(iq, args.uri, args.freq, args.samp_rate,
                  args.tx_gain, bandwidth)

    stop_event = threading.Event()

    def stop(sig, frame):
        if stop_event.is_set():
            # second Ctrl-C: user is impatient, bail out immediately
            print("Forcing exit.")
            os._exit(1)
        print("\nStopping transmit...")
        stop_event.set()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    print("\nTransmitting. Ctrl-C to stop.")
    tb.start()

    try:
        while not stop_event.wait(0.2):
            pass
    except KeyboardInterrupt:
        stop_event.set()

    # tb.wait() can block forever if the Soapy sink thread is parked inside a
    # writeStream() call to the device. Run the graceful shutdown on a daemon
    # thread and hard-exit if it doesn't finish promptly.
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
