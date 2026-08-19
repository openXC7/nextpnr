# 03 — Algorithmic diff: placer / router / legaliser

> **Status: final draft for review.** Read-only comparison of the placement /
> routing / legalisation ALGORITHMS.
>
> - **UPSTREAM (port target)** = `/devel/HDL/kintex-reveng/nextpnr` @ tag `nextpnr-0.11.1`
>   (`62e659e`). Shared algorithms in `common/place/` and `common/route/`; the xilinx
>   consumer is the **himbaechel** uarch `himbaechel/uarch/xilinx/`.
> - **FORK (feature reference for filling blanks)** = `/devel/HDL/kintex-reveng/nextpnr-xilinx` @ `68aeeb39`
>   (branch `main`, tag `0.9.3`). Fork algorithms are modified copies of an **old
>   (0.7-era) upstream**, in `common/{placer1,placer_heap,router1,router2}.*`, with the
>   xc7-specific legalisation in `xilinx/arch_place.cc`.
>
> Line numbers cite the path given in the heading. "himbaechel" = upstream
> `himbaechel/uarch/xilinx/`. "fork" = `nextpnr-xilinx/`.
>
> **The decision being supported**: upstream nextpnr is the successor architecture
> (years of algorithmic development), so the upstream placer/router/legaliser are
> **kept**; the fork's *legality data + missing passes* are ported into them **only to
> fill blanks**. Every algorithmic divergence is catalogued so porting neither
> reintroduces bugs nor silently drops the fork's placement-quality work where it
> still matters.

---

## 0. Summary of the structural mismatch (read this first)

The two codebases sit on different placement *frameworks*, so "diff the algorithm"
must account for an API/architecture gap before any heuristic gap:

| Axis | Fork (0.7-era base) | Upstream 0.11 (himbaechel) |
|---|---|---|
| Cell grouping | `constr_parent/constr_children/constr_x/y/z` on `CellInfo`, walked directly by the placer | `ClusterId` + `getClusterRootCell/getClusterPlacement/getClusterOffset/isClusterStrict` (himbaechel `arch.h:744-756`) |
| Bel matching | `cell->type == getBelType(bel)` (string id) | `BelBucketId` (`getBelBucketForCellType/getBelBucketForBel/isValidBelForCellType`, `arch.h:731-739`) |
| FF control sets | handled in `xcu/xc7_logic_tile_valid` half-tile check only | dedicated control-set API in `PlacerHeapCfg` (`ff_bel_bucket`, `ff_control_set_groups`, `ctrl_set_max_radius`, `get_cell_control_set`) + `alloc_control_sets()` |
| Placer family | `placer_heap` (HeAP) + `placer1` (SA). No static, no parallel refine | `placer_heap`, `placer1`, `placer_static` (ePlace), `parallel_refine`, `detail_place_core`, `timing_opt` |
| Post-placement | `fixupPlacement()` (large, includes legality repair) | `fixup_placement()` (smaller: LUT pin merge + PS7 tie) |
| Detail/refine | `placer1_refine` (budget-based SA refine) | `parallel_refine` (partition parallel SA) *or* `placer1_refine` |
| Timing optimisation | inside placer1 (`budgetBased`, `slack_redist_iter`) | standalone `timing_opt.cc` pass |

Consequence: the fork's placer hacks manipulate `constr_*` and `cell->type`; porting
them to himbaechel means re-expressing them over `ClusterId`/`BelBucketId` (the
himbaechel packer already populates both, see `himbaechel/uarch/xilinx/pack.cc`).

---

## 1. Upstream (himbaechel / 0.11.1) algorithm summaries

### 1.1 `common/place/placer_heap.cc` — HeAPPlacer (analytical)

Steps (`place()` at `common/place/placer_heap.cc:175-418`):

1. `place_constraints()` (`:463`) — bind every `BEL`-attr cell with `STRENGTH_USER`,
   reject invalid bel-type / already-bound / invalid-location (`:481-498`).
2. `build_fast_bels()` (`:507`) — build a `fast_bels[type][x][y]` bel grid +
   `nearest_row/col_with_bel` + region bounding boxes.
3. `alloc_control_sets()` — pre-allocate control-set slots (uses
   `cfg.ff_control_set_groups` / `cfg.ctrl_set_max_radius`).
4. `seed_placement()` — random legal-ish seed.
5. 4 initial analytic iterations: `build_solve_direction(false/-1)`,
   `build_solve_direction(true/-1)` run as two threads (`:188-205`).
6. Main loop `while (stalled < 5 && solved_hpwl <= legal_hpwl*0.8)` (`:249`): for each
   **bel-bucket run** (`heap_runs`, built at `:218-242`; `placeAllAtOnce` collapses to
   one "all" run):
   - `setup_solve_cells(&run)` → `build_solve_direction` (x and y, parallel when
     ≥500 cells) → **Eigen ConjugateGradient** solve of the bound-to-bound linear
     system (`build_equations` `:883-975`, `EquationSystem::solve` `:101-130`).
   - `update_all_chains()` (chain root + children positions).
   - **CutSpreader** spreading per `cfg.cellGroups` (`:280-288`, class `:1479-2152`).
   - `legalise_placement_strict()` (`StrictLegaliser` `:1067-1477`) — a **bipartite-
     matching / assignment** legaliser, not the fork's greedy walk.
   - `tmg.run()` timing pass if `cfg.timing_driven` (`:305`).
   - keep best solution snapshot.
