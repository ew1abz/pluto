# PlutoSDR 10 GHz IF Downconverter

Uses a single ADALM-Pluto as a fixed-shift frequency translator, letting a
70cm-capable radio (Yaesu FTX-1) receive 10 GHz narrowband signals.

```
10.386 GHz (dish)
   -> LNB (LO 9,750,154,910 Hz)
   -> IF ~635.845 MHz
   -> Pluto RX -> [optional narrow filter] -> Pluto TX
   -> 432.300 MHz
   -> FTX-1
```

The LNB does the large frequency jump; the Pluto does the fine one so the
signal lands somewhere the FTX-1 can tune. This is the software equivalent of
a transverter's IF stage.

---

## Scripts

| Script | Purpose |
|---|---|
| `pluto_downconverter.py` | **Main program.** RX at the LNB IF, TX into 70cm. |
| `pluto_tone.py` | Continuous carrier. Minimal "does TX work" test. |
| `pluto_cw_beacon.py` | Keyed Morse beacon with raised-cosine shaping. |
| `pluto_10ghz_xlator.py` | *Superseded.* Early native gr-iio version, kept for reference. Uses the opposite gain convention — see below. |

### Usage

```bash
# main downconverter
python pluto_downconverter.py --tx-gain 60 --bufflen 65536

# with narrow filtering
python pluto_downconverter.py --filter --filter-bw 3000 --tx-gain 60

# TX test tone
python pluto_tone.py --freq 432.3e6 --tx-gain 60

# CW beacon
python pluto_cw_beacon.py --text "VVV DE KM6RNJ" --freq 432.3e6 --tx-gain 89
```

---

## Frequency plan

```
IF center = RF_target - LNB_LO = 10,386,000,000 - 9,750,154,910 = 635,845,090 Hz

RX LO = IF center - offset = 635,745,090 Hz
TX LO = out_freq  - offset = 432,200,000 Hz
                    signal -> 432,300,000 Hz
```

### Why offset tuning

Pluto has a **DC offset spike at the RX center** and **LO leakage at the TX
center**. Both LOs are deliberately tuned low by the same amount
(`--if-offset`, default 100 kHz). The wanted signal lands exactly at
`out_freq`, and both artifacts collect at `out_freq - offset` (432.200 MHz),
out of the passband.

With `--filter`, the xlating filter removes the DC spike entirely, and a
rotator re-applies the offset before TX so the signal stays clear of TX LO
leakage.

---

## Findings

Everything below cost real debugging time. None of it is documented anywhere
obvious.

### 1. Soapy TX gain is inverted from the raw IIO attribute

**This was the big one.** SoapyPlutoSDR follows the SoapySDR convention where
higher = more output. The underlying IIO `hardwaregain` attribute is
*attenuation*, where 0 = full power and −89.75 = minimum.

```
Soapy TX gain  ≈  hardwaregain + 89.75
```

| `--tx-gain` | `hardwaregain` | Output |
|---|---|---|
| 0 | −89.75 dB | essentially nothing |
| 10 | −79.75 dB | inaudible |
| 60 | −29.75 dB | usable |
| 89 | −0.75 dB | full power |

**Symptom when wrong:** a brief blip of signal at startup, then silence. The
DAC runs at the default 0 dB attenuation for a moment before Soapy applies the
requested gain, so you hear full power, then an 80 dB drop into the noise.

**This is not a streaming bug**, though it perfectly mimics one. Hours went
into chasing buffer lengths, USB vs network transport, DMA-vs-DDS mode, and
sample rates before the actual cause surfaced.

> **Diagnostic rule:** a signal that *starts strong and vanishes* is
> attenuation. A signal that *plays once and stops* is a buffer problem.
> They sound identical. On a waterfall they look completely different — the
> attenuated carrier is still visibly sitting there at −80 dB.
> **Use an FFT display, not a speaker.**

The native gr-iio blocks use the opposite convention — raw negative
attenuation, where `-10` means −10 dB. `pluto_10ghz_xlator.py` is written
against that and should *not* be "fixed" to use 0–89.

### 2. Full duplex saturates the USB transport above ~1 MSps

At 2.5 MSps in both directions, that's roughly 10 MB/s each way (~20 MB/s
total) through Pluto's USB network gadget. The result is choppy, broken audio.

