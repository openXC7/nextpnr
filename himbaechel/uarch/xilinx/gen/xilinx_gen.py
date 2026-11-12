from os import path
import sys
import argparse
import xilinx_device
import filters
import struct
import itertools
import parse_sdf
sys.path.append(path.join(path.dirname(__file__), "../../.."))
from himbaechel_dbgen.chip import *

def lookup_port_type(t):
    if t == "INPUT": return PinType.INPUT
    elif t == "OUTPUT": return PinType.OUTPUT
    elif t == "BIDIR": return PinType.INOUT
    else: assert False

def gen_bel_name(site, bel_name):
    prim_st = site.primary.site_type()
    if prim_st in ("IOB33M", "IOB33S", "IOB33", "IOB18M", "IOB18S", "IOB18"):
        return f"{site.site_type()}.{bel_name}"
    else:
        return f"{bel_name}"

@dataclass
class PipClass(Enum):
    TILE_ROUTING = 0
    SITE_ENTRANCE = 1
    SITE_EXIT = 2
    SITE_INTERNAL = 3
    LUT_PERMUTATION = 4
    LUT_ROUTETHRU = 5
    CONST_DRIVER = 6

@dataclass
class PipExtraData(BBAStruct):
    site_key: int = -1
    bel_name: IdString = field(default_factory=IdString)
    pip_config: int = 0

    def serialise_lists(self, context: str, bba: BBAWriter):
        pass
    def serialise(self, context: str, bba: BBAWriter):
        bba.u32(self.site_key)
        bba.u32(self.bel_name.index)
        bba.u32(self.pip_config)

@dataclass
class BelExtraData(BBAStruct):
    name_in_site: IdString

    def serialise_lists(self, context: str, bba: BBAWriter):
        pass
    def serialise(self, context: str, bba: BBAWriter):
        bba.u32(self.name_in_site.index)

@dataclass
class SiteInst(BBAStruct):
    name_prefix: IdString
    site_x: int
    site_y: int
    rel_x: int
    rel_y: int
    int_x: int
    int_y: int
    variants: list[IdString] = field(default_factory=list)

    def serialise_lists(self, context: str, bba: BBAWriter):
        bba.label(f"{context}_variants")
        for i, variant in enumerate(self.variants):
            bba.u32(variant.index)
    def serialise(self, context: str, bba: BBAWriter):
        bba.u32(self.name_prefix.index)
        bba.u16(self.site_x)
        bba.u16(self.site_y)
        bba.u16(self.rel_x)
        bba.u16(self.rel_y)
        bba.u16(self.int_x)
        bba.u16(self.int_y)
        bba.slice(f"{context}_variants", len(self.variants))

@dataclass
class TileExtraData(BBAStruct):
    name_prefix: IdString
    tile_x: int
    tile_y: int
    sites: list[SiteInst] = field(default_factory=list)

    def serialise_lists(self, context: str, bba: BBAWriter):
        for i, site in enumerate(self.sites):
            site.serialise_lists(f"{context}_si{i}", bba)
        bba.label(f"{context}_sites")
        for i, site in enumerate(self.sites):
            site.serialise(f"{context}_si{i}", bba)
    def serialise(self, context: str, bba: BBAWriter):
        bba.u32(self.name_prefix.index)
        bba.u16(self.tile_x)
        bba.u16(self.tile_y)
        bba.slice(f"{context}_sites", len(self.sites))

def timing_pip_classs(pip: xilinx_device.PIP):
    return f"{'buf' if pip.is_buffered() else 'sw'}_d{pip.max_delay()}_r{pip.resistance()}_c{pip.capacitance()}"

seen_pip_timings = set()
seen_node_timings = set()

# --- measured timing calibration (--timing-cal) ------------------------------
# A JSON written by a fit of nextpnr's own pip delays to silicon (synthgui runs/2026-11-09-npcal/calfit.py):
#   classes: {class: {rise: f, fall: f}}  scale factor on nextpnr's delay for pips into that wire class
#   per_use: {class: {rise: ps, fall: ps}} a fixed delay for pips into that class (FAN/BYP, which prjxray gives about 0 ps)
#   lut_pin: {A1..A6: {rise: ps, fall: ps}} cost of a LUT stage entered on that physical pin, beyond its IMUX pip
# Scaling a class's pip intrinsic delay, pip output resistance and node resistance by f scales Arch::getPipDelay's
# (int_delay + RC) for that class by f, with no change to nextpnr itself. nextpnr times one value per pip (slow_max):
# max takes the fall figure, min the rise figure. Without --timing-cal nothing below changes the chipdb.
import re as _re
TIMING_CAL = None
LUT_CAL_BASE = 10  # ps left on the LUT cell arc; the rest of a stage's cost goes on the physical pin's permutation pip
LUT_PIN_ARC = {}   # --timing-cal: physical pin j -> its LUT arc (LUT_CAL_BASE, or less on a pin that costs less than that)
CAL_WIRE_CLASSES = {"sgl", "dbl", "quad", "hex", "lvb", "lv", "lh"}   # "classes" keys of a --timing-cal file
CAL_PER_USE_CLASSES = {"fan", "byp"}                                   # "per_use" keys
def cal_wire_class(dst):
    """Same classes as runs/2026-11-09-npcal/npmodel.py wire_class: a pip's class is its destination wire's."""
    for pat, c in [(r'(EE|WW|NN|SS|NE|NW|SE|SW|EL|ER|WL|WR|NL|NR|SL|SR)1BEG(_[NS])?\d', 'sgl'),
                   (r'(EE|WW|NN|SS|NE|NW|SE|SW)2BEG\d', 'dbl'),
                   (r'(EE|WW)4BEG\d', 'quad'),
                   (r'(NN|SS|NE|NW|SE|SW)6BEG\d', 'hex'),
                   (r'LVB(_L)?\d+', 'lvb'), (r'LV(_L)?\d+', 'lv'), (r'LH\d+', 'lh'),
                   (r'(FAN_ALT|FAN_BOUNCE|GFAN)\d+', 'fan'), (r'(BYP_ALT|BYP_BOUNCE)\d+', 'byp')]:
        if _re.fullmatch(pat, dst): return c
    return None
def cal_scale(c):
    """(rise, fall) scale factors for class c, or None"""
    if TIMING_CAL is None or c is None or c not in TIMING_CAL.get("classes", {}): return None
    v = TIMING_CAL["classes"][c]; return (float(v["rise"]), float(v["fall"]))
def cal_per_use(c):
    if TIMING_CAL is None or c is None or c not in TIMING_CAL.get("per_use", {}): return None
    v = TIMING_CAL["per_use"][c]; return (float(v["rise"]), float(v["fall"]))
def cal_tv(lo, hi):
    """A TimingValue from a measured (rise, fall) pair. TimingValue has no edges: its fields are min and max, and
    nextpnr keeps one delay per pip and arc (setup analysis reads the max). So the pair goes in as min and max in
    whichever order they fall, and the slower edge is the one timed. Negative values clamp to 0 (no negative delay)."""
    lo, hi = int(round(max(lo, 0))), int(round(max(hi, 0)))
    return TimingValue(min(lo, hi), max(lo, hi))
def cal_node_class(wires):
    """class of a node for scaling its resistance: the class of the first of its wires that has one"""
    if TIMING_CAL is None: return None
    for w in wires:
        c = cal_wire_class(w.name())
        if cal_scale(c): return c
    return None