7. Restore best solution (`:330-346`), verify all cells bound, run a
   **post-placement `isBelLocationValid` sweep** (`:368-381`).
8. Refine: `parallel_refine(ctx, …)` if `cfg.parallelRefine`, else
   `placer1_refine(ctx, Placer1Cfg(ctx))` (`:401-412`).

`PlacerHeapCfg` (`common/place/placer_heap.h:36-91`) adds over the fork: `parallelRefine`,
`chainRipup`, `cell_placement_timeout`, `get_cell_legalisation_weight`, `disableCtrlSet`,
and the whole control-set API. Config ctor `:2161-2189`.

### 1.2 `common/place/placer1.cc` — SAPlacer (VPR-style simulated annealing)

- `place()` at `:1245`; `placer1_refine` at `:1267` (re-runs SA in refine mode).
- Cost = `lambda·timing + (1−lambda)·wirelen + constraintWeight·constraint +
  netShareWeight·netShare` with hard-coded `lambda = 0.5`, `crit_exp = 8`.
- Cooling: range-limiter (`diameter*(0.56+Raccept)`), temperature ×0.5/0.9/0.95/0.8,
  15 inner sweeps/iteration.
- Legality via `isValidBelForCellType` at move sites; cluster/chain moves via
  `getClusterPlacement` (the 0.11 ClusterId API).
- `Placer1Cfg` (`placer1.h:27-36`): `constraintWeight=10`, `netShareWeight=0`,
  `minBelsForGridPick=64`, `startTemp=1`, `timingFanoutThresh=INT_MAX`, `timing_driven`,
  `hpwl_scale_*`. (No `budgetBased`, no `slack_redist_iter` — those are fork-only.)

### 1.3 `common/place/placer_static.cc` — StaticPlacer (ePlace electrostatic)

- Global placement by electrostatic analogy: **DCT/IDCT FFT** density solve,
  **Nesterov-accelerated** gradient descent, WA-wirelength model, spacer/"dark-node"
  area balancing (`StaticPlacer` `:242`; `placer_static()` `:1712`).
- Legalisation: two-phase greedy (DSP/BRAM first, then logic) using `cell_groups` +
  `bel_area/cell_area` StaticRect metrics.
- Always finishes with `placer1_refine`.
- `PlacerStaticCfg` ctor `:1718-1724` reads almost nothing; the arch fills it in.

### 1.4 Supporting pieces (upstream-only, absent in fork)

- `detail_place_core.cc/.h` — thread-safe swap/cost evaluation core; used by
  `parallel_refine`.
- `parallel_refine.cc/.h` — partition-based parallel SA refinement.
- `timing_opt.cc/.h` — BFS-on-critical-path delay optimisation (30 iters); invoked by
  arches, not automatically by the placer.
- `place_common.cc/.h` — relative-constraint legalisation, HPWL/TNS metrics.
- `fast_bels.h`, `static_util.h` — fast bel lookup, FFT helpers.

### 1.5 `common/route/router1.cc` — Router1 (PathFinder/A*)

- Per-arc A* with `delay + congestionPenalty + togo − reuseBonus` heap.
- Ripup-based congestion: `wireScores/netScores` scale `wireRipupPenalty/netRipupPenalty`;
  `find_slack_thresh` rips up 5% worst slack (`common/route/router1.cc`).
- `Router1Cfg` ctor `:1147`; `router1()` `:1161`; `checkRoutedDesign()` `:1305`;
  `getActualRouteDelay()` is a **stub** at `:1488`.

### 1.6 `common/route/router2.cc` — Router2 (CRoute-style bidirectional A*)

- Bidirectional meet-in-the-middle A*, historical + present congestion, per-net
  bounding box (expanded every 3 failures), resource-group value tracking
  (`getResourceKeyForPip/getResourceValueForPip`).
- **Wire reservation**: `reserved_net` field, `find_all_reserved_wires()` +
  `reserve_driver_wires_for_arc`/`reserve_sink_wires_for_arc` (`:597-700`).
- Timing via shared `TimingAnalyser` (clock-skew enabled).
- Multithreaded 4-quadrant partition; finishes with a `router1()` legality pass.
- `Router2Cfg` (`router2.h`): `backwards_max_iter`, `global_backwards_max_iter`,
  `bb_margin_{x,y}`, `ipin_cost_adder`, `bias_cost_factor`, `init_curr_cong_weight`,
  `hist_cong_weight`, `curr_cong_mult`, `estimate_weight`, `perf_profile`,
  **`heatmap`**, **`get_base_cost`** (the last two are 0.11 additions absent in the fork).

