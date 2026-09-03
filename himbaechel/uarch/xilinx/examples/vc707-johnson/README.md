# VC707 Johnson counter (xc7vx485tffg1761-2)

A Virtex-7 regression design for the himbaechel/xilinx uarch.  Small on
purpose: a 28-bit PRBS driving an 8-bit Johnson counter, ~1000 FASM lines.
Its value is not the logic, it is the I/O and clocking it forces:

| feature | where | exercised by |
| --- | --- | --- |
| HP-bank differential receiver | `RIOB18_X312Y287`, bank 38 | `IBUFDS` on `sysclk_p/n` (E19/E18), `IOSTANDARD LVDS`, `DIFF_TERM` |
| BUFG in a `CLK_BUFG_TOP_R` tile | BUFGCTRL row 21 | `BUFG` on the 200 MHz clock |
| HP-bank single-ended input | `LIOB18_X81Y128` | `IBUF` on `rst` (AV40), `LVCMOS18` |
| HP-bank outputs | `LIOB18_X81Y{133,139,143,145}`, `LIOB18_SING_X81Y51` | 8x `OBUF` on the LEDs, `LVCMOS18` |

None of that is reachable from `demos.yml`: the openXC7 demo projects are
artix7, spartan7 and kintex7, and only the kintex7 one has HP banks at all.

## Building it

The gate (`.github/workflows/virtex7.yml`) runs no yosys -- `top.json` is the
committed synthesis output, so the golden cannot drift with a runner's yosys
package.  To rebuild the netlist from source:

    yosys -p 'synth_xilinx -flatten -abc9 -nobram -arch xc7 -top top; \
              write_json top.full.json' top.v counter25_core.v
    ../../../../../.github/scripts/strip-netlist-blackboxes.py \
        top.full.json top.json

`synth_xilinx` emits the entire Xilinx cell library as blackbox declarations,
433 modules and ~10 MB, whatever the design uses.  The strip step keeps only
the modules the netlist instantiates -- 11 here, 51 kB -- and nextpnr produces
byte-identical FASM from either, so the committed netlist is the small one.

Then place and route.  **Do not pass `--seed`**: the goldens were produced
with the default RNG state, and `--seed` with the default *value* is not the
same thing -- `DeterministicRNG()` sets `rngstate` directly while `rngseed()`
also advances the generator five steps.

    nextpnr-himbaechel --device xc7vx485tffg1761-2 \
        -o xdc=top.xdc --json top.json -o fasm=johnson.fasm --router router2

FASM to bitstream needs a virtex7-capable prjxray (`e1302fc6`); the revision
`demos.yml` pins fails on this part with `KeyError: 'HCLK_IOI3_X82Y130'`.

## Known gap against the legacy flow

The same design, part and pin constraints built through the legacy
`nextpnr-xilinx` flow (68aeeb3, prjxray-generated chipdb) produce an IOB
feature set that this uarch does not yet match.  Because both flows are
pin-constrained to identical sites, the sets are directly comparable.

The **input** side matches: `RIOB18_X312Y287` (the LVDS clock) and
`LIOB18_X81Y128` (the reset) carry the same features in both, including
`IBUFDS_BANK_GLUE`, `DIFF.ZIBUF_LOW_PWR`, `IBUF_HP_BANK_GLUE` and the
partner-half `PULLTYPE.PULLDOWN`.

The **output** side does not.  Per LED half the legacy flow writes 8 features
and this uarch writes 3, and the difference is a strict subset -- 36 features
the legacy flow emits and this one omits, nothing the other way round:

    OBUF_HP_BANK_GLUE
    LVCMOS18.DRIVE.I12_I8
    LVCMOS12_LVCMOS15_LVCMOS18_LVCMOS25_LVCMOS33_LVTTL_SSTL135_SSTL15.SLEW.SLOW
    LVCMOS12_LVCMOS15_LVCMOS18_SSTL135_SSTL15.STEPDOWN

`LIOB18_X81Y133.IOB_Y1` is missing entirely -- the legacy flow configures the
unused partner half of an HP output pair, this one leaves it blank.

That gap is recorded, not asserted to be correct, in
`.github/references/vc707-johnson-iob-delta.txt`, and
`.github/scripts/check-vc707-iob-parity.sh` fails if it changes in either
direction.  Closing it is the point; the check exists so that closing it is
visible and deliberate rather than a side effect.

The frames golden, by contrast, only records what this uarch currently
produces.  It is a drift detector.  It says nothing about whether the
bitstream is right -- and this design has not been verified on hardware
through the himbaechel flow.