**Fix:** drop the sample rate. 600 kSps is the default now — still ~30× more
bandwidth than an SSB signal needs.

### 3. Rates below 2.083 MSps need the on-chip FIR decimator

`sampling_frequency_available` reports `[2083333 1 61440000]`. That is the raw
AD9363 floor. SoapyPlutoSDR auto-loads FIR decimator taps to reach lower rates,
but **verify rather than assume** — a silently clamped rate shifts every
frequency in the flowgraph proportionally.

```sh
iio_attr -o -c ad9361-phy voltage0 sampling_frequency   # expect 600000
iio_attr -o -c ad9361-phy voltage0 filter_fir_en        # expect 1
```

Raw libiio does **not** do this for you. `pluto_downconverter.c` originally
wrote `sampling_frequency` directly and got `-EINVAL` (-22) back, leaving the
device at its 30.72 MSps reset default:

```
warn: write sampling_frequency = 600000 failed (-22)
```

It now calls `ad9361_set_bb_rate()` from libad9361, which designs and loads
the decimate/interpolate-by-4 FIR when the requested rate is below the
FIR-bypassed floor, and sets RX and TX together. That means linking
`-lad9361`; `libad9361.so.0` is already in the Pluto rootfs.

### 4. `bufflen` is a device arg, not a stream arg

```python
# WRONG - raises "Unsupported stream argument bufflen for channel 0"
soapy.sink("driver=plutosdr,uri=...", "fc32", 1, "", "bufflen=65536", [""], [""])

# RIGHT
soapy.sink("driver=plutosdr,uri=...,bufflen=65536", "fc32", 1, "", "", [""], [""])
```

Driver default is 4096 samples — only ~7 ms at 600 kSps.

### 5. `rational_resampler` wants `taps=[]`, not `None`

GNU Radio 3.10+ raises `TypeError: incompatible constructor arguments` on
`None`.

### 6. Don't duplicate the radio's crystal filter

The default `--filter-bw` is 20000 Hz, not 3000. The software filter's job is
killing the DC spike and limiting broadband noise; the FTX-1's crystal filter
does final SSB/CW shaping far better. Narrow software filtering means a large
resampler ratio for no benefit.

Resampler cost at 600 kSps:

| `--filter-bw` | decim/interp ratio |
|---|---|
| 20000 Hz | 3 |
| 3000 Hz | 25 |
| 500 Hz | 150 |

(At the old 2.5 MSps default these were 15 / 104 / 625 — dropping the sample
rate made narrow filtering practical as a side effect.)

### 7. The C filter path needed a rewrite to run in real time

The first native-C `--filter` implementation could not keep up on Pluto's
single 667 MHz Cortex-A9. Measured at 600 kSps with `--tx-atten -89`:

| | CPU (one core) | throughput |
|---|---|---|
| original | 98.5% — pegged | ~66% of real time, dropping samples |
| current | 38.9% | 100% of real time |

Three things were eating the core, none of them the MAC loop people expect:

1. **`cos()`/`sin()` per sample.** The NCO called libm twice per sample on
   each of the down- and up-mixes — four double-precision transcendentals
   per input sample, 600k times a second, on a core with no hardware for
   them. Replaced with a recursive complex rotator (one complex multiply per
   sample) that is renormalised to the unit circle every 1024 samples.
   Measured phase error after 2M samples: 8e-9 rad.
2. **A `memmove` of the whole history per sample.** The FIR kept a shift
   register, so every input sample moved ~1 KB. History is now a contiguous
   window with one `memmove` per *block*.
3. **Scalar MACs.** The dot product is now NEON, four taps at a time.

Polyphase decomposition was *not* needed — the original already computed
only at the output rate, so it would have bought nothing.

The rewrite also fixed a latent off-by-one: the old shift register applied
`taps[0]` and `taps[1]` both to the newest sample and delayed every other
tap by one, because it wrote the current sample into the history *and* then
used it again directly. `dsp_test.c` checks the filter against a
double-precision direct convolution and would fail on the old code.

```sh
make dsp_test
scp dsp_test root@192.168.2.1:/tmp/ && ssh root@192.168.2.1 /tmp/dsp_test
```

Run it on the target, not a host — that is where the NEON path is real.