### 1.7 What the himbaechel xilinx uarch configures

`himbaechel/arch.cc`:
- `place()` (`:266-289`): `prePlace()` → `placer_heap` (default `"heap"`, `:410`) or
  `placer_static` or `placer1` (`"sa"`) → `postPlace()`. No arch-level detail/timing
  call — those live inside the placer functions.
- `route()` (`:291-313`): `preRoute()` → `router2` (uarch default, `xilinx.h:147`) →
  `postRoute()`.

`himbaechel/uarch/xilinx/xilinx.cc`:
- `configurePlacerHeap` (`:321-349`): `hpwl_scale_x=2, hpwl_scale_y=1, beta=0.5,
  placeAllAtOnce=true`; `get_cell_legalisation_weight` returns 100 for memory LUTs
  else 1; `ff_bel_bucket=SLICE_FFX`; `ff_control_set_groups` = two half-slices ×
  (FF,FF2); `ctrl_set_max_radius = {18,15,12,9,6,3}`; `get_cell_control_set` returns
  `tags->ff.control_set` (indexed by `index_control_sets()` `:617-635`).
- `configurePlacerStatic` (`:351-438`): `glbBufTypes` (PSEUDO_GND/VCC, BUFGCTRL,
  BUFG_BUFG); `timing_c=500, timing_mx=25, timing_my=50`; `cell_groups` COMB/FF/RAM/DSP
  with StaticRect areas; `get_cell_area_override` (sliding LUT area by input count).
- `prePlace()` (`:308-313`) = `assign_cell_tags()` + `index_control_sets()`.
- `postPlace()` (`:315-319`) = `fixup_placement()` + `assignArchInfo()`.
- `preRoute()` (`:440-460`) = tags (if pre-placed) + `find_source_sink_locs()` +
  `route_clocks()`.
- `postRoute()` (`:462-470`) = `fixup_routing()` + FASM write.

`himbaechel/uarch/xilinx/xilinx_place.cc` (the current legality surface):
- `xc7_logic_tile_valid` (`:43-353`) — see §4 for what it *lacks* vs the fork.
- `isBelLocationValid` (`:355-387`) — logic + BRAM onehot only.
- `fixup_placement` (`:389-510`) — LUT5/LUT6 pin re-merge, PS7 tie.
- `fixup_routing` (`:512-638`) — LUT permutation pip → X_ORIG_PORT, OSERDESE3.
- Timing: `estimateDelay/predictDelay` (`xilinx.cc:735-780`) carry an explicit
  `// TODO: improve sophistication here based on old nextpnr-xilinx code`.

---

## 2. Fork algorithm summaries + per-algorithm diffs

### 2.1 `common/placer_heap.cc` (fork) vs upstream `placer_heap`

Fork class `HeAPPlacer` (`common/placer_heap.cc:140-1371`), `placer_heap()` at `:1976`
(`#ifdef`-guarded duplicate at `:2003` is the non-templated/legacy shim — see
`:1974-2013`).

Fork `PlacerHeapCfg` (`common/placer_heap.h:34-54`) is a *subset* of upstream:
`alpha, beta, criticalityExponent, timingWeight, timing_driven, solverTolerance,
placeAllAtOnce, netShareWeight, hpwl_scale_x/y, spread_scale_x/y, ioBufTypes,
cellGroups`. It **lacks** upstream's `parallelRefine`, `chainRipup`,
`cell_placement_timeout`, `get_cell_legalisation_weight`, `disableCtrlSet`, and the
entire control-set API.

Fork-specific legalisation (`legalise_placement_strict`, `:879-1180`):

