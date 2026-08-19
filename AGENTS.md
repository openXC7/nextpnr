# AGENTS.md — Porting nextpnr-xilinx features to upstream nextpnr (himbaechel)

## Goal

Port the `nextpnr-xilinx` features that are missing in
`./himbaechel/uarch/xilinx` into this repository (upstream nextpnr) —
**xc7 (7-series) only**.

`nextpnr-xilinx` is the openXC7 fork located at
`/devel/HDL/kintex-reveng/nextpnr-xilinx` (branch `main`). Its xc7
architecture implementation lives in that repository's `xilinx/`
directory and is substantially ahead of the upstream himbaechel xilinx
uarch in terms of xc7 feature coverage: supported primitives, device
support, legality/bugfix experience, and bitstream correctness.

## Governing philosophy

1. **Upstream first.** Upstream nextpnr — and its himbaechel/xilinx
   uarch — is the *successor* of `nextpnr-xilinx` and carries years of
   architecture/algorithmic development (the RelPtr chipdb model,
   StrictLegaliser, the control-set API, parallel_refine, timing_opt,
   placer_static, the router2 superset). Prefer the upstream solution
   wherever one exists.
2. **The fork fills blanks only.** `nextpnr-xilinx` material is
   imported only where upstream has a *blank*: missing legality
   checks, missing packers, missing FASM correctness fixes, missing
   validation. Never replace existing upstream code with the fork's
   version; anything upstream already has (e.g. `--report`, the
   himbaechel framework, control-set checks) is explicitly *not*
   ported.
3. **xc7 only.** UltraScale/UltraScale+ porting (the fork's xcup
   RapidWright flow, E2/E3/E4 packers, xcup device databases, DCP
   export) is **excluded**. UltraScale-related observations in the
   documents are recorded for completeness only.

The analysis documents in `docs/xilinx-porting/` (README + docs 01–05)
encode this philosophy; the illustrated architecture guide is
`docs/himbaechel-doc/himbaechel.tex` (→ `himbaechel.pdf`).

## Required work

The analysis phase is **complete** — the deliverables live in
`docs/xilinx-porting/`:

- `01-himbaechel-architecture.md` — himbaechel/xilinx architecture and
  current degree of support (xc7-only uarch; families, primitives,
  constraints, timing, limitations).
- `02-fork-feature-inventory.md` — complete `nextpnr-xilinx` xc7
  feature reference (incl. the 0.9.2→0.9.3 commit digest).
- `03-algorithmic-diff.md` — placer/router/legaliser comparison,
  must-port legality checks, obsolete-workaround list, porting risks.
- `04-gap-assessment.md` — feature-by-feature gap list (upstream blanks).
- `05-porting-plan.md` — ordered work packages WP0–WP9 with validation
  steps; xc7-only, UltraScale excluded.

Status: the plan (WP0–WP9 + WP7b PCIE) is **implemented and validated** —
`05-porting-plan.md` tracks the per-package results.  Deferred on purpose:
the virtex7 GT-clock bodge (not ported) and UltraScale (excluded).  Keep
the documents up to date as further features land.

## Constraints

- Never commit, push, or force-push without explicit user approval.
- Analysis, assessment, and plan are documents first: present them for
  review before any code changes.
- Keep the upstream repository usable: do not break non-xilinx arches.
