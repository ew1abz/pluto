# Agent Context — PlutoSDR 10 GHz IF Downconverter

Handoff document for resuming this project with an AI coding agent. Written to
be pasted or loaded as context so an agent has the hard-won facts up front
instead of rediscovering them.

Operator: Vlad, KM6RNJ (General class), El Segundo CA.
Hardware: ADALM-Pluto, 10 GHz LNB, Yaesu FTX-1.

---

## What this project is

A single ADALM-Pluto used as a fixed-shift frequency translator so a 70cm
radio can receive 10 GHz narrowband signals.

```
10.386 GHz (dish)
  -> LNB (LO 9,750,154,910 Hz)
  -> IF 635.845090 MHz
  -> Pluto RX -> [optional narrow filter] -> Pluto TX
  -> 432.300 MHz
  -> FTX-1
```

The LNB makes the large jump; the Pluto makes the fine one. RX and TX use
independent synthesizers on the AD9363, so this is genuine full duplex on one
chip, not a passive mixer.

**Status: working.** Clean audio at 600 kSps, passthrough and filtered.

---

## Files

| File | Role |
|---|---|
| `pluto_downconverter.py` | Main program. gr-soapy. **Use this one.** |
| `pluto_downconverter.c` | Native libiio port for Pluto's own ARM. Compiles clean, DSP verified numerically, **not yet run on hardware.** |
| `pluto_tone.py` | Continuous carrier. Minimal TX test. |
| `pluto_cw_beacon.py` | Keyed Morse beacon, raised-cosine shaping. |
| `pluto_10ghz_xlator.py` | **Superseded.** Native gr-iio version. Kept for reference only. |
| `README.md` | Human-facing project documentation. |

---

## Invariants — do not "fix" these

### Three different gain conventions are in play

This is the single biggest trap in the project. The same radio exposes TX gain
three ways depending on the access path:

| Path | Parameter | Range | Direction |
|---|---|---|---|
| gr-soapy (`.py` scripts) | `--tx-gain` | 0..89 | **higher = louder** |
| raw libiio (`.c`, `iio_attr`) | `--tx-atten` / `hardwaregain` | 0..-89.75 | **0 = full power** |
| native gr-iio (`pluto_10ghz_xlator.py`) | `tx_atten` | 0..-89.75 | 0 = full power |

```
Soapy TX gain  ≈  hardwaregain + 89.75
```

An agent tidying these files will be tempted to "make the gain arguments
consistent." **Do not.** Each matches its own driver. `pluto_10ghz_xlator.py`
using `-10` is correct for that file.

RX gain is unambiguous everywhere: 0..73 dB, higher = more sensitive.

### Offset tuning is deliberate

Both LOs are tuned low by `--if-offset` (default 100 kHz):

```
RX LO = (rf_target - lnb_lo) - offset = 635,745,090 Hz
TX LO = out_freq - offset             = 432,200,000 Hz
                               signal -> 432,300,000 Hz
```

Pluto has a DC spike at RX center and LO leakage at TX center. Offsetting both
by the same amount parks both artifacts at 432.200 MHz, clear of the signal.
This is not a bug and the LOs are not "off by 100 kHz."

In filter mode the chain mixes down by the offset, filters, then mixes back up
with a rotator so the signal still clears TX LO leakage.

### Sample rate is 600 kSps for a reason

Not arbitrary, not a placeholder. See gotcha #2 below.

### `--filter-bw` defaults to 20000, not 3000

The software filter's job is killing the DC spike and limiting broadband
noise. The FTX-1's crystal filter does SSB/CW shaping far better. Narrow
software filtering costs a large resampler ratio for no benefit.

---

## Gotchas, in the order they cost time

### 1. Soapy TX gain inversion

**Symptom:** brief blip of signal at startup, then silence.

**Cause:** the DAC runs at default 0 dB attenuation for a moment before Soapy
applies the requested gain. At `--tx-gain 0` (which is *minimum*, not maximum)
you hear full power, then an 80 dB drop into the noise.

This perfectly mimics a streaming bug. Hours went into buffer lengths, USB vs
network transport, DMA-vs-DDS mode, and sample rates before the actual cause
surfaced.

> **Diagnostic rule:** a signal that *starts strong and vanishes* is
> attenuation. A signal that *plays once and stops* is a buffer problem.
> They sound identical through a speaker and look completely different on a
> waterfall. **Use an FFT display, not your ears.**

### 2. Full duplex saturates USB above ~1 MSps

At 2.5 MSps both directions that's ~20 MB/s total through the USB network
gadget. Result is choppy audio. 600 kSps fixed it and is still ~30x more
bandwidth than SSB needs.

### 3. Rates below 2.083 MSps need the FIR decimator

`sampling_frequency_available` is `[2083333 1 61440000]` — that's the raw
AD9363 floor. SoapyPlutoSDR auto-loads FIR taps below it, but **verify**, don't
assume. A silently clamped rate shifts every frequency proportionally.

```sh
iio_attr -o -c ad9361-phy voltage0 sampling_frequency   # expect 600000
iio_attr -o -c ad9361-phy voltage0 filter_fir_en        # expect 1
```

### 4. `bufflen` is a device arg, not a stream arg

```python
# WRONG - "Unsupported stream argument bufflen for channel 0"
soapy.sink("driver=plutosdr,uri=...", "fc32", 1, "", "bufflen=65536", [""], [""])
# RIGHT
soapy.sink("driver=plutosdr,uri=...,bufflen=65536", "fc32", 1, "", "", [""], [""])
```

### 5. `rational_resampler` wants `taps=[]`, not `None`

GNU Radio 3.10+ raises `TypeError: incompatible constructor arguments`.

