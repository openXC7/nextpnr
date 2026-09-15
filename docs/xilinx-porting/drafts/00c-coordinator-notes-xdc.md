# Draft: coordinator first-hand notes on XDC parsers (upstream vs fork)

*Written by the coordinator from direct full reads of both files:
`himbaechel/uarch/xilinx/xdc.cc` (270 lines) and
`nextpnr-xilinx/xilinx/xdc.cc` (309 lines). Merges into doc 02/04.*

## Upstream himbaechel xdc.cc

- `set_property`: target must be `get_ports` (else hard error); `-dict`
  supported; `INTERNAL_VREF` and `[current_design]` ignored; duplicate
  property on a cell → nonfatal error (counted, aborts at end); a target
  matching no cell → **warning always logged** (no verbose gate); multiple
  cell targets allowed (all args after position 3).
- `create_clock`: `-period` required; `-add`/`-name`/`-waveform` ignored
  with warning; targets via `get_ports`/`get_nets` (get_nets does a
  lowercase retry); duplicate clock constraint on a net → nonfatal error.
- Anything else → warning "ignoring unsupported XDC command".
- No `set_multicycle_path`, no `[0]`-suffix retry, no virtual-clock guard
  (a create_clock with no nets → just a warning, no crash).

## Fork xdc.cc (differences from upstream)

- Robustness fixes (from fork commits e8ba5168, 3da43687, 555d326c,
  b257be4d, 813bb715):
  - `name[0]` debus: Vivado writes one-bit vector ports as `a[0]`; the JSON
    frontend collapses them to `a`; the fork strips a trailing `[0]` and
    retries (fixes #46).
  - Missing XDC targets are **silent unless `--verbose`** (board-level XDC
    practice), with a summary line at the end (fixes #99, #105 area).
  - Virtual clocks (`create_clock` with no target) → warning + skip,
    avoiding std::out_of_range crash.
  - `create_clock` no-match → loud warning that the constraint was NOT
    applied (so users do not silently believe timing was checked).
  - Trailing `;` and trailing whitespace on lines accepted.
  - `[current_project]` also ignored; non-`get_ports` set_property targets
    → warning, not error.
- New constraint support (fork commit 813bb715):
  - `set_multicycle_path <N> [-setup|-hold] -from [sel] -to [sel]`, with a
    `*`/`?` glob matcher for `-to [get_cells -hier -filter {NAME =~ ...}]`;
    setup multicycle factors are tagged on matching cells via attribute
    `NEXTPNR_MCP_SETUP` for the timing engine.
- Divergences to weigh when porting:
  - Duplicate clkconstr: upstream errors, fork silently overwrites.
  - set_property arity: upstream allows multiple targets, fork requires
    exactly 4 args (single target).
  - Unknown command: upstream warns, fork log_info.

## Interpretation

Upstream parser is stricter/noisier and lacks: multicycle paths, the
`[0]` debus retry, silent-miss behaviour, virtual-clock guard, and the
"constraint NOT applied" warning. All five are small, self-contained
porting items; the multicycle one additionally needs the timing-engine
side (fork: NEXTPNR_MCP_SETUP handling) — upstream has its own SDC/mcp
path via common/kernel (sdc.cc), which should be checked before porting
the attribute hack.
