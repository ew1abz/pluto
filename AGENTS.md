# AGENTS.md

## Purpose

This workspace contains a Pluto SDR application and a customized Pluto firmware repository. The application behavior and radio assumptions are documented in [apps/pluto_downconverter/README.md](apps/pluto_downconverter/README.md).

## Project layout

- [apps/pluto_downconverter](apps/pluto_downconverter): host-side scripts and the native C application.
- [apps/pluto_downconverter/README.md](apps/pluto_downconverter/README.md): radio architecture, operating assumptions, and debugging notes.
- [apps/pa_gpo](apps/pa_gpo): AD9363 GPO pin control for keying an external PA from the radio's Rx/Tx state.
- [apps/pa_gpo/README.md](apps/pa_gpo/README.md): GPO electrical limits, ENSM timing, and the TDD/FDD constraint.
- [firmware/plutosdr-fw](firmware/plutosdr-fw): Pluto firmware repository as a Git submodule.
- [firmware/plutosdr-fw/br2-external](firmware/plutosdr-fw/br2-external): Buildroot external tree containing the application package.

## Working conventions

- Prefer small, targeted edits over broad cleanup.
- Do not change radio behavior that is intentionally different across drivers unless the user explicitly asks for a change.
- Preserve the known operational invariants from [apps/pluto_downconverter/README.md](apps/pluto_downconverter/README.md):
  - Soapy TX gain is not equivalent to raw libiio attenuation.
  - The offset tuning is deliberate and not a defect.
  - 600 kSps is a deliberate operating point for the full-duplex path.
  - The working path is the Soapy Pluto SDR stack, not the native gr-iio route.
- GPO slaving in [apps/pa_gpo](apps/pa_gpo) requires TDD mode, which is mutually exclusive with the full-duplex downconverter path. Do not enable it as a "fix" for PA switching in the translator, and restore FDD afterwards.
- If a change touches gain, frequency math, filtering, or streaming behavior, explain the reasoning and validate against the documented behavior instead of assuming a generic RF rule applies.
- Keep hardware-specific details and warnings intact; do not “simplify” them away.

## Build and validation guidance

- For Pluto SDR script work, use the run patterns and safeguards captured in [apps/pluto_downconverter/README.md](apps/pluto_downconverter/README.md), including the documented `--tx-gain`, offset, filter, and shutdown behavior.
- For firmware changes, build from `firmware/plutosdr-fw` and preserve its pinned submodule revisions.
- If a bug report sounds like an RF or stream issue, diagnose with the project’s checklist before changing code: waterfall/FFT view first, then read-back device settings, then compare against the known-good state.

## Useful links

- [apps/pluto_downconverter/README.md](apps/pluto_downconverter/README.md)
- [apps/pa_gpo/README.md](apps/pa_gpo/README.md)
- [firmware/plutosdr-fw/README.md](firmware/plutosdr-fw/README.md)
- [Controlling External Devices on the ADALM-PLUTO](https://wiki.analog.com/university/tools/pluto/hacking/power_amp) — source of the GPO/ENSM code in [apps/pa_gpo](apps/pa_gpo).
