# Live filter tuning

**Status:** idea, not implemented. Deferred deliberately — `--listen` covers
the immediate need.

## What exists today

`pluto_downconverter --filter --listen 432.150e6` centres the narrow filter
on any 70cm frequency inside the captured window. Both mixers move by the
same amount, so the signal is put back exactly where it came from and the
1:1 mapping between the 3cm and 70cm dials is preserved.

It is a **startup option**. Changing where you listen means restarting the
program, which drops audio for a second or two.

## The idea

Make it a knob instead: the filter follows the radio, so you turn the FTX-1
dial and the passband tracks it. Two ways in, and they are not exclusive —
the second is a reasonable stepping stone to the first.

### 1. CAT control (the real answer)

Poll the FTX-1's VFO frequency over its CAT serial interface, convert to a
`tune` offset, retune the NCOs in place.

Unknowns to settle before writing code:

- Which CAT dialect the FTX-1 speaks and whether the USB CDC port is
  reachable from the Pluto, or whether this has to run on a host that then
  talks to the Pluto. Running it on the Pluto keeps the standalone property,
  which is the point of the native C version.
- Poll rate. Fast enough to feel attached to the dial, slow enough not to
  eat the ~29% of one core that is left at the default 1.2 MSps. A few tens
  of hertz should be ample; the retune itself is trivial.
- Whether the radio reports the dial frequency or the carrier, and how that
  interacts with USB/LSB/CW offsets. Getting this wrong puts the passband a
  few hundred Hz off and it will sound like the filter is mistuned.
- What to do when the operator tunes outside the captured window. Refusing
  is wrong for a knob. Probably clamp the filter to the window edge and say
  so, or retune the Pluto RX LO — but moving the RX LO changes the frequency
  plan mid-stream and needs thought.

### 2. Control socket (cheap stepping stone)

Read a `tune` value between blocks from a UDP socket or a FIFO. No radio
integration, no CAT dialect to reverse-engineer, and it makes the retune
path testable on its own. Drives equally well from a script, a rotary
encoder, or an eventual CAT daemon.

This is the part worth building first either way: once retuning works
glitch-free, the CAT side is just deciding what number to feed it.

## Why it is cheap

An NCO costs the same at any frequency — one complex multiply per sample.
Measured 71.4% / 71.6% / 71.4% of one core at 0, +150 and −400 kHz, all at
100% of real time. Retuning changes nothing about the cost; the FIR is a
baseband lowpass and does not move at all.

## Implementation note: retune without a click

`nco_t` holds a running phasor (`pi`, `pq`) and a per-sample step (`si`,
`sq`). To change frequency, **recompute only the step and leave the phasor
alone**:

```c
static void nco_retune(nco_t *n, double freq_hz, double fs)
{
    double w = 2.0 * M_PI * freq_hz / fs;
    n->si = (float)cos(w);
    n->sq = (float)sin(w);
    /* pi, pq, and n are deliberately untouched */
}
```

The phase is then continuous across the change and there is no click. Reset
the phasor to 1+0j and you get a discontinuity you will hear on every dial
movement.

Both mixers have to move together, and by the same amount, or the 1:1
mapping breaks — `nco_down` by `-mix` and `nco_up` by `+mix`, where
`mix = if_offset + tune`. See `pluto_downconverter.c` around the `--listen`
handling and the `nco_init` pair in the DSP setup.

Calling this between blocks (in the main loop, not from a signal handler) is
safe. The held interpolator sample and the FIR history stay valid: they are
baseband quantities and do not care what the mixers are doing.

## Constraints that carry over

Both are already enforced for `--listen` and apply unchanged:

- The passband must stay inside the captured window:
  `|if_offset + tune| + filter_bw < samp_rate/2`, which is
  431.320 – 432.480 MHz at the current defaults.
- Tuning onto `out_freq - if_offset` (431.900 MHz) puts the RX DC spike in
  the passband. Warn, do not refuse — the TX LO leakage sitting at the same
  frequency is hardware and is not removable either way.

## Why not now

`--listen` handles the actual use case: park on a frequency and work it. A
knob only earns its complexity once there is enough activity on 3cm to be
tuning around during a session.
