# Demodulators in C

**Status:** notes, not implemented. Written for learning rather than for a
station that needs them — the FTX-1 already demodulates better than a first
pass in software will.

**The short version:** three of the four are mostly built already. The work
that is actually hard is AGC and getting audio out of the box, neither of
which is a demodulator.

## The structure you already have is a demodulator

`--filter` does this:

```
down-mix by `mix`  ->  real lowpass  ->  up-mix by `mix`  ->  retransmit
```

Every demodulator below is that same chain with the last step changed and
the two mixer frequencies chosen differently. `nco_t`, `cfir_t`,
`taps_for()` and `design_lowpass()` carry over untouched.

Worth internalising: **the up-mix frequency means something different in
each mode.** In the translator it undoes the down-mix. In SSB it is half
the sideband width. In CW it is the pitch you want to hear. Same code.

## SSB

A USB signal with suppressed carrier at `fc` has energy only in
`[fc, fc+B]`. Mix `fc` to zero and the spectrum occupies `[0, B]` — nothing
at negative frequencies. Taking `Re{z}` mirrors the spectrum into
`[-B, 0]`, which was empty, so nothing collides and the real part *is* the
audio. That is the whole trick.

In practice you centre the passband instead of sitting it against zero, so
an ordinary symmetric lowpass can do the work:

| step | with B = 2.4 kHz |
|---|---|
| down-mix | `fc + 1.2 kHz` to DC |
| lowpass | ±1.2 kHz |
| up-mix | +1.2 kHz |
| output | `Re{z}`, audio in 0–2.4 kHz |

The lowpass is what rejects the opposite sideband: the LSB of the same
carrier lands at `[-3.6, -1.2] kHz` after the shift, outside the passband.

**LSB costs one line.** Conjugating the baseband (`q = -q`) mirrors the
spectrum, so LSB is USB on the conjugate. No second code path.

Difficulty: low. The chain exists; you are changing two constants and
replacing the TX write with `Re{}`.

## CW

SSB with `filter_bw` around 200–500 Hz, and the up-mix frequency set to the
pitch you want. Mix the carrier to DC, lowpass ±250 Hz, up-mix by 700 Hz,
take `Re{}` — a 700 Hz beat note.

Difficulty: none beyond SSB. Different constants.

The interesting part is not the demodulator, it is that a 250 Hz filter
makes AGC behaviour obvious. Pumping and clicking that you would not notice
at 2.4 kHz is glaring at 250 Hz.

## AM

Envelope detection is about twenty lines:

```
a[n] = sqrt(i^2 + q^2)   ->  DC block (strips the carrier)  ->  lowpass
```

Non-coherent, so it does not care about frequency error. The DC block's time
constant is the only subtlety: too fast and it eats the low audio.

The version worth writing for its own sake is **synchronous** AM — lock a
PLL to the carrier, then take `Re{}` as in SSB. Better SNR, and it does not
distort on selective fading the way an envelope detector does. It is also
the smallest excuse to write a PLL, which is reusable.

## FM

The only mode needing a primitive you do not have: a phase discriminator.

```
a[n] = arg( z[n] * conj(z[n-1]) )
```

The product's angle is the phase advance between samples, which is
frequency. `atan2` per output sample is affordable at audio rates — 48 k/s
is nothing — but the classic way to avoid it is

```
a[n] = (i*q' - q*i') / (i^2 + q^2)
```

with a differentiator for the primed terms, or a small polynomial `atan`.
Worth doing both and comparing; the approximation error shows up as
distortion you can actually measure.

Then a one-pole IIR for de-emphasis (time constant per whatever
pre-emphasis convention applies — check this rather than assuming) and a
squelch, usually noise power measured above the audio band.

**One trap:** do not decimate below roughly ±(deviation + audio) before
discriminating, or you clip the deviation and it sounds distorted in a way
that looks like a broken discriminator. Keep ~±8 kHz for NBFM.

Difficulty: moderate, and only because it is genuinely new code.

## What is actually hard

**AGC.** Without it, SSB and CW are unusable across a 60 dB range — one
station deafens you and the next is inaudible. Fast attack (~5 ms), slow
decay (hundreds of ms), a hang timer so it does not pump between CW
elements, and it is easier to reason about in the log domain. This is
fiddlier than all four demodulators together and it is where the time goes.

**Getting audio out.** Pluto has no codec and no jack, and the AD9361
AUXDAC pins are SPI-rate DC outputs, not audio. So audio has to leave the
box, and a network transport brings back exactly the bug class that
produced the 12 Hz chop: pacing, jitter, buffer arithmetic.

For academic work, skip it: write raw `s16le` to stdout, then
`ssh pluto ... | aplay -f S16_LE -r 48000 -c 1` or redirect to a file and
analyse offline. No transport, no new bugs, and a file is better than audio
for measuring anything.

## Rates and cost

CPU **improves**, because demodulating means no TX path — that deletes the
up-mix NCO and the int16 packing at the full 1.2 M samples/s. The demod
runs at the decimated rate, so it is nearly free. Expect to land well under
the current 71%.

Rate arithmetic is kind from 1.2 MSps:

| audio rate | ratio | |
|---|---|---|
| 48 kHz | 25 | exact |
| 24 kHz | 50 | exact |
| 8 kHz | 150 | exact |
| 44.1 kHz | 27.21 | needs a rational resampler — don't |

## How to test without touching the radio

Extend `dsp_test.c` the way it already works: synthesise a signal in the
test, run it through, and check the result numerically. A modulator is
easier to write than a demodulator, so generate a known AM / FM / SSB
signal from a known audio tone, demodulate it, and measure what comes back
against what went in — SNR, THD, frequency error.

That is a better feedback loop than listening, it runs on the host as well
as the target, and it catches the sign errors and off-by-ones that
otherwise present as "sounds a bit odd".

The 10 GHz CW generator is then the end-to-end check for CW and SSB.

## Suggested order

1. **SSB** — shows that the chain you have is already a demodulator.
2. **AM**, both ways — envelope, then synchronous, and compare. Gets you a
   PLL.
3. **FM** — the new primitive; do `atan2` and the approximation and measure
   the difference.
4. **CW** — free once SSB works. Then add AGC, because CW is where you will
   hear every flaw in it.