| Fork mechanism | `common/placer_heap.cc` | Upstream 0.11 equivalent |
|---|---|---|
| Greedy **largest-macro-first** (priority queue keyed by `chain_size`) | `:896-899` | same greedy-walk family refactored (`StrictLegaliser`, `placer_heap.cc:1067-1477`) — upstream is *not* a different algorithm, just a cleaned-up one |
| **Ripup radius doubling** (`ripup_radius` starts 2, doubles when `total_iters > solve_cells.size()`) | `:900-964` | no direct equivalent (upstream walk has a different iteration structure) |
| **Budgeted radius growth** (radius grows 1 per `10*(radius+1)` iters; capped by region bounds) | `:985-1024` | n/a |
| **Per-cell timeout fail-fast** (`attempts > max(10000, 3*cells) + 10*max_x*max_y` → `log_error`) | `:981-983` | upstream has `cell_placement_timeout` (unused by himbaechel) but no equivalent wall-clock guard |
| **Deterministic full-scan fallback for sparse sites** (single-cell types like DCIRESET: after 5000 attempts with no candidate, linear scan of the bel grid, free bels only) | `:1025-1071` | n/a (upstream bucket/control-set allocation avoids the random-miss failure) |
| **Eviction as last resort** (`scarce_type` = type population ≤4096; only evict when no free valid home was seen; `bestFreeBel`/`bestBel` split) | `:923-932, 946-955, 1087-1107` | no equivalent in the upstream walk — **worth porting as a termination/quality guarantee** (corrigendum) |
| `require_validity` → `ctx->isBelLocationValid(sz)` on every candidate | `:1122, 1057` | upstream also checks validity (fedc910d equivalent is inherent) |
| **Carry-chain-aware placement** (`chain_root`, `chain_size`, `update_chain`, `update_all_chains`; `:359-372, 663-709`) | fork | upstream has `update_all_chains` + `chainRipup` flag (himbaechel leaves `chainRipup` default) |
| **Relocation of pre-bound leaf chain members** (`bind_children` binds `constr_children` of a BEL-pinned parent, imposing the parent's slice half on absolute z) | `:419-459` | upstream `place_constraints` handles constraints; cluster children bound via ClusterId |
| Ends with `placer1_refine` | `:320` | `parallel_refine` *or* `placer1_refine` (`:401-412`) |

Diff verdict (corrigendum from the analyst's final report): upstream's
`StrictLegaliser` is the **same greedy random-walk family** as the fork's,
refactored (bel buckets, control-set pre-allocation, cleaner iteration
structure) — not a different, more global algorithm. The fork's
**fail-fast, deterministic fallback, and eviction-as-last-resort** were
written to fix *livelock/endless-loop* bugs in that walk family; because
the family is retained upstream, these robustness guarantees are
**candidates to port as-is** (their intent: never spin forever, never
evict when a free home exists, never miss a singleton site).

### 2.2 `common/placer1.cc` (fork) vs upstream `placer1`

Fork `SAPlacer` (`common/placer1.cc:62-1371`), `placer1()` `:1387`, `placer1_refine()`
`:1407`, `place(bool refine=false)` `:151`.

Fork `Placer1Cfg` (`common/placer1.h:27-38`) **adds** `budgetBased` (default false)
and `slack_redist_iter` over upstream. These are the fork's timing/slack features that
upstream moved into `timing_opt.cc`:

- `budgetBased` — SA driven by per-port timing budgets (`assign_budget`) rather than
  criticality-weighted wirelen.
- `slack_redist_iter` — number of slack-redistribution iterations inside the SA loop.

Otherwise the fork placer1 is the same VPR-style SA (lambda=0.5, crit_exp=8, temp=10,
diameter=35, `legalise_dia=4`, `:1356-1369`) but operates on `constr_*` directly
(`:208-243, 517-530, 693-724`) instead of the ClusterId API. Fork also keeps the
`getClusterPlacement`-free path: cluster geometry is walked from `constr_children`
(`:208-222`).

Diff verdict: the fork's `budgetBased` + `slack_redist_iter` are the *predecessors* of
upstream `timing_opt.cc`/`assign_budget`. After rebase to 0.11 these are redundant with
the standalone timing machinery — do not port them as placer modes; keep the timing
data (`getBudgetOverride`, `assign_budget`) as the fork's route() already does.

### 2.3 `common/router1.cc` (fork) vs upstream `router1`

Fork `Router1` (`common/router1.cc:94-1004`), `router1()` `:1028`,
`checkRoutedDesign()` `:1146`, `getActualRouteDelay()` stub `:1332`.

Fork `Router1Cfg` (`common/router1.h`) **adds** three fields absent upstream:
- `arcMaxVisitCnt` — per-arc A* node budget (default `INT_MAX`); prevents an unroutable
  sink from exploring the whole chip (effective hang).
- `skipFailedArcs` — when an arc exhausts the budget it is left unrouted (for an
  external router) instead of aborting. `NEXTPNR_SKIP_FAILED_ARCS` env.
- `constNoRipup` — `route_const_arc()` uses only free wires (no ripup) for GND/VCC, so
  the const net never tears up a placed signal (`NEXTPNR_GND_NO_RIPUP`).

These three are fork-only workarounds for the old router1's unbounded search / const-net
ripup; upstream router1 has no such knobs (and the fork hybrid flow uses router1 for
constants *after* router2 for signals).

### 2.4 `common/router2.cc` (fork) vs upstream `router2`

Fork `Router2` (`common/router2.cc:46-1594`), `Router2Cfg` ctor `:1595`.

- Fork `Router2Cfg` is upstream-minus-`heatmap`-minus-`get_base_cost` (0.11 added
  configurable base cost + congestion heatmap the fork predates).
- **Wire reservation is present in BOTH** — and (corrigendum) upstream 0.11 is the
  superset: driver-wire + sink-wire reservation plus GroupId resource reservation
  (`reserve_driver_wires_for_arc`/`reserve_sink_wires_for_arc`,
  `getResourceKeyForPip`; `router2.cc:597-700`), while the fork has
  `reserved_net` (`:97`), `reserved_locked` (`:100`, LOCKED bindings `:236`) and a
  `RESERVED_CONTESTED` sentinel (`:79-94`) with `reserve_wires_for_arc` (`:461-525`)
  running a capped fixpoint that *marks contested* wires instead of `log_error`-ing
  (`:499-500`). The fork's genuine add is the **conflict-resolution policy +
  capped fixpoint** (for the frozen hard-macro flow), not reservation itself.
- The fork's `reserved_locked`/`RESERVED_CONTESTED` machinery exists to support the
  **frozen hard-macro fixed-routes** flow (LOCKED wires must stay with their net;
  `applyFixedRoutes` `arch.cc:1134`, `--fixed-routes`). This is a fork-only routing
  feature (hybrid Vivado-place/nextpnr-route).
- Arch config differs: fork `bb_margin_{x,y}=4`, `backwards_max_iter=200`,
  `perf_profile=true` (`arch.cc:2141-2145`); himbaechel uses stock `Router2Cfg`
  defaults (no arch overrides).

### 2.5 `common/place_common.cc` / timing (fork)

- Fork `place_common.cc` (625 lines) provides the old 0.7-era `PlaceCommon` helpers
  (timer, wirelen, `legalise_relative_constraints`, `bind_unplaced_children` used by
  placer1) — superseded by upstream `place_common.cc` + `place_common.h`.
- Fork `common/timing.cc` (`:707 assign_budget`, `:720 assign_budget(ctx,quiet)`) is
  the budget engine the fork's `route()` calls (`arch.cc:2099`) and its `--report`
  (`:833`) reads. Upstream has the same `assign_budget` in `common/place/timing_opt.cc`
  + `timing_opt.h`; the JSON `--report` is mainline in 0.11.

---

## 3. `xilinx/arch_place.cc` pass inventory (classification a/b/c)

Classification key:
- **(a) pure xc7 legality data** — portable as data/checks; needed to avoid porting bugs.
- **(b) algorithmic placement work** — may be obsolete or superior to upstream; note which
  upstream 0.11 feature already covers it.
- **(c) workaround for old-upstream limitations** — obsolete after rebase to 0.11 (keep
  only if the hybrid imported-placement flow is retained).

| Lines | Function / pass | What it does | Class |
|---|---|---|---|
| 29-34 | `port_or_nullptr` | port lookup helper | — |
| 42-52 | `dbg_validity_runtime` + `DBG()/DBG_RT()` | `NEXTPNR_DUMP_INVALID_TILE`-gated invalid-arm tracing | (a) diagnostics |
| 54-323 | `xcu_logic_tile_valid` | UltraScale+ CARRY8/F9MUX/DI+I logic-tile validity | (a) [xcup-only] |
| 325-367 | `xc7_logic_tile_valid` **frozen-tile fast path** | all-`STRENGTH_USER` tile ⇒ trust Vivado placement | (c) hybrid-import |
| 368-384 | **SLICEM-only guard** (unconditional scan) | memory/SRL LUT illegal in SLICEL, even when dirty-cache clean | (a) |
| 385-423 | **5LUT A6/O6 port-count gate** (unconditional) | count *connected* input ports; reject >5 inputs / A6 / 2 outputs | (a) |
| 424-506 | **Per-position OUTMUX (site-exit) budget** | ≤1 claimant among 5LUT O5 / carry O / carry CO / 5FF Q | (a) |
| 507-512 | `tile_is_memory/small_memory/wclk` prologue | distributed-RAM write-port state | (a) |
| 515-624 | **LUT6/LUT5 coexistence** (8-tile dirty-gated loop) | input_count==6 / output_count==2 forbids 5LUT; shared-input count; `srl_pair`/`imported_carry_feedthrough`/`imported_slot` exemptions | (a) + (c) exemptions |
| 626-656 | **5LUT constraint in loop** | ≤5 inputs/1 output/no A6 | (a) |
| 658-787 | **X-input overuse + FF feed shapes** | F7/F8 sel, carry X, FF1/FF2 D agree on one X; `direct_feed`/`lut_routethru_feed` exemptions; **cross-position carry→FF flat reject** (`:731-745`), 5FF-by-carry reject (`:766-774`) | (a) |
| 789-804 | **FF1-uses-X + 5FF co-pack rejection** | main FF via X bypass + a 5FF ⇒ reject (unless both BEL-pinned) | (a) |
| 806-819 | memory address-MSB collision | top_lut WA7/WA8 vs X | (a) |
| 821-904 | **Output-mux contention** O5 / carry O / carry CO / F7F8 / 5FF | claim counting; `NEXTPNR_ALLOW_CO_5FF_CONTENTION` opt-out | (a) |
| 913-1005 | **Half-tile control-set guards** | clk/sr/ce + clkinv/srinv/latch/ffsync match; wclk == FF clk in bottom half | (a) |
| 1009-1030 | `dumpTileStatus` | debug tile dump + force re-validate | (a) diagnostics |
| 1032-1077 | `isBelLocationValid` | dispatch xc7/xcu; BRAM onehot; `usp_bel_hard_unavail` | (a) |
| 1079-1191 | `isValidBelForCell` | per-candidate gate: SLICEM guard (`:1093-1099`), 5LUT A6/O6 gate (`:1120-1148`), `NEXTPNR_EXCLUDE_STAMPED_BBOX` (`:1154-1189`) | (a) + (c) bbox |
| 1193-1335 | `fixupPlacement` **validity-repair pass** | relocate stranded cluster roots (LUT6-on-5LUT, mem/SRL-in-SLICEL) to nearest valid bel, carry trees excluded | (c) |
| 1336-1479 | `fixupPlacement` **LUT pin re-merge** | re-merge 5LUT/6LUT inputs onto ≤6 shared A-pins, O6→O5 rename, A6 tie; STRENGTH_USER skip | (a) |
| 1480-1555 | `fixupPlacement` **PSS/PS7/BITSLICE const tying** | tie unused hard-IP inputs to VCC/GND | (a) |
| 1556-1666 | `fixupPlacement` **reserved_wires bouncewire** (xcu only) | reserve X/I bouncewires per slice | (a) [xcup] |
| 1669-1781 | `fixupRouting` **LUT perm pip → X_ORIG_PORT** | physical↔logical remap for RapidWright/Vivado | (a) |
| 1782-1845 | `fixupRouting` **PAD net BFS re-route** | re-route inout PAD nets post-router | (b)/(c) |
| 1847-1857 | `fixupRouting` **OSERDESE3 T_BYPASS** | set OSERDES_T_BYPASS when T_OUT unused | (a) |

Cross-reference to himbaechel (`xilinx_place.cc`): himbaechel's `xc7_logic_tile_valid`
(`:43-353`) contains the *dirty-gated* versions of the LUT/FF/X-input/control-set checks
and the basic `mux_output_used` contention, but is **missing** the fork's unconditional
SLICEM guard, unconditional 5LUT port-count gate, per-position OUTMUX budget,
cross-position/5FF carry→FF rejection, and FF1-uses-X+5FF co-pack rejection (see §4).

---

## 4. Legality checks that MUST be ported (fork → himbaechel)

"Fork file:line" = implementation in the fork. "Himbaechel" = presence/absence in
`himbaechel/uarch/xilinx/`. These are the checks whose absence causes **silent bitstream/
legalisation corruption** or **unroutable sites**, not merely QoR loss.

| # | Rule | Fork file:line | Himbaechel status |
|---|---|---|---|
| 1 | **Carry chain continuation**: only CO3 rides COUT→CIN; CO0..2 can only reach fabric via OUTMUX; a CO with non-CIN users claims the OUTMUX | `arch_place.cc:477-491, 867-887`; `pack_carry_xc7.cc:1087-1094` | partial: `xilinx_place.cc:160-169` handles carry `x_sigs` but has **no** CO/OUTMUX budget |
| 2 | **Per-position site-exit (OUTMUX) budget**: ≤1 claimant (5LUT O5, carry O, carry CO, 5FF Q) per letter position | `arch_place.cc:424-506` | **MISSING** (only a coarse `mux_output_used` per-eight at `xilinx_place.cc:223-261`) |
| 3 | **In-slice cross-position carry→FF pairing rejection** (pos A's FFMUX cannot see O3; 5FF cannot be fed by carry at all) | `arch_place.cc:731-745, 766-774` | **MISSING** |
| 4 | **Main-FF-via-X-bypass + 5FF co-pack rejection** (xFFMUX emission bug corrupts D) | `arch_place.cc:789-804` | **MISSING** |
| 5 | **5LUT bel A6/O6 constraints**, enforced at candidate-selection AND tile-validate time, counting *connected* input ports (ABC LUTs carry logical I0..I5) | `arch_place.cc:385-423, 626-656, 1120-1148` | partial: `xilinx_place.cc:122-127, 87-106` checks input_count/output_count but **not** the connected-port/A6 count and **not** in `isValidBelForCell` (no such hook) |
| 6 | **SLICEM-only for memory/SRL LUTs, unconditional** (not dirty-cache-gated) | `arch_place.cc:368-384, 1083-1099` | partial: present in dirty-gated eight check (`xilinx_place.cc:64-67, 110-113`) — the fork found the cache could strand a mem/SRL LUT in SLICEL |
| 7 | **SRL cascade placement** (SRLC32E Q31→MC31; ≤4 SRLs per slice D,C,B,A; longer chains via fabric Q) | `pack.cc:700-910` (`constrain_srl_cascades`) | **MISSING** — himbaechel `pack.cc:497` is literally `// FIXME: Q31 support` |
| 8 | **Control-set half-tile guards** (clk/sr/ce + clkinv/srinv/latch/ffsync match; wclk==FF clk bottom half; latch only on FF1) | `arch_place.cc:913-1005` | present `xilinx_place.cc:270-351` (equivalent) |
| 9 | **Config primitive preplacement** (BSCAN JTAG_CHAIN; ICAP/STARTUP/FRAME_ECC/USR_ACCESS/DNA_PORT/EFUSE_USR/DCIRESET single-site) | `pack_io_xc7.cc:1232-1271` | **MISSING** |
| 10 | **IDELAYCTRL/IDELAYE2 grouping** (replicate per ioctrl site, RDY-AND tree; warn-if-no-delays) | `pack_io_xc7.cc:1129-1230` | present `pack_io.cc:929-991` (equivalent, incl. warn path) |
| 11 | **Global buffer preplacement** (BUFG/BUFGCTRL short-route + fabric-driven BUFG; PLL/MMCM BUFGs bottom region) | `pack_clocking_xc7.cc:230`; `pack.h:164-165` (`try_preplace/preplace_unique`) | present `pack_clocking.cc:71-217` (equivalent) |
| 12 | **RIOB18/IOB pairing + SLEW groups + HP-bank glue** (FASM) | `fasm.cc:1088-1235` | present `fasm.cc:776-968` (equivalent) |
| 13 | **BRAM onehot** (RAMFIFO36/RAM36/FIFO36; 18-bit vs 36-bit) | `arch_place.cc:1047-1070` | present `xilinx_place.cc:361-385` (equivalent) |

**Implementation status**: items **1–6** PORTED into `xilinx_place.cc`
(WP1); item **7** (SRL cascades) PORTED as cluster-based D-C-B-A grouping
in `pack.cc` (WP3.4); item **9** (config preplacement) PORTED as
`pack_cfg()` (WP6); items 8, 10–13 were already present upstream. The
hybrid-flow exemptions (frozen-tile trust, imported-slot BEL exemptions)
were deliberately not ported.

Bottom line for porting: items **2, 3, 4, 7, 9** are the highest-risk *missing* checks;
items **1, 5, 6** exist in weaker/cache-gated form and must be strengthened.

---

## 5. Obsolete-workaround list (fork hacks the 0.11 rebase retires)

1. **`fixupPlacement` validity-repair pass** (`arch_place.cc:1213-1335`) — relocates
   cells stranded by the *old* placer's legalisation gaps (LUT6-on-5LUT, mem/SRL in
   SLICEL). Upstream's strict legaliser + `isValidBelForCell` should prevent the stranding;
   keep only as a belt-and-braces net, and keep the "skip carry trees" guard.
2. **`legalise_placement_strict` greedy walk + fail-fast/fallback/eviction**
   (`common/placer_heap.cc:879-1180`) — upstream's `StrictLegaliser` is the same
   walk family refactored (bel buckets, control-set pre-search), NOT a different
   algorithm. The fork's `ripup_radius` doubling, per-cell timeout, deterministic
   full-scan, and eviction-as-last-resort exist to stop the *greedy* walk from
   livelocking; since the family survives upstream, **port these termination
   guarantees too** (corrigendum), not just their intent.
3. **`relocate_carry_o_fabric`** (`pack_carry_xc7.cc:1052`) — carry-O fabric-fanout
   duplication/split exists because the old placer/router could not legalise carry O
   with both FF and fabric fanout. Upstream 0.11 has no equivalent; decide whether the
   upstream legaliser+router can now absorb this or whether the pass must be ported as-is
   (it changes the *netlist* pre-placement, so it is placer-independent).
4. **placer1 `budgetBased` / `slack_redist_iter`** (`common/placer1.h:32,36`) — superseded
   by `timing_opt.cc` + `assign_budget`.
5. **router1 `arcMaxVisitCnt`/`skipFailedArcs`/`constNoRipup`** (`common/router1.h`) —
   unbounded-search and const-ripup workarounds; relevant only if the hybrid flow keeps
   router1-for-constants after router2.
6. **`NEXTPNR_FRESH_REGION_MARGIN`** (`pack.cc:1463-1501`) and
   **`NEXTPNR_EXCLUDE_STAMPED_BBOX`** (`arch_place.cc:1154-1189`) — hybrid
   Vivado-place/nextpnr-route confinement hacks.
7. **Frozen-tile fast path + imported-slot/imported-carry-feedthrough exemptions**
   (`arch_place.cc:327-367, 548-623`) — only meaningful for imported Vivado placements.
8. **`applyFixedRoutes`/`writeFixedRoutes` + router2 `reserved_locked`/`RESERVED_CONTESTED`**
   (`arch.cc:1134/1726`, `router2.cc:79-100`) — hybrid fixed-routes machinery; drop if
   the frozen-macro flow is not ported, but keep the conflict-aware reservation concept.

---

## 6. Porting risks (places where fork hacks rely on old behaviour)

1. **`constr_*` vs `ClusterId`/`BelBucketId` API.** Fork placers read/write
   `constr_parent/constr_children/constr_x/y/z` and match bels by `cell->type`. Any
   fork heuristic ported into himbaechel must be re-expressed over `ClusterId` +
   `getClusterPlacement` + `getBelBucketForCellType` (himbaechel `arch.h:731-756`),
   or it will silently no-op / mis-place.
2. **Control-set model.** Himbaechel *already* wires the 0.11 control-set API
   (`configurePlacerHeap`, `xilinx.cc:335-348`) which the fork placer does not have.
   Porting the fork's half-tile control-set *checks* is fine, but do **not** port the
   fork placer's lack of control-set awareness — that would regress QoR.
3. **`placeAllAtOnce` + bucket runs vs fork's single legalisation.** The fork's greedy
   legaliser assumes one global `solve_cells` set; upstream splits by bel-bucket runs.
   The fork's `chain_size` priority ordering has no direct home in `StrictLegaliser`.
4. **Carry/sum-FF co-location.** The fork fixes several carry→FF and sum-FF
   control-set-mismatch bugs in *pack* (`pack_carry_xc7.cc`) + *validity*
   (`arch_place.cc:700-904`). If only the placer is swapped and these checks are not
   ported into `xc7_logic_tile_valid`, the upstream legaliser will happily produce the
   same illegal slices the fork rejects → router "Failed to route arc CARRY4_O3 →
   AFFMUX_OUT" or corrupt bitstream.
5. **Delay estimation.** Himbaechel `estimateDelay/predictDelay` is a crude Manhattan
   formula with a `TODO` (`xilinx.cc:735-780`). The fork's site-`inter_x/inter_y`,
   `sink_locs/source_locs`, wire-intent-aware version (`arch.cc:650-762`) is what makes
   fork router2 timing decent; porting the fork's *placement* without its *delay model*
   will give worse timing-driven results.
6. **`fixupPlacement` ordering.** The fork runs `fixupPlacement()` *after* the placer and
   its pin re-merge assumes a legal placement; himbaechel runs `fixup_placement()` in
   `postPlace()` too, but its version (`xilinx_place.cc:389`) lacks the fork's
   STRENGTH_USER-vs-STRONG distinction for the "skip imported LUT slots" decision — the
   fork's comment (`arch_place.cc:1354-1362`) documents a real bug (STRONG is what the
   legaliser binds chains with). Port that distinction.
7. **router2 config.** Himbaechel does not set `bb_margin/backwards_max_iter/perf_profile`
   for xilinx; fork does (`arch.cc:2141-2145`). If router2 QoR matters, port these
   overrides (himbaechel `route()` currently uses stock `Router2Cfg`).
8. **`routeVcc`/`routeClock` ordering.** Fork routes clocks *before* Vcc and locks macro
   routing before both (`arch.cc:2097-2125`); himbaechel only does `route_clocks()` in
   `preRoute()` and has no `routeVcc` fill or fixed-routes. The "Vcc floods the clock
   backbone" failure the fork documents (`arch.cc:2101-2108`) can recur if clock/Vcc
   handling is not ported together with placement.

---

## 7. Algorithm diff table (one line per placer/router)

| Algorithm | Upstream 0.11 (himbaechel) | Fork (0.7 base) | Fork extras to consider porting | Fork things to drop |
|---|---|---|---|---|
| placer_heap | HeAP + Eigen CG + CutSpreader + `StrictLegaliser` (same greedy-walk family, refactored: bel buckets + control-set pre-search) + control-set API + `parallel_refine`/`placer1_refine` | HeAP + greedy largest-macro-first legalisation + chain_root/update_chain | carry-chain walk (`update_all_chains`), fail-fast/fallback/eviction guarantees | greedy legaliser body, `constr_*` walk, missing control-set API |
| placer1 | SA + ClusterId API | SA + `budgetBased` + `slack_redist_iter` + `constr_*` walk | nothing (timing covered by timing_opt) | `budgetBased`/`slack_redist_iter` |
| placer_static | ePlace electrostatic + cell_groups | **absent** | n/a | n/a |
| detail/refine | `parallel_refine` + `detail_place_core` | `placer1_refine` only | n/a | — |
| timing | `timing_opt.cc` standalone | inside placer1 + `assign_budget` in route() | keep `assign_budget` in route() | placer1 slack loop |
| router1 | PathFinder/A* + ripup + slack-thresh | same + `arcMaxVisitCnt`/`skipFailedArcs`/`constNoRipup` | the 3 knobs (only if hybrid const-route kept) | — |
| router2 | CRoute bidirectional A* + reservation + `heatmap`/`get_base_cost` | same minus heatmap/get_base_cost, plus `reserved_locked`/`RESERVED_CONTESTED` | conflict-aware reservation (if fixed-routes kept) | — |
| legaliser (arch) | `xc7_logic_tile_valid` (dirty-gated, no OUTMUX budget) | `xc7_logic_tile_valid` (frozen fast-path + OUTMUX budget + carry→FF rejects + unconditional guards) | **the fork validity engine** (see §4) | frozen/imported exemptions (unless hybrid kept) |
| fixup | `fixup_placement` (pin merge + PS7) | `fixupPlacement` (validity repair + pin merge + hard-IP tie + xcu reserved wires) | validity repair (as safety net), pin-merge strength fix | carry-O relocation (keep in pack instead) |

---

*End of draft 03. Companion documents: `02-fork-feature-inventory-draft.md` (feature
inventory + commit digest), `01-himbaechel-architecture.md` (upstream architecture).*