# --- prjxray bitstream-representation lookup -------------------------------
# A tile-routing pip is only usable if the bitstream can express it, i.e. its
# fasm feature "<TILETYPE>.<dst_wire>.<src_wire>" appears in either
# segbits_<tiletype>.db (has config bits) or ppips_<tiletype>.db (a pseudo-pip
# needing no bits).  A pip in neither is a trap: nextpnr will happily route
# through it, the fasm line is then dropped by fasm2frames, and the resulting
# bitstream silently lacks the connection.  Mark those so the router can avoid
# them.  See PIP_CFG_NO_BITS in extra_data.h.
PIP_CFG_ROUTETHRU = 0x1
PIP_CFG_NO_BITS = 0x80000000

xraydb_root_for_bits = None
_bits_cache = {}
bits_stats = {"known": 0, "nobits": 0, "notiledb": 0}
nobits_by_tiletype = {}

def dealias_tile_type(tile_type):
    """The tile type whose bit database actually expresses this tile.

    A top/bottom-of-column ``*_SING`` I/O tile carries no ``segbits_*_sing.db``
    of its own in any family: prjxray expresses it through the *base* tile type
    (``RIOI3_SING`` -> ``RIOI3``, ``LIOB33_SING`` -> ``LIOB33``, ...), resolving
    the tilegrid ``alias`` at assembly time (grid.py, TileSegbitsAlias).  The
    db revisions we build chipdbs against do not all carry that alias metadata
    in tilegrid.json, and the ``_SING`` suffix is the stable convention, so we
    strip it here.  Only a ``ppips_*_sing.db`` (the "0"-half pseudo-pips) ever
    exists for a SING tile, and it is merged in on top of the base type."""
    if tile_type.endswith("_SING"):
        return tile_type[:-len("_SING")]
    return tile_type

def dealias_wire_name(wire):
    """The name the base tile's bit database knows this SING wire by.

    The wires that exist only in a ``*_SING`` tile carry an extra ``_SING``
    infix over the base tile's name for the same wire:
    ``IOI_SING_LEAF_GCLK1`` is ``IOI_LEAF_GCLK1`` in ``segbits_rioi3.db``.
    The FASM writer strips exactly that infix before it emits the pip
    (fasm.cc, ``src_name.erase(spos, 5)`` for ``RIOI3_SING`` / ``LIOI3_SING``
    / ``RIOI_SING`` tiles), so the feature key the bitstream will actually
    carry is the stripped one and the classifier has to look that one up."""
    return wire.replace("_SING_", "_", 1)

def tile_type_features(tile_type):
    """Set of fasm feature keys prjxray can express for this tile type.
    Returns None if the tile type has no bit database at all."""
    if tile_type in _bits_cache:
        return _bits_cache[tile_type]
    feats = set()
    found_any = False
    # Read this tile's own db files, and -- for an aliased SING tile -- the
    # base type's as well.  Feature rows are prefixed with the file's own tile
    # type (``RIOI3.`` vs ``RIOI3_SING.``); pip_has_bits keys on the actual
    # tile type, so base-type keys are re-prefixed to match.  Without this,
    # every bitful pip of a SING tile (all in the base segbits db) is falsely
    # marked NO_BITS and the router cannot bring a clock into an upper-SING
    # OLOGIC -- the "Failed to route ... OLOGIC ... CLKINV_OUT" abort.
    base = dealias_tile_type(tile_type)
    lookup_types = [tile_type] if base == tile_type else [tile_type, base]
    for lt in lookup_types:
        for kind in ("segbits", "ppips"):
            fn = path.join(xraydb_root_for_bits, f"{kind}_{lt.lower()}.db")
            if not path.exists(fn):
                continue
            found_any = True
            with open(fn) as f:
                for ln in f:
                    ln = ln.strip()
                    if not ln:
                        continue
                    key = ln.split()[0]
                    key_names_base_tile = lt != tile_type and key.startswith(lt + ".")
                    if key_names_base_tile:
                        key = tile_type + key[len(lt):]
                    feats.add(key)
    result = feats if found_any else None
    _bits_cache[tile_type] = result
    return result

def pip_has_bits(tile_type, dst_wire, src_wire):
    feats = tile_type_features(tile_type)
    if feats is None:
        # No database for this tile type at all -- we cannot tell, so do not
        # accuse the pip.  Counted separately and reported.
        bits_stats["notiledb"] += 1
        return True
    keys = [f"{tile_type}.{dst_wire}.{src_wire}"]
    tile_is_aliased = dealias_tile_type(tile_type) != tile_type
    if tile_is_aliased:
        # The base type's rows, merged in by tile_type_features(), name the
        # wires without the _SING infix -- and so does the FASM writer for
        # this tile.  Without this second key every clock pip of a SING tile
        # (IOI_OLOGIC0_CLKDIV <- IOI_SING_LEAF_GCLK*, the leaf clock into the
        # OLOGIC) is judged against a key nothing ever writes, marked
        # PIP_CFG_NO_BITS, and the router cannot bring a clock into a
        # top/bottom-of-column OLOGIC -- the "Failed to route ... OLOGIC ...
        # CLKDIVINV_OUT" abort on the LiteX Arty S7 demos.
        keys.append(f"{tile_type}.{dealias_wire_name(dst_wire)}.{dealias_wire_name(src_wire)}")
    pip_has_a_feature = any(k in feats for k in keys)
    if pip_has_a_feature:
        bits_stats["known"] += 1
        return True
    bits_stats["nobits"] += 1
    nobits_by_tiletype[tile_type] = nobits_by_tiletype.get(tile_type, 0) + 1
    return False

def add_pseudo_constants(ch: Chip, tt):
    # pseudo ground bels
    tt.create_wire(f"GND", "GND", const_value="GND")
    tt.create_wire(f"VCC", "VCC", const_value="VCC")

    gnd = tt.create_bel(f"PSEUDO_GND", f"PSEUDO_GND", z=0)
    gnd.site = -1
    gnd.extra_data = BelExtraData(name_in_site=ch.strs.id("PSEUDO_GND"))
    gnd.flags |= BEL_FLAG_GLOBAL | BEL_FLAG_HIDDEN
    tt.add_bel_pin(gnd, "Y", "GND", PinType.OUTPUT)

    vcc = tt.create_bel(f"PSEUDO_VCC", f"PSEUDO_VCC", z=1)
    vcc.site = -1
    vcc.extra_data = BelExtraData(name_in_site=ch.strs.id("PSEUDO_VCC"))
    vcc.flags |= BEL_FLAG_GLOBAL | BEL_FLAG_HIDDEN
    tt.add_bel_pin(vcc, "Y", "VCC", PinType.OUTPUT)

def make_pseudo_corner(ch: Chip):
    # --window: the corner tile of the ring, with no device tile behind it
    add_pseudo_constants(ch, ch.create_tile_type("NULL_CORNER"))

