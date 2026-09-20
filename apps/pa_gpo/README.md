# GPO / External PA Control

Keys an external power amplifier or T-R relay from Pluto's own Rx/Tx state,
using the AD9363's GPO pins — no separate PTT line from the host.

Adapted from the Analog Devices wiki page
[Controlling External Devices on the ADALM-PLUTO](https://wiki.analog.com/university/tools/pluto/hacking/power_amp),
with error checking, options, and an FDD restore path added.

| File | Purpose |
|---|---|
| `pluto_gpo_ptt.c` | **Main program.** Sets up GPO slaving, sets ENSM state, benchmarks swap time. |
| `pluto_gpo_ptt.sh` | Same thing in `iio_attr` calls. Slow, but good for scoping the pins. |

---

## How it works

The AD9363 Enable State Machine (ENSM) is either in FDD (Rx and Tx chains both
live) or in one of the TDD states. The four GPO pins can be *slaved* to that
state machine, so they follow Rx/Tx without any further software involvement:

```
adi,frequency-division-duplex-mode-enable = 0   # 0 = TDD, 1 = FDD
adi,gpo0-slave-rx-enable                  = 1   # GPO_0 asserts in Rx
adi,gpo1-slave-tx-enable                  = 1   # GPO_1 asserts in Tx
initialize                                = 1   # apply the above
```

These are *debug* attributes (`iio_attr -D`), and nothing takes effect until
`initialize` is written. After setup, writing `rx` or `tx` to `ensm_mode` swaps
the pins.

Verify with `ensm_mode_available`: it lists `rx tx` in TDD and `fdd` in FDD.

---

## Usage

```sh
./pluto_gpo_ptt --setup                  # TDD + GPO slaving, leave in rx
./pluto_gpo_ptt --state tx               # key transmit
./pluto_gpo_ptt --state rx               # back to receive
./pluto_gpo_ptt --bench                  # toggle rx/tx until Ctrl-C, report rate
./pluto_gpo_ptt --restore                # back to FDD
```

`--uri` defaults to the local context, which is what you want running on Pluto
itself. From a host, `--uri ip:192.168.2.1` or `--uri usb:1.3.5`.

`--gpo-rx N` / `--gpo-tx N` pick different pins (0–3).

### Build

```sh
# host
gcc -O2 -o pluto_gpo_ptt pluto_gpo_ptt.c -liio

# cross-compile for Pluto, using the firmware staging tree
arm-linux-gnueabihf-gcc -O2 -mfpu=neon -mfloat-abi=hard \
    -I<fw>/buildroot/output/staging/usr/include \
    -L<fw>/buildroot/output/staging/usr/lib \
    -o pluto_gpo_ptt pluto_gpo_ptt.c -liio

scp pluto_gpo_ptt root@192.168.2.1:/tmp/
```

Same toolchain and staging directory as `pluto_downconverter.c`; see
[../pluto_downconverter/README.md](../pluto_downconverter/README.md).

### In the firmware image

Built into the image by the `pluto_gpo_ptt` Buildroot package in the
`br2-external` tree, enabled by `BR2_PACKAGE_PLUTO_GPO_PTT=y` in
`configs/zynq_pluto_defconfig`. The package builds straight out of this
directory (`SITE_METHOD = local`), so there is no separate copy of the source
to keep in sync — same arrangement as `pluto_downconverter`.

Installed on target as:

```
/usr/bin/pluto_gpo_ptt
/usr/bin/pluto_gpo_ptt.sh
```

---

## TDD is incompatible with the downconverter

**This does not coexist with `pluto_downconverter`.** That program is
full-duplex: it receives the LNB IF and transmits into 70cm at the same time,
which requires FDD. GPO slaving requires TDD, and in TDD:

- capturing a buffer while the part is in `tx` blocks until it times out,
- pushing a buffer while in `rx` blocks the same way.

So this is for half-duplex / push-to-talk use — the beacon, the test tone, or
anything keyed. Run `--restore` before starting the downconverter again.

If the downconverter ever needs external PA switching, the GPO route is not
the answer; drive the PA from a host GPIO or from the FPGA instead.

---

## Electrical limits

`VDD_GPO` on Pluto is **1.3 V**. Each GPO pin drives roughly 1.04–1.30 V at
about 10 mA typical (32 Ω on-resistance, 15 Ω off).

That is not enough to switch a relay or most PA enable inputs directly —
**level shift and buffer it.** Wiring a PA enable pin straight to a GPO test
point will either do nothing or load the pin.

Side effect of the 1.3 V rail: `AUX_ADC` and `AUX_DAC` are non-functional on
this board.

The GPO_[0:3] pins come out to test points; check the Pluto schematic for
their location on your board revision.

---

## Timing

Software (SPI) control only, from the wiki:

| Platform | Transport | Infrastructure | Rx/Tx swap |
|---|---|---|---|
| host | USB | shell | 80 ms |
| host | USB | C code | 0.6 – 1.3 ms |
| pluto | local | shell | 30 – 35 ms |
| pluto | local | C code | 0.2 – 0.6 ms |

`--bench` measures this on your own setup.

For slotted systems needing sub-30 µs swaps, ENSM **pin control** from an FPGA
state machine is required — not reachable from userspace.

Assertion can be delayed per pin with the `gpo0-rx-delay-us` /
`gpo0-tx-delay-us` debug attributes (useful for letting a relay settle before
RF appears). Not wrapped by these scripts.

---

## Operating notes

- Confirm the pins with a scope before connecting a PA. `--bench` gives a
  steady square wave to look at.
- Slaving is per-direction: `gpoN-slave-rx-enable` and `gpoN-slave-tx-enable`
  are separate bits, and nothing stops you slaving the same pin to both.
- The C program returns the part to `rx` when `--bench` is interrupted, so a
  PA keyed off the Tx pin doesn't stay hot on Ctrl-C. The shell version traps
  `INT`/`TERM` for the same reason.
- Debug attribute writes fail if the kernel lacks IIO debugfs support. The
  program reports this rather than silently continuing.
