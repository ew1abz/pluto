# Pluto

A single ADALM-Pluto used as a 3cm-to-70cm receive converter, so a 70cm
radio (Yaesu FTX-1) can work 10 GHz narrowband. The Pluto runs the
translation on its own ARM core — no host PC in the signal path.

```
10368 MHz (dish)
   -> LNB (LO 9,750,154,910 Hz)
   -> IF 617.845090 MHz
   -> Pluto RX -> [optional narrow filter] -> Pluto TX
   -> 432.000 MHz
   -> FTX-1
```

The LNB makes the large frequency jump; the Pluto makes the fine one. It is
the software equivalent of a transverter's IF stage.

**The dial reads straight across.** `rf_target - out_freq` is exactly
9936 MHz, a whole number, so the kHz digits match on both radios:
10368.100 comes out on 432.100. Change one end and you must change the
other by a whole number of MHz or that breaks.

## Layout

| Path | What |
|---|---|
| [apps/pluto_downconverter](apps/pluto_downconverter) | The converter. Native C program, Python/Soapy equivalents, TX test tools, DSP tests. |
| [apps/pa_gpo](apps/pa_gpo) | Keying an external PA from Pluto's own Rx/Tx state via the AD9363 GPO pins. |
| [docs](docs) | Design notes and ideas that are not built yet. |
| [firmware/plutosdr-fw](firmware/plutosdr-fw) | Pluto firmware, as a submodule. Contains a nested `br2-external` submodule with the Buildroot packages for these apps. |

Each app's README carries the detail: frequency plan, gain conventions,
and a long list of traps that cost real time to find. Read
[apps/pluto_downconverter/README.md](apps/pluto_downconverter/README.md)
before changing anything about gain, frequency math, or streaming.

## Two paths, and they are not interchangeable

**Native C** (`pluto_downconverter.c`) runs on the Pluto itself. Samples
never leave the device, so it is limited only by the ARM core: at the
default 1.2 MSps it uses 19% of one core in passthrough and 71% with the
filter on. This is the path that works standalone.

**Python / Soapy** (`pluto_downconverter.py`) runs on a host over USB and
needs GNU Radio. Every sample crosses the USB gadget, which is why it
defaults to 600 kSps — above roughly 1 MSps full duplex the transport
saturates and the audio breaks up. Do not copy sample rates or gain values
between the two: Soapy TX gain is 0..89 with higher meaning louder, while
raw libiio `hardwaregain` is attenuation, 0 meaning full power.

## Build and run

```sh
cd apps/pluto_downconverter
make                 # cross-build against the plutosdr-fw staging tree
make install         # stripped copy into apps/bin
make dsp_test        # numerical checks for the FIR and NCO

scp ../bin/pluto_downconverter root@192.168.2.1:/tmp/
ssh root@192.168.2.1 /tmp/pluto_downconverter
```

It links `libiio` and `libad9361`, both already in the stock Pluto rootfs.
`/tmp` is volatile — for a persistent install, build it into the firmware
image via the Buildroot package in `br2-external`.

Run `dsp_test` **on the Pluto**, not on a host: that is where the NEON path
is the one being exercised.

Stop it with **SIGTERM**, not SIGKILL. It handles SIGTERM and releases the
DMA buffers; killing it outright has taken the USB network gadget down.

### Firmware

```sh
cd firmware/plutosdr-fw
make prepare-sources
export VIVADO_SETTINGS=/opt/Xilinx/2025.1/Vivado/settings64.sh
make SKIP_LEGAL=1
```

`make` depends on `clean-build`, which wipes `build/` — copy anything you
want to keep first. See
[firmware/plutosdr-fw/README.md](firmware/plutosdr-fw/README.md).

Three nested repos are in play, so a change to a Buildroot package is three
commits: `br2-external`, then the `plutosdr-fw` gitlink, then this one.

## Health check

The program reports what it is actually achieving, not just that it is
running:

```
814 blocks   1200.0 kSps   100% of real time
```

The RX refill blocks, so it can never run fast. Anything below ~100% means
samples are going on the floor, and you will hear it.
