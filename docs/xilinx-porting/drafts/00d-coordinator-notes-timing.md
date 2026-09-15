# Draft: coordinator first-hand notes on timing & reporting

*Written by the coordinator from targeted greps in both repos. Merges
into docs 02/03/04.*

## Upstream (himbaechel, nextpnr 0.11.1)

- **Complete chipdb-based timing model**: `himbaechel/arch.h` L592–882 has
  pip timing (`PipTimingPOD` via `timing_idx`), node timing
  (`NodeTimingPOD`), cell timing lookup
  (`get_cell_timing_idx`, `lookup_cell_seq_timings` → setup/hold arcs,
  `getCellDelay` via `DelayQuad`), port timing classes
  (`get_port_timing_class_default`). `getDelayFromNS` = ns*1000.
- **Mainline `--report`**: `common/kernel/report.cc` produces JSON with
  critical paths (per-clock and cross-clock), detailed net timings,
  utilization — upstream has this natively.
- `common/place/timing_opt.cc` exists (timing optimization during
  placement); timing-driven placement is part of upstream common/place.

## Fork (nextpnr-xilinx)

- Timing from its own binary chipdb:
  `chip_info->timing_data->tile_cell_timings[tt_id].instances[inst_id]`,
  `xc7_cell_timing_lookup` (xilinx/arch.cc L2577+); `estimateDelay`
  (arch.cc L650); router2 runs final timing analysis with slack
  histogram/fmax/path print (arch.cc L2159–2166); default setup/hold/ck2q
  fallback of 0.1 ns when data missing (L2507–2509).
- Fork has its own flat `common/` (command.cc, timing.cc, place_common.cc,
  placer1.cc, placer_heap.cc, router1.cc, router2.cc) — old layout, no
  common/place + common/route split, no common/kernel.
- `--report` was backported mainline-style in fork commit 3266cf4d
  ("common: add mainline-style --report (timing + utilization JSON)");
  lives in fork's `common/command.cc` (upstream already has it in
  common/kernel/report.cc, so this backport is NOT needed upstream).
- Multicycle: fork tags cells with `NEXTPNR_MCP_SETUP` attribute
  (xdc.cc) — the timing-engine consumption point needs checking; upstream
  has its own SDC path (`common/kernel/sdc.cc`).

## Interpretation

- Timing model: both have chipdb-derived timing; upstream's is generic
  and complete for xc7 via meta/RapidWright timings — no porting needed,
  except verifying that the xc7 timing data upstream is complete for the
  parts we care about (fork's 0.1 ns fallbacks suggest gaps there too).
- Reporting: upstream has it; fork's backport is redundant.
- The fork's "final timing analysis after router2" (commit 7ea51730)
  needs a check against upstream router2 behavior — upstream router2 may
  already do this or may need the same hook.
