# VC707 multiple BUFGs (xc7vx485tffg1761-2)

`vc707-johnson` binds exactly one `BUFGCTRL`.  This design binds three, from
one `IBUFDS` into three `BUFG`s driving three independent counters, so the
phantom-BUFGCTRL guard is exercised with several real sites in one tile
alongside the unused ones.

All three land in the same tile:

    CLK_BUFG_TOP_R_X192Y209.BUFGCTRL.BUFGCTRL_X0Y0
    CLK_BUFG_TOP_R_X192Y209.BUFGCTRL.BUFGCTRL_X0Y8
    CLK_BUFG_TOP_R_X192Y209.BUFGCTRL.BUFGCTRL_X0Y9

Note the site names are tile-local (`rel_site_loc()`), while
`bufgctrl_bound_slots` stores the device-global `site_y`.  That mismatch is
what `bufgctrl_tile_guard()` in `fasm.cc` documents, and this design is where
its consequences are visible on a Virtex-7 part: 36 lines in that one tile
survive the guard, and the FASM assembles.

It does not yet reach a `CLK_BUFG_BOT_R` tile -- the placer put all three in
the top half.  A design that forces a BOT placement would be a useful
addition; `blinky-qmtech` is the only project in either gate whose BUFG lands
there, and it is kintex7.

Goldened on the frames hash, which is drift detection only.  Not
hardware-verified.