def import_tiletype(ch: Chip, tile: xilinx_device.Tile):
    tile_type = tile.tile_type()
    if tile.x == 0 and tile.y == 0:
        assert tile_type == "NULL"
        tile_type = "NULL_CORNER"
    tt = ch.create_tile_type(tile_type)
    # import tile wires
    for wire in tile.wires():
        nw = tt.create_wire(wire.name(), wire.intent())
        nw.flags = -1 # not a site wire
        if wire.is_gnd():
            nw.const_value = ch.strs.id("GND")
        if wire.is_vcc():
            nw.const_value = ch.strs.id("VCC")
    if tile_type == "NULL_CORNER":
        add_pseudo_constants(ch, tt)
    def add_pip(src_wire, dst_wire, pip_class=PipClass.TILE_ROUTING, timing="",
        site_key=-1, bel_name="", pip_config=0):
        np = tt.create_pip(src_wire, dst_wire, timing)
        np.flags = pip_class.value
        np.extra_data = PipExtraData(site_key=site_key, bel_name=ch.strs.id(bel_name),
            pip_config=pip_config)

    def lookup_site_wire(sw):
        canon_name = f"{sw.site.rel_name()}.{sw.name()}"
        if not tt.has_wire(canon_name):
            nw = tt.create_wire(name=canon_name,
                type="INTENT_SITE_GND" if sw.name() == "GND_WIRE" else "INTENT_SITE_WIRE")
            nw.flags = sw.site.primary.index
        return canon_name

    def add_site_io_pip(pin):
        pn = pin.name()
        s = pin.site
        site_key = (s.index << 8)
        if pin.tile_wire() is None:
            return None
        # TODO: timing class
        if pin.dir() in ("OUTPUT", "BIDIR"):
            if s.primary.site_type() == "IPAD" and pn == "O":
                return None
            # if s.rel_xy()[0] == 0 and s.primary.site_type() == "SLICEL" and pin.site_wire().name() == "A":
            #     # Add ground pip
            #     # self.add_pseudo_pip(self.row_gnd_wire_index, self.sitewire_to_tilewire(pin.site_wire()),
            #     #    pip_type=NextpnrPipType.CONST_DRIVER)
            #     pass
            add_pip(lookup_site_wire(pin.site_wire()), pin.tile_wire().name(),
                pip_class=PipClass.SITE_EXIT, timing="SITE_NULL")
        else:
            if s.site_type() in ("SLICEL", "SLICEM"):
                # Add permuation pseudo-pips for LUT inputs
                swn = pin.site_wire().name()
                if len(swn) == 2 and swn[0] in "ABCDEFGH" and swn[1] in "123456":
                    i = int(swn[1])
                    for j in range(1, 7):
                        if (i == 6) != (j == 6):
                            continue # don't allow permutation of input 6
                        pip_config = ("ABCDEFGH".index(swn[0]) << 8) | ((j - 1) << 4) | (i - 1)
                        if s.rel_xy()[0] == 1:
                            pip_config |= (4 << 8)
                        # physical pin j: with --timing-cal its measured stage cost (LUTPIN<j>), else the stub
                        add_pip(s.pin(f"{swn[0]}{j}").tile_wire().name(), lookup_site_wire(pin.site_wire()),
                            pip_class=PipClass.LUT_PERMUTATION,
                            pip_config=pip_config,
                            site_key=(s.index << 8),
                            timing=(f"LUTPIN{j}" if TIMING_CAL is not None and "lut_pin" in TIMING_CAL else "SITE_NULL")
                        )
                    # Add LUT route-through pseudo-pip: the input straight to the
                    # 6LUT's O6 site wire, from which the site's own pips reach
                    # the output pin (a fabric buffer) and the flip-flop mux (how
                    # a fabric signal reaches the main FF of a position whose
                    # bypass pin the 5FF already uses: Vivado's route-through).
                    # Ending at the O6 wire rather than at the output pin keeps
                    # the two uses on one resource, so one LUT never carries two
                    # route-throughs, whose INITs could not coexist.  Bit 0 of
                    # the config marks this form for the uarch.
                    rt_config = ("ABCDEFGH".index(swn[0]) << 8)
                    if s.rel_xy()[0] == 1:
                        rt_config |= (4 << 8)
                    rt_config |= ((i - 1) << 1)
                    o6 = None
                    for b in s.bels():
                        if b.name() == f"{swn[0]}6LUT":
                            for bp in b.pins():
                                if bp.name == "O6":
                                    o6 = bp.site_wire()
                    assert o6 is not None, f"no O6 on {swn[0]}6LUT"
                    add_pip(pin.tile_wire().name(), lookup_site_wire(o6),
                            pip_class=PipClass.LUT_ROUTETHRU,
                            pip_config=rt_config | 1,
                            site_key=(s.index << 8),
                            # with --timing-cal a route-through on physical pin i costs a stage on that pin (LUTRT<i>)
                            timing=(f"LUTRT{i}" if TIMING_CAL is not None and "lut_pin" in TIMING_CAL else "SITE_NULL")
                        )

                    return

            add_pip(pin.tile_wire().name(), lookup_site_wire(pin.site_wire()),
                pip_class=PipClass.SITE_ENTRANCE, timing="SITE_NULL")

    # TODO: ground/vcc
    tile_wire_count = len(tt.wires)
    # prjxray bel z is only unique within a site; the per-tile map below
    # re-allocates colliding z values so the archcheck location roundtrip
    # (bel <-> Loc) stays bijective within each tile type.
    seen_bel_z = {}
    # BRAM tiles carry semantic z slots (0..11, see extra_data.h BEL_*) that
    # the packer/fasm writer index directly; the inversion bels (RDCLKINV &
    # co) must keep clear of them, so they start at z 12 in these tiles.
    is_bram_tile = tile_type in ("BRAM_L", "BRAM_R")
    bram_nonsem_count = 0
    for site in tile.sites():
        seen_pins = set()
        seen_pips = set()
        for variant_idx, variant in enumerate(site.available_variants()):
            if variant in ("FIFO36E1", ): #unsupported atm
                continue
            sv = site.variant(variant)
            variant_key = (site.index << 8) | (variant_idx & 0xFF)
            # Import site bels
            for bel in sv.bels():
                default_z = (12 + bram_nonsem_count) if is_bram_tile else len(tt.bels)
                z = filters.get_bel_z_override(bel, default_z)
                # Overriden z of -1 means we skip this bel
                if z == -1:
                    continue
                is_non_semantic_bram_bel = is_bram_tile and z == default_z
                if is_non_semantic_bram_bel:
                    bram_nonsem_count += 1
                bel_name = gen_bel_name(sv, bel.name())
                # Site variants can import overlapping bels (e.g. the
                # ILOGICE3 site's ILOGICE3/ILOGICE2/ISERDESE2 variants all
                # carry a CE1USED bel); give non-primary variants a distinct
                # name suffix so bel names stay unique within a tile
                # (archcheck `bel != bel2` would otherwise fail).
                full_bel_name = f"{site.rel_name()}.{bel_name}"
                if variant_idx > 0:
                    full_bel_name += f"~{variant}"
                # Site variants can import overlapping bels sharing a z (e.g.
                # the ILOGICE3 site's ILOGICE3/ILOGICE2/ISERDESE2 variants all
                # carry a CE1USED bel): re-allocate a fresh tile-wide unique z
                # for the later one.
                if z in seen_bel_z:
                    z = len(tt.bels)
                    while z in seen_bel_z:
                        z += 1
                seen_bel_z[z] = full_bel_name
                nb = tt.create_bel(name=full_bel_name, type=filters.get_bel_type_override(bel.bel_type()), z=z)
                nb.site = variant_key
                nb.extra_data = BelExtraData(name_in_site=ch.strs.id(bel_name))
                if bel.bel_class() == "RBEL": nb.flags |= 2
                if filters.is_global_bel(bel):
                    nb.flags |= BEL_FLAG_GLOBAL
                # TODO: extra data (site etc)
                for pin in bel.pins():
                    tt.add_bel_pin(nb, pin.name, lookup_site_wire(pin.site_wire()), lookup_port_type(pin.dir()))
            # Import site pins
            for pin in sv.pins():
                pin_key = (pin.name(), pin.site_wire().name())
                if pin_key not in seen_pins:
                    add_site_io_pip(pin)
                    seen_pins.add(pin_key)
            # Import site pips
            for site_pip in sv.pips():
                if "LUT" in site_pip.bel().bel_type():
                    continue # ignore site LUT route-throughs
                bel_name = site_pip.bel().name()
                bel_pin = site_pip.bel_input()
                if (bel_name == "ADI1MUX" and bel_pin == "BDI1") or \
                    (bel_name == "BDI1MUX" and bel_pin == "DI") or \
                    (bel_name == "CDI1MUX" and bel_pin == "DI") or \
                    (bel_name.startswith("TFBUSED")) or \
                    (bel_name == "OMUX") or \
                    (bel_name == "IDELMUXE3"):

                    continue
                # Site variants can re-import the same site pip (e.g. every
                # ILOGICE3 variant carries IDATAININV_OUT -> IDATAIN); import
                # each (src, dst) once so pip names stay unique within a tile
                # (archcheck `pip != pip2` would otherwise fail).
                pip_key = (site_pip.src_wire().name(), site_pip.dst_wire().name())
                if pip_key in seen_pips:
                    continue
                seen_pips.add(pip_key)
                add_pip(lookup_site_wire(site_pip.src_wire()),
                    lookup_site_wire(site_pip.dst_wire()),
                    pip_class=PipClass.SITE_INTERNAL,
                    site_key=variant_key,
                    bel_name=bel_name,
                    pip_config=ch.strs.id(bel_pin).index,
                    timing="SITE_NULL"
                )
    # Import tile pips
    for pip in tile.pips():
        if not filters.include_pip(tile.tile_type(), pip):
            continue
        tcls = timing_pip_classs(pip)
        ccls = cal_wire_class(pip.dst_wire().name()) if TIMING_CAL is not None else None
        sc, pu = cal_scale(ccls), cal_per_use(ccls)
        if sc or pu: tcls = f"{tcls}_cal{ccls}"
        if tcls not in seen_pip_timings:
            delay = TimingValue(int(pip.min_delay()*1000), int(pip.max_delay()*1000)) # ps
            out_res = TimingValue(int(pip.resistance())) # mohm
            if sc:
                delay = cal_tv(pip.min_delay()*1000*sc[0], pip.max_delay()*1000*sc[1])
                out_res = TimingValue(int(pip.resistance()*sc[1]))
            elif pu:
                delay = cal_tv(pu[0], pu[1])
            ch.timing.set_pip_class(f"DEFAULT", tcls,
                delay=delay,
                in_cap=TimingValue(int(pip.capacitance()*1000)), # fF
                out_res=out_res,
                is_buffered=pip.is_buffered())
            seen_pip_timings.add(tcls)
        rt = PIP_CFG_ROUTETHRU if pip.is_route_thru() else 0
        # Route-thru pips are not tile config bits at all (they borrow a site
        # bel), so the segbits/ppips test does not apply to them.
        fwd_nb = 0 if rt else (
            0 if pip_has_bits(tile_type, pip.dst_wire().name(), pip.src_wire().name()) else PIP_CFG_NO_BITS)
        add_pip(pip.src_wire().name(), pip.dst_wire().name(), pip_class=PipClass.TILE_ROUTING, timing=tcls,
            pip_config=rt | fwd_nb)
        if pip.is_bidi():
            rev_nb = 0 if rt else (
                0 if pip_has_bits(tile_type, pip.src_wire().name(), pip.dst_wire().name()) else PIP_CFG_NO_BITS)
            add_pip(pip.dst_wire().name(), pip.src_wire().name(), pip_class=PipClass.TILE_ROUTING, timing=tcls,
                pip_config=rt | rev_nb)