### 6. Native gr-iio blocks never worked here

`PlutoSDR Source` produced no usable signal on this setup (Windows /
radioconda) while `Soapy PlutoSDR Source` worked immediately. **Root cause
never established.** Soapy is the working path — don't migrate back without a
reason.

### 7. Ctrl-C needs a hard-exit fallback

`tb.wait()` blocks indefinitely when a Soapy sink thread is parked inside
`writeStream()`. All scripts run graceful shutdown on a daemon thread with a
3 s timeout, then `os._exit()`. Second Ctrl-C forces immediate exit. Keep this
pattern in any new script.

### 8. SATSAGEN interaction

SATSAGEN drives the AD9361 **hardware DDS** (on-chip, no host streaming) —
hence rock solid at full power. GNU Radio switches the DAC to DMA mode and
doesn't switch back, so SATSAGEN needs an app reload, or the Pluto a power
cycle, to recover.

Useful as a known-good reference: run SATSAGEN, confirm output, then dump
attributes over SSH without touching cables and diff against the broken state.

Note `raw` is **not** the DDS enable — SATSAGEN transmits with `raw = 0`.

---

## Environment (Windows / radioconda at `C:\Users\tmp\radioconda`)

- `activate.bat` **does not work in PowerShell** — it's a batch file, so
  PowerShell runs it in a child `cmd` that exits immediately. No `(base)`
  prefix appears. Use `conda init powershell` once, or call the interpreter
  directly: `C:\Users\tmp\radioconda\python.exe script.py`
- The executable is `python`, not `python3`.
- `volk_profile` has been run. Don't re-suggest it.

Scripts live in `F:\GNURadio\`.

---

## Commands

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

Pluto shell: `ssh root@192.168.2.1`, password `analog`.

---

## Debugging checklist

1. **Waterfall, not speaker.** Attenuation and buffer problems sound the same.
2. **Read back what the device accepted.** Requested != applied.
3. **Check gain direction** for the driver you're on.
4. **Diff working vs broken.** If another program works, dump attributes in
   both states.
5. **Power-cycle between programs.** Apps leave the DAC in incompatible modes.

---

## Open work

### Frequency stability — the one that actually matters

Pluto's internal TCXO is tens of ppm. At a 636 MHz RX LO that's potentially
several kHz of drift, rough for CW/SSB. The LNB LO is calibrated to the Hz and
likely already disciplined.

**Fix:** feed the same 10 MHz reference into Pluto's external clock input so
both conversion stages lock to one source. Hardware mod — check board revision
for the right pads.

### Calibration check

Transmit a known 10 GHz reference or find a beacon at a known frequency, see
where it lands relative to 432.300. Offset = residual LNB LO error; trim
`--lnb-lo`.

### Native C version — untested on hardware

`pluto_downconverter.c` compiles clean with `-Wall -Wextra` and its DSP is
verified numerically (FIR: unity DC gain, -6 dB at cutoff, ~70-78 dB stopband;
NCO down/up round trip is identity to float precision). It has **never run on
the Pluto.**

Two known-rough parts, both flagged in the source:

- **Interpolator is zero-order hold** — each filtered sample repeated `decim`
  times. Puts images at multiples of the decimated rate. Narrow `filter_bw`
  pushes them far out and the FTX-1 front end should reject them, but a
  polyphase interpolator is the proper fix if artifacts are audible.
- **FIR is direct-form with a `memmove` per sample** — readable, not fast. May
  not keep up at 600 kSps with 129 taps on a 667 MHz A9.

**If it underruns, try this first:** stock Pluto ships with only one of its two
Cortex-A9 cores enabled.

```sh
fw_setenv maxcpus
pluto_reboot reset
```

That's a free 2x before writing any NEON. Then polyphase decomposition (skips
outputs the decimator discards, roughly a `decim`x saving), then NEON
intrinsics.

Cross-compile:
```bash
arm-linux-gnueabihf-gcc -O2 -mfpu=neon -mfloat-abi=hard \
    -o pluto_downconverter pluto_downconverter.c -liio -lm
scp pluto_downconverter root@192.168.2.1:/tmp/
```
`/tmp` is volatile; persistent install needs a firmware rebuild.

### FPGA implementation (optional)

RX->TX entirely in the Zynq fabric, no host. Start from
`analogdevicesinc/hdl` at `projects/pluto/`, insert a filter between the
`axi_ad9361` core's RX output and TX input. Package with
`analogdevicesinc/plutosdr-fw`.

Vivado version is pinned per release — check `plutosdr-fw`'s README/docker dir
first, mismatches are the usual build failure.

**No clean dynamic bitstream reload on Pluto.** The Zynq PCAP port supports it
in principle, but stock firmware loads one bitstream via FSBL at boot and the
dev cycle is rebuild-and-reflash. Not PYNQ/Kria-style overlay swapping.

Since latency doesn't matter for receive-only, the on-ARM C route is cheaper
than HDL for standalone operation.

### Hardware upgrade path

512 MB RAM was never the constraint — the rootfs size in flash is. Nobody
retrofits memory onto a stock Pluto. The community answer is **Pluto+**
(`github.com/plutoplus/plutoplus`): same Zynq 7010 and 512 MB, but adds
microSD boot (removes the rootfs ceiling, makes a real userland with an
on-board compiler plausible) and Gigabit Ethernet (sidesteps the USB
saturation in gotcha #2). Only worth it for those two features, not as a fix
for a wall already hit.

---

## Operating notes

- Dummy load or heavy attenuation while testing.
- TX on 432.3 while receiving on the same chip desenses your own RX before it
  bothers anyone else. Minimum readable gain.
- Avoid 432.100 (70cm CW/EME calling frequency) for anything repeating.
