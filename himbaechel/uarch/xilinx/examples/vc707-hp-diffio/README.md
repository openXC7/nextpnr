# VC707 HP-bank differential I/O (xc7vx485tffg1761-2)

`vc707-johnson` covers the HP differential *receiver* only -- an input-only
LVDS clock.  This design adds the two shapes it never reaches:

* **`OBUFDS`, `IOSTANDARD LVDS`** on bank 38 pair `C19/B19`
  (`RIOB18_X312Y309`) -- a differential driver on a high-performance bank.
* **`IOBUFDS`, `IOSTANDARD DIFF_SSTL15`, `SLEW FAST`** on `A16/A15`
  (`RIOB18_X312Y307`) -- a pad that is both driven and received.  This is the
  shape that produced `FasmInconsistentBits` on `litex-ddr-qmtech-kintex7`
  before the receiver's slew line was guarded on `!is_output`: HP-bank slew is
  a single shared bit whose two FASM names differ only in polarity, so
  `SLEW.SLOW` from the receiver and `SLEW.FAST` from the driver cannot
  coexist.  `SLEW FAST` in the XDC is load-bearing -- at the default `SLOW`
  both halves agree and the file assembles either way.

The bidirectional pad comes out clean.  `RIOB18_X312Y307` carries
`SSTL15.SLEW.FAST` on both halves from the driver and no
`LVCMOS12_LVCMOS15_LVCMOS18.SLEW.SLOW` from the receiver -- that pairing is
the `FasmInconsistentBits` -- alongside `IBUFDS_BANK_GLUE`, both `IN_DIFF`
groups, `DIFF.ZIBUF_LOW_PWR` and `OUT_DIFF`.

## What it found

The first run of this design did not assemble:

    FasmLookupError: Segment DB RIOB18, key RIOB18.IOB_Y1.LVDS.DRIVE.I_FIXED
    not found from line 'RIOB18_X312Y309.IOB_Y1.LVDS.DRIVE.I_FIXED'

`LVDS.*` is a master-half feature group -- the database defines
`LVDS.DRIVE.I_FIXED` and `LVDS.OUT` on `IOB_Y0` only, on `LIOB18` and
`RIOB18` alike and on kintex7's `RIOB18` too -- and the HP branch of the
DRIVE block wrote it for whichever half it was emitting, with no
`yLoc == 0` guard.  The IOB33 branch below it has carried that guard for
`LVDS_25` / `TMDS_33` all along.

Comparing against a reference bitstream for the same pins showed the driver
was three features out, not one: the master half also wants `LVDS.OUT`, the
slave half is marked `…SSTL12_SSTL135_SSTL15.IN_ONLY`, and the tile-level
`OUT_DIFF` is not part of an LVDS output at all -- that belongs to the
pseudo-differential `DIFF_SSTL*` outputs, whose S half is really driven
through the inverter.  All four are fixed; this tile now matches the
reference line for line.

Not hardware-verified.