Interpolation back up to the full rate is still a zero-order hold, which is
crude. It is cheap and its images land far out, but with ~60% of the core
now free that is the obvious next thing to improve if it proves audible.

### 8. Native gr-iio blocks vs Soapy

The native `PlutoSDR Source` block never produced usable signal on this setup
(Windows / radioconda) while `Soapy PlutoSDR Source` worked immediately.
Root cause never established. **Soapy is the working path.**

### 8. Run `volk_profile` once

Every run warns about the missing VOLK config. Unprofiled VOLK falls back to
generic kernel implementations, which matters for FIR and resampler
performance. One-time, takes a few minutes.

### 9. Ctrl-C needs a hard-exit fallback

`tb.wait()` can block indefinitely when a Soapy sink thread is parked inside a
`writeStream()` call. All scripts run graceful shutdown on a daemon thread with
a 3-second timeout, then `os._exit()`. A second Ctrl-C forces immediate exit.

### 10. Windows / radioconda environment

- `activate.bat` **does not work in PowerShell** — it's a batch file, so
  PowerShell runs it in a child `cmd` that exits immediately, discarding the
  environment. No `(base)` prefix appears.
- Use `conda init powershell` once, or call the interpreter directly:
  `C:\Users\tmp\radioconda\python.exe script.py`
- The executable is `python`, not `python3`.

### 11. SATSAGEN interaction

SATSAGEN uses the AD9361's **hardware DDS** (on-chip tone generator, no host
streaming), which is why it's rock solid at full power. GNU Radio switches the
DAC to DMA mode and doesn't switch back, so SATSAGEN needs an app reload —
or the Pluto a power cycle — to recover afterwards.

Useful for comparing working vs broken state: run SATSAGEN, confirm output,
then dump attributes over SSH without touching cables.

```sh
ssh root@192.168.2.1        # password: analog
iio_attr -o -c ad9361-phy voltage0
iio_attr -o -c cf-ad9361-dds-core-lpc altvoltage0
```

Note `raw` is **not** the DDS enable — SATSAGEN transmits successfully with
`raw = 0`.

---

## Debugging checklist

When something doesn't work, in order:

1. **Look at a waterfall, not a speaker.** Attenuation and buffer problems
   sound the same and look completely different.
2. **Read back what the device accepted.** Requested ≠ applied, for sample
   rate especially.
3. **Check gain direction.** Confirm which end of the scale is loud on the
   driver you're using.
4. **Diff working vs broken.** If another program works, dump attributes in
   both states and compare.
5. **Power-cycle between programs.** Applications leave the DAC in
   incompatible modes.

---

## Outstanding work

### Frequency stability (not yet done)

Pluto's internal TCXO is good to tens of ppm. At a 636 MHz RX LO that is
potentially several kHz of drift — rough for CW and SSB. The LNB LO is
calibrated to the Hz and likely already disciplined.

**Fix:** feed the same 10 MHz reference into Pluto's external clock input so
both conversion stages are locked to one source. This is a hardware
modification (check board revision for the right pads/input), not a software
change.

### Calibration check

Transmit a known 10 GHz reference or find a beacon at a known frequency, then
see where it lands relative to 432.300 MHz. Any offset is residual LNB LO
error; trim `--lnb-lo` to compensate.

### FPGA implementation (optional)

The whole RX→TX path could run in the Zynq fabric, removing the host entirely.
Start from `analogdevicesinc/hdl` (`projects/pluto/`) and insert a filter
between the `axi_ad9361` core's RX output and TX input.

Note Pluto has **no clean dynamic bitstream reload path** like PYNQ/Kria. The
Zynq PCAP port supports it in principle, but stock firmware loads one bitstream
via FSBL at boot, and the normal dev cycle is rebuild-firmware-and-reflash.

Since latency doesn't matter for receive-only use, a cheaper route to
standalone operation is running GNU Radio directly on Pluto's own ARM core —
no HDL work at all.

---

## Operating notes

- Dummy load or heavy attenuation while testing. Pluto TX is ~0 dBm max, but
  a continuous carrier on 70cm is still a real transmission.
- Transmitting on 432.3 while receiving on the same chip will desense your own
  RX before it bothers anyone else. Use the minimum readable gain.
- Avoid calling frequencies (432.100 is the 70cm CW/EME calling frequency) for
  anything repeating.
