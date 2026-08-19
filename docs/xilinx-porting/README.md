# Xilinx porting: nextpnr-xilinx → upstream himbaechel

Analysis, gap assessment, and porting plan for bringing the openXC7
`nextpnr-xilinx` feature set into upstream nextpnr's
`himbaechel/uarch/xilinx` uarch.

## Document set

| # | Document | Status | Purpose |
|---|----------|--------|---------|
| 1 | `01-himbaechel-architecture.md` | ✅ final draft | Architecture of the himbaechel framework + `uarch/xilinx`, and its current degree of Xilinx support (families, primitives, constraints, timing) |
| 2 | `02-fork-feature-inventory.md` | ✅ final draft | Complete feature inventory of `nextpnr-xilinx` (feature reference for filling blanks), incl. digest of the 114 commits in 0.9.2→0.9.3 |
| 3 | `03-algorithmic-diff.md` | ✅ final draft | Algorithmic differences between the upstream placer/router/legaliser (kept as-is) and the fork's, incl. must-port legality checks and porting risks |
| 4 | `04-gap-assessment.md` | ✅ final draft | Feature-by-feature gap list: what is missing in himbaechel vs the fork |
| 5 | `05-porting-plan.md` | ✅ final draft | Concrete, ordered porting plan with validation steps |

## Context

- Upstream: this repo at tag `nextpnr-0.11.1`; Xilinx uarch under
  `himbaechel/uarch/xilinx`; shared algorithms under `common/place`
  and `common/route`.
- Fork (feature reference, used only to fill upstream blanks):
  `/devel/HDL/kintex-reveng/nextpnr-xilinx` (branch `main`), based on
  ~2022-era upstream, with a much more complete xc7 implementation in
  `xilinx/`.
- Premise: **prefer upstream solutions** — upstream nextpnr/himbaechel
  is the successor architecture; the fork's features are ported only to
  fill blanks (missing legality, packers, FASM fixes, validation), and
  its legality knowledge is carried over to avoid reintroducing
  placement/bitstream bugs.

## Policy

- Read-only analysis until documents are reviewed; no code changes,
  no commits, no pushes without explicit user approval.
