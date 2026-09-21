# Passthrough in the FPGA fabric

**Status:** idea, not implemented.

**Headline: there is no HDL to write.** The ADC-to-DAC loopback is already
in the bitstream Pluto ships with. What is missing is a way to select it.

## Why passthrough is a wire

In passthrough the frequency translation is done entirely by LO placement —
the ARM copies RX samples to TX unchanged. The function being performed is
the identity. Everything the program does in that mode is transport.

That includes offset tuning, which is LO placement and nothing else, so it
survives untouched. The signal still lands on `out_freq`, and the RX DC
spike and TX LO leakage still collect at `out_freq - if_offset`.

## The loopback already exists

In `axi_ad9361_tx_channel.v`, the DAC source mux:

```verilog
case (dac_data_sel_s)
  4'h9: dac_data_out_int <= dac_pn_data;
  4'h8: dac_data_out_int <= adc_data;      // <-- this one
  4'h3: dac_data_out_int <= 12'd0;
  4'h2: dac_data_out_int <= dma_data[15:4];
  ...
```

and in `axi_ad9361.v` the RX data is wired straight into the TX block on the
same clock:

```verilog
) i_tx (
  .dac_clk (clk),
  .adc_data (adc_data_s),
```

Three things follow, all of which would otherwise be design work:

- **Single clock domain.** RX and TX share `clk`, so there is no CDC and no
  rate-matching FIFO. This holds because `ad9361_set_bb_rate()` sets both
  directions together; it would not hold if they were ever set apart.
- **The enable logic is already right.** `dac_enable_int` asserts only for
  `dac_data_sel == 4'h2` (DMA), so in loopback the channel correctly stops
  asking the DMA for data.
- **The widths already line up.** The DMA path takes `dma_data[15:4]` while
  loopback takes `adc_data` directly — both end up 12-bit aligned, so
  there is no scaling mismatch to fix.

One thing to be aware of: the mux feeds `ad_iqcor`, so TX IQ correction is
applied *after* the mux and still acts on looped-back data. That is
probably useful, but it is not a bypass.

## What it buys

| | ARM passthrough today | fabric |
|---|---|---|
| CPU | 19% of one core at 1.2 MSps | none |
| Latency | two 16384-sample buffers, ~27 ms at 1.2 MSps | a few clock cycles |
| Dropped samples | possible | structurally impossible |
| Max rate | CPU-bound; 31.8% at 2 MSps, so binding somewhere near 5–6 MSps | an RF choice only |

The dropped-sample point is the strongest one. Every streaming bug found in
this project — the partially filled TX buffer, the 12 Hz chop, the 34%
sample loss before the DSP rewrite — lives in the transport. None of them
can exist in a wire. A fabric passthrough also keeps running if userspace
dies, because there is no userspace.

Raising the rate is the second prize. At 2 MSps the sky window is
10366.900 – 10368.900; the fabric does not care how wide you make it, so
the rate becomes a question of how much spectrum you want to retransmit
into 70cm, not how fast the A9 is.

## What it gives up

- **All filtering.** That is the premise, but it means the DC spike is only
  displaced by offset tuning, never removed. The ARM `--filter` path stays
  the answer when the spike matters.
- **The health readout.** There is nothing to report a percentage of real
  time, because there is nothing that can fall behind. Fair trade, but the
  observability goes.
- **Any DSP hook at all.** `--listen`, gain scaling, anything later — all of
  it lives in the ARM path.

Fabric passthrough replaces `pluto_downconverter` with no filter. It does
not replace `--filter`.

## Turning it on

The register, from `cf_axi_dds.h`:

```c
#define ADI_REG_CHAN_CNTRL_7(c)  (0x0418 + (c) * 0x40)
#define ADI_DAC_DDS_SEL(x)       (((x) & 0xF) << 0)
```

with `DATA_SEL_LB = 8` and the DAC core at `0x79024000`
(`cf-ad9361-dds-core-lpc@79024000` in `zynq-pluto-sdr.dtsi`):

| channel | address |
|---|---|
| 0 (I) | `0x79024418` |
| 1 (Q) | `0x79024458` |

Set bits [3:0] to `8` in each, **read-modify-write** — the driver does the
same and the rest of the register is not ours to clear.

These addresses are derived from the source, not yet exercised on hardware:
the Pluto dropped off the network before the read-back could be confirmed.
Verify with a read before writing.

Two operational constraints:

- **Nothing may have the TX buffer open.** `cf_axi_dds_update_scan_mode()`
  rewrites the selector to `DATA_SEL_DMA` or `DATA_SEL_DDS` for every
  channel whenever the scan mask changes, so enabling a TX buffer will
  stamp on the setting. Set the LOs with `iio_attr`, then poke, and do not
  run `pluto_downconverter` alongside it.
- **FDD**, as before. `apps/pa_gpo` needs TDD and is still mutually
  exclusive with any of this.

## The version worth building

`DATA_SEL_LB` is defined in `cf_axi_dds.h` but nothing in the driver ever
selects it — `cf_axi_dds_datasel()` is only ever called with `DMA`, `DDS`
or `SED`. A short patch exposing it, as a device attribute on
`cf-ad9361-dds-core-lpc` calling `cf_axi_dds_datasel(st, -1, DATA_SEL_LB)`,
would be perhaps twenty lines and is better than `devmem` in every way: it
is discoverable through libiio, it can re-assert itself around buffer
enable, and it does not require knowing a physical address.

That is the piece of work here. The FPGA is done.

## Open questions

- Does the driver need to hold the selector across `update_scan_mode()`, or
  is it enough to refuse to open a TX buffer while in loopback? The second
  is simpler and probably correct.
- What sets the LOs and gains in a fabric-only setup — a boot script, or
  something small and long-lived? There is no program in the signal path
  any more, but something still has to configure the AD9361.
- Is the AD9361's own BIST loopback (`bist_loopback` in the driver) a
  simpler lever for the same effect? It loops inside the chip rather than
  in fabric, which may bypass parts of the path we want.