def import_sdf_timings(variant, sdfcell):
    # TODO: anything other than comb
    for entry in sdfcell.entries:
        if isinstance(entry, parse_sdf.IOPath):
            variant.add_comb_arc(entry.from_pin, entry.to_pin,
                TimingValue(int(min(entry.rising.minv, entry.falling.minv)*1000),
                    int(max(entry.rising.maxv, entry.falling.maxv)*1000)))

def import_bram_timings(timing, sdf, is_virtex7=False):
    # RAMB18 hold figures read off Vivado's own probed paths on the VC707
    # oracle (docs/extracted-timing.md), in place of the SDF's generic
    # (and much wider) device-level hold envelope. No equivalent
    # measurement exists for any other family.
    def vivado_hold_ps(port):
        if not is_virtex7: return None
        table = {
            "DIADI": 255, "DIPADIP": 255, "DIBDI": 296, "DIPBDIP": 296,
            "ADDRARDADDR": 183, "ADDRBWRADDR": 183,
            "ENARDEN": 96, "ENBWREN": 96,
            "WEA": 46, "WEBWE": 46,
        }
        # bram36's control-bus ports carry a trailing L/U (18K-half) suffix
        if port not in table and port[-1:] in ("L", "U") and port[:-1] in table:
            port = port[:-1]
        return table.get(port)

    def import_bus_sethold(cell, port, width, clock, entry):
        override = vivado_hold_ps(port)
        hold = TimingValue(override) if override is not None else TimingValue(int(entry.hold.minv*1000), int(entry.hold.maxv*1000))
        for i in range(width):
            cell.add_setup_hold(clock, f"{port}{i}", ClockEdge.RISING, TimingValue(int(entry.setup.minv*1000), int(entry.setup.maxv*1000)),
                hold)

    def import_bus_clkq(cell, port, width, clock, entry):
        for i in range(width):
            cell.add_clock_out(clock, f"{port}{i}", ClockEdge.RISING, TimingValue(int(min(entry.rising.minv, entry.falling.minv)*1000),
                int(max(entry.rising.maxv, entry.falling.maxv)*1000)))


    def import_pin_sethold(cell, port, clock, entry):
        override = vivado_hold_ps(port)
        hold = TimingValue(override) if override is not None else TimingValue(int(entry.hold.minv*1000), int(entry.hold.maxv*1000))
        cell.add_setup_hold(clock, f"{port}", ClockEdge.RISING, TimingValue(int(entry.setup.minv*1000), int(entry.setup.maxv*1000)),
                hold)

    # The data-in setup and hold depend on the port's WRITE_MODE: the SDF has
    # one cell per RAM_MODE and WRITE_MODE (NC = NO_CHANGE, RF = READ_FIRST,
    # WF = WRITE_FIRST), and READ_FIRST's setup and hold are shorter. DIADI and
    # DIPADIP take WRITE_MODE_A's entry, DIBDI and DIPBDIP WRITE_MODE_B's, so
    # each variant name ends in both modes; the uarch picks it per cell.
    def import_data_in_sethold(cell, di_width, dip_width, wsdp, mode_a, mode_b, clk_sfx=""):
        # In SDP both halves of the data in are written by the port B clock. clk_sfx: see the RAMB36 note below.
        clock_a = ("CLKBWRCLK" if wsdp else "CLKARDCLK") + clk_sfx
        ram_mode = "RAMB18SDP" if wsdp else "RAMB18TDP"
        for port, mode in (("A", mode_a), ("B", mode_b)):
            sdfcell = sdf.cells[(f"RAMBFIFO36E1RAM_MODE_{ram_mode}_U_WRITE_MODE_U_{mode}_EN_ECC_READ_FALSE_EN_ECC_WRITE_FALSE", "RAMBFIFO36E1")]
            for entry in sdfcell.entries:
                if isinstance(entry, parse_sdf.SetupHoldCheck):
                    if port == "A" and entry.pin == "DIADIU": import_bus_sethold(cell, "DIADI", di_width, clock_a, entry)
                    if port == "A" and entry.pin == "DIPADIPU": import_bus_sethold(cell, "DIPADIP", dip_width, clock_a, entry)
                    if port == "B" and entry.pin == "DIBDIU": import_bus_sethold(cell, "DIBDI", di_width, "CLKBWRCLK" + clk_sfx, entry)
                    if port == "B" and entry.pin == "DIPBDIPU": import_bus_sethold(cell, "DIPBDIP", dip_width, "CLKBWRCLK" + clk_sfx, entry)

    write_modes = ("NC", "RF", "WF")
    for wsdp, rsdp, mode_a, mode_b in itertools.product((False, True), (False, True), write_modes, write_modes):
        variant = f"{'WSDP' if wsdp else 'WTDP'}_{'RSDP' if rsdp else 'RTDP'}_{mode_a}_{mode_b}"
        bram18 = timing.add_cell_variant("DEFAULT", f"RAMB18E1_RAMB18E1_{variant}")

        for entry in sdf.cells[("RAMBFIFO36E1", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.SetupHoldCheck):
                if entry.pin == "ADDRAU": import_bus_sethold(bram18, "ADDRARDADDR", 14, "CLKARDCLK", entry)
                if entry.pin == "ADDRBU": import_bus_sethold(bram18, "ADDRBWRADDR", 14, "CLKBWRCLK", entry)
                if entry.pin == "WEAU": import_bus_sethold(bram18, "WEA", 4, "CLKARDCLK", entry)
                if entry.pin == "WEBU": import_bus_sethold(bram18, "WEBWE", 8, "CLKBWRCLK", entry)
        for entry in sdf.cells[("RAMBFIFO36E1_ISFIFO_FALSE", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.SetupHoldCheck):
                if entry.pin == "ENARDENU": import_pin_sethold(bram18, "ENARDEN", "CLKARDCLK", entry)
                if entry.pin == "ENBWRENU": import_pin_sethold(bram18, "ENBWREN", "CLKBWRCLK", entry)
                if entry.pin == "RSTRAMAU": import_pin_sethold(bram18, "RSTRAMARSTRAM", "CLKARDCLK", entry)
                if entry.pin == "RSTRAMBU": import_pin_sethold(bram18, "RSTRAMB", "CLKBWRCLK", entry)
        import_data_in_sethold(bram18, 16, 2, wsdp, mode_a, mode_b)
        for entry in sdf.cells[("RAMBFIFO36E1RAM_MODE_U_RAMB18SDP_U_DOA_REG_U_0_EN_ECC_READ_FALSE", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.IOPath):
                if entry.to_pin == "DOADOU": import_bus_clkq(bram18, "DOADO", 16, "CLKARDCLK", entry)
                if entry.to_pin == "DOPADOPU": import_bus_clkq(bram18, "DOPADOP", 2, "CLKARDCLK", entry)
        for entry in sdf.cells[("RAMBFIFO36E1RAM_MODE_U_RAMB18TDP_U_DOB_REG_U_0_EN_ECC_READ_FALSE", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.IOPath):
                if entry.to_pin == "DOBDOU": import_bus_clkq(bram18, "DOBDO", 16, "CLKARDCLK" if rsdp else "CLKBWRCLK", entry)
                if entry.to_pin == "DOPBDOPU": import_bus_clkq(bram18, "DOPBDOP", 2, "CLKARDCLK" if rsdp else "CLKBWRCLK", entry)

        bram36 = timing.add_cell_variant("DEFAULT", f"RAMB36E1_RAMB36E1_{variant}")
        # The packed RAMB36 has no CLKARDCLK or CLKBWRCLK port, only the L and U halves (CLKARDCLKL/U, CLKBWRCLKL/U),
        # which the packer connects to the same clock net. An arc that names the unsuffixed clock is dropped by the
        # timing analyser without a word, which left every RAMB36 read (clock-to-out of DO*) and its address and
        # data-in setup untimed. The full-width buses take the L clock; WE, EN and RSTRAM take their own half's.

        for u in ('L', 'U'):
            for entry in sdf.cells[("RAMBFIFO36E1", "RAMBFIFO36E1")].entries:
                if isinstance(entry, parse_sdf.SetupHoldCheck):
                    if entry.pin == "ADDRAU": import_bus_sethold(bram36, f"ADDRARDADDR{u}", 15 if u == 'L' else 14, f"CLKARDCLK{u}", entry)
                    if entry.pin == "ADDRBU": import_bus_sethold(bram36, f"ADDRBWRADDR{u}", 15 if u == 'L' else 14, f"CLKBWRCLK{u}", entry)
                    if entry.pin == "WEAU": import_bus_sethold(bram36, f"WEA{u}", 4, f"CLKARDCLK{u}", entry)
                    if entry.pin == "WEBU": import_bus_sethold(bram36, f"WEBWE{u}", 8, f"CLKBWRCLK{u}", entry)
            for entry in sdf.cells[("RAMBFIFO36E1_ISFIFO_FALSE", "RAMBFIFO36E1")].entries:
                if isinstance(entry, parse_sdf.SetupHoldCheck):
                    if entry.pin == "ENARDENU": import_pin_sethold(bram36, f"ENARDEN{u}", f"CLKARDCLK{u}", entry)
                    if entry.pin == "ENBWRENU": import_pin_sethold(bram36, f"ENBWREN{u}", f"CLKBWRCLK{u}", entry)
                    if entry.pin == "RSTRAMAU": import_pin_sethold(bram36, f"RSTRAMARSTRAM{u}", f"CLKARDCLK{u}", entry)
                    if entry.pin == "RSTRAMBU": import_pin_sethold(bram36, f"RSTRAMB{u}", f"CLKBWRCLK{u}", entry)
        import_data_in_sethold(bram36, 32, 4, wsdp, mode_a, mode_b, clk_sfx="L")
        for entry in sdf.cells[("RAMBFIFO36E1RAM_MODE_U_RAMB18SDP_U_DOA_REG_U_0_EN_ECC_READ_FALSE", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.IOPath):
                if entry.to_pin == "DOADOU": import_bus_clkq(bram36, "DOADO", 32, "CLKARDCLKL", entry)
                if entry.to_pin == "DOPADOPU": import_bus_clkq(bram36, "DOPADOP", 4, "CLKARDCLKL", entry)
        for entry in sdf.cells[("RAMBFIFO36E1RAM_MODE_U_RAMB18TDP_U_DOB_REG_U_0_EN_ECC_READ_FALSE", "RAMBFIFO36E1")].entries:
            if isinstance(entry, parse_sdf.IOPath):
                if entry.to_pin == "DOBDOU": import_bus_clkq(bram36, "DOBDO", 32, "CLKARDCLKL" if rsdp else "CLKBWRCLKL", entry)
                if entry.to_pin == "DOPBDOPU": import_bus_clkq(bram36, "DOPBDOP", 4, "CLKARDCLKL" if rsdp else "CLKBWRCLKL", entry)

def main():
    xlbase = path.join(path.dirname(path.realpath(__file__)), "..")

    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", help="Project X-Ray device database path for current family (e.g. ../prjxray-db/artix7)", type=str, required=True)
    parser.add_argument("--metadata", help="nextpnr-xilinx site metadata root", type=str, default=path.join(xlbase, "meta", "artix7"))
    parser.add_argument("--device", help="name of device to export", type=str, required=True)
    parser.add_argument("--constids", help="name of nextpnr constids file to read", type=str, default=path.join(xlbase, "constids.inc"))
    parser.add_argument("--bba", help="bba file to write", type=str, required=True)
    parser.add_argument("--window", help="x0,y0,x1,y1 (tile grid, inclusive): write a chipdb of that window of the "
                        "device only, ringed by border tiles that carry a PARTPIN_IN and a PARTPIN_OUT bel on every "
                        "node crossing the window's edge (a window build's interface to the static design around it)",
                        type=str)
    parser.add_argument("--window-map", help="with --window: JSON of the boundary nodes, their wires and bels", type=str)
    parser.add_argument("--timing-cal", help="JSON of measured timing: per wire class scale factors and per-use delays, "
                        "per physical LUT pin stage costs (see TIMING_CAL above)", type=str)
    args = parser.parse_args()
    global TIMING_CAL
    if args.timing_cal:
        import json
        with open(args.timing_cal) as f:
            TIMING_CAL = json.load(f)
        # a key the generator does not know would leave that class at its prjxray timing while the chipdb looks
        # calibrated: refuse it rather than skip it
        known = {"classes": CAL_WIRE_CLASSES, "per_use": CAL_PER_USE_CLASSES, "lut_pin": {f"A{j}" for j in range(1, 7)}}
        for section, names in known.items():
            unknown = sorted(set(TIMING_CAL.get(section, {})) - names)
            if unknown:
                sys.exit(f"--timing-cal {args.timing_cal}: unknown {section} key(s) {unknown}; known: {sorted(names)}")
        missing = sorted(known["lut_pin"] - set(TIMING_CAL.get("lut_pin", known["lut_pin"])))
        if missing:
            sys.exit(f"--timing-cal {args.timing_cal}: lut_pin lacks {missing}")
        print(f"--timing-cal {args.timing_cal}: classes {sorted(TIMING_CAL.get('classes', {}))}, "
              f"per use {sorted(TIMING_CAL.get('per_use', {}))}, lut pins {'lut_pin' in TIMING_CAL}", file=sys.stderr)

    # Init database paths
    metadata_root = args.metadata
    xraydb_root = args.xray
    if "xc7z" in args.device:
        metadata_root = metadata_root.replace("artix7", "zynq7")
        xraydb_root = xraydb_root.replace("artix7", "zynq7")
    if "xc7k" in args.device:
        metadata_root = metadata_root.replace("artix7", "kintex7")
        xraydb_root = xraydb_root.replace("artix7", "kintex7")
    if "xc7s" in args.device:
        metadata_root = metadata_root.replace("artix7", "spartan7")
        xraydb_root = xraydb_root.replace("artix7", "spartan7")
    if "xc7v" in args.device:
        metadata_root = metadata_root.replace("artix7", "virtex7")
        xraydb_root = xraydb_root.replace("artix7", "virtex7")
    is_virtex7 = "xc7v" in args.device
    # segbits_*.db / ppips_*.db live alongside the tile_type_*.json we import
    global xraydb_root_for_bits
    xraydb_root_for_bits = xraydb_root
    # Load prjxray device data
    d = xilinx_device.import_device(args.device, xraydb_root, metadata_root)
    # --window: the chip is the window plus a one-tile ring of border tiles;
    # grid (0, 0) is the pseudo-constant corner as on a whole device, and
    # every real tile keeps its own name (TileExtraData) for the fasm.
    win = None
    if args.window:
        wx0, wy0, wx1, wy1 = (int(v) for v in args.window.split(","))
        assert 0 <= wx0 <= wx1 < d.width and 0 <= wy0 <= wy1 < d.height, "--window is off the tile grid"
        win = (wx0, wy0, wx1, wy1)
        chip_w, chip_h = wx1 - wx0 + 3, wy1 - wy0 + 3
        def inside(tile): return wx0 <= tile.x <= wx1 and wy0 <= tile.y <= wy1
        def gx(x): return x - wx0 + 1
        def gy(y): return y - wy0 + 1
    else:
        chip_w, chip_h = d.width, d.height
        def inside(tile): return True
        def gx(x): return x
        def gy(y): return y
    # Init constant ids
    ch = Chip("xilinx", args.device, chip_w, chip_h)
    ch.strs.read_constids(path.join(path.dirname(__file__), args.constids))
    ch.set_speed_grades(["DEFAULT", ]) # TODO: figure out how speed grades are supposed to work in prjxray
    if win is not None:
        make_pseudo_corner(ch)
        ch.set_tile_type(0, 0, "NULL_CORNER").extra_data = TileExtraData(ch.strs.id("PRBORDER"), 0, 0)
        ch.create_tile_type("PRNULL")
    # Import tile types
    for tile in d.tiles:
        if not inside(tile):
            continue
        tile_type = tile.tile_type()
        if tile.x == 0 and tile.y == 0:
            assert tile_type == "NULL"
            # location for pseudo gnd/vcc bels
            tile_type = "NULL_CORNER"
        if tile_type not in ch.tile_type_idx:
            import_tiletype(ch, tile)
        ti = ch.set_tile_type(gx(tile.x), gy(tile.y), tile_type)
        prefix, tx, ty = tile.split_name()
        ti.extra_data = TileExtraData(ch.strs.id(prefix), tx, ty)
        for site in tile.sites():
            ti.extra_data.sites.append(SiteInst(
                name_prefix=ch.strs.id(site.prefix),
                site_x=site.grid_xy[0], site_y=site.grid_xy[1],
                rel_x=site.rel_xy()[0], rel_y=site.rel_xy()[1],
                int_x=tile.interconn_xy[0], int_y=tile.interconn_xy[1],
                variants=[ch.strs.id(v) for v in site.available_variants()]
            ))
    # Import nodes
    seen_nodes = set()
    tt_used_wires = {}
    def used_in_tile(w):
        tt = w.tile.tile_type()
        if tt not in tt_used_wires:
            tt_used_wires[tt] = w.tile.used_wire_indices()
        return w.index in tt_used_wires[tt]
    # --window: a node with a usable wire outside the window is a boundary
    # node.  It gets one more wire in a border tile and a PARTPIN_IN /
    # PARTPIN_OUT bel pair on it, so a window build can drive it (an input
    # from the static design) or consume it (an output to it).  Deferred
    # until the border tile types exist.
    boundary = []            # (node_wires, timing_class, canonical name, all wire names, border x, border y)
    border_records = {}      # (bx, by) -> list of canonical names
    print("Processing nodes...")
    for row in range(d.height):
        # TODO: GND and VCC row connectivity...
        for col in range(d.width):
            t = d.tiles_by_xy[col, row]
            for w in t.wires():
                n = w.node()
                uid = n.unique_index()
                if uid in seen_nodes:
                    continue
                seen_nodes.add(uid)
                if win is not None:
                    ins = [nw for nw in n.wires if inside(nw.tile)]
                    if not ins:
                        continue
                    outs = [nw for nw in n.wires if not inside(nw.tile) and used_in_tile(nw)]
                    wires_of_node = ins
                else:
                    outs = []
                    wires_of_node = n.wires
                if len(n.wires) > 1:
                    node_wires = []
                    node_cap = 0
                    node_res = 0
                    # Add interconnect tiles first for better delay estimates in nextpnr
                    for j in range(2):
                        for w in wires_of_node:
                            if (w.tile.tile_type() in ("INT", "INT_L", "INT_R")) != (j == 0):
                                continue
                            node_cap += int(w.capacitance() * 1000)
                            node_res += int(w.resistance())
                            # prune wires from nodes that just pass through a tile without connecting to
                            # bels/pips, because nextpnr doesn't care about these
                            if not used_in_tile(w):
                                continue
                            node_wires.append(NodeWire(gx(w.tile.x), gy(w.tile.y), w.index))
                    timing_class = f"node_c{node_cap}_c{node_res}"
                    ncls = cal_node_class(wires_of_node)
                    if ncls:  # --timing-cal: the node's resistance carries its class's (fall) scale factor
                        node_res = int(node_res * cal_scale(ncls)[1])
                        timing_class = f"{timing_class}_cal{ncls}"
                    is_boundary = bool(outs and node_wires)
                    if (is_boundary or len(node_wires) > 1) and timing_class not in seen_node_timings:
                        # only for a node that will exist: a whole-device chipdb stays as it was
                        ch.timing.set_node_class("DEFAULT", timing_class, delay=TimingValue(0), cap=TimingValue(node_cap), res=TimingValue(node_res))
                        seen_node_timings.add(timing_class)
                    if is_boundary:
                        ow = outs[0]
                        bx = min(max(gx(ow.tile.x), 0), chip_w - 1)
                        by = min(max(gy(ow.tile.y), 0), chip_h - 1)
                        if (bx, by) == (0, 0):
                            bx = 1
                        canon = f"{n.wires[0].tile.name}/{n.wires[0].name()}"
                        names = [f"{nw.tile.name}/{nw.name()}" for nw in n.wires]
                        boundary.append((node_wires, timing_class, canon, names, bx, by))
                        border_records.setdefault((bx, by), []).append(canon)
                    elif len(node_wires) > 1:
                        ch.add_node(node_wires, timing_class=timing_class)
                    del node_wires
    if win is not None:
        window_map = {"window": list(win), "grid": [chip_w, chip_h], "nodes": {}, "wires": {}}
        for (bx, by), canons in sorted(border_records.items()):
            tt = ch.create_tile_type(f"PRBORDER_{bx}_{by}")
            for k, canon in enumerate(canons):
                dotted = canon.replace("/", ".")
                bw = tt.create_wire(dotted, "PARTPIN")
                bw.flags = -1 # a tile wire, not a site wire
                for z, (kind, pin, pdir, prefix) in enumerate(((PinType.OUTPUT, "O", PinType.OUTPUT, "PIN"),
                                                               (PinType.INPUT, "I", PinType.INPUT, "POUT"))):
                    bel = tt.create_bel(f"{prefix}.{dotted}", "PARTPIN_IN" if prefix == "PIN" else "PARTPIN_OUT", z=2 * k + z)
                    bel.site = -1
                    bel.extra_data = BelExtraData(name_in_site=ch.strs.id(f"{prefix}.{dotted}"))
                    tt.add_bel_pin(bel, pin, dotted, pdir)
        for by in range(chip_h):
            for bx in range(chip_w):
                on_the_ring = bx in (0, chip_w - 1) or by in (0, chip_h - 1)
                if not on_the_ring or (bx, by) == (0, 0):
                    continue
                ti = ch.set_tile_type(bx, by, f"PRBORDER_{bx}_{by}" if (bx, by) in border_records else "PRNULL")
                ti.extra_data = TileExtraData(ch.strs.id("PRBORDER"), bx, by)
        for node_wires, timing_class, canon, names, bx, by in boundary:
            dotted = canon.replace("/", ".")
            ch.add_node(node_wires + [NodeWire(bx, by, dotted)], timing_class=timing_class)
            tile = f"X{bx}Y{by}"   # nextpnr's own tile naming is the bare grid position
            window_map["nodes"][canon] = {"in": f"{tile}/PIN.{dotted}", "out": f"{tile}/POUT.{dotted}",
                                          "root": f"{names[0]}", "wires": names}
            for nm in names:
                window_map["wires"][nm] = canon
        print(f"--window: {len(boundary)} boundary nodes on {len(border_records)} border tiles", file=sys.stderr)
        if args.window_map:
            import json
            with open(args.window_map, "w") as f:
                json.dump(window_map, f, indent=1)
    # Stub timing class
    ch.timing.set_pip_class("DEFAULT", "SITE_NULL", delay=TimingValue(20))
    if TIMING_CAL is not None and "lut_pin" in TIMING_CAL:
        # a stage on pin j costs lut_pin[Aj] beyond its IMUX pip: LUTPIN<j> + the LUT arc (LUT_CAL_BASE) + what nextpnr already
        # prices in every stage outside the fit's IMUX/wire classes (site exit stub, CLB tile pips): lut_stage_overhead, measured by
        # comparing nextpnr's SDF with the fit's model on the same routes (runs/2026-11-09-npcal/consist.py); 20 if absent
        ovh = float(TIMING_CAL.get("lut_stage_overhead", 20))
        for j in range(1, 7):
            v = TIMING_CAL["lut_pin"][f"A{j}"]
            # what this pin's stage costs beyond the overhead, per edge; the LUT arc takes up to LUT_CAL_BASE of it and the
            # permutation pip the rest, so a pin cheaper than LUT_CAL_BASE + overhead (A6, about 0-16 ps measured) is not
            # charged the full arc. Below the overhead itself nothing here can take it lower: say how much is left over.
            beyond = [float(v["rise"]) - ovh, float(v["fall"]) - ovh]
            arc = [min(max(b, 0.0), LUT_CAL_BASE) for b in beyond]
            LUT_PIN_ARC[j] = cal_tv(*arc)
            ch.timing.set_pip_class("DEFAULT", f"LUTPIN{j}", delay=cal_tv(beyond[0] - arc[0], beyond[1] - arc[1]))
            excess = max(-b for b in beyond)
            if excess > 0:
                print(f"--timing-cal: A{j} measures {v['rise']}/{v['fall']} ps, below the {ovh:g} ps stage overhead; "
                      f"its stage is modelled {excess:.0f} ps slow", file=sys.stderr)
            # a LUT route-through goes from pin j's tile wire straight to O6, skipping the permutation pip and the LUT arc,
            # and costs the same as a stage on pin j (measured: windows 1 and 2 of 2026-11-05, runs/2026-11-09-npcal/layouts.py)
            ch.timing.set_pip_class("DEFAULT", f"LUTRT{j}",
                delay=cal_tv(float(v["rise"]) - ovh, float(v["fall"]) - ovh))
    # Stub bel timings. On virtex7 the LUT comb delay and the FF D/CK->Q
    # figures are replaced by the VC707 Vivado-oracle calibration
    # (docs/extracted-timing.md): nextpnr's generic guesses of 0.30 ns
    # clk->Q and 0.20 ns FF hold measured as low by 3x and 2x respectively
    # against Vivado's own probed paths.
    lut_delay = TimingValue(45, 124) if is_virtex7 else None
    if TIMING_CAL is not None and "lut_pin" in TIMING_CAL:
        lut_delay = TimingValue(LUT_CAL_BASE, LUT_CAL_BASE)  # the pin-dependent part is on the permutation pips (and LUT_PIN_ARC)
    ff_d_setup = TimingValue(34) if is_virtex7 else TimingValue(100, 100)
    ff_d_hold = TimingValue(87) if is_virtex7 else TimingValue(200, 200)
    ff_clk_q = TimingValue(100, 303) if is_virtex7 else TimingValue(300, 350)
    def add_lut_timing(lut):
        for i in range(1, 6+1):
            arc = LUT_PIN_ARC.get(i, lut_delay)   # --timing-cal: per physical pin, else one value for all
            lut.add_comb_arc(f"A{i}", "O6", arc or TimingValue(100, 125))
            if i <= 5: lut.add_comb_arc(f"A{i}", "O5", arc or TimingValue(120, 150))
        for i in range(1, 8+1):
            lut.add_setup_hold("CLK", f"WA{i}", ClockEdge.RISING, TimingValue(100, 500), TimingValue(100, 200))
        lut.add_setup_hold("CLK", "WE", ClockEdge.RISING, TimingValue(100, 600), TimingValue(100, 100))
        lut.add_setup_hold("CLK", "DI1", ClockEdge.RISING, TimingValue(100, 100), TimingValue(100, 100))
        # DI2 is the data input of the O6 half of a 32-deep LUT-RAM (RAMD32 at
        # O6) and of an SRL16E: without a setup check those writes go untimed.
        lut.add_setup_hold("CLK", "DI2", ClockEdge.RISING, TimingValue(100, 100), TimingValue(100, 100))
    add_lut_timing(ch.timing.add_cell_variant("DEFAULT", "SLICE_LUTX"))
    ff = ch.timing.add_cell_variant("DEFAULT", "SLICE_FFX")
    ff.add_setup_hold("CK", "CE", ClockEdge.RISING, TimingValue(100, 100), TimingValue(0, 0))
    ff.add_setup_hold("CK", "SR", ClockEdge.RISING, TimingValue(100, 100), TimingValue(0, 0))
    ff.add_setup_hold("CK", "D", ClockEdge.RISING, ff_d_setup, ff_d_hold)
    ff.add_clock_out("CK", "Q", ClockEdge.RISING, ff_clk_q)

    # Load SDF for carry and mux
    timings_root = xraydb_root
    if "kintex7" in xraydb_root: # TODO: missing
        timings_root = xraydb_root.replace("kintex7", "artix7")
    slicem_sdf = parse_sdf.parse_sdf_file(path.join(timings_root, "timings", "slicem.sdf"))
    mux = ch.timing.add_cell_variant("DEFAULT", "SELMUX2_1")
    import_sdf_timings(mux, slicem_sdf.cells[("SELMUX2_1", "SLICEM/F7BMUX")])
    carry = ch.timing.add_cell_variant("DEFAULT", "CARRY4")
    import_sdf_timings(carry, slicem_sdf.cells[("CARRY4", "SLICEM")])
    # A LUT-RAM or SRL output also changes on the clock, after a write or a
    # shift. Those cells take one of these variants (assign_cell_tags in
    # xilinx.cc): the LUT arcs above plus clock-to-out, the slowest of the four
    # LUTs in the SDF. They are named after the attribute the packer gives
    # those cells, which is no cell type, so nothing else picks them up.
    def sdf_clock_out(celltype, port):
        arcs = [e for (ct, inst), cell in slicem_sdf.cells.items() if ct == celltype for e in cell.entries
                if isinstance(e, parse_sdf.IOPath) and e.from_pin == "CLK" and e.to_pin == port]
        if not arcs:
            return None
        return TimingValue(int(min(min(e.rising.minv, e.falling.minv) for e in arcs)*1000),
            int(max(max(e.rising.maxv, e.falling.maxv) for e in arcs)*1000))
    for variant, outputs in (("X_LUT_AS_DRAM", (("O6", "LUT_OR_MEM6LRAM"), ("O5", "LUT_OR_MEM5LRAM"))),
                             ("X_LUT_AS_SRL", (("O6", "LUT_OR_MEM6SHFREG"), ("MC31", "LUT_OR_MEM6SHFREG")))):
        clock_outs = [(port, sdf_clock_out(celltype, port)) for port, celltype in outputs]
        if any(delay is None for port, delay in clock_outs):
            continue
        mem = ch.timing.add_cell_variant("DEFAULT", variant)
        add_lut_timing(mem)
        for port, delay in clock_outs:
            mem.add_clock_out("CLK", port, ClockEdge.RISING, delay)

    import_bram_timings(ch.timing, parse_sdf.parse_sdf_file(path.join(timings_root, "timings", "BRAM_L.sdf")), is_virtex7)

    # Import package pins
    for package_name, package in sorted(d.packages.items(), key=lambda x:x[0]):
        pkg = ch.create_package(package_name)
        for pin, site in sorted(package.pin_map.items(), key=lambda x:x[0]):
            site_data = d.sites_by_name[site]
            if not inside(site_data.tile):
                continue
            bel_name = gen_bel_name(site_data, "PAD")
            tile_type = ch.tile_type_at(gx(site_data.tile.x), gy(site_data.tile.y))
            pkg.create_pad(pin, f"X{gx(site_data.tile.x)}Y{gy(site_data.tile.y)}",
            f"{site_data.rel_name()}.{bel_name}",
            "", 0) # TODO: bank
    # Report the bitstream-representation census.  A non-zero "no bits" count
    # is not an error -- it is the number of pips the router must now avoid
    # because prjxray cannot express them.  A large "no tile db" count means
    # whole tile types went unchecked and their pips are still traps.
    tot = bits_stats["known"] + bits_stats["nobits"]
    print("prjxray bitstream check: %d tile-routing pips, %d expressible, "
          "%d NOT expressible (%.2f%%), %d unchecked (tile type has no bit db)"
          % (tot, bits_stats["known"], bits_stats["nobits"],
             100.0 * bits_stats["nobits"] / max(tot, 1), bits_stats["notiledb"]),
          file=sys.stderr)
    for t, c in sorted(nobits_by_tiletype.items(), key=lambda kv: -kv[1])[:10]:
        print("    no bits: %-28s %6d pips" % (t, c), file=sys.stderr)
    ch.write_bba(args.bba)

if __name__ == '__main__':
    main()
