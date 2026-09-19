# AGENTS.md

## Purpose

This workspace contains a Pluto SDR radio project plus the ADI HDL reference-design tree in [hdl/](hdl/). The highest-value project context is in [AGENT_CONTEXT.md](AGENT_CONTEXT.md); treat that as the source of truth for the radio stack, gain conventions, signal-processing assumptions, and debug workflow.

## Project layout

- [AGENT_CONTEXT.md](AGENT_CONTEXT.md): Pluto-specific architecture, hardware assumptions, pitfalls, and debugging notes.
- [hdl/README.md](hdl/README.md): overview of the HDL reference-design repository and build workflow.
- [hdl/projects](hdl/projects): FPGA project directories and board-specific builds.
- [hdl/library](hdl/library): reusable HDL IP and utility modules.

## Working conventions

- Prefer small, targeted edits over broad cleanup.
- Do not change radio behavior that is intentionally different across drivers unless the user explicitly asks for a change.
- Preserve the known operational invariants from [AGENT_CONTEXT.md](AGENT_CONTEXT.md):
  - Soapy TX gain is not equivalent to raw libiio attenuation.
  - The offset tuning is deliberate and not a defect.
  - 600 kSps is a deliberate operating point for the full-duplex path.
  - The working path is the Soapy Pluto SDR stack, not the native gr-iio route.
- If a change touches gain, frequency math, filtering, or streaming behavior, explain the reasoning and validate against the documented behavior instead of assuming a generic RF rule applies.
- Keep hardware-specific details and warnings intact; do not “simplify” them away.

## Build and validation guidance

- For HDL work, follow the build flow in [hdl/README.md](hdl/README.md): use Make in the relevant project directory, for example:

  ```bash
  cd hdl/projects/<project>/<board>
  make
  ```

- For Pluto SDR script work, use the run patterns and safeguards captured in [AGENT_CONTEXT.md](AGENT_CONTEXT.md), including the documented `--tx-gain`, offset, filter, and shutdown behavior.
- If a bug report sounds like an RF or stream issue, diagnose with the project’s checklist before changing code: waterfall/FFT view first, then read-back device settings, then compare against the known-good state.

## Useful links

- [AGENT_CONTEXT.md](AGENT_CONTEXT.md)
- [hdl/README.md](hdl/README.md)
- [hdl/docs](hdl/docs)
- [hdl/projects](hdl/projects)
