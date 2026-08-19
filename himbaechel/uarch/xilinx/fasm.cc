/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019-2023  gatecat <gatecat@ds0.me>
 *  Copyright (C) 2023  Hans Baier <hansfbaier@gmail.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <boost/algorithm/string.hpp>
#include <boost/range/adaptor/reversed.hpp>

#include <fstream>
#include <regex>

#include "extra_data.h"
#include "himbaechel_api.h"
#include "log.h"
#include "nextpnr.h"
#include "pins.h"
#include "util.h"

#include "xilinx.h"
#include "version.h"

#define HIMBAECHEL_CONSTIDS "uarch/xilinx/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

namespace Xc7MMCM {
extern const uint16_t filter_lookup_low[];
extern const uint16_t filter_lookup_low_ss[];
extern const uint16_t filter_lookup_high[];
extern const uint16_t filter_lookup_optimized[];
extern const int64_t lk_table[];
}; // namespace Xc7MMCM

namespace {
struct FasmBackend
{
    Context *ctx;
    XilinxImpl *uarch;
    std::ostream &out;
    std::vector<std::string> fasm_ctx;
    dict<int, std::vector<PipId>> pips_by_tile;

    // (tile_index, slot_y) pairs where a BUFGCTRL cell is actually bound.
    // Used by write_pip to suppress phantom BUFGCTRL.BUFGCTRL_X0Y*.IN_USE /
    // IS_*_INVERTED / ZINV_* bits and CLK_BUFG_*_R IMUX pip bits that the
    // router produces merely by crossing an idle BUFG tile.  Without this,
    // a single-BUFG design programs both the active CLK_BUFG_TOP_R site AND
    // a phantom one in the adjacent CLK_BUFG_BOT_R tile, contending for the
    // clock distribution backbone and leaving the FF clock dead on hardware
    // (port of nextpnr-xilinx, task #47).
    std::set<std::pair<int, int>> bufgctrl_bound_slots;
    void populate_bufgctrl_bound_slots()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_BUFGCTRL && ci->bel != BelId()) {
                SiteIndex site = uarch->get_bel_site(ci->bel);
                const auto &site_data = uarch->tile_extra_data(site.tile)->sites[site.site];
                bufgctrl_bound_slots.insert({site.tile, site_data.site_y});
            }
        }
    }

    dict<std::pair<int, int>, unsigned> lut_route_throughs;

    dict<IdString, pool<IdString>> invertible_pins;

    FasmBackend(Context *ctx, XilinxImpl *uarch, std::ostream &out) : ctx(ctx), uarch(uarch), out(out) {};

    void push(const std::string &x) { fasm_ctx.push_back(x); }

    void pop() { fasm_ctx.pop_back(); }

    void pop(int N)
    {
        for (int i = 0; i < N; i++)
            fasm_ctx.pop_back();
    }
    bool last_was_blank = true;
    void blank()
    {
        if (!last_was_blank)
            out << std::endl;
        last_was_blank = true;
    }

    void write_prefix()
    {
        for (auto &x : fasm_ctx)
            out << x << ".";
        last_was_blank = false;
    }

    void write_bit(const std::string &name, bool value = true)
    {
        if (value) {
            write_prefix();
            out << name << std::endl;
        }
    }

    void write_vector(const std::string &name, const std::vector<bool> &value, bool invert = false)
    {
        write_prefix();
        out << name << " = " << int(value.size()) << "'b";
        for (auto bit : boost::adaptors::reverse(value))
            out << ((bit ^ invert) ? '1' : '0');
        out << std::endl;
    }

    void write_int_vector(const std::string &name, uint64_t value, int width, bool invert = false)
    {
        std::vector<bool> bits(width, false);
        for (int i = 0; i < width; i++)
            bits[i] = (value & (1ULL << i)) != 0;
        write_vector(name, bits, invert);
    }

    struct PseudoPipKey
    {
        IdString tileType;
        IdString dest;
        IdString source;

        bool operator==(const PseudoPipKey &b) const
        {
            return std::tie(this->tileType, this->dest, this->source) == std::tie(b.tileType, b.dest, b.source);
        }

        unsigned int hash() const { return mkhash(mkhash(tileType.hash(), source.hash()), dest.hash()); }
    };

    dict<PseudoPipKey, std::vector<std::string>> pp_config;
    void get_pseudo_pip_data()
    {
        /*
         * Create the mapping from pseudo pip tile type, dest wire, and source wire, to
         * the config bits set when that pseudo pip is used
         */
        for (std::string s : {"L", "R"})
            for (std::string s2 : {"", "_TBYTESRC", "_TBYTETERM", "_SING"})
                for (std::string i :
                     (s2 == "_SING") ? std::vector<std::string>{"", "0", "1"} : std::vector<std::string>{"0", "1"}) {
                    pp_config[{ctx->id(s + "IOI3" + s2), ctx->id(s + "IOI_OLOGIC" + i + "_OQ"),
                               ctx->id("IOI_OLOGIC" + i + "_D1")}] = {"OLOGIC_Y" + i + ".OMUX.D1",
                                                                      "OLOGIC_Y" + i + ".OQUSED",
                                                                      "OLOGIC_Y" + i + ".OSERDES.DATA_RATE_TQ.BUF"};
                    pp_config[{ctx->id(s + "IOI3" + s2), ctx->id("IOI_ILOGIC" + i + "_O"),
                               ctx->id(s + "IOI_ILOGIC" + i + "_D")}] = {"IDELAY_Y" + i + ".IDELAY_TYPE_FIXED",
                                                                         "ILOGIC_Y" + i + ".ZINV_D"};
                    pp_config[{ctx->id(s + "IOI3" + s2), ctx->id("IOI_ILOGIC" + i + "_O"),
                               ctx->id(s + "IOI_ILOGIC" + i + "_DDLY")}] = {"ILOGIC_Y" + i + ".IDELMUXE3.P0",
                                                                            "ILOGIC_Y" + i + ".ZINV_D"};
                    pp_config[{ctx->id(s + "IOI3" + s2), ctx->id(s + "IOI_OLOGIC" + i + "_TQ"),
                               ctx->id("IOI_OLOGIC" + i + "_T1")}] = {"OLOGIC_Y" + i + ".ZINV_T1"};
                    if (i == "0") {
                        pp_config[{ctx->id(s + "IOB33" + s2), id_IOB_O_IN1, id_IOB_O_OUT0}] = {};
                        pp_config[{ctx->id(s + "IOB33" + s2), id_IOB_O_OUT0, id_IOB_O0}] = {};
                        pp_config[{ctx->id(s + "IOB33" + s2), id_IOB_T_IN1, id_IOB_T_OUT0}] = {};
                        pp_config[{ctx->id(s + "IOB33" + s2), id_IOB_T_OUT0, id_IOB_T0}] = {};
                        pp_config[{ctx->id(s + "IOB33" + s2), id_IOB_DIFFI_IN0, id_IOB_PADOUT1}] = {};
                    }
                }

        for (std::string s2 : {"", "_TBYTESRC", "_TBYTETERM", "_SING"})
            for (std::string i : (s2 == "_SING") ? std::vector<std::string>{"0"} : std::vector<std::string>{"0", "1"}) {
                pp_config[{ctx->id("RIOI" + s2), ctx->id("RIOI_OLOGIC" + i + "_OQ"),
                           ctx->id("IOI_OLOGIC" + i + "_D1")}] = {"OLOGIC_Y" + i + ".OMUX.D1",
                                                                  "OLOGIC_Y" + i + ".OQUSED",
                                                                  "OLOGIC_Y" + i + ".OSERDES.DATA_RATE_TQ.BUF"};
                pp_config[{ctx->id("RIOI" + s2), ctx->id("RIOI_OLOGIC" + i + "_OFB"),
                           ctx->id("RIOI_OLOGIC" + i + "_OQ")}] = {};
                pp_config[{ctx->id("RIOI" + s2), ctx->id("RIOI_O" + i), ctx->id("RIOI_ODELAY" + i + "_DATAOUT")}] = {};
                pp_config[{ctx->id("RIOI" + s2), ctx->id("RIOI_OLOGIC" + i + "_OFB"),
                           ctx->id("IOI_OLOGIC" + i + "_D1")}] = {"OLOGIC_Y" + i + ".OMUX.D1",
                                                                  "OLOGIC_Y" + i + ".OSERDES.DATA_RATE_TQ.BUF"};
                pp_config[{ctx->id("RIOI" + s2), ctx->id("IOI_ILOGIC" + i + "_O"), ctx->id("RIOI_ILOGIC" + i + "_D")}] =
                        {"ILOGIC_Y" + i + ".ZINV_D"};
                pp_config[{ctx->id("RIOI" + s2), ctx->id("IOI_ILOGIC" + i + "_O"),
                           ctx->id("RIOI_ILOGIC" + i + "_DDLY")}] = {"ILOGIC_Y" + i + ".IDELMUXE3.P0",
                                                                     "ILOGIC_Y" + i + ".ZINV_D"};
                pp_config[{ctx->id("RIOI" + s2), ctx->id("RIOI_OLOGIC" + i + "_TQ"),
                           ctx->id("IOI_OLOGIC" + i + "_T1")}] = {"OLOGIC_Y" + i + ".ZINV_T1"};
                pp_config[{ctx->id("RIOI" + s2), ctx->id("RIOI_OLOGIC" + i + "_OFB"),
                           ctx->id("RIOI_ODELAY" + i + "_ODATAIN")}] = {"OLOGIC_Y" + i + ".ZINV_ODATAIN"};
                if (i == "0") {
                    pp_config[{ctx->id("RIOB18" + s2), id_IOB_O_IN1, id_IOB_O_OUT0}] = {};
                    pp_config[{ctx->id("RIOB18" + s2), id_IOB_O_OUT0, id_IOB_O0}] = {};
                    pp_config[{ctx->id("RIOB18" + s2), id_IOB_T_IN1, id_IOB_T_OUT0}] = {};
                    pp_config[{ctx->id("RIOB18" + s2), id_IOB_T_OUT0, id_IOB_T0}] = {};
                    pp_config[{ctx->id("RIOB18" + s2), id_IOB_DIFFI_IN0, id_IOB_PADOUT1}] = {};
                }
            }

        for (std::string s1 : {"TOP", "BOT"}) {
            for (std::string s2 : {"L", "R"}) {
                for (int i = 0; i < 12; i++) {
                    std::string ii = std::to_string(i);
                    std::string hck = s2 + ii;
                    std::string buf = std::string((s2 == "R") ? "X1Y" : "X0Y") + ii;
                    pp_config[{ctx->id("CLK_HROW_" + s1 + "_R"), ctx->id("CLK_HROW_CK_HCLK_OUT_" + hck),
                               ctx->id("CLK_HROW_CK_MUX_OUT_" + hck)}] = {"BUFHCE.BUFHCE_" + buf + ".IN_USE",
                                                                          "BUFHCE.BUFHCE_" + buf + ".ZINV_CE"};
                }
            }

            for (int i = 0; i < 16; i++) {
                std::string ii = std::to_string(i);
                pp_config[{ctx->id("CLK_BUFG_" + s1 + "_R"), ctx->id("CLK_BUFG_BUFGCTRL" + ii + "_O"),
                           ctx->id("CLK_BUFG_BUFGCTRL" + ii + "_I0")}] = {
                        "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".IN_USE", "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".IS_IGNORE1_INVERTED",
                        "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".ZINV_CE0", "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".ZINV_S0"};
                pp_config[{ctx->id("CLK_BUFG_" + s1 + "_R"), ctx->id("CLK_BUFG_BUFGCTRL" + ii + "_O"),
                           ctx->id("CLK_BUFG_BUFGCTRL" + ii + "_I1")}] = {
                        "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".IN_USE", "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".IS_IGNORE0_INVERTED",
                        "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".ZINV_CE1", "BUFGCTRL.BUFGCTRL_X0Y" + ii + ".ZINV_S1"};
            }
        }

        int rclk_y_to_i[4] = {2, 3, 0, 1};
        for (int y = 0; y < 4; y++) {
            std::string yy = std::to_string(y);
            std::string ii = std::to_string(rclk_y_to_i[y]);
            pp_config[{id_HCLK_IOI3, ctx->id("HCLK_IOI_RCLK_OUT" + ii), ctx->id("HCLK_IOI_RCLK_BEFORE_DIV" + ii)}] = {
                    "BUFR_Y" + yy + ".IN_USE", "BUFR_Y" + yy + ".BUFR_DIVIDE.BYPASS"};
            pp_config[{id_HCLK_IOI, ctx->id("HCLK_IOI_RCLK_OUT" + ii), ctx->id("HCLK_IOI_RCLK_BEFORE_DIV" + ii)}] = {
                    "BUFR_Y" + yy + ".IN_USE", "BUFR_Y" + yy + ".BUFR_DIVIDE.BYPASS"};
        }

        // FIXME: shouldn't these be in the X-RAY ppips database?
        for (char c : {'L', 'R'}) {
            for (int i = 0; i < 24; i++) {
                pp_config[{ctx->idf("INT_INTERFACE_%c", c), ctx->idf("INT_INTERFACE_LOGIC_OUTS_%c%d", c, i),
                           ctx->idf("INT_INTERFACE_LOGIC_OUTS_%c_B%d", c, i)}];
            }
        }
    }

    void write_pip(PipId pip, NetInfo *net)
    {

        pips_by_tile[pip.tile].push_back(pip);

        auto dst_intent = ctx->getWireType(ctx->getPipDstWire(pip));
        if (dst_intent == id_PSEUDO_GND || dst_intent == id_PSEUDO_VCC)
            return;

        auto &pd = chip_pip_info(ctx->chip_info, pip);
        const auto &extra_data = *reinterpret_cast<const XlnxPipExtraDataPOD *>(pd.extra_data.get());
        unsigned pip_type = pd.flags;

        if (pip_type != PIP_TILE_ROUTING && pip_type != PIP_SITE_INTERNAL)
            return;

        IdString src = IdString(chip_tile_info(ctx->chip_info, pip.tile).wires[pd.src_wire].name);
        IdString dst = IdString(chip_tile_info(ctx->chip_info, pip.tile).wires[pd.dst_wire].name);

        // handle certain site internal pips:
        // this is necessary, because in tristate outputs, the
        // ZINV_T1 bit needs to be set, because in the OLOGIC tiles the
        // tristate control signals are inverted if this bit is not set
        // this only applies to router1, because router2 does not generate
        // site internal pips here.
        if (pip_type == PIP_SITE_INTERNAL) {
            if (src.str(ctx) == "T1" && dst.str(ctx) == "T1INV_OUT") {
                auto srcwire_uphill_iter = ctx->getPipsUphill(ctx->getPipSrcWire(pip));
                auto uphill = srcwire_uphill_iter.begin();
                if (uphill != srcwire_uphill_iter.end()) {
                    // source wire should be like: LIOI3_X0Y73/IOI_OLOGIC1_T1
                    auto loc = ctx->getWireName(ctx->getPipSrcWire(*uphill)).str(ctx);
                    boost::replace_all(loc, "/", ".");
                    boost::erase_all(loc, "_T1");
                    boost::replace_all(loc, "IOI_OLOGIC", "OLOGIC_Y");
                    // the replacements transformed it into : LIOI3_X0Y73.OLOGIC_Y1
                    out << loc << "." << "ZINV_T1" << std::endl;
                }
            }
            return;
        }

        // handle tile routing pips
        IdString tile_type = IdString(chip_tile_info(ctx->chip_info, pip.tile).type_name);
        PseudoPipKey ppk{tile_type, dst, src};

        if (pp_config.count(ppk)) {
            auto &pp = pp_config.at(ppk);
            std::string tile_name = uarch->tile_name(pip.tile);
            // Phantom-BUFGCTRL guard (pseudo-pip variant): if the router
            // crosses a CLK_BUFG_*_R tile that has NO bound BUFGCTRL, every
            // pseudo-pip feature for that tile is a phantom (the chipdb's
            // clock-distribution graph allows the path, but Vivado doesn't
            // actually use it).  Drop the whole emission -- not just the
            // BUFGCTRL.* config bits but also the CLK_BUFG_BUFGCTRL*_I0/I1
            // IMUX features that program a phantom clock-input mux at the
            // empty BUFG site, contending with the real one in the active
            // tile.  (Port of nextpnr-xilinx, task #47.)
            bool tile_is_clk_bufg_r =
                    (boost::starts_with(tile_name, "CLK_BUFG_TOP_R") || boost::starts_with(tile_name, "CLK_BUFG_BOT_R"));
            if (tile_is_clk_bufg_r) {
                bool any_bound_here = false;
                for (int slot = 0; slot < 16; ++slot)
                    if (bufgctrl_bound_slots.count({pip.tile, slot})) {
                        any_bound_here = true;
                        break;
                    }
                if (!any_bound_here)
                    return;
            }
            for (auto c : pp) {
                if (boost::starts_with(tile_name, "RIOI3_SING") || boost::starts_with(tile_name, "LIOI3_SING") ||
                    boost::starts_with(tile_name, "RIOI_SING")) {
                    // Need to flip for top HCLK
                    bool is_top_sing = pip.tile < uarch->hclk_for_ioi(pip.tile);
                    if (is_top_sing) {
                        auto y0pos = c.find("Y0");
                        if (y0pos != std::string::npos)
                            c.replace(y0pos, 2, "Y1");
                    }
                }
                // Phantom-BUFGCTRL guard (per-slot variant): suppress
                // BUFGCTRL.BUFGCTRL_X0Y<n>.* features on (tile, slot) pairs
                // that have no actually-bound BUFGCTRL cell.
                if (boost::starts_with(c, "BUFGCTRL.BUFGCTRL_X0Y")) {
                    const std::string prefix = "BUFGCTRL.BUFGCTRL_X0Y";
                    size_t start = prefix.size();
                    size_t end = c.find('.', start);
                    if (end == std::string::npos)
                        end = c.size();
                    int slot = -1;
                    try {
                        slot = std::stoi(c.substr(start, end - start));
                    } catch (...) {
                        slot = -1;
                    }
                    if (slot >= 0 && !bufgctrl_bound_slots.count({pip.tile, slot}))
                        continue;
                }
                out << tile_name << "." << c << std::endl;
            }
            if (!pp.empty())
                last_was_blank = false;
        } else {
            if (extra_data.pip_config == 1)
                log_warning("Unprocessed route-thru %s.%s.%s\n!", tile_type.c_str(ctx), src.c_str(ctx), dst.c_str(ctx));

            std::string tile_name = uarch->tile_name(pip.tile);
            std::string dst_name = dst.str(ctx);
            std::string src_name = src.str(ctx);

            // Phantom-BUFGCTRL guard (regular-pip variant): the pp_config
            // branch already filters pseudo-pip emissions at CLK_BUFG_*_R
            // tiles that hold no bound BUFGCTRL; this handles the regular
            // PIPs the router crossed through the same tiles
            // (CLK_BUFG_BUFGCTRL*_I0/I1 IMUX hops, CLK_BUFG_CK_GCLK* output
            // PIPs).  Both classes program the unused-slot BUFGCTRL site;
            // the cell-config + routing pair together is what kills the
            // clock distribution on hardware.  (Port of nextpnr-xilinx.)
            if (boost::starts_with(tile_name, "CLK_BUFG_TOP_R") || boost::starts_with(tile_name, "CLK_BUFG_BOT_R")) {
                bool any_bound_here = false;
                for (int slot = 0; slot < 16; ++slot)
                    if (bufgctrl_bound_slots.count({pip.tile, slot})) {
                        any_bound_here = true;
                        break;
                    }
                if (!any_bound_here)
                    return;
            }

            if (boost::starts_with(tile_name, "DSP_L") || boost::starts_with(tile_name, "DSP_R")) {
                // FIXME: PPIPs missing for DSPs
                return;
            }
            std::string orig_dst_name = dst_name;
            if (boost::starts_with(tile_name, "RIOI3_SING") || boost::starts_with(tile_name, "LIOI3_SING") ||
                boost::starts_with(tile_name, "RIOI_SING")) {
                // FIXME: PPIPs missing for SING IOI3s
                if ((boost::contains(src_name, "IMUX") || boost::contains(src_name, "CTRL0")) &&
                    !boost::contains(dst_name, "CLK"))
                    return;
                auto spos = src_name.find("_SING_");
                if (spos != std::string::npos)
                    src_name.erase(spos, 5);
                // Need to flip for top HCLK
                // TODO

                bool is_top_sing = pip.tile < uarch->hclk_for_ioi(pip.tile);
                if (is_top_sing) {
                    auto us0pos = dst_name.find("_0");
                    if (us0pos != std::string::npos)
                        dst_name.replace(us0pos, 2, "_1");
                    auto ol0pos = dst_name.find("OLOGIC0");
                    if (ol0pos != std::string::npos) {
                        dst_name.replace(ol0pos, 7, "OLOGIC1");
                        us0pos = src_name.find("_0");
                        if (us0pos != std::string::npos)
                            src_name.replace(us0pos, 2, "_1");
                    }
                }
            }
            if (boost::contains(tile_name, "IOI")) {
                if (boost::contains(dst_name, "OCLKB") && boost::contains(src_name, "IOI_OCLKM_"))
                    return; // missing, not sure if really a ppip?
            }

            out << tile_name << ".";
            out << dst_name << ".";
            out << src_name << std::endl;

            if (boost::contains(tile_name, "IOI") && boost::starts_with(dst_name, "IOI_OCLK_")) {
                dst_name.insert(dst_name.find("OCLK") + 4, 1, 'M');
                orig_dst_name.insert(dst_name.find("OCLK") + 4, 1, 'M');

                WireId w = uarch->lookup_wire(pip.tile, ctx->id(orig_dst_name));

                NPNR_ASSERT(w != WireId());
                if (ctx->getBoundWireNet(w) == nullptr) {
                    out << tile_name << ".";
                    out << dst_name << ".";
                    out << src_name << std::endl;
                }
            }

            last_was_blank = false;
        }
    };

    // Get the set of input signals for a LUT-type cell
    std::vector<IdString> get_inputs(CellInfo *cell)
    {
        IdString type = ctx->id(str_or_default(cell->attrs, id_X_ORIG_TYPE, ""));
        if (type == id_LUT1)
            return {id_I0};
        else if (type == id_LUT2)
            return {id_I0, id_I1};
        else if (type == id_LUT3)
            return {id_I0, id_I1, id_I2};
        else if (type == id_LUT4)
            return {id_I0, id_I1, id_I2, id_I3};
        else if (type == id_LUT5)
            return {id_I0, id_I1, id_I2, id_I3, id_I4};
        else if (type == id_LUT6)
            return {id_I0, id_I1, id_I2, id_I3, id_I4, id_I5};
        else if (type == id_RAMD64E)
            return {id_RADR0, id_RADR1, id_RADR2, id_RADR3, id_RADR4, id_RADR5};
        else if (type == id_SRL16E)
            return {id_A0, id_A1, id_A2, id_A3};
        else if (type == id_SRLC32E)
            return {ctx->id("A[0]"), ctx->id("A[1]"), ctx->id("A[2]"), ctx->id("A[3]"), ctx->id("A[4]")};
        else if (type == id_RAMD32)
            return {id_RADR0, id_RADR1, id_RADR2, id_RADR3, id_RADR4};
        else
            NPNR_ASSERT_FALSE("unsupported LUT-type cell");
    }

    // Process LUT initialisation
    std::vector<bool> get_lut_init(CellInfo *lut6, CellInfo *lut5)
    {
        std::vector<bool> bits(64, false);

        std::vector<IdString> phys_inputs;
        for (int i = 1; i <= 6; i++)
            phys_inputs.push_back(ctx->id("A" + std::to_string(i)));

        for (int i = 0; i < 2; i++) {
            CellInfo *lut = (i == 1) ? lut5 : lut6;
            if (lut == nullptr)
                continue;
            auto lut_inputs = get_inputs(lut);
            dict<int, std::vector<std::string>> phys_to_log;
            dict<std::string, int> log_to_bit;
            for (int j = 0; j < int(lut_inputs.size()); j++)
                log_to_bit[lut_inputs[j].str(ctx)] = j;
            for (int j = 0; j < 6; j++) {
                // Get the LUT physical to logical mapping
                phys_to_log[j];
                if (!lut->attrs.count(ctx->idf("X_ORIG_PORT_%s", phys_inputs[j].c_str(ctx))))
                    continue;
                std::string orig = lut->attrs.at(ctx->idf("X_ORIG_PORT_%s", phys_inputs[j].c_str(ctx))).as_string();
                boost::split(phys_to_log[j], orig, boost::is_any_of(" "));
            }
            int lbound = 0, ubound = 64;
            // Fracturable LUTs
            if (lut5 && lut6) {
                lbound = (i == 1) ? 0 : 32;
                ubound = (i == 1) ? 32 : 64;
            }
            Property init = get_or_default(lut->params, id_INIT, Property()).extract(0, 64);
            for (int j = lbound; j < ubound; j++) {
                int log_index = 0;
                for (int k = 0; k < 6; k++) {
                    if ((j & (1 << k)) == 0)
                        continue;
                    for (auto &p2l : phys_to_log[k])
                        log_index |= (1 << log_to_bit[p2l]);
                }
                bits[j] = (init.str.at(log_index) == Property::S1);
            }
        }
        return bits;
    };

    // Return the name for a half-logic-tile
    std::string get_half_name(int half, bool is_m)
    {
        if (is_m)
            return half ? "SLICEL_X1" : "SLICEM_X0";
        else
            return half ? "SLICEL_X1" : "SLICEL_X0";
    }

    std::string get_bel_name(BelId bel) { return uarch->bel_name_in_site(bel).str(ctx); }

    void write_routing_bel(WireId dst_wire)
    {
        for (auto pip : ctx->getPipsUphill(dst_wire)) {
            if (ctx->getBoundPipNet(pip) != nullptr) {
                auto &pd = chip_pip_info(ctx->chip_info, pip);
                const auto &extra_data = *reinterpret_cast<const XlnxPipExtraDataPOD *>(pd.extra_data.get());
                std::string belname = IdString(extra_data.bel_name).str(ctx);
                std::string pinname = IdString(extra_data.pip_config).str(ctx);
                bool skip_pinname = false;
                // Ignore modes with no associated bit (X-ray omission??)
                if (belname == "WEMUX" && pinname == "WE")
                    continue;

                if (belname.substr(1) == "DI1MUX") {
                    // prjxray names the non-default leg of these muxes after
                    // BOTH signals sharing it -- the LUTRAM write-data
                    // broadcast and the SRL MC31 cascade run through one
                    // config bit: ADI1MUX -> BDI1_BMC31, BDI1MUX -> DI_CMC31,
                    // CDI1MUX -> DI_DMC31 (there is no DDI1MUX).  The
                    // default own-letter leg (AI/BI/CI) keeps its plain name.
                    // Emitting the bare site-wire name (e.g. BMC31) makes
                    // fasm2frames reject the feature.  (Port of
                    // nextpnr-xilinx fasm.cc.)
                    if (pinname != std::string(1, belname[0]) + "I") {
                        switch (belname[0]) {
                        case 'A':
                            pinname = "BDI1_BMC31";
                            break;
                        case 'B':
                            pinname = "DI_CMC31";
                            break;
                        case 'C':
                            pinname = "DI_DMC31";
                            break;
                        default:
                            break;
                        }
                    }
                    belname = "DI1MUX";
                }

                if (belname.substr(1) == "CY0") {
                    if (pinname.substr(1) == "5")
                        skip_pinname = true;
                    else
                        continue;
                }

                write_prefix();
                out << belname;
                if (!skip_pinname)
                    out << "." << pinname;
                out << std::endl;
            }
        }
    }

    // Process flipflops in a half-tile
    void write_ffs_config(int tile, int half)
    {
        bool found_ff = false;
        bool negedge_ff = false;
        bool is_latch = false;
        bool is_sync = false;
        bool is_clkinv = false;
        bool is_srused = false;
        bool is_ceused = false;

#define SET_CHECK(dst, src)                                                                                            \
    do {                                                                                                               \
        if (found_ff)                                                                                                  \
            NPNR_ASSERT(dst == (src));                                                                                 \
        else                                                                                                           \
            dst = (src);                                                                                               \
    } while (0)

        std::string tname = uarch->tile_name(tile);

        const auto &lts = uarch->tile_status.at(tile).lts;
        if (!lts)
            return;

        push(tname);
        push(get_half_name(half, boost::contains(tname, "CLBLM")));

        for (int i = 0; i < 4; i++) {
            CellInfo *ff1 = lts->cells[(half << 6) | (i << 4) | BEL_FF];
            CellInfo *ff2 = lts->cells[(half << 6) | (i << 4) | BEL_FF2];
            for (int j = 0; j < 2; j++) {
                CellInfo *ff = (j == 1) ? ff2 : ff1;
                if (ff == nullptr)
                    continue;
                push(get_bel_name(ff->bel));
                bool zrst = false, zinit = false;
                zinit = (int_or_default(ff->params, id_INIT, 0) != 1);
                IdString srsig;
                std::string type = str_or_default(ff->attrs, id_X_ORIG_TYPE, "");
                if (type == "FDRE") {
                    zrst = true;
                    SET_CHECK(negedge_ff, false);
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, true);
                } else if (type == "FDRE_1") {
                    zrst = true;
                    SET_CHECK(negedge_ff, true);
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, true);
                } else if (type == "FDSE") {
                    zrst = false;
                    SET_CHECK(negedge_ff, false);
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, true);
                } else if (type == "FDSE_1") {
                    zrst = false;
                    SET_CHECK(negedge_ff, true);
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, true);
                } else if (type == "FDCE") {
                    zrst = true;
                    SET_CHECK(negedge_ff, false);
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, false);
                } else if (type == "FDCE_1") {
                    zrst = true;
                    SET_CHECK(negedge_ff, true);
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, false);
                } else if (type == "FDPE") {
                    zrst = false;
                    SET_CHECK(negedge_ff, false);
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, false);
                } else if (type == "FDPE_1") {
                    zrst = false;
                    SET_CHECK(negedge_ff, true);
                    SET_CHECK(is_latch, false);
                    SET_CHECK(is_sync, false);
                } else {
                    log_error("unsupported FF type: '%s'\n", type.c_str());
                }

                write_bit("ZINI", zinit);
                write_bit("ZRST", zrst);

                pop();
                if (negedge_ff)
                    SET_CHECK(is_clkinv, true);
                else
                    SET_CHECK(is_clkinv, int_or_default(ff->params, id_IS_C_INVERTED) == 1);

                NetInfo *sr = ff->getPort(id_SR), *ce = ff->getPort(id_CE);

                SET_CHECK(is_srused, sr != nullptr && sr->name != ctx->id("$PACKER_GND_NET"));
                SET_CHECK(is_ceused, ce != nullptr && ce->name != ctx->id("$PACKER_VCC_NET"));

                // Input mux
                write_routing_bel(ctx->getBelPinWire(ff->bel, id_D));

                found_ff = true;
            }
        }
        write_bit("LATCH", is_latch);
        write_bit("FFSYNC", is_sync);
        write_bit("CLKINV", is_clkinv);
        write_bit("NOCLKINV", !is_clkinv);
        write_bit("SRUSEDMUX", is_srused);
        write_bit("CEUSEDMUX", is_ceused);
        pop(2);
    }

    // Get a named wire in the same site as a bel
    WireId get_site_wire(BelId site_bel, std::string name)
    {
        IdStringList bel_name = ctx->getBelName(site_bel);
        NPNR_ASSERT(bel_name.size() == 2);
        IdString tile_name = bel_name[0];
        const std::string &bel_name_str = bel_name[1].str(ctx);
        size_t sep_pos = bel_name_str.find('.');
        NPNR_ASSERT(sep_pos != std::string::npos);
        std::string site_name = bel_name_str.substr(0, sep_pos);
        IdString wire_name = ctx->idf("%s.%s", site_name.c_str(), name.c_str());
        WireId wire = ctx->getWireByName(IdStringList::concat(tile_name, wire_name));
        NPNR_ASSERT(wire != WireId());
        return wire;
    }

    // Process LUTs and associated functionality in a half
    void write_luts_config(int tile, int half)
    {
        bool wa7_used = false, wa8_used = false;

        std::string tname = uarch->tile_name(tile);
        bool is_mtile = boost::contains(tname, "CLBLM");
        bool is_slicem = is_mtile && (half == 0);

        const auto &lts = uarch->tile_status.at(tile).lts;

        push(tname);
        push(get_half_name(half, is_mtile));

        // Write route through pips
        for (int i = 0; i < 4; i++) {
            auto found_rt = lut_route_throughs.find(std::make_pair(tile, half * 4 + i));
            if (found_rt != lut_route_throughs.end()) {
                std::string lutname = stringf("%cLUT", "ABCD"[i]);
                push(lutname);
                std::vector<bool> rt_init(64, false);
                for (unsigned b = 0; b < 64; b++) {
                    if (b & (1U << found_rt->second))
                        rt_init[b] = true;
                }
                write_vector("INIT[63:0]", rt_init);
                pop();
            }
        }
        if (lts) {
            // Write logic
            BelId bel_in_half =
                    ctx->getBelByLocation(Loc(tile % ctx->chip_info->width, tile / ctx->chip_info->width, half << 6));

            for (int i = 0; i < 4; i++) {
                CellInfo *lut6 = lts->cells[(half << 6) | (i << 4) | BEL_6LUT];
                CellInfo *lut5 = lts->cells[(half << 6) | (i << 4) | BEL_5LUT];
                // Write LUT initialisation
                if (lut6 != nullptr || lut5 != nullptr) {
                    std::string lutname = stringf("%cLUT", "ABCD"[i]);
                    push(lutname);
                    write_vector("INIT[63:0]", get_lut_init(lut6, lut5));

                    // Write LUT mode config
                    bool is_small = false, is_ram = false, is_srl = false;
                    for (int j = 0; j < 2; j++) {
                        CellInfo *lut = (j == 1) ? lut5 : lut6;
                        if (lut == nullptr)
                            continue;
                        std::string type = str_or_default(lut->attrs, id_X_ORIG_TYPE);
                        if (type == "RAMD64E" || type == "RAMS64E") {
                            is_ram = true;
                        } else if (type == "RAMD32" || type == "RAMS32") {
                            is_ram = true;
                            is_small = true;
                        } else if (type == "SRL16E") {
                            is_srl = true;
                            is_small = true;
                        } else if (type == "SRLC32E") {
                            is_srl = true;
                        }
                        wa7_used |= (lut->getPort(id_WA7) != nullptr);
                        wa8_used |= (lut->getPort(id_WA8) != nullptr);
                    }
                    if (is_slicem && i != 3) {
                        write_routing_bel(get_site_wire(bel_in_half, stringf("%cDI1MUX_OUT", "ABCD"[i])));
                    }
                    write_bit("SMALL", is_small);
                    write_bit("RAM", is_ram);
                    write_bit("SRL", is_srl);
                    pop();
                }
                write_routing_bel(get_site_wire(bel_in_half, stringf("%cMUX", "ABCD"[i])));
            }
            write_bit("WA7USED", wa7_used);
            write_bit("WA8USED", wa8_used);
            if (is_slicem)
                write_routing_bel(get_site_wire(bel_in_half, "WEMUX_OUT"));
        }

        pop(2);
    }

    void write_carry_config(int tile, int half)
    {
        std::string tname = uarch->tile_name(tile);
        bool is_mtile = boost::contains(tname, "CLBLM");

        const auto &lts = uarch->tile_status.at(tile).lts;
        if (!lts)
            return;

        CellInfo *carry = lts->cells[half << 6 | BEL_CARRY4];
        if (carry == nullptr)
            return;

        push(tname);
        push(get_half_name(half, is_mtile));

        write_routing_bel(get_site_wire(carry->bel, "PRECYINIT_OUT"));
        if (carry->getPort(id_CIN) != nullptr)
            write_bit("PRECYINIT.CIN");
        push("CARRY4");
        for (char c : {'A', 'B', 'C', 'D'})
            write_routing_bel(get_site_wire(carry->bel, stringf("%cCY0_OUT", c)));
        pop(3);
    }

    void write_logic()
    {
        std::set<int> used_logic_tiles;
        for (auto &cell : ctx->cells) {
            if (uarch->is_logic_tile(cell.second->bel))
                used_logic_tiles.insert(cell.second->bel.tile);
        }
        for (auto &net : ctx->nets) {
            for (const auto &wire_pair : net.second->wires) {
                PipId pip = wire_pair.second.pip;
                if (pip == PipId())
                    continue;
                const auto &pip_data = chip_pip_info(ctx->chip_info, pip);
                const auto &extra_data = *reinterpret_cast<const XlnxPipExtraDataPOD *>(pip_data.extra_data.get());
                unsigned pip_type = pip_data.flags;
                if (pip_type != PIP_LUT_ROUTETHRU)
                    continue;
                unsigned lut_idx = (extra_data.pip_config >> 8);
                unsigned lut_input = (extra_data.pip_config >> 1) & 0x7;
                lut_route_throughs[std::make_pair(pip.tile, lut_idx)] = lut_input;
                used_logic_tiles.insert(pip.tile);
            }
        }
        for (int tile : used_logic_tiles) {
            write_luts_config(tile, 0);
            write_luts_config(tile, 1);
            write_ffs_config(tile, 0);
            write_ffs_config(tile, 1);
            write_carry_config(tile, 0);
            write_carry_config(tile, 1);
            blank();
        }
    }

    void write_routing()
    {
        get_pseudo_pip_data();
        for (auto &net : ctx->nets) {
            NetInfo *ni = net.second.get();
            out << stringf("# routing for net %s", ni->name.c_str(ctx)) << std::endl;
            for (auto &w : ni->wires) {
                if (w.second.pip != PipId())
                    write_pip(w.second.pip, ni);
            }
            blank();
        }
    }

    struct BankIoConfig
    {
        bool stepdown = false;
        bool vref = false;
        bool tmds_33 = false;
        bool lvds_25 = false;
        bool only_diff = false;
    };

    dict<int, BankIoConfig> ioconfig_by_hclk;

    bool warned_dci = false;

    void write_io_config(CellInfo *pad)
    {
        NetInfo *pad_net = pad->getPort(id_PAD);
        NPNR_ASSERT(pad_net != nullptr);
        std::string iostandard = str_or_default(pad->attrs, id_IOSTANDARD, "LVCMOS33");
        std::string pulltype = str_or_default(pad->attrs, id_PULLTYPE, "NONE");
        std::string slew = str_or_default(pad->attrs, id_SLEW, "SLOW");

        Loc ioLoc = uarch->rel_site_loc(uarch->get_bel_site(pad->bel));
        bool is_output = false, is_input = false;
        if (pad_net->driver.cell != nullptr)
            is_output = true;
        for (auto &usr : pad_net->users)
            if (boost::contains(usr.cell->type.str(ctx), "INBUF"))
                is_input = true;
        std::string tile = uarch->tile_name(pad->bel.tile);
        push(tile);

        if (boost::ends_with(iostandard, "_T_DCI")) {
            if (!warned_dci)
                log_warning("DCI is not supported, will be removed.\n");
            warned_dci = true;
            iostandard.erase(iostandard.size() - 6, iostandard.size());
        }

        bool is_riob18 = boost::starts_with(tile, "RIOB18_");
        bool is_sing = boost::contains(tile, "_SING_");
        bool is_top_sing = pad->bel.tile < uarch->hclk_for_iob(pad->bel);
        bool is_stepdown = false;
        bool is_lvcmos = boost::starts_with(iostandard, "LVCMOS");
        bool is_low_volt_lvcmos = iostandard == "LVCMOS12" || iostandard == "LVCMOS15" || iostandard == "LVCMOS18";

        auto yLoc = is_sing ? (is_top_sing ? 1 : 0) : (1 - ioLoc.y);
        push("IOB_Y" + std::to_string(yLoc));

        bool has_diff_prefix = boost::starts_with(iostandard, "DIFF_");
        bool is_tmds33 = iostandard == "TMDS_33";
        bool is_lvds25 = iostandard == "LVDS_25";
        bool is_lvds = boost::starts_with(iostandard, "LVDS");
        bool only_diff = is_tmds33 || is_lvds;
        bool is_diff = only_diff || has_diff_prefix;
        if (has_diff_prefix)
            iostandard.erase(0, 5);
        bool is_sstl = iostandard == "SSTL12" || iostandard == "SSTL135" || iostandard == "SSTL15";

        int hclk = uarch->hclk_for_iob(pad->bel);

        if (only_diff)
            ioconfig_by_hclk[hclk].only_diff = true;
        if (is_tmds33)
            ioconfig_by_hclk[hclk].tmds_33 = true;
        if (is_lvds25)
            ioconfig_by_hclk[hclk].lvds_25 = true;

        if (is_output) {
            // DRIVE
            int default_drive = (is_riob18 && iostandard == "LVCMOS12") ? 8 : 12;
            int drive = int_or_default(pad->attrs, id_DRIVE, default_drive);

            if ((iostandard == "LVCMOS33" || iostandard == "LVTTL") && is_riob18)
                log_error("high performance banks (RIOB18) do not support IO standard %s\n", iostandard.c_str());

            if (iostandard == "SSTL135")
                write_bit("SSTL135.DRIVE.I_FIXED");
            else if (is_riob18) {
                if ((iostandard == "LVCMOS18" || iostandard == "LVCMOS15"))
                    write_bit("LVCMOS15_LVCMOS18.DRIVE.I12_I16_I2_I4_I6_I8");
                else if (iostandard == "LVCMOS12")
                    write_bit("LVCMOS12.DRIVE.I2_I4_I6_I8");
                else if (iostandard == "LVDS")
                    write_bit("LVDS.DRIVE.I_FIXED");
                else if (is_sstl) {
                    write_bit(iostandard + ".DRIVE.I_FIXED");
                }
            } else { // IOB33
                if (iostandard == "TMDS_33" && yLoc == 0) {
                    write_bit("TMDS_33.DRIVE.I_FIXED");
                    write_bit("TMDS_33.OUT");
                } else if (iostandard == "LVDS_25" && yLoc == 0) {
                    write_bit("LVDS_25.DRIVE.I_FIXED");
                    write_bit("LVDS_25.OUT");
                } else if ((iostandard == "LVCMOS15" && drive == 16) || iostandard == "SSTL15")
                    write_bit("LVCMOS15_SSTL15.DRIVE.I16_I_FIXED");
                else if (iostandard == "LVCMOS18" && (drive == 12 || drive == 8))
                    write_bit("LVCMOS18.DRIVE.I12_I8");
                else if ((iostandard == "LVCMOS33" && drive == 16) || (iostandard == "LVTTL" && drive == 16))
                    write_bit("LVCMOS33_LVTTL.DRIVE.I12_I16");
                else if ((iostandard == "LVCMOS33" && (drive == 8 || drive == 12)) ||
                         (iostandard == "LVTTL" && (drive == 8 || drive == 12)))
                    write_bit("LVCMOS33_LVTTL.DRIVE.I12_I8");
                else if ((iostandard == "LVCMOS33" && drive == 4) || (iostandard == "LVTTL" && drive == 4))
                    write_bit("LVCMOS33_LVTTL.DRIVE.I4");
                else if (drive == 8 && (iostandard == "LVCMOS12" || iostandard == "LVCMOS25"))
                    write_bit("LVCMOS12_LVCMOS25.DRIVE.I8");
                else if (drive == 4 &&
                         (iostandard == "LVCMOS15" || iostandard == "LVCMOS18" || iostandard == "LVCMOS25"))
                    write_bit("LVCMOS15_LVCMOS18_LVCMOS25.DRIVE.I4");
                else if (is_lvcmos || iostandard == "LVTTL")
                    write_bit(iostandard + ".DRIVE.I" + std::to_string(drive));
            }

            // SSTL output used
            if (is_riob18 && is_sstl)
                write_bit(iostandard + ".IN_USE");

            // SLEW
            if (is_riob18 && slew == "SLOW") {
                if (iostandard == "SSTL135")
                    write_bit("SSTL135.SLEW.SLOW");
                else if (iostandard == "SSTL15")
                    write_bit("SSTL15.SLEW.SLOW");
                else
                    write_bit("LVCMOS12_LVCMOS15_LVCMOS18.SLEW.SLOW");
            } else if (slew == "SLOW") {
                if (iostandard != "LVDS_25" && iostandard != "TMDS_33")
                    write_bit("LVCMOS12_LVCMOS15_LVCMOS18_LVCMOS25_LVCMOS33_LVTTL_SSTL135_SSTL15.SLEW.SLOW");
            } else if (is_riob18)
                write_bit(iostandard + ".SLEW.FAST");
            else if (iostandard == "SSTL135" || iostandard == "SSTL15")
                write_bit("SSTL135_SSTL15.SLEW.FAST");
            else
                write_bit("LVCMOS12_LVCMOS15_LVCMOS18_LVCMOS25_LVCMOS33_LVTTL.SLEW.FAST");
        }

        if (is_input) {
            if (!is_diff) {
                if (iostandard == "LVCMOS33" || iostandard == "LVTTL" || iostandard == "LVCMOS25") {
                    if (!is_riob18)
                        write_bit("LVCMOS25_LVCMOS33_LVTTL.IN");
                    else
                        log_error("high performance banks (RIOB18) do not support IO standard %s\n",
                                  iostandard.c_str());
                }

                if (is_sstl) {
                    ioconfig_by_hclk[hclk].vref = true;
                    if (!is_riob18)
                        write_bit("SSTL135_SSTL15.IN");

                    if (is_riob18) {
                        write_bit("SSTL12_SSTL135_SSTL15.IN");
                    }

                    if (!is_riob18 && pad->attrs.count(id_IN_TERM))
                        write_bit("IN_TERM." + pad->attrs.at(id_IN_TERM).as_string());
                }

                if (is_low_volt_lvcmos) {
                    write_bit("LVCMOS12_LVCMOS15_LVCMOS18.IN");
                }
            } else /* is_diff */ {
                if (is_riob18) {
                    // vivado generates these bits only for Y0 of a diff pair
                    if (yLoc == 0) {
                        write_bit("LVDS_SSTL12_SSTL135_SSTL15.IN_DIFF");
                        if (iostandard == "LVDS")
                            write_bit("LVDS.IN_USE");
                    }
                } else {
                    if (iostandard == "TDMS_33")
                        write_bit("TDMS_33.IN_DIFF");
                    else
                        write_bit("LVDS_25_SSTL135_SSTL15.IN_DIFF");
                }

                if (pad->attrs.count(id_IN_TERM))
                    write_bit("IN_TERM." + pad->attrs.at(id_IN_TERM).as_string());
            }

            // IN_ONLY
            if (!is_output) {
                if (is_riob18) {
                    // vivado also sets this bit for DIFF_SSTL
                    if (is_diff && (yLoc == 0))
                        write_bit("LVDS.IN_ONLY");
                    else
                        write_bit("LVCMOS12_LVCMOS15_LVCMOS18_SSTL12_SSTL135_SSTL15.IN_ONLY");
                } else
                    write_bit("LVCMOS12_LVCMOS15_LVCMOS18_LVCMOS25_LVCMOS33_LVDS_25_LVTTL_SSTL135_SSTL15_TMDS_33.IN_"
                              "ONLY");
            }
        }

        if (!is_riob18 && (is_low_volt_lvcmos || is_sstl)) {
            if (iostandard == "SSTL12") {
                log_error("SSTL12 is only available on high performance banks.");
            }
            write_bit("LVCMOS12_LVCMOS15_LVCMOS18_SSTL135_SSTL15.STEPDOWN");
            ioconfig_by_hclk[hclk].stepdown = true;
            is_stepdown = true;
        }

        if (is_riob18 && (is_input || is_output) && (boost::contains(iostandard, "SSTL") || iostandard == "LVDS")) {
            if (((yLoc == 0) && (iostandard == "LVDS")) || boost::contains(iostandard, "SSTL")) {
                // TODO: I get bit conflicts with this, it seems to work anyway. Test more.
                // write_bit("LVDS.IN_USE");
            }
        }

        if (is_input && is_output && !is_diff && yLoc == 1) {
            if (is_riob18 && boost::starts_with(iostandard, "SSTL"))
                write_bit("SSTL12_SSTL135_SSTL15.IN");
        }

        // IN_TERM.NONE and IN_ONLY for TMDS_33 output, e.g. HDMI signals
        if (is_output && is_diff) {
            if (is_tmds33 && yLoc == 1) {
                if (pad->attrs.count(id_IN_TERM))
                    write_bit("IN_TERM." + pad->attrs.at(id_IN_TERM).as_string());
                else
                    write_bit("IN_TERM.NONE");
                write_bit("LVCMOS12_LVCMOS15_LVCMOS18_LVCMOS25_LVCMOS33_LVDS_25_LVTTL_SSTL135_SSTL15_TMDS_33.IN_ONLY");
            }
        }

        write_bit("PULLTYPE." + pulltype);
        pop(); // IOB_YN

        BelId inv;

        auto pad_bel_site = uarch->get_bel_site(pad->bel);

        if (is_riob18)
            inv = uarch->get_site_bel(pad_bel_site, ctx->id("IOB18S.O_ININV"));
        else
            inv = uarch->get_site_bel(pad_bel_site, ctx->id("IOB33S.O_ININV"));

        if (inv != BelId() && ctx->getBoundBelCell(inv) != nullptr)
            write_bit("OUT_DIFF");

        if (is_stepdown && !is_sing)
            write_bit("IOB_Y" + std::to_string(ioLoc.y) + ".LVCMOS12_LVCMOS15_LVCMOS18_SSTL135_SSTL15.STEPDOWN");

        pop(); // tile
    }

    void write_iol_config(CellInfo *ci)
    {
        std::string tile = uarch->tile_name(ci->bel.tile);
        push(tile);
        bool is_sing = boost::contains(tile, "_SING_");
        bool is_top_sing = ci->bel.tile < uarch->hclk_for_ioi(ci->bel.tile);

        auto site_key = uarch->get_bel_site(ci->bel);
        std::string site = uarch->get_site_name(site_key).str(ctx);
        std::string sitetype = site.substr(0, site.find('_'));
        Loc siteloc = uarch->rel_site_loc(site_key);
        push(stringf("%s_Y%d", sitetype.c_str(), is_sing ? (is_top_sing ? 1 : 0) : (1 - siteloc.y)));

        if (ci->type == id_ILOGICE3_IFF) {
            write_bit("IDDR.IN_USE");
            write_bit("IDDR_OR_ISERDES.IN_USE");
            write_bit("ISERDES.MODE.MASTER");
            write_bit("ISERDES.NUM_CE.N1");

            // Switch IDELMUXE3 to include the IDELAY element, if we have an IDELAYE2 driving D
            NetInfo *d = ci->getPort(id_D);
            if (d == nullptr || d->driver.cell == nullptr)
                log_error("%s '%s' has disconnected D input\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            CellInfo *drv = d->driver.cell;
            if (boost::contains(drv->type.str(ctx), "IDELAYE2"))
                write_bit("IDELMUXE3.P0");
            else
                write_bit("IDELMUXE3.P1");

            // Clock edge.  DDR_CLK_EDGE is a three-valued parameter encoded in
            // two bits, and the third value is the state where BOTH bits are
            // clear:
            //
            //     OPPOSITE_EDGE         !bit_a    bit_b
            //     SAME_EDGE               bit_a  !bit_b
            //     SAME_EDGE_PIPELINED   !bit_a  !bit_b
            //
            // So SAME_EDGE_PIPELINED is expressed by writing NEITHER of the
            // other two -- there is no third feature to write (adding one
            // only produces a FasmLookupError; no such key exists in any
            // database).  (Port of nextpnr-xilinx 9a6a7e3b.)
            std::string edge = str_or_default(ci->params, id_DDR_CLK_EDGE, "OPPOSITE_EDGE");
            if (edge == "SAME_EDGE")
                write_bit("IFF.DDR_CLK_EDGE.SAME_EDGE");
            else if (edge == "OPPOSITE_EDGE")
                write_bit("IFF.DDR_CLK_EDGE.OPPOSITE_EDGE");
            else if (edge == "SAME_EDGE_PIPELINED") { /* both bits clear: write nothing */
            } else
                log_error("unsupported clock edge parameter for cell '%s' at %s: %s. Supported are: SAME_EDGE, "
                          "OPPOSITE_EDGE and SAME_EDGE_PIPELINED",
                          ci->name.c_str(ctx), site.c_str(), edge.c_str());

            std::string srtype = str_or_default(ci->params, id_SRTYPE, "SYNC");
            if (srtype == "SYNC")
                write_bit("IFF.SRTYPE.SYNC");
            else
                write_bit("IFF.SRTYPE.ASYNC");

            write_bit("IFF.ZINV_C", !bool_or_default(ci->params, id_IS_CLK_INVERTED, false));
            write_bit("ZINV_D", !bool_or_default(ci->params, id_IS_D_INVERTED, false));

            // The IFF is physically a four-flop block shared with ISERDESE2;
            // an IDDR only exposes Q1/Q2, so Q3/Q4 were left unwritten -- and
            // on silicon that is observable (outputs read the wrong value
            // despite programmed INIT).  IDDR has no INIT_Q3/Q4 parameters,
            // so those default to 0.  (Port of nextpnr-xilinx d455ae52.)
            for (int i = 1; i <= 4; i++) {
                auto init = int_or_default(ci->params, ctx->id("INIT_Q" + std::to_string(i)), 0);
                if (init == 0)
                    write_bit("IFF.ZINIT_Q" + std::to_string(i));
            }

            auto sr_name = str_or_default(ci->attrs, id_X_ORIG_PORT_SR, "R");
            if (sr_name == "R") {
                for (int i = 1; i <= 4; i++)
                    write_bit("IFF.ZSRVAL_Q" + std::to_string(i));
            }
        } else if (ci->type.in(id_OLOGICE2_OUTFF, id_OLOGICE3_OUTFF)) {
            std::string edge = str_or_default(ci->params, id_DDR_CLK_EDGE, "OPPOSITE_EDGE");
            if (edge == "SAME_EDGE")
                write_bit("ODDR.DDR_CLK_EDGE.SAME_EDGE");

            write_bit("ODDR_TDDR.IN_USE");
            write_bit("OQUSED");
            write_bit("OSERDES.DATA_RATE_OQ.DDR");
            write_bit("OSERDES.DATA_RATE_TQ.BUF");

            std::string srtype = str_or_default(ci->params, id_SRTYPE, "SYNC");
            if (srtype == "SYNC")
                write_bit("OSERDES.SRTYPE.SYNC");

            for (std::string d : {"D1", "D2"})
                write_bit("IS_" + d + "_INVERTED",
                          bool_or_default(ci->params, ctx->id("IS_" + d + "_INVERTED"), false));

            auto init = int_or_default(ci->params, id_INIT, 1);
            if (init == 0)
                write_bit("ZINIT_OQ");

            write_bit("ODDR.SRUSED", ci->getPort(id_SR) != nullptr);
            auto sr_name = str_or_default(ci->attrs, id_X_ORIG_PORT_SR, "R");
            if (sr_name == "R")
                write_bit("ZSRVAL_OQ");

            auto clk_inv = bool_or_default(ci->params, id_IS_CLK_INVERTED);
            if (!clk_inv)
                write_bit("ZINV_CLK");
        } else if (ci->type == id_OSERDESE2_OSERDESE2) {
            write_bit("ODDR.DDR_CLK_EDGE.SAME_EDGE");
            write_bit("ODDR.SRUSED");
            write_bit("ODDR_TDDR.IN_USE");

            auto serdes_mode = str_or_default(ci->params, id_SERDES_MODE, "MASTER");
            bool is_cascaded = (serdes_mode == "SLAVE");

            // For cascaded OSERDESE2, OQUSED must be set even though OQ is not connected
            write_bit("OQUSED", is_cascaded || ci->getPort(id_OQ));
            write_bit("ZINV_CLK", !bool_or_default(ci->params, id_IS_CLK_INVERTED, false));
            for (std::string t : {"T1", "T2", "T3", "T4"})
                write_bit("ZINV_" + t, (ci->getPort(ctx->id(t)) != nullptr || t == "T1") &&
                                               !bool_or_default(ci->params, ctx->id("IS_" + t + "_INVERTED"), false));
            for (std::string d : {"D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8"})
                write_bit("IS_" + d + "_INVERTED",
                          bool_or_default(ci->params, ctx->id("IS_" + d + "_INVERTED"), false));
            write_bit("ZINIT_OQ", !bool_or_default(ci->params, id_INIT_OQ, false));
            write_bit("ZINIT_TQ", !bool_or_default(ci->params, id_INIT_TQ, false));
            write_bit("ZSRVAL_OQ", !bool_or_default(ci->params, id_SRVAL_OQ, false));
            write_bit("ZSRVAL_TQ", !bool_or_default(ci->params, id_SRVAL_TQ, false));

            push("OSERDES");
            write_bit("IN_USE");
            std::string type = str_or_default(ci->params, id_DATA_RATE_OQ, "BUF");
            write_bit(std::string("DATA_RATE_OQ.") + ((ci->getPort(id_OQ) != nullptr) ? type : "DDR"));
            write_bit(std::string("DATA_RATE_TQ.") +
                      ((ci->getPort(id_TQ) != nullptr) ? str_or_default(ci->params, id_DATA_RATE_TQ, "BUF") : "BUF"));
            int width = int_or_default(ci->params, id_DATA_WIDTH, 8);
#if 0
            write_bit("DATA_WIDTH.W" + std::to_string(width));
            if (type == "DDR" && (width == 6 || width == 8)) {
                write_bit("DATA_WIDTH.DDR.W6_8");
                write_bit("DATA_WIDTH.SDR.W2_4_5_6");
            } else if (type == "SDR" && (width == 2 || width == 4 || width == 5 || width == 6)) {
                write_bit("DATA_WIDTH.SDR.W2_4_5_6");
            }
#else
            if (type == "DDR")
                write_bit("DATA_WIDTH.DDR.W" + std::to_string(width));
            else if (type == "SDR")
                write_bit("DATA_WIDTH.SDR.W" + std::to_string(width));
            else
                write_bit("DATA_WIDTH.W" + std::to_string(width));
#endif
            write_bit("SRTYPE.SYNC");
            write_bit("TSRTYPE.SYNC");
            // TRISTATE_WIDTH is a two-valued parameter whose W1 state is
            // encoded as the absence of bits, so only W4 appears in segbits.
            // Never writing it left every OSERDESE2 programmed as
            // TRISTATE_WIDTH=1, silently overriding the cell's own parameter
            // (defaults to 4 in the library).
            // (Port of nextpnr-xilinx c05f0d05.)
            if (int_or_default(ci->params, ctx->id("TRISTATE_WIDTH"), 4) == 4)
                write_bit("TRISTATE_WIDTH.W4");
            if (is_cascaded)
                write_bit("SERDES_MODE.SLAVE");
            pop();
            // An explicitly requested CLKDIV inversion on an OSERDESE2 was
            // discarded: the bit (OLOGIC_Y*.IS_CLKDIV_INVERTED) was written
            // nowhere.  It sits on the OLOGIC, not inside the OSERDES prefix.
            // (Port of nextpnr-xilinx b9ed05a2.)
            write_bit("IS_CLKDIV_INVERTED",
                      bool_or_default(ci->params, ctx->id("IS_CLKDIV_INVERTED"), false));
        } else if (ci->type == id_ISERDESE2_ISERDESE2) {
            std::string data_rate = str_or_default(ci->params, id_DATA_RATE);
            write_bit("IDDR_OR_ISERDES.IN_USE");
            if (data_rate == "DDR")
                write_bit("IDDR.IN_USE");
            write_bit("IFF.DDR_CLK_EDGE.OPPOSITE_EDGE");
            write_bit("IFF.SRTYPE.SYNC");
            for (int i = 1; i <= 4; i++) {
                write_bit("IFF.ZINIT_Q" + std::to_string(i),
                          !bool_or_default(ci->params, ctx->idf("INIT_Q%d", i), false));
                write_bit("IFF.ZSRVAL_Q" + std::to_string(i),
                          !bool_or_default(ci->params, ctx->idf("SRVAL_Q%d", i), false));
            }
            write_bit("IFF.ZINV_C", !bool_or_default(ci->params, id_IS_CLK_INVERTED, false));
            // INV_OCLK and ZINV_OCLK are two DISTINCT physical bits; the
            // fuzzer sets them as exact complements, so exactly one of the
            // two must always be set.  Writing only the Z half left
            // IS_OCLK_INVERTED=TRUE with neither bit set, an unprogrammed
            // state that corresponds to no value of the parameter.
            // (Port of nextpnr-xilinx c05f0d05.)
            bool oclk_inv = bool_or_default(ci->params, id_IS_OCLK_INVERTED, false);
            write_bit("IFF.INV_OCLK", oclk_inv);
            write_bit("IFF.ZINV_OCLK", !oclk_inv);

            std::string iobdelay = str_or_default(ci->params, id_IOBDELAY, "NONE");
            write_bit("IFFDELMUXE3.P0", (iobdelay == "IFD"));
            write_bit("ZINV_D", !bool_or_default(ci->params, id_IS_D_INVERTED, false) && (iobdelay != "IFD"));

            push("ISERDES");
            write_bit("IN_USE");
            int width = int_or_default(ci->params, id_DATA_WIDTH, 8);
            std::string mode = str_or_default(ci->params, id_INTERFACE_TYPE, "NETWORKING");
            std::string rate = str_or_default(ci->params, id_DATA_RATE, "DDR");
            write_bit(mode + "." + rate + ".W" + std::to_string(width));
            write_bit("MODE." + str_or_default(ci->params, id_SERDES_MODE, "MASTER"));
            write_bit("NUM_CE.N" + std::to_string(int_or_default(ci->params, id_NUM_CE, 1)));
            pop();
        } else if (ci->type == id_IDELAYE2_IDELAYE2) {
            write_bit("IN_USE");
            write_bit("CINVCTRL_SEL", str_or_default(ci->params, id_CINVCTRL_SEL, "FALSE") == "TRUE");
            write_bit("PIPE_SEL", str_or_default(ci->params, id_PIPE_SEL, "FALSE") == "TRUE");
            write_bit("HIGH_PERFORMANCE_MODE", str_or_default(ci->params, id_HIGH_PERFORMANCE_MODE, "FALSE") == "TRUE");
            write_bit("DELAY_SRC_" + str_or_default(ci->params, id_DELAY_SRC, "IDATAIN"));
            write_bit("IDELAY_TYPE_" + str_or_default(ci->params, id_IDELAY_TYPE, "FIXED"));
            write_int_vector("IDELAY_VALUE[4:0]", int_or_default(ci->params, id_IDELAY_VALUE, 0), 5, false);
            write_int_vector("ZIDELAY_VALUE[4:0]", int_or_default(ci->params, id_IDELAY_VALUE, 0), 5, true);
            write_bit("IS_DATAIN_INVERTED", bool_or_default(ci->params, id_IS_DATAIN_INVERTED, false));
            write_bit("IS_IDATAIN_INVERTED", bool_or_default(ci->params, id_IS_IDATAIN_INVERTED, false));
        } else if (ci->type == id_ODELAYE2_ODELAYE2) {
            write_bit("IN_USE");
            write_bit("CINVCTRL_SEL", str_or_default(ci->params, id_CINVCTRL_SEL, "FALSE") == "TRUE");
            write_bit("HIGH_PERFORMANCE_MODE", str_or_default(ci->params, id_HIGH_PERFORMANCE_MODE, "FALSE") == "TRUE");
            auto type = str_or_default(ci->params, id_ODELAY_TYPE, "FIXED");
            if (type != "FIXED")
                write_bit("ODELAY_TYPE_" + type);
            write_int_vector("ODELAY_VALUE[4:0]", int_or_default(ci->params, id_ODELAY_VALUE, 0), 5, false);
            write_int_vector("ZODELAY_VALUE[4:0]", int_or_default(ci->params, id_ODELAY_VALUE, 0), 5, true);
            write_bit("ZINV_ODATAIN", !bool_or_default(ci->params, id_IS_ODATAIN_INVERTED, false));
        } else {
            NPNR_ASSERT_FALSE("unsupported IOLOGIC");
        }
        pop(2);
    }

    void write_io()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_PAD) {
                // GT pads are configured by the GT writers, not the IO path
                // (their tiles have no HCLK/IOI structure)
                if (ci->bel != BelId()) {
                    // GT pads live in the GT/OPAD tiles and are configured by
                    // the GT writers, not the IO path
                    std::string belname = ctx->nameOfBel(ci->bel);
                    if (boost::contains(belname, "OPAD") || boost::contains(belname, "IPAD") ||
                        boost::contains(belname, "GTPE2_") || boost::contains(belname, "GTXE2_")) {
                        blank();
                        continue;
                    }
                }
                write_io_config(ci);
                blank();
            } else if (ci->type.in(id_ILOGICE3_IFF, id_OLOGICE2_OUTFF, id_OLOGICE3_OUTFF, id_OSERDESE2_OSERDESE2,
                                   id_ISERDESE2_ISERDESE2, id_IDELAYE2_IDELAYE2, id_ODELAYE2_ODELAYE2)) {
                write_iol_config(ci);
                blank();
            }
        }
        for (auto &hclk : ioconfig_by_hclk) {
            push(uarch->tile_name(hclk.first));
            write_bit("STEPDOWN", hclk.second.stepdown);
            write_bit("VREF.V_675_MV", hclk.second.vref);
            write_bit("ONLY_DIFF_IN_USE", hclk.second.only_diff);
            write_bit("TMDS_33_IN_USE", hclk.second.tmds_33);
            write_bit("LVDS_25_IN_USE", hclk.second.lvds_25);
            pop();
        }
    }

    std::vector<std::string> used_wires_starting_with(int tile, const std::string &prefix, bool is_source)
    {
        std::vector<std::string> wires;
        if (!pips_by_tile.count(tile))
            return wires;
        for (auto pip : pips_by_tile[tile]) {
            auto &pd = chip_pip_info(ctx->chip_info, pip);
            int wire_index = is_source ? pd.src_wire : pd.dst_wire;
            std::string wire = IdString(chip_wire_info(ctx->chip_info, WireId(pip.tile, wire_index)).name).str(ctx);
            if (boost::starts_with(wire, prefix))
                wires.push_back(wire);
        }
        return wires;
    }

    void write_clocking()
    {
        std::string name, type;

        std::set<std::string> all_gclk;
        dict<int, std::set<std::string>> hclk_by_row;

        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_BUFGCTRL) {
                push(uarch->tile_name(ci->bel.tile));
                auto xy = uarch->rel_site_loc(uarch->get_bel_site(ci->bel));
                push(stringf("BUFGCTRL.BUFGCTRL_X%dY%d", xy.x, xy.y));
                write_bit("IN_USE");
                write_bit("INIT_OUT", bool_or_default(ci->params, id_INIT_OUT));
                write_bit("IS_IGNORE0_INVERTED", bool_or_default(ci->params, id_IS_IGNORE0_INVERTED));
                write_bit("IS_IGNORE1_INVERTED", bool_or_default(ci->params, id_IS_IGNORE1_INVERTED));
                write_bit("ZINV_CE0", !bool_or_default(ci->params, id_IS_CE0_INVERTED));
                write_bit("ZINV_CE1", !bool_or_default(ci->params, id_IS_CE1_INVERTED));
                write_bit("ZINV_S0", !bool_or_default(ci->params, id_IS_S0_INVERTED));
                write_bit("ZINV_S1", !bool_or_default(ci->params, id_IS_S1_INVERTED));
                pop(2);
            } else if (ci->type == id_BUFHCE_BUFHCE) {
                push(uarch->tile_name(ci->bel.tile));
                auto xy = uarch->rel_site_loc(uarch->get_bel_site(ci->bel));
                push(stringf("BUFHCE.BUFHCE_X%dY%d", xy.x, xy.y));
                write_bit("IN_USE");
                write_bit("CE_TYPE.ASYNC", str_or_default(ci->params, id_CE_TYPE, "SYNC") == "ASYNC");
                write_bit("INIT_OUT", bool_or_default(ci->params, id_INIT_OUT));
                // CE is tied active by prepare_clocking; the physical pin is
                // active-low, so invert it (port of nextpnr-xilinx fasm.cc)
                write_bit("ZINV_CE", !bool_or_default(ci->params, id_IS_CE_INVERTED));
                pop(2);
            } else if (ci->type == id_PLLE2_ADV_PLLE2_ADV) {
                write_pll(ci);
            } else if (ci->type == id_MMCME2_ADV_MMCME2_ADV) {
                write_mmcm(ci);
            }
            blank();
        }

        for (int tile = 0; tile < ctx->chip_info->tile_insts.ssize(); tile++) {
            std::string name = uarch->tile_name(tile);
            std::string type = ctx->get_tile_type(tile).str(ctx);
            push(name);
            if (type == "HCLK_L" || type == "HCLK_R" || type == "HCLK_L_BOT_UTURN" || type == "HCLK_R_BOT_UTURN") {
                auto used_sources = used_wires_starting_with(tile, "HCLK_CK_", true);
                push("ENABLE_BUFFER");
                for (auto s : used_sources) {
                    if (boost::contains(s, "BUFHCLK")) {
                        write_bit(s);
                        hclk_by_row[tile / ctx->chip_info->width].insert(s.substr(s.find("BUFHCLK")));
                    }
                }
                pop();
            } else if (boost::starts_with(type, "CLK_HROW")) {
                auto used_gclk = used_wires_starting_with(tile, "CLK_HROW_R_CK_GCLK", true);
                auto used_ck_in = used_wires_starting_with(tile, "CLK_HROW_CK_IN", true);
                for (auto s : used_gclk) {
                    write_bit(s + "_ACTIVE");
                    all_gclk.insert(s.substr(s.find("GCLK")));
                }
                for (auto s : used_ck_in) {
                    if (boost::contains(s, "HROW_CK_INT"))
                        continue;
                    write_bit(s + "_ACTIVE");
                }
            } else if (boost::starts_with(type, "HCLK_CMT")) {
                auto used_ccio = used_wires_starting_with(tile, "HCLK_CMT_CCIO", true);
                for (auto s : used_ccio) {
                    write_bit(s + "_ACTIVE");
                    write_bit(s + "_USED");
                }
                auto used_hclk = used_wires_starting_with(tile, "HCLK_CMT_CK_", true);
                for (auto s : used_hclk) {
                    if (boost::contains(s, "BUFHCLK")) {
                        write_bit(s + "_USED");
                        hclk_by_row[tile / ctx->chip_info->width].insert(s.substr(s.find("BUFHCLK")));
                    }
                }
            }
            pop();
            blank();
        }

        for (int tile = 0; tile < ctx->chip_info->tile_insts.ssize(); tile++) {
            std::string name = uarch->tile_name(tile);
            std::string type = ctx->get_tile_type(tile).str(ctx);
            push(name);
            if (type == "CLK_BUFG_REBUF") {
                for (auto &gclk : all_gclk) {
                    write_bit(gclk + "_ENABLE_ABOVE");
                    write_bit(gclk + "_ENABLE_BELOW");
                }
            } else if (boost::starts_with(type, "HCLK_CMT")) {
                for (auto &hclk : hclk_by_row[tile / ctx->chip_info->width]) {
                    write_bit("HCLK_CMT_CK_" + hclk + "_USED");
                }
            }
            pop();
            blank();
        }
    }

    void write_bram_width(CellInfo *ci, const std::string &name, bool is_36, bool is_y1)
    {
        // SDP mode spans both data sides of the memory, so the
        // "opposite-side" width markers must also be configured wide,
        // exactly like Vivado's golden bitstreams:
        //   - RAMB36E1 READ_WIDTH_A=72: the 72-bit read is the A port (lower
        //     36 bits) PLUS the B port (upper 36 bits) -> READ_WIDTH_B_18 on
        //     BOTH RAMB18 halves.  yosys leaves READ_WIDTH_B at 0; the
        //     width-1 default kills the B-side read path on silicon.
        //   - RAMB18E1 READ_WIDTH_A=36: READ_WIDTH_B_18.
        //   - RAMB18E1 WRITE_WIDTH_B=36: WRITE_WIDTH_A_18.
        // (Port of nextpnr-xilinx f1c77134.)
        const int read_width_a = int_or_default(ci->params, ctx->id("READ_WIDTH_A"), 0);
        const int write_width_b = int_or_default(ci->params, ctx->id("WRITE_WIDTH_B"), 0);
        const int this_width = int_or_default(ci->params, ctx->id(name), 0);
        const bool this_width_param_is_unset = (this_width == 0);
        const bool this_param_is_read_width_b = (name == "READ_WIDTH_B");
        const bool this_param_is_write_width_a = (name == "WRITE_WIDTH_A");
        const bool b_side_reads_half_the_word = (is_36 ? (read_width_a == 72) : (read_width_a == 36));
        const bool a_side_writes_half_the_word = (!is_36 && (write_width_b == 36));

        if (this_width_param_is_unset && this_param_is_read_width_b && b_side_reads_half_the_word) {
            write_bit("READ_WIDTH_B_18");
            return;
        }
        if (this_width_param_is_unset && this_param_is_write_width_a && a_side_writes_half_the_word) {
            write_bit("WRITE_WIDTH_A_18");
            return;
        }

        int width = this_width;
        if (width == 0)
            return;
        int actual_width = width;
        if (is_36) {
            if (width == 1)
                actual_width = 1;
            else
                actual_width = width / 2;
        }
        if (((is_36 && width == 72) || (is_y1 && actual_width == 36)) && name == "READ_WIDTH_A") {
            write_bit(name + "_18");
        }
        if (actual_width == 36) {
            write_bit("SDP_" + name.substr(0, name.length() - 2) + "_36");
            // The 36-wide mode lives in the SDP bit plus the marker of the
            // WIDE side only.  Writing the other side's 18 marker collides
            // with that side's own width field (e.g. a RAMB18E1 with
            // READ_WIDTH_A=36 and an unused B port: READ_WIDTH_B_18 and the
            // B-side default READ_WIDTH_B_1 encode the SAME prjxray bit).
            // (Port of nextpnr-xilinx 1b7d51b9.)
            if (name == "WRITE_WIDTH_A" || name == "WRITE_WIDTH_B")
                write_bit(name.substr(0, name.size() - 1) + ((name == "WRITE_WIDTH_B") ? "B_18" : "A_18"));
            else if (name == "READ_WIDTH_B")
                write_bit(name.substr(0, name.size() - 1) + "B_18");
        } else {
            // A 36-bit (SDP) port already emits the _18 bits for BOTH the A
            // and B halves of its direction; the paired port of the same
            // direction is unused in SDP mode and defaults to width 1, which
            // would emit a conflicting _1 bit for a half the SDP branch set
            // to _18.  Skip it.  (Port of nextpnr-xilinx 11f9b694.)
            bool dir_is_sdp36 = false;
            {
                std::string dir = name.substr(0, name.size() - 2);
                for (const char *ab : {"A", "B"}) {
                    int w = int_or_default(ci->params, ctx->id(dir + "_" + ab), 0);
                    int aw = (is_36 && w != 1 && w != 0) ? w / 2 : w;
                    if (aw == 36)
                        dir_is_sdp36 = true;
                }
            }
            if (dir_is_sdp36 && actual_width == 1)
                return;
            write_bit(name + "_" + std::to_string(actual_width));
        }
    }

    void write_bram_init(int half, CellInfo *ci, bool is_36)
    {
        for (std::string mode : {"", "P"}) {
            for (int i = 0; i < (mode == "P" ? 8 : 64); i++) {
                bool has_init = false;
                std::vector<bool> init_data(256, false);
                if (is_36) {
                    for (int j = 0; j < 2; j++) {
                        IdString param = ctx->idf("INIT%s_%02X", mode.c_str(), i * 2 + j);
                        if (ci->params.count(param)) {
                            auto &init0 = ci->params.at(param);
                            has_init = true;
                            for (int k = half; k < 256; k += 2) {
                                if (k >= int(init0.str.size()))
                                    break;
                                init_data[j * 128 + (k / 2)] = init0.str[k] == Property::S1;
                            }
                        }
                    }
                } else {
                    IdString param = ctx->idf("INIT%s_%02X", mode.c_str(), i);
                    if (ci->params.count(param)) {
                        auto &init = ci->params.at(param);
                        has_init = true;
                        for (int k = 0; k < 256; k++) {
                            if (k >= int(init.str.size()))
                                break;
                            init_data[k] = init.str[k] == Property::S1;
                        }
                    }
                }
                if (has_init)
                    write_vector(stringf("INIT%s_%02X[255:0]", mode.c_str(), i), init_data);
            }
        }
    }

    void write_bram_half(int tile, int half, CellInfo *ci)
    {
        push(uarch->tile_name(tile));
        push("RAMB18_Y" + std::to_string(half));
        if (ci != nullptr) {
            bool is_36 = ci->type == id_RAMB36E1_RAMB36E1;
            write_bit("IN_USE");
            write_bram_width(ci, "READ_WIDTH_A", is_36, half == 1);
            write_bram_width(ci, "READ_WIDTH_B", is_36, half == 1);
            write_bram_width(ci, "WRITE_WIDTH_A", is_36, half == 1);
            write_bram_width(ci, "WRITE_WIDTH_B", is_36, half == 1);
            write_bit("DOA_REG", bool_or_default(ci->params, id_DOA_REG, false));
            write_bit("DOB_REG", bool_or_default(ci->params, id_DOB_REG, false));
            for (auto &invpin : invertible_pins[ctx->id(ci->attrs[id_X_ORIG_TYPE].as_string())])
                write_bit("ZINV_" + invpin.str(ctx),
                          !bool_or_default(ci->params, ctx->id("IS_" + invpin.str(ctx) + "_INVERTED"), false));
            // REGCLKARDRCLK and REGCLKB are SITE pins, not RAMB18E1/RAMB36E1
            // cell ports, so they are absent from invertible_pins.  prjxray's
            // fuzzer rule: with DO*_REG == 1 the output-register clock FOLLOWS
            // the corresponding data clock, with DO*_REG == 0 it is always
            // inverted -- i.e. tag 0, the bit clear, which is what emitting
            // nothing already gives.  So only the registered case needs
            // anything written.  (Port of nextpnr-xilinx e71acda2.)
            if (bool_or_default(ci->params, ctx->id("DOA_REG"), false))
                write_bit("ZINV_REGCLKARDRCLK",
                          !bool_or_default(ci->params, ctx->id("IS_CLKARDCLK_INVERTED"), false));
            if (bool_or_default(ci->params, ctx->id("DOB_REG"), false))
                write_bit("ZINV_REGCLKB",
                          !bool_or_default(ci->params, ctx->id("IS_CLKBWRCLK_INVERTED"), false));
            for (auto wrmode : {"WRITE_MODE_A", "WRITE_MODE_B"}) {
                std::string mode = str_or_default(ci->params, ctx->id(wrmode), "WRITE_FIRST");
                if (mode != "WRITE_FIRST")
                    write_bit(std::string(wrmode) + "_" + mode);
            }
            write_vector("ZINIT_A[17:0]", std::vector<bool>(18, true));
            write_vector("ZINIT_B[17:0]", std::vector<bool>(18, true));
            write_vector("ZSRVAL_A[17:0]", std::vector<bool>(18, true));
            write_vector("ZSRVAL_B[17:0]", std::vector<bool>(18, true));

            write_bram_init(half, ci, is_36);
        }
        pop();
        if (half == 0) {
            auto used_rdaddrcasc = used_wires_starting_with(tile, "BRAM_CASCOUT_ADDRARDADDR", false);
            auto used_wraddrcasc = used_wires_starting_with(tile, "BRAM_CASCOUT_ADDRBWRADDR", false);
            write_bit("CASCOUT_ARD_ACTIVE", !used_rdaddrcasc.empty());
            write_bit("CASCOUT_BWR_ACTIVE", !used_wraddrcasc.empty());
        }
        pop();
    }

    void write_bram()
    {
        for (int tile = 0; tile < ctx->chip_info->tile_insts.ssize(); tile++) {
            IdString type = ctx->get_tile_type(tile);
            if (type.in(id_BRAM_L, id_BRAM_R)) {
                CellInfo *l = nullptr, *u = nullptr;
                const auto &bts = uarch->tile_status[tile].bts;
                if (bts) {
                    if (bts->cells[BEL_RAM36] != nullptr) {
                        l = bts->cells[BEL_RAM36];
                        u = bts->cells[BEL_RAM36];
                    } else {
                        l = bts->cells[BEL_RAM18_L];
                        u = bts->cells[BEL_RAM18_U];
                    }
                }
                write_bram_half(tile, 0, l);
                write_bram_half(tile, 1, u);
                blank();
            }
        }
    }

    double float_or_default(CellInfo *ci, const std::string &name, double def)
    {
        IdString p = ctx->id(name);
        if (!ci->params.count(p))
            return def;
        auto &prop = ci->params.at(p);
        if (prop.is_string)
            return std::stod(prop.as_string());
        else
            return prop.as_int64();
    }

    void write_pll_clkout(const std::string &name, CellInfo *ci)
    {
        // FIXME: variable duty cycle
        int high = 1, low = 1, phasemux = 0, delaytime = 0, frac = 0;
        bool no_count = false, edge = false;
        double divide = float_or_default(ci, name + ((name == "CLKFBOUT") ? "_MULT" : "_DIVIDE"), 1);
        double phase = float_or_default(ci, name + "_PHASE", 1);
        if (divide <= 1) {
            no_count = true;
        } else {
            high = floor(divide / 2);
            low = int(floor(divide) - high);
            if (high != low)
                edge = true;
            if (name == "CLKOUT1" || name == "CLKFBOUT")
                frac = floor(divide * 8) - floor(divide) * 8;
            int phase_eights = floor((phase / 360) * divide * 8);
            phasemux = phase_eights % 8;
            delaytime = phase_eights / 8;
        }
        bool used = false;
        if (name == "DIVCLK" || name == "CLKFBOUT") {
            used = true;
        } else {
            used = ci->getPort(ctx->id(name)) != nullptr;
        }
        if (name == "DIVCLK") {
            write_int_vector("DIVCLK_DIVCLK_HIGH_TIME[5:0]", high, 6);
            write_int_vector("DIVCLK_DIVCLK_LOW_TIME[5:0]", low, 6);
            write_bit("DIVCLK_DIVCLK_EDGE[0]", edge);
            write_bit("DIVCLK_DIVCLK_NO_COUNT[0]", no_count);
        } else if (used) {
            write_bit(name + "_CLKOUT1_OUTPUT_ENABLE[0]");
            write_int_vector(name + "_CLKOUT1_HIGH_TIME[5:0]", high, 6);
            write_int_vector(name + "_CLKOUT1_LOW_TIME[5:0]", low, 6);
            write_int_vector(name + "_CLKOUT1_PHASE_MUX[2:0]", phasemux, 3);
            write_bit(name + "_CLKOUT2_EDGE[0]", edge);
            write_bit(name + "_CLKOUT2_NO_COUNT[0]", no_count);
            write_int_vector(name + "_CLKOUT2_DELAY_TIME[5:0]", delaytime, 6);
            if (frac != 0) {
                write_bit(name + "_CLKOUT2_FRAC_EN[0]", edge);
                write_int_vector(name + "_CLKOUT2_FRAC[2:0]", frac, 3);
            }
        }
    }

    void write_pll(CellInfo *ci)
    {
        push(uarch->tile_name(ci->bel.tile));
        push("PLLE2_ADV");
        write_bit("IN_USE");
        // FIXME: should be INV not ZINV (XRay error?)
        write_bit("ZINV_PWRDWN", bool_or_default(ci->params, id_IS_PWRDWN_INVERTED, false));
        write_bit("ZINV_RST", bool_or_default(ci->params, id_IS_RST_INVERTED, false));
        write_bit("INV_CLKINSEL", bool_or_default(ci->params, id_IS_CLKINSEL_INVERTED, false));
        write_pll_clkout("DIVCLK", ci);
        write_pll_clkout("CLKFBOUT", ci);
        write_pll_clkout("CLKOUT0", ci);
        write_pll_clkout("CLKOUT1", ci);
        write_pll_clkout("CLKOUT2", ci);
        write_pll_clkout("CLKOUT3", ci);
        write_pll_clkout("CLKOUT4", ci);
        write_pll_clkout("CLKOUT5", ci);

        std::string comp = str_or_default(ci->params, id_COMPENSATION, "INTERNAL");
        push("COMPENSATION");
        if (comp == "INTERNAL") {
            // write_bit("INTERNAL");
            write_bit("Z_ZHOLD_OR_CLKIN_BUF");
        } else {
            NPNR_ASSERT_FALSE("unsupported compensation type");
        }
        pop();

        // PLLE2 lock & loop-filter configuration, from CLKFBOUT_MULT.
        //
        // These are PLL-specific tables harvested from Vivado golden
        // bitstreams (one minimal PLLE2_ADV design per CLKFBOUT_MULT 2..64,
        // disassembled with prjxray bit2fasm):
        //  - the values depend on CLKFBOUT_MULT only;
        //  - LKTABLE matches the XAPP888 MMCM lock table only up to
        //    MULT=10; from MULT=11 the PLL table diverges (delay fields
        //    saturate at 31, LockCnt decreases per MULT).  The MMCM lock
        //    table must not be reused here;
        //  - TABLE (loop filter: CP/RES/LFHF) varies across the whole
        //    range; the old hardcoded value is Vivado's for MULT=4 only.
        //    A wrong loop filter yields a clock clean enough for a
        //    free-running counter but too jittery for synchronous logic;
        //  - BANDWIDTH=HIGH programs the same filter as OPTIMIZED; LOW has
        //    its own filter table; LKTABLE does not depend on BANDWIDTH.
        // (Port of nextpnr-xilinx e33b5f1a/74357a79.)
        static const int64_t plle2_lock_table[63] = {
                0x31BE8FA401LL, 0x423E8FA401LL, 0x5AFE8FA401LL, 0x73BE8FA401LL,
                0x8C7E8FA401LL, 0x9CFE8FA401LL, 0xB5BE8FA401LL, 0xCE7E8FA401LL,
                0xE73E8FA401LL, 0xFFF84FA401LL, 0xFFF39FA401LL, 0xFFEEEFA401LL,
                0xFFEBCFA401LL, 0xFFE8AFA401LL, 0xFFE71FA401LL, 0xFFE3FFA401LL,
                0xFFE26FA401LL, 0xFFE0DFA401LL, 0xFFDF4FA401LL, 0xFFDDBFA401LL,
                0xFFDC2FA401LL, 0xFFDA9FA401LL, 0xFFD90FA401LL, 0xFFD90FA401LL,
                0xFFD77FA401LL, 0xFFD5EFA401LL, 0xFFD5EFA401LL, 0xFFD45FA401LL,
                0xFFD45FA401LL, 0xFFD2CFA401LL, 0xFFD2CFA401LL, 0xFFD2CFA401LL,
                0xFFD13FA401LL, 0xFFD13FA401LL, 0xFFD13FA401LL, 0xFFCFAFA401LL,
                0xFFCFAFA401LL, 0xFFCFAFA401LL, 0xFFCFAFA401LL, 0xFFCFAFA401LL,
                0xFFCFAFA401LL, 0xFFCFAFA401LL, 0xFFCFAFA401LL, 0xFFCFAFA401LL,
                0xFFCFAFA401LL, 0xFFCFAFA401LL, 0xFFCFAFA401LL, 0xFFCFAFA401LL,
                0xFFCFAFA401LL, 0xFFCFAFA401LL, 0xFFCFAFA401LL, 0xFFCFAFA401LL,
                0xFFCFAFA401LL, 0xFFCFAFA401LL, 0xFFCFAFA401LL, 0xFFCFAFA401LL,
                0xFFCFAFA401LL, 0xFFCFAFA401LL, 0xFFCFAFA401LL, 0xFFCFAFA401LL,
                0xFFCFAFA401LL, 0xFFCFAFA401LL, 0xFFCFAFA401LL};
        static const uint16_t plle2_filter_optimized[63] = {
                0x0DC, 0x17C, 0x1FC, 0x1EC, 0x35C, 0x3AC, 0x3B4, 0x3F4,
                0x3DC, 0x3EC, 0x3F4, 0x3CC, 0x394, 0x3D4, 0x3D4, 0x3D4,
                0x3D4, 0x1D8, 0x1D8, 0x1D8, 0x1D8, 0x170, 0x170, 0x170,
                0x304, 0x304, 0x304, 0x304, 0x304, 0x304, 0x304, 0x304,
                0x108, 0x108, 0x108, 0x0A0, 0x0A0, 0x0A0, 0x0D0, 0x0A0,
                0x0A0, 0x0A0, 0x0A0, 0x0A0, 0x0A0, 0x0A0, 0x0A0, 0x0A0,
                0x0A0, 0x0A0, 0x0A0, 0x0A0, 0x130, 0x130, 0x130, 0x130,
                0x130, 0x130, 0x130, 0x090, 0x090, 0x090, 0x090};
        static const uint16_t plle2_filter_low[63] = {
                0x0BC, 0x09C, 0x0B4, 0x094, 0x094, 0x0A4, 0x0B8, 0x0B8,
                0x084, 0x084, 0x098, 0x098, 0x098, 0x098, 0x0A8, 0x0A8,
                0x0A8, 0x0A8, 0x0B0, 0x0B0, 0x0B0, 0x0B0, 0x0B0, 0x0B0,
                0x0B0, 0x0B0, 0x0B0, 0x0B0, 0x0B0, 0x088, 0x088, 0x088,
                0x088, 0x088, 0x088, 0x088, 0x088, 0x088, 0x088, 0x0F0,
                0x0F0, 0x0F0, 0x0F0, 0x0F0, 0x0F0, 0x0F0, 0x090, 0x090,
                0x090, 0x090, 0x090, 0x090, 0x090, 0x090, 0x090, 0x090,
                0x090, 0x090, 0x090, 0x090, 0x090, 0x090, 0x090};
        int pll_mult = (int)float_or_default(ci, "CLKFBOUT_MULT", 1);
        if (pll_mult < 2)
            pll_mult = 2;
        if (pll_mult > 64)
            pll_mult = 64;
        std::string pll_bw = str_or_default(ci->params, ctx->id("BANDWIDTH"), "OPTIMIZED");
        write_int_vector("FILTREG1_RESERVED[11:0]", 0x8, 12);
        write_int_vector("LKTABLE[39:0]", plle2_lock_table[pll_mult - 2], 40);
        write_bit("LOCKREG3_RESERVED[0]");
        write_int_vector("TABLE[9:0]", (pll_bw == "LOW") ? plle2_filter_low[pll_mult - 2]
                                                         : plle2_filter_optimized[pll_mult - 2],
                         10);
        pop(2);
    }

    void write_mmcm_clkout(const std::string &name, CellInfo *ci)
    {
        // FIXME: variable duty cycle
        int high = 1, low = 1, phasemux = 0, delaytime = 0, frac = 0;
        bool no_count = false, edge = false;
        double divide = float_or_default(
                ci, name + ((name == "CLKFBOUT") ? "_MULT_F" : (name == "CLKOUT0" ? "_DIVIDE_F" : "_DIVIDE")), 1);
        double phase = float_or_default(ci, name + "_PHASE", 1);
        if (divide <= 1) {
            no_count = true;
        } else {
            high = floor(divide / 2);
            low = int(floor(divide) - high);
            if (high != low)
                edge = true;
            if (name == "CLKOUT0" || name == "CLKFBOUT")
                frac = floor(divide * 8) - floor(divide) * 8;
            int phase_eights = floor((phase / 360) * divide * 8);
            phasemux = phase_eights % 8;
            delaytime = phase_eights / 8;
        }
        bool used = false;
        if (name == "DIVCLK" || name == "CLKFBOUT") {
            used = true;
        } else {
            used = ci->getPort(ctx->id(name)) != nullptr;
        }
        if (name == "DIVCLK") {
            write_int_vector("DIVCLK_DIVCLK_HIGH_TIME[5:0]", high, 6);
            write_int_vector("DIVCLK_DIVCLK_LOW_TIME[5:0]", low, 6);
            write_bit("DIVCLK_DIVCLK_EDGE[0]", edge);
            write_bit("DIVCLK_DIVCLK_NO_COUNT[0]", no_count);
        } else if (used) {
            auto is_clkout_5_or_6 = name == "CLKOUT5" || name == "CLKOUT6";
            auto is_clkout0 = name == "CLKOUT0";
            auto is_clkfbout = name == "CLKFBOUT";

            if ((is_clkout0 || is_clkfbout) && frac != 0) {
                --high;
                --low;

                auto frac_shifted = frac >> 1;
                // CLKOUT0 controls CLKOUT5_CLKOUT2, CLKFBOUT controls CLKOUT6_CLKOUT2
                std::string frac_conf_name = is_clkout0 ? "CLKOUT5_CLKOUT2_" : "CLKOUT6_CLKOUT2_";

                if (1 <= frac_shifted) {
                    write_bit(frac_conf_name + "FRACTIONAL_FRAC_WF_F[0]");
                    write_int_vector(frac_conf_name + "FRACTIONAL_PHASE_MUX_F[1:0]", frac_shifted, 2);
                }
            }

            write_bit(name + "_CLKOUT1_OUTPUT_ENABLE[0]");
            write_int_vector(name + "_CLKOUT1_HIGH_TIME[5:0]", high, 6);
            write_int_vector(name + "_CLKOUT1_LOW_TIME[5:0]", low, 6);

            auto phase_mux_feature =
                    name + (is_clkout_5_or_6 ? "_CLKOUT2_FRACTIONAL_PHASE_MUX_F[0]" : "_CLKOUT2_PHASE_MUX[0]");
            write_int_vector(name + "_CLKOUT1_PHASE_MUX[2:0]", phasemux, 3);

            auto edge_feature = name + (is_clkout_5_or_6 ? "_CLKOUT2_FRACTIONAL_EDGE[0]" : "_CLKOUT2_EDGE[0]");
            write_bit(edge_feature, edge);

            auto no_count_feature =
                    name + (is_clkout_5_or_6 ? "_CLKOUT2_FRACTIONAL_NO_COUNT[0]" : "_CLKOUT2_NO_COUNT[0]");
            write_bit(no_count_feature, no_count);

            auto delay_time_feature =
                    name + (is_clkout_5_or_6 ? "_CLKOUT2_FRACTIONAL_DELAY_TIME[5:0]" : "_CLKOUT2_DELAY_TIME[5:0]");
            write_int_vector(delay_time_feature, delaytime, 6);

            if (!is_clkout_5_or_6 && frac != 0) {
                write_bit(name + "_CLKOUT2_FRAC_EN[0]", 1);
                write_bit(name + "_CLKOUT2_FRAC_WF_R[0]", 1);
                write_int_vector(name + "_CLKOUT2_FRAC[2:0]", frac, 3);
            }
        }
    }

    // From openXC7
    void write_mmcm(CellInfo *ci)
    {
        push(uarch->tile_name(ci->bel.tile));
        push("MMCME2_ADV");
        write_bit("IN_USE");
        // FIXME: should be INV not ZINV (XRay error?)
        write_bit("ZINV_PWRDWN", bool_or_default(ci->params, id_IS_PWRDWN_INVERTED, false));
        write_bit("ZINV_RST", bool_or_default(ci->params, id_IS_RST_INVERTED, false));
        write_bit("ZINV_PSEN", bool_or_default(ci->params, id_IS_PSEN_INVERTED, false));
        write_bit("ZINV_PSINCDEC", bool_or_default(ci->params, id_IS_PSINCDEC_INVERTED, false));
        write_bit("INV_CLKINSEL", bool_or_default(ci->params, id_IS_CLKINSEL_INVERTED, false));
        write_mmcm_clkout("DIVCLK", ci);
        write_mmcm_clkout("CLKFBOUT", ci);
        write_mmcm_clkout("CLKOUT0", ci);
        write_mmcm_clkout("CLKOUT1", ci);
        write_mmcm_clkout("CLKOUT2", ci);
        write_mmcm_clkout("CLKOUT3", ci);
        write_mmcm_clkout("CLKOUT4", ci);
        write_mmcm_clkout("CLKOUT5", ci);
        write_mmcm_clkout("CLKOUT6", ci);

        std::string comp = str_or_default(ci->params, id_COMPENSATION, "INTERNAL");
        push("COMP");
        if (comp == "INTERNAL" || comp == "ZHOLD") {
            // does not seem to make a difference in vivado
            // both modes set this bit
            write_bit("Z_ZHOLD");
        } else {
            log_error("unsupported COMPENSATION type '%s' for MMCM (supported compensation types: INTERNAL, ZHOLD)\n",
                      comp.c_str());
        }
        pop();

        auto clkfbout_mult = (int)float_or_default(ci, "CLKFBOUT_MULT_F", 5.000);
        if (63 < clkfbout_mult)
            log_error("MMCME2_ADV: CLKFBOUT_MULT_F must not be greater than 63");
        if (0 == clkfbout_mult)
            log_error("MMCME2_ADV: CLKFBOUT_MULT_F must not be 0");
        write_int_vector("LKTABLE[39:0]", Xc7MMCM::lk_table[clkfbout_mult - 1], 40);

        std::string bandwidth = str_or_default(ci->params, id_BANDWIDTH, "OPTIMIZED");
        const uint16_t *filter_lookup;
        if (bandwidth == "LOW")
            filter_lookup = Xc7MMCM::filter_lookup_low;
        else if (bandwidth == "LOW_SS")
            filter_lookup = Xc7MMCM::filter_lookup_low_ss;
        else if (bandwidth == "HIGH")
            filter_lookup = Xc7MMCM::filter_lookup_high;
        else
            filter_lookup = Xc7MMCM::filter_lookup_optimized;
        write_int_vector("FILTREG1_RESERVED[11:0]", filter_lookup[clkfbout_mult - 1], 12);

        // 0x9900 enables fractional counters
        // only int counters would be 0x1 << 8
        // 0xffff enables everything, I suppose, this is what is used in xap888
        write_int_vector("POWER_REG_POWER_REG_POWER_REG[15:0]", 0xffff, 16);
        write_bit("LOCKREG3_RESERVED[0]");
        write_int_vector("TABLE[9:0]", 0x3d4, 10);
        pop(2);
    }
    void write_dsp_cell(CellInfo *ci)
    {
        auto tile_name = uarch->tile_name(ci->bel.tile);
        auto tile_side = tile_name.at(4);
        push(tile_name);
        push("DSP48");
        auto xy = uarch->rel_site_loc(uarch->get_bel_site(ci->bel));
        auto dsp = stringf("DSP_%d", xy.y);
        push(dsp);

        auto write_bus_zinv = [&](std::string name, int width) {
            for (int i = 0; i < width; i++) {
                std::string b = stringf("[%d]", i);
                bool inv = (int_or_default(ci->params, ctx->id("IS_" + name + "_INVERTED"), 0) >> i) & 0x1;
                inv |= bool_or_default(ci->params, ctx->id("IS_" + name + b + "_INVERTED"), false);
                write_bit("ZIS_" + name + "_INVERTED" + b, !inv);
            }
        };

        // value 1 is equivalent to 2, according to UG479
        // but in real life, Vivado sets AREG_0 is 0,
        // no bit is 1, and AREG_2 is 2
        auto areg = int_or_default(ci->params, ctx->id("AREG"), 1);
        if (areg == 0 || areg == 2)
            write_bit("AREG_" + std::to_string(areg));

        auto ainput = str_or_default(ci->params, ctx->id("A_INPUT"), "DIRECT");
        if (ainput == "CASCADE")
            write_bit("A_INPUT[0]");

        // value 1 is equivalent to 2, according to UG479
        // but in real life, Vivado sets AREG_0 is 0,
        // no bit is 1, and AREG_2 is 2
        auto breg = int_or_default(ci->params, ctx->id("BREG"), 1);
        if (breg == 0 || breg == 2)
            write_bit("BREG_" + std::to_string(breg));

        auto binput = str_or_default(ci->params, ctx->id("B_INPUT"), "DIRECT");
        if (binput == "CASCADE")
            write_bit("B_INPUT[0]");

        // Tolerate both int and string types for interoperability purposes
        auto use_dport = boolstr_or_default(ci->params, ctx->id("USE_DPORT"), false);
        if (use_dport == true)
            write_bit("USE_DPORT[0]");

        auto use_simd = str_or_default(ci->params, ctx->id("USE_SIMD"), "ONE48");
        if (use_simd == "TWO24")
            write_bit("USE_SIMD_FOUR12_TWO24");
        if (use_simd == "FOUR12")
            write_bit("USE_SIMD_FOUR12");

        // PATTERN
        const size_t pattern_size = 48;
        std::vector<bool> pattern_vector(pattern_size, false);
        bool pattern_found = boolvec_populate(ci->params, ctx->id("PATTERN"), pattern_vector);
        if (pattern_found) {
            write_vector("PATTERN[47:0]", pattern_vector);
        }

        auto autoreset_patdet = str_or_default(ci->params, ctx->id("AUTORESET_PATDET"), "NO_RESET");
        if (autoreset_patdet == "RESET_MATCH")
            write_bit("AUTORESET_PATDET_RESET");
        if (autoreset_patdet == "RESET_NOT_MATCH")
            write_bit("AUTORESET_PATDET_RESET_NOT_MATCH");

        // MASK
        // Yosys gives us 48 bit, but prjxray only recognizes 46 bits
        // The most significant two bits seem to be zero, so let us just truncate them
        const size_t mask_size = 48;
        std::vector<bool> mask_vector(mask_size, true);
        bool mask_found = boolvec_populate(ci->params, ctx->id("MASK"), mask_vector);
        if (mask_found) {
            mask_vector.resize(46);
            write_vector("MASK[45:0]", mask_vector);
        }

        auto sel_mask = str_or_default(ci->params, ctx->id("SEL_MASK"), "MASK");
        if (sel_mask == "C")
            write_bit("SEL_MASK_C");
        if (sel_mask == "ROUNDING_MODE1")
            write_bit("SEL_MASK_ROUNDING_MODE1");
        if (sel_mask == "ROUNDING_MODE2")
            write_bit("SEL_MASK_ROUNDING_MODE2");

        write_bit("ZADREG[0]", !bool_or_default(ci->params, ctx->id("ADREG"), true));
        write_bit("ZALUMODEREG[0]", !bool_or_default(ci->params, ctx->id("ALUMODEREG")));
        write_bit("ZAREG_2_ACASCREG_1", !bool_or_default(ci->params, ctx->id("ACASCREG")));
        write_bit("ZBREG_2_BCASCREG_1", !bool_or_default(ci->params, ctx->id("BCASCREG")));
        write_bit("ZCARRYINREG[0]", !bool_or_default(ci->params, ctx->id("CARRYINREG")));
        write_bit("ZCARRYINSELREG[0]", !bool_or_default(ci->params, ctx->id("CARRYINSELREG")));
        write_bit("ZCREG[0]", !bool_or_default(ci->params, ctx->id("CREG"), true));
        write_bit("ZDREG[0]", !bool_or_default(ci->params, ctx->id("DREG"), true));
        write_bit("ZINMODEREG[0]", !bool_or_default(ci->params, ctx->id("INMODEREG")));
        write_bus_zinv("ALUMODE", 4);
        write_bus_zinv("INMODE", 5);
        write_bus_zinv("OPMODE", 7);
        write_bit("ZMREG[0]", !bool_or_default(ci->params, ctx->id("MREG")));
        write_bit("ZOPMODEREG[0]", !bool_or_default(ci->params, ctx->id("OPMODEREG")));
        write_bit("ZPREG[0]", !bool_or_default(ci->params, ctx->id("PREG")));
        write_bit("USE_DPORT[0]", boolstr_or_default(ci->params, ctx->id("USE_DPORT"), false));
        write_bit("ZIS_CLK_INVERTED", !bool_or_default(ci->params, ctx->id("IS_CLK_INVERTED")));
        write_bit("ZIS_CARRYIN_INVERTED", !bool_or_default(ci->params, ctx->id("IS_CARRYIN_INVERTED")));
        pop(2);

        auto write_const_pins = [&](std::string const_net_name) {
            std::vector<std::string> pins;
            const auto attr_name = "DSP_" + const_net_name + "_PINS";
            const auto attr_value = str_or_default(ci->attrs, ctx->id(attr_name), "");
            boost::split(pins, attr_value, boost::is_any_of(" "));
            for (auto pin : pins) {
                if (boost::empty(pin))
                    continue;
                auto pin_basename = pin;
                boost::erase_all(pin_basename, "0123456789");
                auto inv = bool_or_default(ci->params, ctx->id("IS_" + pin_basename + "_INVERTED"), 0);
                auto net_name = inv ? (const_net_name == "GND" ? "VCC" : "GND") : const_net_name;
                write_bit(stringf("%s_%s.DSP_%s_%c", dsp.c_str(), pin.c_str(), net_name.c_str(), tile_side));
            }
        };

        write_const_pins("GND");
        write_const_pins("VCC");

        pop();
    }

    // A placed BUFR is configured from the cell, not from the route: the
    // pp_config entries only fire when a BUFR site is traversed as routing,
    // and they hardcode BUFR_DIVIDE.BYPASS.  Emitting from the cell makes the
    // configuration follow the instance, which is where the parameter lives.
    // (Port of nextpnr-xilinx 0b914578.)
    void write_bufr(CellInfo *ci)
    {
        // Site name is BUFR_X0Y<y> and the feature is BUFR_Y<y>, so the
        // site's y within the tile is the slot index.
        auto xy = uarch->rel_site_loc(uarch->get_bel_site(ci->bel));

        // Vivado's BUFR_DIVIDE is a string: "BYPASS" or "1".."8".
        std::string divide = str_or_default(ci->params, ctx->id("BUFR_DIVIDE"), "BYPASS");
        std::string divide_feature;
        if (divide == "BYPASS" || divide.empty()) {
            divide_feature = "BYPASS";
        } else if (divide.size() == 1 && divide[0] >= '1' && divide[0] <= '8') {
            divide_feature = std::string("D") + divide[0];
        } else {
            // Emitting an undocumented feature instead would fail later in
            // fasm2frames with a FasmLookupError naming a bit.
            log_error("BUFR '%s' has BUFR_DIVIDE=\"%s\"; supported are BYPASS and 1..8\n",
                      ci->name.c_str(ctx), divide.c_str());
        }

        push(uarch->tile_name(ci->bel.tile));
        push("BUFR_Y" + std::to_string(xy.y));
        write_bit("IN_USE");
        push("BUFR_DIVIDE");
        write_bit(divide_feature);
        pop();
        pop();
        pop();
    }

    void write_cfg()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->bel == BelId())
                continue;
            std::string tile_name = uarch->tile_name(ci->bel.tile);
            if (!boost::starts_with(tile_name, "CFG_CENTER_"))
                continue;

            push(tile_name);
            if (ci->type == id_BSCAN) {
                push("BSCAN");
                int chain = int_or_default(ci->params, id_JTAG_CHAIN, 1);
                if (chain < 1 || 4 < chain)
                    log_error("Invalid JTAG_CHAIN number of '%d'. Allowed values are: 1-4.\n", chain);
                write_bit("JTAG_CHAIN_" + std::to_string(chain));
                pop();
            }
            if (ci->type == id_DCIRESET_DCIRESET)
                write_bit("DCIRESET.ENABLED");
            if (ci->type == id_ICAP_ICAP) {
                push("ICAP");
                std::string width = str_or_default(ci->params, id_ICAP_WIDTH, "X32");
                if (width != "X32" && width != "X16" && width != "X8")
                    log_error("Unknown ICAP_WIDTH of '%s'. Allowed values are: X32, X16 and X8.\n", width.c_str());
                if (width == "X16")
                    write_bit("ICAP_WIDTH_X16");
                if (width == "X8")
                    write_bit("ICAP_WIDTH_X8");
                pop();
            }
            if (ci->type == id_STARTUP_STARTUP) {
                std::string prog_usr = str_or_default(ci->params, id_PROG_USR, "FALSE");
                if (prog_usr != "TRUE" && prog_usr != "FALSE")
                    log_error("Invalid PROG_USR attribute in STARTUPE2 of '%s'. Allowed values are: TRUE, FALSE.\n",
                              prog_usr.c_str());
                write_bit("STARTUP.PROG_USR", prog_usr == "TRUE");
                NetInfo *usrclk = ci->getPort(id_USRCCLKO);
                bool usrcclko_connected =
                        usrclk != nullptr && usrclk->driver.cell != nullptr &&
                        !usrclk->driver.cell->type.in(id_PSEUDO_GND, id_PSEUDO_VCC);
                write_bit("STARTUP.USRCCLKO_CONNECTED", usrcclko_connected);
            }
            pop();
        }
    }

    void write_ibufds_gte2(CellInfo * ci)
    {
        push(uarch->tile_name(ci->bel.tile));
        Loc siteLoc = uarch->rel_site_loc(uarch->get_bel_site(ci->bel));
        push("IBUFDS_GTE2_Y" + std::to_string(siteLoc.y));
        write_bit("IN_USE");
        auto clkcm_cfg = bool_or_default(ci->params, ctx->id("CLKCM_CFG"), true);
        if (!clkcm_cfg) log_warning("%s/%s: According to ug482, CLKCM_CFG should always be on\n",
                                    ci->hierpath.c_str(ctx), ci->name.c_str(ctx));
        write_bit("CLKCM_CFG", clkcm_cfg);
        auto clkrcv_trst = bool_or_default(ci->params, ctx->id("CLKRCV_TRST"), true);
        if (!clkrcv_trst) log_warning("%s/%s: According to ug482, CLKRCV_TRST should always be on\n",
                                       ci->hierpath.c_str(ctx), ci->name.c_str(ctx));
        write_bit("CLKRCV_TRST", clkrcv_trst);
        pop(2);
    }
    void write_gtp_pll(CellInfo *ci)
    {
        push(uarch->tile_name(ci->bel.tile));

        push("GTPE2_COMMON");
        write_bit("IN_USE");
        write_bit("ENABLE_DRP", bool_or_default(ci->params, ctx->id("_DRPCLK_USED"), false));
        write_bit("BOTH_GTREFCLK_USED", bool_or_default(ci->params, ctx->id("_BOTH_GTREFCLK_USED"), false));
        write_bit("GTREFCLK0_USED", bool_or_default(ci->params, ctx->id("_GTREFCLK0_USED"), false));
        write_bit("GTREFCLK1_USED", bool_or_default(ci->params, ctx->id("_GTREFCLK1_USED"), false));
        write_bit("GTGREFCLK0_USED", bool_or_default(ci->params, ctx->id("_GTGREFCLK_USED"), false));
        auto clkswing_cfg = int_or_default(ci->params, ctx->id("CLKSWING_CFG"), 3);
        if (clkswing_cfg != 3) log_warning("%s/%s: According to ug482, CLK should always be 0b11\n",
                                           ci->hierpath.c_str(ctx), ci->name.c_str(ctx));
        write_int_vector("IBUFDS_GTE2.CLKSWING_CFG[1:0]", clkswing_cfg, 2);
        write_bit("INV_DRPCLK", bool_or_default(ci->params, ctx->id("IS_DRPCLK_INVERTED")));
        write_bit("INV_PLL0LOCKDETCLK", bool_or_default(ci->params, ctx->id("IS_PLL0LOCKDETCLK_INVERTED")));
        write_bit("INV_PLL1LOCKDETCLK", bool_or_default(ci->params, ctx->id("IS_PLL1LOCKDETCLK_INVERTED")));

        auto bias_cfg = int_or_default(ci->params, ctx->id("BIAS_CFG"), 0);
        write_int_vector("BIAS_CFG[63:0]", bias_cfg, 64);
        auto common_cfg = int_or_default(ci->params, ctx->id("COMMON_CFG"), 0);
        write_int_vector("COMMON_CFG[31:0]", common_cfg, 32);

        // according to ug482, these attributes contain magic undocumented and reserved wizard values
        write_int_vector("PLL0_CFG[20:0]", 0b111110000001111011100, 21);
        write_int_vector("PLL1_CFG[20:0]", 0b111110000001111011100, 21);
        write_int_vector("PLL0_INIT_CFG[4:0]", 0b11110, 5);
        write_int_vector("PLL1_INIT_CFG[4:0]", 0b11110, 5);
        write_int_vector("PLL0_LOCK_CFG[8:0]", 0b111101000, 9);
        write_int_vector("PLL1_LOCK_CFG[8:0]", 0b111101000, 9);

        auto pll0_refclk_div = int_or_default(ci->params, ctx->id("PLL0_REFCLK_DIV"), 1);
        if (pll0_refclk_div < 1 || pll0_refclk_div > 2)
            log_error("PLL0_REFCLK_DIV can only be 1 or 2, but is: %d", pll0_refclk_div);
        write_bit("PLL0_REFCLK_DIV[4]", pll0_refclk_div == 1);
        auto pll1_refclk_div = int_or_default(ci->params, ctx->id("PLL1_REFCLK_DIV"), 1);
        if (pll1_refclk_div < 1 || pll1_refclk_div > 2)
            log_error("PLL1_REFCLK_DIV can only be 1 or 2, but is: %d", pll1_refclk_div);
        write_bit("PLL1_REFCLK_DIV[4]", pll1_refclk_div == 1);

        auto pll0_fbdiv = int_or_default(ci->params, ctx->id("PLL0_FBDIV"), 1);
        if (pll0_fbdiv < 1 || pll0_fbdiv > 5)
            log_error("PLL0_FBDIV can only be 1, 2, 3, 4 or 5, but is: %d", pll0_fbdiv);
        if (pll0_fbdiv == 1) write_bit("PLL0_FBDIV[4]");
        else write_int_vector("PLL0_FBDIV[1:0]", pll0_fbdiv - 2, 2);
        auto pll1_fbdiv = int_or_default(ci->params, ctx->id("PLL1_FBDIV"), 1);
        if (pll1_fbdiv < 1 || pll1_fbdiv > 5)
            log_error("PLL1_FBDIV can only be 1, 2, 3, 4 or 5, but is: %d", pll1_fbdiv);
        if (pll1_fbdiv == 1) write_bit("PLL1_FBDIV[4]");
        else write_int_vector("PLL1_FBDIV[1:0]", pll1_fbdiv - 2, 2);

        auto pll0_fbdiv_45 = int_or_default(ci->params, ctx->id("PLL0_FBDIV_45"), 4);
        if (pll0_fbdiv_45 < 4 || pll0_fbdiv_45 > 5)
            log_error("PLL0_FBDIV_45 can only be 4 or 5, but is: %d", pll0_fbdiv);
        write_bit("PLL0_FBDIV_45[0]", pll0_fbdiv_45 == 5);
        auto pll1_fbdiv_45 = int_or_default(ci->params, ctx->id("PLL1_FBDIV_45"), 4);
        if (pll1_fbdiv_45 < 4 || pll1_fbdiv_45 > 5)
            log_error("PLL1_FBDIV_45 can only be 4 or 5, but is: %d", pll1_fbdiv);
        write_bit("PLL1_FBDIV_45[0]", pll1_fbdiv_45 == 5);

        auto pll0_dmon_cfg = bool_or_default(ci->params, ctx->id("PLL0_DMON_CFG"), false);
        write_bit("PLL0_DMON_CFG[0]", pll0_dmon_cfg);
        auto pll1_dmon_cfg = bool_or_default(ci->params, ctx->id("PLL1_DMON_CFG"), false);
        write_bit("PLL1_DMON_CFG[0]", pll1_dmon_cfg);

        auto rsvd_attr0 = int_or_default(ci->params, ctx->id("RSVD_ATTR0"), 0);
        write_int_vector("RSVD_ATTR0[15:0]", rsvd_attr0, 16);
        auto rsvd_attr1 = int_or_default(ci->params, ctx->id("RSVD_ATTR1"), 0);
        write_int_vector("RSVD_ATTR1[15:0]", rsvd_attr1, 16);

        auto pll_clkout_cfg = int_or_default(ci->params, ctx->id("PLL_CLKOUT_CFG"), 0);
        write_int_vector("PLL_CLKOUT_CFG[7:0]", pll_clkout_cfg, 8);
        pop(); // GTPE2_COMMON

        pop(); // tile name
    }
    void write_gtp_channel(CellInfo *ci)
    {
        push(uarch->tile_name(ci->bel.tile));
        push("GTPE2_CHANNEL");

        write_bit("IN_USE");

        auto write_str_bool = [&](std::string attribute, std::string bit, std::string deflt = "FALSE") {
            auto val = str_or_default(ci->params, ctx->id(attribute), deflt);
            boost::algorithm::to_upper(val);
            write_bit(bit, val == "TRUE");
        };

        auto acjtag_debug_mode = bool_or_default(ci->params, ctx->id("ACJTAG_DEBUG_MODE"), false);
        write_bit("ACJTAG_DEBUG_MODE[0]", acjtag_debug_mode);
        auto acjtag_mode = bool_or_default(ci->params, ctx->id("ACJTAG_MODE"), false);
        write_bit("ACJTAG_MODE[0]", acjtag_mode);
        auto acjtag_reset = bool_or_default(ci->params, ctx->id("ACJTAG_RESET"), false);
        write_bit("ACJTAG_RESET[0]", acjtag_reset);

        auto adapt_cfg0 = int_or_default(ci->params, ctx->id("ADAPT_CFG0"), 0);
        write_int_vector("ADAPT_CFG0[19:0]", adapt_cfg0, 20);

        write_str_bool("ALIGN_COMMA_DOUBLE", "ALIGN_COMMA_DOUBLE");

        auto align_comma_enable = int_or_default(ci->params, ctx->id("ALIGN_COMMA_ENABLE"), 0b1111111111);
        write_int_vector("ALIGN_COMMA_ENABLE[9:0]", align_comma_enable, 10);

        auto align_comma_word = int_or_default(ci->params, ctx->id("ALIGN_COMMA_WORD"), 1);
        if (align_comma_word < 1 || 2 < align_comma_word)
            log_error("ALIGN_COMMA_WORD may only be 1 or 2, but is: %d\n", align_comma_word);
        if (align_comma_word == 1)
            write_bit("ALIGN_COMMA_WORD[0]");
        else
            write_bit("ALIGN_COMMA_WORD[1]");

        write_str_bool("ALIGN_MCOMMA_DET", "ALIGN_MCOMMA_DET");
        auto align_mcomma_value = int_or_default(ci->params, ctx->id("ALIGN_MCOMMA_VALUE"), 0b1010000011);
        write_int_vector("ALIGN_MCOMMA_VALUE[9:0]", align_mcomma_value, 10);

        write_str_bool("ALIGN_PCOMMA_DET", "ALIGN_PCOMMA_DET");
        auto align_pcomma_value = int_or_default(ci->params, ctx->id("ALIGN_PCOMMA_VALUE"), 0b0101111100);
        write_int_vector("ALIGN_PCOMMA_VALUE[9:0]", align_pcomma_value, 10);

        auto cbcc_data_source_sel = str_or_default(ci->params, ctx->id("CBCC_DATA_SOURCE_SEL"), "DECODED");
        if (cbcc_data_source_sel == "DECODED") write_bit("CBCC_DATA_SOURCE_SEL.DECODED");

        auto cfok_cfg = get_or_default(ci->params, ctx->id("CFOK_CFG"), Property(int64_t(0))).as_int64();
        write_int_vector("CFOK_CFG[42:0]", cfok_cfg, 43);
        auto cfok_cfg2 = int_or_default(ci->params, ctx->id("CFOK_CFG2"), 0);
        write_int_vector("CFOK_CFG2[6:0]", cfok_cfg2, 7);
        auto cfok_cfg3 = int_or_default(ci->params, ctx->id("CFOK_CFG3"), 0);
        write_int_vector("CFOK_CFG3[6:0]", cfok_cfg3, 7);
        auto cfok_cfg4 = bool_or_default(ci->params, ctx->id("CFOK_CFG4"), false);
        write_bit("CFOK_CFG4[0]", cfok_cfg4);
        auto cfok_cfg5 = int_or_default(ci->params, ctx->id("CFOK_CFG5"), 0);
        write_int_vector("CFOK_CFG5[1:0]", cfok_cfg5, 2);
        auto cfok_cfg6 = int_or_default(ci->params, ctx->id("CFOK_CFG6"), 0);
        write_int_vector("CFOK_CFG6[3:0]", cfok_cfg6, 4);

        write_str_bool("CHAN_BOND_KEEP_ALIGN", "CHAN_BOND_KEEP_ALIGN");
        auto chan_bond_max_skew = int_or_default(ci->params, ctx->id("CHAN_BOND_MAX_SKEW"), 0);
        if (chan_bond_max_skew < 1 || 14 < chan_bond_max_skew)
            log_error("CHAN_BOND_MAX_SKEW may only range from 1 to 14, but is: %d\n", chan_bond_max_skew);
        write_int_vector("CHAN_BOND_MAX_SKEW[3:0]", chan_bond_max_skew, 4);

        auto chan_bond_seq_1_enable = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_1_ENABLE"), 0);
        write_int_vector("CHAN_BOND_SEQ_1_ENABLE[3:0]", chan_bond_seq_1_enable, 4);
        auto chan_bond_seq_1_1 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_1_1"), 0);
        write_int_vector("CHAN_BOND_SEQ_1_1[9:0]", chan_bond_seq_1_1, 10);
        auto chan_bond_seq_1_2 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_1_2"), 0);
        write_int_vector("CHAN_BOND_SEQ_1_2[9:0]", chan_bond_seq_1_2, 10);
        auto chan_bond_seq_1_3 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_1_3"), 0);
        write_int_vector("CHAN_BOND_SEQ_1_3[9:0]", chan_bond_seq_1_3, 10);
        auto chan_bond_seq_1_4 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_1_4"), 0);
        write_int_vector("CHAN_BOND_SEQ_1_4[9:0]", chan_bond_seq_1_4, 10);

        write_str_bool("CHAN_BOND_SEQ_2_USE", "CHAN_BOND_SEQ_2_USE");
        auto chan_bond_seq_2_enable = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_2_ENABLE"), 0);
        write_int_vector("CHAN_BOND_SEQ_2_ENABLE[3:0]", chan_bond_seq_2_enable, 4);
        auto chan_bond_seq_2_1 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_2_1"), 0);
        write_int_vector("CHAN_BOND_SEQ_2_1[9:0]", chan_bond_seq_2_1, 10);
        auto chan_bond_seq_2_2 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_2_2"), 0);
        write_int_vector("CHAN_BOND_SEQ_2_2[9:0]", chan_bond_seq_2_2, 10);
        auto chan_bond_seq_2_3 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_2_3"), 0);
        write_int_vector("CHAN_BOND_SEQ_2_3[9:0]", chan_bond_seq_2_3, 10);
        auto chan_bond_seq_2_4 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_2_4"), 0);
        write_int_vector("CHAN_BOND_SEQ_2_4[9:0]", chan_bond_seq_2_4, 10);

        auto chan_bond_seq_len = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_LEN"), 0);
        if (chan_bond_seq_len < 1 || 4 < chan_bond_seq_len)
            log_error("CHAN_BOND_SEQ_LEN may only range from 1 to 4, but is: %d\n", chan_bond_seq_len);
        write_int_vector("CHAN_BOND_SEQ_LEN[1:0]", chan_bond_seq_len - 1, 2);

        auto clk_common_swing = bool_or_default(ci->params, ctx->id("CLK_COMMON_SWING"), false);
        write_bit("CLK_COMMON_SWING[0]", clk_common_swing);
        write_str_bool("CLK_COR_KEEP_IDLE", "CLK_COR_KEEP_IDLE");
        auto clk_cor_max_lat = int_or_default(ci->params, ctx->id("CLK_COR_MAX_LAT"), 0);
        write_int_vector("CLK_COR_MAX_LAT[5:0]", clk_cor_max_lat, 6);
        auto clk_cor_min_lat = int_or_default(ci->params, ctx->id("CLK_COR_MIN_LAT"), 0);
        write_int_vector("CLK_COR_MIN_LAT[5:0]", clk_cor_min_lat, 6);
        write_str_bool("CLK_COR_PRECEDENCE", "CLK_COR_PRECEDENCE");
        auto clk_cor_repeat_wait = int_or_default(ci->params, ctx->id("CLK_COR_REPEAT_WAIT"), 0);
        write_int_vector("CLK_COR_REPEAT_WAIT[4:0]", clk_cor_repeat_wait, 5);

        auto clk_cor_seq_1_enable = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_1_ENABLE"), 0);
        write_int_vector("CLK_COR_SEQ_1_ENABLE[3:0]", clk_cor_seq_1_enable, 4);
        auto clk_cor_seq_1_1 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_1_1"), 0);
        write_int_vector("CLK_COR_SEQ_1_1[9:0]", clk_cor_seq_1_1, 10);
        auto clk_cor_seq_1_2 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_1_2"), 0);
        write_int_vector("CLK_COR_SEQ_1_2[9:0]", clk_cor_seq_1_2, 10);
        auto clk_cor_seq_1_3 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_1_3"), 0);
        write_int_vector("CLK_COR_SEQ_1_3[9:0]", clk_cor_seq_1_3, 10);
        auto clk_cor_seq_1_4 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_1_4"), 0);
        write_int_vector("CLK_COR_SEQ_1_4[9:0]", clk_cor_seq_1_4, 10);

        write_str_bool("CLK_COR_SEQ_2_USE", "CLK_COR_SEQ_2_USE");
        auto clk_cor_seq_2_enable = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_2_ENABLE"), 0);
        write_int_vector("CLK_COR_SEQ_2_ENABLE[3:0]", clk_cor_seq_2_enable, 4);
        auto clk_cor_seq_2_1 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_2_1"), 0);
        write_int_vector("CLK_COR_SEQ_2_1[9:0]", clk_cor_seq_2_1, 10);
        auto clk_cor_seq_2_2 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_2_2"), 0);
        write_int_vector("CLK_COR_SEQ_2_2[9:0]", clk_cor_seq_2_2, 10);
        auto clk_cor_seq_2_3 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_2_3"), 0);
        write_int_vector("CLK_COR_SEQ_2_3[9:0]", clk_cor_seq_2_3, 10);
        auto clk_cor_seq_2_4 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_2_4"), 0);
        write_int_vector("CLK_COR_SEQ_2_4[9:0]", clk_cor_seq_2_4, 10);

        auto clk_cor_seq_len = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_LEN"), 0);
        if (clk_cor_seq_len < 1 || 4 < clk_cor_seq_len)
            log_error("CLK_COR_SEQ_LEN may only range from 1 to 4, but is: %d\n", clk_cor_seq_len);
        write_int_vector("CLK_COR_SEQ_LEN[1:0]", clk_cor_seq_len - 1, 2);

        write_str_bool("CLK_CORRECT_USE", "CLK_CORRECT_USE");

        write_str_bool("DEC_MCOMMA_DETECT", "DEC_MCOMMA_DETECT");
        write_str_bool("DEC_PCOMMA_DETECT", "DEC_PCOMMA_DETECT");
        write_str_bool("DEC_VALID_COMMA_ONLY", "DEC_VALID_COMMA_ONLY");

        auto dmonitor_cfg = int_or_default(ci->params, ctx->id("DMONITOR_CFG"), 0x008101);
        write_int_vector("DMONITOR_CFG[23:0]", dmonitor_cfg, 24);

        auto es_clk_phase_sel = bool_or_default(ci->params, ctx->id("ES_CLK_PHASE_SEL"), false);
        write_bit("ES_CLK_PHASE_SEL[0]", es_clk_phase_sel);
        auto es_control = int_or_default(ci->params, ctx->id("ES_CONTROL"), 0);
        write_int_vector("ES_CONTROL[5:0]", es_control, 6);
        write_str_bool("ES_ERRDET_EN", "ES_ERRDET_EN");
        write_str_bool("ES_EYE_SCAN_EN", "ES_EYE_SCAN_EN");
        auto es_horz_offset = int_or_default(ci->params, ctx->id("ES_HORZ_OFFSET"), 0x010);
        write_int_vector("ES_HORZ_OFFSET[11:0]", es_horz_offset, 12);
        auto es_pma_cfg = int_or_default(ci->params, ctx->id("ES_PMA_CFG"), 0);
        write_int_vector("ES_PMA_CFG[9:0]", es_pma_cfg, 10);
        auto es_prescale = int_or_default(ci->params, ctx->id("ES_PRESCALE"), 0);
        write_int_vector("ES_PRESCALE[4:0]", es_prescale, 5);
        auto es_qual_mask = int_or_default(ci->params, ctx->id("ES_QUAL_MASK"), 0);
        write_int_vector("ES_QUAL_MASK[79:0]", es_qual_mask, 80);
        auto es_qualifier = int_or_default(ci->params, ctx->id("ES_QUALIFIER"), 0);
        write_int_vector("ES_QUALIFIER[79:0]", es_qualifier, 80);
        auto es_sdata_mask = int_or_default(ci->params, ctx->id("ES_SDATA_MASK"), 0);
        write_int_vector("ES_SDATA_MASK[79:0]", es_sdata_mask, 80);
        auto es_vert_offset = int_or_default(ci->params, ctx->id("ES_VERT_OFFSET"), 0);
        write_int_vector("ES_VERT_OFFSET[8:0]", es_vert_offset, 9);

        auto fts_deskew_seq_enable = int_or_default(ci->params, ctx->id("FTS_DESKEW_SEQ_ENABLE"), 0b1111);
        write_int_vector("FTS_DESKEW_SEQ_ENABLE[3:0]", fts_deskew_seq_enable, 4);
        auto fts_lane_deskew_cfg = int_or_default(ci->params, ctx->id("FTS_LANE_DESKEW_CFG"), 0);
        write_int_vector("FTS_LANE_DESKEW_CFG[3:0]", fts_lane_deskew_cfg, 4);
        write_str_bool("FTS_LANE_DESKEW_EN", "FTS_LANE_DESKEW_EN");

        auto gearbox_mode = int_or_default(ci->params, ctx->id("GEARBOX_MODE"), 0);
        write_int_vector("GEARBOX_MODE[2:0]", gearbox_mode, 3);

        auto write_inv = [&](std::string name) {
            write_bit("INV_" + name, bool_or_default(ci->params, ctx->id("IS_" + name + "_INVERTED"), false));
        };
        // only these have been fuzzed yet,
        write_inv("DMONITORCLK");
        write_inv("DRPCLK");
        write_inv("RXUSRCLK");
        write_inv("SIGVALIDCLK");
        write_inv("TXPHDLYTSTCLK");
        write_inv("TXUSRCLK");
        write_inv("CLKRSVD0");
        write_inv("CLKRSVD1");
        write_inv("RXUSRCLK2");
        write_inv("TXUSRCLK2");

        auto loopback_cfg = bool_or_default(ci->params, ctx->id("LOOPBACK_CFG"), false);
        write_bit("LOOPBACK_CFG[0]", loopback_cfg);

        auto outrefclk_sel_inv = int_or_default(ci->params, ctx->id("OUTREFCLK_SEL_INV"), 0);
        write_int_vector("OUTREFCLK_SEL_INV[1:0]", outrefclk_sel_inv, 2);

        write_str_bool("PCS_PCIE_EN", "PCS_PCIE_EN");

        auto rsvd_attr = int_or_default(ci->params, ctx->id("PCS_RSVD_ATTR"), 0);
        write_int_vector("PCS_RSVD_ATTR[47:0]", rsvd_attr, 48);

        auto pd_trans_time_from_p2 = int_or_default(ci->params, ctx->id("PD_TRANS_TIME_FROM_P2"), 0);
        write_int_vector("PD_TRANS_TIME_FROM_P2[11:0]", pd_trans_time_from_p2, 12);
        auto pd_trans_time_none_p2 = int_or_default(ci->params, ctx->id("PD_TRANS_TIME_NONE_P2"), 0);
        write_int_vector("PD_TRANS_TIME_NONE_P2[7:0]", pd_trans_time_none_p2, 8);
        auto pd_trans_time_to_p2 = int_or_default(ci->params, ctx->id("PD_TRANS_TIME_TO_P2"), 0);
        write_int_vector("PD_TRANS_TIME_TO_P2[7:0]", pd_trans_time_to_p2, 8);

        auto pma_loopback_cfg = bool_or_default(ci->params, ctx->id("PMA_LOOPBACK_CFG"), false);
        write_bit("PMA_LOOPBACK_CFG[0]", pma_loopback_cfg);
        auto pma_rsv = int_or_default(ci->params, ctx->id("PMA_RSV"), 0);
        write_int_vector("PMA_RSV[31:0]", pma_rsv, 32);
        auto pma_rsv2 = int_or_default(ci->params, ctx->id("PMA_RSV2"), 0);
        write_int_vector("PMA_RSV2[31:0]", pma_rsv2, 32);
        auto pma_rsv3 = int_or_default(ci->params, ctx->id("PMA_RSV3"), 0);
        write_int_vector("PMA_RSV3[1:0]", pma_rsv3, 2);
        auto pma_rsv4 = int_or_default(ci->params, ctx->id("PMA_RSV4"), 0);
        write_int_vector("PMA_RSV4[3:0]", pma_rsv4, 4);
        auto pma_rsv5 = bool_or_default(ci->params, ctx->id("PMA_RSV5"), false);
        write_bit("PMA_RSV5[0]", pma_rsv5);
        auto pma_rsv6 = bool_or_default(ci->params, ctx->id("PMA_RSV6"), false);
        write_bit("PMA_RSV6[0]", pma_rsv6);
        auto pma_rsv7 = bool_or_default(ci->params, ctx->id("PMA_RSV7"), false);
        write_bit("PMA_RSV7[0]", pma_rsv7);

        auto rx_bias_cfg = int_or_default(ci->params, ctx->id("RX_BIAS_CFG"), 0);
        write_int_vector("RX_BIAS_CFG[15:0]", rx_bias_cfg, 16);

        auto rx_buffer_cfg = int_or_default(ci->params, ctx->id("RX_BUFFER_CFG"), 0);
        write_int_vector("RX_BUFFER_CFG[5:0]", rx_buffer_cfg, 6);
        auto rx_clkmux_en = bool_or_default(ci->params, ctx->id("RX_CLKMUX_EN"), false);
        write_bit("RX_CLKMUX_EN[0]", rx_clkmux_en);
        auto rx_cm_sel = int_or_default(ci->params, ctx->id("RX_CM_SEL"), 0);
        write_int_vector("RX_CM_SEL[1:0]", rx_cm_sel, 2);
        auto rx_cm_trim = int_or_default(ci->params, ctx->id("RX_CM_TRIM"), 0);
        write_int_vector("RX_CM_TRIM[3:0]", rx_cm_trim, 4);
        auto rx_data_width = int_or_default(ci->params, ctx->id("RX_DATA_WIDTH"), 0);
        switch (rx_data_width) {
            case 16:
                rx_data_width = 2; break;
            case 20:
                rx_data_width = 3; break;
            case 32:
                rx_data_width = 4; break;
            case 40:
                rx_data_width = 5; break;
            default:
                log_error("Invalid RX_DATA_WIDTH parameter '%d' for GTPE2_CHANNEL instance %s\n", rx_data_width, ci->name.c_str(ctx));
        }
        write_int_vector("RX_DATA_WIDTH[2:0]", rx_data_width, 3);
        auto rx_ddi_sel = int_or_default(ci->params, ctx->id("RX_DDI_SEL"), 0);
        write_int_vector("RX_DDI_SEL[5:0]", rx_ddi_sel, 6);
        auto rx_debug_cfg = int_or_default(ci->params, ctx->id("RX_DEBUG_CFG"), 0);
        write_int_vector("RX_DEBUG_CFG[13:0]", rx_debug_cfg, 14);
        write_str_bool("RX_DEFER_RESET_BUF_EN", "RX_DEFER_RESET_BUF_EN");
        write_str_bool("RX_DISPERR_SEQ_MATCH", "RX_DISPERR_SEQ_MATCH");
        auto rx_os_cfg = int_or_default(ci->params, ctx->id("RX_OS_CFG"), 0);
        write_int_vector("RX_OS_CFG[12:0]", rx_os_cfg, 13);
        auto rx_sig_valid_dly = int_or_default(ci->params, ctx->id("RX_SIG_VALID_DLY"), 0) - 1;
        write_int_vector("RX_SIG_VALID_DLY[4:0]", rx_sig_valid_dly, 5);
        auto rx_xclk_sel = str_or_default(ci->params, ctx->id("RX_XCLK_SEL"), "RXUSR");
        if (rx_xclk_sel != "RXUSR" && rx_xclk_sel != "RXREC")
            log_error("RX_XCLK_SEL may only have values 'RXREC' or 'RXUSR' but is: '%s'\n", rx_xclk_sel.c_str());
        write_bit("RX_XCLK_SEL.RXUSR", rx_xclk_sel == "RXUSR");
        auto rx_clk25_div = int_or_default(ci->params, ctx->id("RX_CLK25_DIV"), 0) - 1;
        write_int_vector("RX_CLK25_DIV[4:0]", rx_clk25_div, 5);

        auto rxbuf_addr_mode = str_or_default(ci->params, ctx->id("RXBUF_ADDR_MODE"), "PMA");
        if (rxbuf_addr_mode != "FULL" && rxbuf_addr_mode != "FAST")
            log_error("RXBUF_ADDR_MODE may only have values 'FULL' or 'FAST' but is: '%s'\n", rxbuf_addr_mode.c_str());
        write_bit("RXBUF_ADDR_MODE.FAST", rxbuf_addr_mode == "FAST");
        auto rxbuf_eidle_hi_cnt = int_or_default(ci->params, ctx->id("RXBUF_EIDLE_HI_CNT"), 0);
        write_int_vector("RXBUF_EIDLE_HI_CNT[3:0]", rxbuf_eidle_hi_cnt, 4);
        auto rxbuf_eidle_lo_cnt = int_or_default(ci->params, ctx->id("RXBUF_EIDLE_LO_CNT"), 0);
        write_int_vector("RXBUF_EIDLE_LO_CNT[3:0]", rxbuf_eidle_lo_cnt, 4);
        write_str_bool("RXBUF_EN", "RXBUF_EN", "TRUE");
        write_str_bool("RXBUF_RESET_ON_CB_CHANGE", "RXBUF_RESET_ON_CB_CHANGE", "TRUE");
        write_str_bool("RXBUF_RESET_ON_COMMAALIGN", "RXBUF_RESET_ON_COMMAALIGN");
        write_str_bool("RXBUF_RESET_ON_EIDLE", "RXBUF_RESET_ON_EIDLE");
        write_str_bool("RXBUF_RESET_ON_RATE_CHANGE", "RXBUF_RESET_ON_RATE_CHANGE", "TRUE");
        write_str_bool("RXBUF_THRESH_OVRD", "RXBUF_THRESH_OVRD");
        auto rxbuf_thresh_ovflw = int_or_default(ci->params, ctx->id("RXBUF_THRESH_OVFLW"), 0);
        write_int_vector("RXBUF_THRESH_OVFLW[5:0]", rxbuf_thresh_ovflw, 6);
        auto rxbuf_thresh_undflw = int_or_default(ci->params, ctx->id("RXBUF_THRESH_UNDFLW"), 0);
        write_int_vector("RXBUF_THRESH_UNDFLW[5:0]", rxbuf_thresh_undflw, 6);
        auto rxbufreset_time = int_or_default(ci->params, ctx->id("RXBUFRESET_TIME"), 0);
        write_int_vector("RXBUFRESET_TIME[4:0]", rxbufreset_time, 5);

        auto rxcdr_cfg = get_or_default(ci->params, ctx->id("RXCDR_CFG"),
            Property("00000000000000000000000000000000000000000000000000000000000000000000000000000000000"));
        write_vector("RXCDR_CFG[82:0]", bits_from_string_default(rxcdr_cfg, 83));
        auto rxcdr_fr_reset_on_eidle = bool_or_default(ci->params, ctx->id("RXCDR_FR_RESET_ON_EIDLE"), false);
        write_bit("RXCDR_FR_RESET_ON_EIDLE[0]", rxcdr_fr_reset_on_eidle);
        auto rxcdr_ph_reset_on_eidle = bool_or_default(ci->params, ctx->id("RXCDR_PH_RESET_ON_EIDLE"), false);
        write_bit("RXCDR_PH_RESET_ON_EIDLE[0]", rxcdr_ph_reset_on_eidle);
        auto rxcdr_hold_during_eidle = bool_or_default(ci->params, ctx->id("RXCDR_HOLD_DURING_EIDLE"), false);
        write_bit("RXCDR_HOLD_DURING_EIDLE[0]", rxcdr_hold_during_eidle);
        auto rxcdr_lock_cfg = int_or_default(ci->params, ctx->id("RXCDR_LOCK_CFG"), 0);
        write_int_vector("RXCDR_LOCK_CFG[5:0]", rxcdr_lock_cfg, 6);
        auto rxcdrfreqreset_time = int_or_default(ci->params, ctx->id("RXCDRFREQRESET_TIME"), 0);
        write_int_vector("RXCDRFREQRESET_TIME[4:0]", rxcdrfreqreset_time, 5);
        auto rxcdrphreset_time = int_or_default(ci->params, ctx->id("RXCDRPHRESET_TIME"), 0);
        write_int_vector("RXCDRPHRESET_TIME[4:0]", rxcdrphreset_time, 5);

        auto rxdly_cfg = int_or_default(ci->params, ctx->id("RXDLY_CFG"), 0);
        write_int_vector("RXDLY_CFG[15:0]", rxdly_cfg, 16);
        auto rxdly_lcfg = int_or_default(ci->params, ctx->id("RXDLY_LCFG"), 0);
        write_int_vector("RXDLY_LCFG[8:0]", rxdly_lcfg, 9);
        auto rxdly_tap_cfg = int_or_default(ci->params, ctx->id("RXDLY_TAP_CFG"), 0);
        write_int_vector("RXDLY_TAP_CFG[15:0]", rxdly_tap_cfg, 16);

        write_str_bool("RXGEARBOX_EN", "RXGEARBOX_EN");

        auto rxiscanreset_time = int_or_default(ci->params, ctx->id("RXISCANRESET_TIME"), 0);
        write_int_vector("RXISCANRESET_TIME[4:0]", rxiscanreset_time, 5);

        auto rxlpm_bias_startup_disable = bool_or_default(ci->params, ctx->id("RXLPM_BIAS_STARTUP_DISABLE"), false);
        write_bit("RXLPM_BIAS_STARTUP_DISABLE[0]", rxlpm_bias_startup_disable);
        auto rxlpm_cfg = int_or_default(ci->params, ctx->id("RXLPM_CFG"), 0);
        write_int_vector("RXLPM_CFG[3:0]", rxlpm_cfg, 4);
        auto rxlpm_cfg1 = int_or_default(ci->params, ctx->id("RXLPM_CFG1"), 0);
        write_int_vector("RXLPM_CFG1[0]", rxlpm_cfg1, 1);
        auto rxlpm_cm_cfg = int_or_default(ci->params, ctx->id("RXLPM_CM_CFG"), 0);
        write_int_vector("RXLPM_CM_CFG[0]", rxlpm_cm_cfg, 1);
        auto rxlpm_gc_cfg = int_or_default(ci->params, ctx->id("RXLPM_GC_CFG"), 0);
        write_int_vector("RXLPM_GC_CFG[8:0]", rxlpm_gc_cfg, 9);
        auto rxlpm_gc_cfg2 = int_or_default(ci->params, ctx->id("RXLPM_GC_CFG2"), 0);
        write_int_vector("RXLPM_GC_CFG2[2:0]", rxlpm_gc_cfg2, 3);
        auto rxlpm_hf_cfg = int_or_default(ci->params, ctx->id("RXLPM_HF_CFG"), 0);
        write_int_vector("RXLPM_HF_CFG[13:0]", rxlpm_hf_cfg, 14);
        auto rxlpm_hf_cfg2 = int_or_default(ci->params, ctx->id("RXLPM_HF_CFG2"), 0);
        write_int_vector("RXLPM_HF_CFG2[4:0]", rxlpm_hf_cfg2, 5);
        auto rxlpm_hf_cfg3 = int_or_default(ci->params, ctx->id("RXLPM_HF_CFG3"), 0);
        write_int_vector("RXLPM_HF_CFG3[3:0]", rxlpm_hf_cfg3, 4);
        auto rxlpm_hold_during_eidle = bool_or_default(ci->params, ctx->id("RXLPM_HOLD_DURING_EIDLE"), false);
        write_bit("RXLPM_HOLD_DURING_EIDLE[0]", rxlpm_hold_during_eidle);
        auto rxlpm_incm_cfg = bool_or_default(ci->params, ctx->id("RXLPM_INCM_CFG"), false);
        write_bit("RXLPM_INCM_CFG[0]", rxlpm_incm_cfg);
        auto rxlpm_ipcm_cfg = bool_or_default(ci->params, ctx->id("RXLPM_IPCM_CFG"), false);
        write_bit("RXLPM_IPCM_CFG[0]", rxlpm_ipcm_cfg);
        auto rxlpm_lf_cfg = int_or_default(ci->params, ctx->id("RXLPM_LF_CFG"), 0);
        write_int_vector("RXLPM_LF_CFG[17:0]", rxlpm_lf_cfg, 18);
        auto rxlpm_lf_cfg2 = int_or_default(ci->params, ctx->id("RXLPM_LF_CFG2"), 0);
        write_int_vector("RXLPM_LF_CFG2[4:0]", rxlpm_lf_cfg2, 5);
        auto rxlpm_osint_cfg = int_or_default(ci->params, ctx->id("RXLPM_OSINT_CFG"), 0);
        write_int_vector("RXLPM_OSINT_CFG[2:0]", rxlpm_osint_cfg, 3);
        auto rxlpmreset_time = int_or_default(ci->params, ctx->id("RXLPMRESET_TIME"), 0);
        write_int_vector("RXLPMRESET_TIME[6:0]", rxlpmreset_time, 7);

        auto rxoob_cfg = int_or_default(ci->params, ctx->id("RXOOB_CFG"), 0);
        write_int_vector("RXOOB_CFG[6:0]", rxoob_cfg, 7);
        auto rxoob_clk_cfg = str_or_default(ci->params, ctx->id("RXOOB_CLK_CFG"), "PMA");
        if (rxoob_clk_cfg != "FABRIC" && rxoob_clk_cfg != "PMA")
            log_error("RXOOB_CLK_CFG may only have values 'FABRIC' or 'PMA' but is: '%s'\n", rxoob_clk_cfg.c_str());
        write_bit("RXOOB_CLK_CFG.FABRIC", rxoob_clk_cfg == "FABRIC");

        auto rxoscalreset_time = int_or_default(ci->params, ctx->id("RXOSCALRESET_TIME"), 0);
        write_int_vector("RXOSCALRESET_TIME[4:0]", rxoscalreset_time, 5);
        auto rxoscalreset_timeout = int_or_default(ci->params, ctx->id("RXOSCALRESET_TIMEOUT"), 0);
        write_int_vector("RXOSCALRESET_TIMEOUT[4:0]", rxoscalreset_timeout, 5);

        auto rxout_div = std::log2(int_or_default(ci->params, ctx->id("RXOUT_DIV"), 1));
        write_int_vector("RXOUT_DIV[1:0]", rxout_div, 2);

        auto rxpcsreset_time = int_or_default(ci->params, ctx->id("RXPCSRESET_TIME"), 0);
        write_int_vector("RXPCSRESET_TIME[4:0]", rxpcsreset_time, 5);

        auto rxph_cfg = int_or_default(ci->params, ctx->id("RXPH_CFG"), 0);
        write_int_vector("RXPH_CFG[23:0]", rxph_cfg, 24);
        auto rxph_monitor_sel = int_or_default(ci->params, ctx->id("RXPH_MONITOR_SEL"), 0);
        write_int_vector("RXPH_MONITOR_SEL[4:0]", rxph_monitor_sel, 5);
        auto rxphdly_cfg = int_or_default(ci->params, ctx->id("RXPHDLY_CFG"), 0);
        write_int_vector("RXPHDLY_CFG[23:0]", rxphdly_cfg, 24);

        auto rxpi_cfg0 = int_or_default(ci->params, ctx->id("RXPI_CFG0"), 0);
        write_int_vector("RXPI_CFG0[2:0]", rxpi_cfg0, 3);
        auto rxpi_cfg1 = bool_or_default(ci->params, ctx->id("RXPI_CFG1"), false);
        write_bit("RXPI_CFG1[0]", rxpi_cfg1);
        auto rxpi_cfg2 = bool_or_default(ci->params, ctx->id("RXPI_CFG2"), false);
        write_bit("RXPI_CFG2[0]", rxpi_cfg2);

        auto rxpmareset_time = int_or_default(ci->params, ctx->id("RXPMARESET_TIME"), 0);
        write_int_vector("RXPMARESET_TIME[4:0]", rxpmareset_time, 5);

        auto rxprbs_err_loopback = bool_or_default(ci->params, ctx->id("RXPRBS_ERR_LOOPBACK"), false);
        write_bit("RXPRBS_ERR_LOOPBACK[0]", rxprbs_err_loopback);

        auto rxslide_auto_wait = int_or_default(ci->params, ctx->id("RXSLIDE_AUTO_WAIT"), 7);
        write_int_vector("RXSLIDE_AUTO_WAIT[3:0]", rxslide_auto_wait, 4);
        auto rxslide_mode = str_or_default(ci->params, ctx->id("RXSLIDE_MODE"), "OFF");
        if (rxslide_mode != "OFF" && rxslide_mode != "AUTO" && rxslide_mode != "PCS" && rxslide_mode != "PMA")
            log_error("RXSLIDE_MODE may only have values 'OFF', 'AUTO', 'PCS' or 'PMA' but is: '%s'\n", rxslide_mode.c_str());
        write_bit("RXSLIDE_MODE.AUTO", rxslide_mode == "AUTO");
        write_bit("RXSLIDE_MODE.PCS",  rxslide_mode == "PCS");
        write_bit("RXSLIDE_MODE.PMA",  rxslide_mode == "PMA");

        auto rxsync_multilane = bool_or_default(ci->params, ctx->id("RXSYNC_MULTILANE"), false);
        write_bit("RXSYNC_MULTILANE[0]", rxsync_multilane);
        auto rxsync_ovrd = bool_or_default(ci->params, ctx->id("RXSYNC_OVRD"), false);
        write_bit("RXSYNC_OVRD[0]", rxsync_ovrd);
        auto rxsync_skip_da = bool_or_default(ci->params, ctx->id("RXSYNC_SKIP_DA"), false);
        write_bit("RXSYNC_SKIP_DA[0]", rxsync_skip_da);

        auto sas_max_com = int_or_default(ci->params, ctx->id("SAS_MAX_COM"), 0);
        write_int_vector("SAS_MAX_COM[6:0]", sas_max_com, 7);
        auto sas_min_com = int_or_default(ci->params, ctx->id("SAS_MIN_COM"), 0);
        write_int_vector("SAS_MIN_COM[6:0]", sas_min_com, 7);

        auto sata_burst_seq_len = int_or_default(ci->params, ctx->id("SATA_BURST_SEQ_LEN"), 0);
        write_int_vector("SATA_BURST_SEQ_LEN[3:0]", sata_burst_seq_len, 4);
        auto sata_burst_val = int_or_default(ci->params, ctx->id("SATA_BURST_VAL"), 0);
        write_int_vector("SATA_BURST_VAL[2:0]", sata_burst_val, 3);
        auto sata_eidle_val = int_or_default(ci->params, ctx->id("SATA_EIDLE_VAL"), 0);
        write_int_vector("SATA_EIDLE_VAL[2:0]", sata_eidle_val, 3);
        auto sata_max_burst = int_or_default(ci->params, ctx->id("SATA_MAX_BURST"), 0);
        write_int_vector("SATA_MAX_BURST[5:0]", sata_max_burst, 6);
        auto sata_max_init = int_or_default(ci->params, ctx->id("SATA_MAX_INIT"), 0);
        write_int_vector("SATA_MAX_INIT[5:0]", sata_max_init, 6);
        auto sata_max_wake = int_or_default(ci->params, ctx->id("SATA_MAX_WAKE"), 0);
        write_int_vector("SATA_MAX_WAKE[5:0]", sata_max_wake, 6);
        auto sata_min_burst = int_or_default(ci->params, ctx->id("SATA_MIN_BURST"), 0);
        write_int_vector("SATA_MIN_BURST[5:0]", sata_min_burst, 6);
        auto sata_min_init = int_or_default(ci->params, ctx->id("SATA_MIN_INIT"), 0);
        write_int_vector("SATA_MIN_INIT[5:0]", sata_min_init, 6);
        auto sata_min_wake = int_or_default(ci->params, ctx->id("SATA_MIN_WAKE"), 0);
        write_int_vector("SATA_MIN_WAKE[5:0]", sata_min_wake, 6);
        auto sata_pll_cfg = str_or_default(ci->params, ctx->id("SATA_PLL_CFG"), "VCO_3000MHZ");
        if (sata_pll_cfg != "VCO_3000MHZ" && sata_pll_cfg != "VCO_1500MHZ" && sata_pll_cfg != "VCO_750MHZ")
            log_error("SATA_PLL_CFG may only have values 'VCO_3000MHZ', 'VCO_1500MHZ' or 'VCO_750MHZ' but is: '%s'\n", sata_pll_cfg.c_str());
        write_bit("SATA_PLL_CFG.VCO_1500MHZ", sata_pll_cfg == "VCO_1500MHZ");
        write_bit("SATA_PLL_CFG.VCO_750MHZ",  sata_pll_cfg == "VCO_750MHZ");

        write_str_bool("SHOW_REALIGN_COMMA", "SHOW_REALIGN_COMMA");

        auto term_rcal_cfg = int_or_default(ci->params, ctx->id("TERM_RCAL_CFG"), 0);
        write_int_vector("TERM_RCAL_CFG[14:0]", term_rcal_cfg, 15);
        auto term_rcal_ovrd = int_or_default(ci->params, ctx->id("TERM_RCAL_OVRD"), 0);
        write_int_vector("TERM_RCAL_OVRD[2:0]", term_rcal_ovrd, 3);

        auto trans_time_rate = int_or_default(ci->params, ctx->id("TRANS_TIME_RATE"), 0);
        write_int_vector("TRANS_TIME_RATE[7:0]", trans_time_rate, 8);

        auto tst_rsv = int_or_default(ci->params, ctx->id("TST_RSV"), 0);
        write_int_vector("TST_RSV[31:0]", tst_rsv, 32);

        auto tx_clkmux_en = bool_or_default(ci->params, ctx->id("TX_CLKMUX_EN"), false);
        write_bit("TX_CLKMUX_EN[0]", tx_clkmux_en);
        auto tx_data_width = int_or_default(ci->params, ctx->id("TX_DATA_WIDTH"), 0);
        switch (tx_data_width) {
            case 16:
                tx_data_width = 2; break;
            case 20:
                tx_data_width = 3; break;
            case 32:
                tx_data_width = 4; break;
            case 40:
                tx_data_width = 5; break;
            default:
                log_error("Invalid TX_DATA_WIDTH parameter '%d' for GTPE2_CHANNEL instance %s\n", tx_data_width, ci->name.c_str(ctx));
        }
        write_int_vector("TX_DATA_WIDTH[2:0]", tx_data_width, 3);

        auto tx_drive_mode = str_or_default(ci->params, ctx->id("TX_DRIVE_MODE"), "DIRECT");
        if (tx_drive_mode != "DIRECT" && tx_drive_mode != "PIPE")
            log_error("TX_DRIVE_MODE may only have values 'PIPE' or 'DIRECT' but is: '%s'\n", tx_drive_mode.c_str());
        write_bit("TX_DRIVE_MODE.PIPE", tx_drive_mode == "PIPE");

        auto tx_eidle_assert_delay = int_or_default(ci->params, ctx->id("TX_EIDLE_ASSERT_DELAY"), 0);
        write_int_vector("TX_EIDLE_ASSERT_DELAY[2:0]", tx_eidle_assert_delay, 3);
        auto tx_eidle_deassert_delay = int_or_default(ci->params, ctx->id("TX_EIDLE_DEASSERT_DELAY"), 0);
        write_int_vector("TX_EIDLE_DEASSERT_DELAY[2:0]", tx_eidle_deassert_delay, 3);

        write_str_bool("TX_LOOPBACK_DRIVE_HIZ", "TX_LOOPBACK_DRIVE_HIZ");

        auto tx_maincursor_sel = bool_or_default(ci->params, ctx->id("TX_MAINCURSOR_SEL"), false);
        write_bit("TX_MAINCURSOR_SEL[0]", tx_maincursor_sel);
        auto tx_margin_full_0 = int_or_default(ci->params, ctx->id("TX_MARGIN_FULL_0"), 0b1001111);
        write_int_vector("TX_MARGIN_FULL_0[6:0]", tx_margin_full_0, 7);
        auto tx_margin_full_1 = int_or_default(ci->params, ctx->id("TX_MARGIN_FULL_1"), 0b1001111);
        write_int_vector("TX_MARGIN_FULL_1[6:0]", tx_margin_full_1, 7);
        auto tx_margin_full_2 = int_or_default(ci->params, ctx->id("TX_MARGIN_FULL_2"), 0b1001111);
        write_int_vector("TX_MARGIN_FULL_2[6:0]", tx_margin_full_2, 7);
        auto tx_margin_full_3 = int_or_default(ci->params, ctx->id("TX_MARGIN_FULL_3"), 0b1000001);
        write_int_vector("TX_MARGIN_FULL_3[6:0]", tx_margin_full_3, 7);
        auto tx_margin_full_4 = int_or_default(ci->params, ctx->id("TX_MARGIN_FULL_4"), 0b1000000);
        write_int_vector("TX_MARGIN_FULL_4[6:0]", tx_margin_full_4, 7);
        auto tx_margin_low_0 = int_or_default(ci->params, ctx->id("TX_MARGIN_LOW_0"), 0b1000111);
        write_int_vector("TX_MARGIN_LOW_0[6:0]", tx_margin_low_0, 7);
        auto tx_margin_low_1 = int_or_default(ci->params, ctx->id("TX_MARGIN_LOW_1"), 0b1000110);
        write_int_vector("TX_MARGIN_LOW_1[6:0]", tx_margin_low_1, 7);
        auto tx_margin_low_2 = int_or_default(ci->params, ctx->id("TX_MARGIN_LOW_2"), 0b1000100);
        write_int_vector("TX_MARGIN_LOW_2[6:0]", tx_margin_low_2, 7);
        auto tx_margin_low_3 = int_or_default(ci->params, ctx->id("TX_MARGIN_LOW_3"), 0b1000000);
        write_int_vector("TX_MARGIN_LOW_3[6:0]", tx_margin_low_3, 7);
        auto tx_margin_low_4 = int_or_default(ci->params, ctx->id("TX_MARGIN_LOW_4"), 0b1000000);
        write_int_vector("TX_MARGIN_LOW_4[6:0]", tx_margin_low_4, 7);
        auto tx_predriver_mode = bool_or_default(ci->params, ctx->id("TX_PREDRIVER_MODE"), false);
        write_bit("TX_PREDRIVER_MODE", tx_predriver_mode);
        auto tx_rxdetect_cfg = int_or_default(ci->params, ctx->id("TX_RXDETECT_CFG"), 0);
        write_int_vector("TX_RXDETECT_CFG[13:0]", tx_rxdetect_cfg, 14);
        auto tx_rxdetect_ref = int_or_default(ci->params, ctx->id("TX_RXDETECT_REF"), 0);
        write_int_vector("TX_RXDETECT_REF[2:0]", tx_rxdetect_ref, 3);
        auto tx_xclk_sel = str_or_default(ci->params, ctx->id("TX_XCLK_SEL"), "TXUSR");
        if (tx_xclk_sel != "TXUSR" && tx_xclk_sel != "TXOUT")
            log_error("TX_XCLK_SEL may only have values 'TXOUT' or 'TXUSR' but is: '%s'\n", tx_xclk_sel.c_str());
        write_bit("TX_XCLK_SEL.TXUSR", tx_xclk_sel == "TXUSR");
        auto tx_clk25_div = int_or_default(ci->params, ctx->id("TX_CLK25_DIV"), 0) - 1;
        write_int_vector("TX_CLK25_DIV[4:0]", tx_clk25_div, 5);
        auto tx_deemph0 = int_or_default(ci->params, ctx->id("TX_DEEMPH0"), 0);
        write_int_vector("TX_DEEMPH0[5:0]", tx_deemph0, 6);
        auto tx_deemph1 = int_or_default(ci->params, ctx->id("TX_DEEMPH1"), 0);
        write_int_vector("TX_DEEMPH1[5:0]", tx_deemph1, 6);
        write_str_bool("TXBUF_EN", "TXBUF_EN");
        write_str_bool("TXBUF_RESET_ON_RATE_CHANGE", "TXBUF_RESET_ON_RATE_CHANGE", "TRUE");
        auto txdly_cfg = int_or_default(ci->params, ctx->id("TXDLY_CFG"), 0);
        write_int_vector("TXDLY_CFG[15:0]", txdly_cfg, 16);
        auto txdly_lcfg = int_or_default(ci->params, ctx->id("TXDLY_LCFG"), 0);
        write_int_vector("TXDLY_LCFG[8:0]", txdly_lcfg, 9);
        auto txdly_tap_cfg = int_or_default(ci->params, ctx->id("TXDLY_TAP_CFG"), 0);
        write_int_vector("TXDLY_TAP_CFG[15:0]", txdly_tap_cfg, 16);
        write_str_bool("TXGEARBOX_EN", "TXGEARBOX_EN");
        auto txoob_cfg = bool_or_default(ci->params, ctx->id("TXOOB_CFG"), false);
        write_bit("TXOOB_CFG[0]", txoob_cfg);
        auto txout_div = std::log2(int_or_default(ci->params, ctx->id("TXOUT_DIV"), 1));
        write_int_vector("TXOUT_DIV[1:0]", txout_div, 2);
        auto txpcsreset_time = int_or_default(ci->params, ctx->id("TXPCSRESET_TIME"), 0);
        write_int_vector("TXPCSRESET_TIME[4:0]", txpcsreset_time, 5);

        auto txph_cfg = int_or_default(ci->params, ctx->id("TXPH_CFG"), 0);
        write_int_vector("TXPH_CFG[15:0]", txph_cfg, 16);
        auto txph_monitor_sel = int_or_default(ci->params, ctx->id("TXPH_MONITOR_SEL"), 0);
        write_int_vector("TXPH_MONITOR_SEL[4:0]", txph_monitor_sel, 5);
        auto txphdly_cfg = int_or_default(ci->params, ctx->id("TXPHDLY_CFG"), 0);
        write_int_vector("TXPHDLY_CFG[23:0]", txphdly_cfg, 24);

        auto txpi_grey_sel = bool_or_default(ci->params, ctx->id("TXPI_GREY_SEL"), false);
        write_bit("TXPI_GREY_SEL[0]", txpi_grey_sel);
        auto txpi_invstrobe_sel = bool_or_default(ci->params, ctx->id("TXPI_INVSTROBE_SEL"), false);
        write_bit("TXPI_INVSTROBE_SEL[0]", txpi_invstrobe_sel);
        auto txpi_ppm_cfg = int_or_default(ci->params, ctx->id("TXPI_PPM_CFG"), 0);
        write_int_vector("TXPI_PPM_CFG[7:0]", txpi_ppm_cfg, 8);
        auto txpi_ppmclk_sel = str_or_default(ci->params, ctx->id("TXPI_PPMCLK_SEL"), "TXUSRCLK");
        if (txpi_ppmclk_sel != "TXUSRCLK" && txpi_ppmclk_sel != "TXUSRCLK2")
            log_error("TXPI_PPMCLK_SEL may only have values 'TXUSRCLK2' or 'TXUSRCLK' but is: '%s'\n", txpi_ppmclk_sel.c_str());
        write_bit("TXPI_PPMCLK_SEL.TXUSRCLK2", txpi_ppmclk_sel == "TXUSRCLK2");
        auto txpi_synfreq_ppm = int_or_default(ci->params, ctx->id("TXPI_SYNFREQ_PPM"), 0);
        if (txpi_synfreq_ppm == 0) log_error("TXPI_SYNFREQ_PPM must not be zero!\n");
        write_int_vector("TXPI_SYNFREQ_PPM[2:0]", txpi_synfreq_ppm, 3);
        auto txpi_cfg0 = int_or_default(ci->params, ctx->id("TXPI_CFG0"), 0);
        write_int_vector("TXPI_CFG0[1:0]", txpi_cfg0, 2);
        auto txpi_cfg1 = int_or_default(ci->params, ctx->id("TXPI_CFG1"), 0);
        write_int_vector("TXPI_CFG1[1:0]", txpi_cfg1, 2);
        auto txpi_cfg2 = int_or_default(ci->params, ctx->id("TXPI_CFG2"), 0);
        write_int_vector("TXPI_CFG2[1:0]", txpi_cfg2, 2);
        auto txpi_cfg3 = bool_or_default(ci->params, ctx->id("TXPI_CFG3"), false);
        write_bit("TXPI_CFG3[0]", txpi_cfg3);
        auto txpi_cfg4 = bool_or_default(ci->params, ctx->id("TXPI_CFG4"), false);
        write_bit("TXPI_CFG4[0]", txpi_cfg4);
        auto txpi_cfg5 = int_or_default(ci->params, ctx->id("TXPI_CFG5"), 0);
        write_int_vector("TXPI_CFG5[1:0]", txpi_cfg5, 2);
        auto txpmareset_time = int_or_default(ci->params, ctx->id("TXPMARESET_TIME"), 0);
        write_int_vector("TXPMARESET_TIME[4:0]", txpmareset_time, 5);

        auto txsync_multilane = bool_or_default(ci->params, ctx->id("TXSYNC_MULTILANE"), false);
        write_bit("TXSYNC_MULTILANE[0]", txsync_multilane);
        auto txsync_ovrd = bool_or_default(ci->params, ctx->id("TXSYNC_OVRD"), false);
        write_bit("TXSYNC_OVRD[0]", txsync_ovrd);
        auto txsync_skip_da = bool_or_default(ci->params, ctx->id("TXSYNC_SKIP_DA"), false);
        write_bit("TXSYNC_SKIP_DA[0]", txsync_skip_da);

        auto ucodeer_clr = bool_or_default(ci->params, ctx->id("UCODEER_CLR"), false);
        write_bit("UCODEER_CLR[0]", ucodeer_clr);

        auto use_pcs_clk_phase_sel = bool_or_default(ci->params, ctx->id("USE_PCS_CLK_PHASE_SEL"), false);
        write_bit("USE_PCS_CLK_PHASE_SEL[0]", use_pcs_clk_phase_sel);

        pop(); // GTPE2_CHANNEL
        pop(); // tile name
    }
    void write_gtx_pll(CellInfo *ci)
    {
        push(uarch->tile_name(ci->bel.tile));

        push("GTXE2_COMMON");
        write_bit("IN_USE");
        write_bit("ENABLE_DRP", bool_or_default(ci->params, ctx->id("_DRPCLK_USED"), false));
        write_bit("BOTH_GTREFCLK_USED", bool_or_default(ci->params, ctx->id("_BOTH_GTREFCLK_USED"), false));
        write_bit("GTREFCLK0_USED", bool_or_default(ci->params, ctx->id("_GTREFCLK0_USED"), false));
        write_bit("GTREFCLK1_USED", bool_or_default(ci->params, ctx->id("_GTREFCLK1_USED"), false));
        if (bool_or_default(ci->params, ctx->id("_GTGREFCLK_USED"), false)) {
            write_bit("GTREFCLK0_USED");
            write_bit("GTREFCLK1_USED");
        }
        auto clkswing_cfg = int_or_default(ci->params, ctx->id("CLKSWING_CFG"), 3);
        if (clkswing_cfg != 3) log_warning("%s/%s: According to ug476, CLK should always be 0b11\n",
                                           ci->hierpath.c_str(ctx), ci->name.c_str(ctx));
        write_int_vector("IBUFDS_GTE2.CLKSWING_CFG[1:0]", clkswing_cfg, 2);
        write_bit("INV_DRPCLK", bool_or_default(ci->params, ctx->id("IS_DRPCLK_INVERTED")));
        write_bit("INV_QPLLLOCKDETCLK", bool_or_default(ci->params, ctx->id("IS_QPLLLOCKDETCLK_INVERTED")));

        write_bit("QPLL_DMONITOR_SEL[0]", bool_or_default(ci->params, ctx->id("QPLL_DMONITOR_SEL")));
        
        auto bias_cfg_found = ci->params.find(ctx->id("BIAS_CFG"));
        auto bias_cfg = bias_cfg_found == ci->params.end() ? 0b1000000000000000000000000000001000000000000UL : bias_cfg_found->second.as_int64();
        write_int_vector("BIAS_CFG[63:0]", bias_cfg, 64);
        auto common_cfg = int_or_default(ci->params, ctx->id("COMMON_CFG"), 0);
        write_int_vector("COMMON_CFG[31:0]", common_cfg, 32);

        // according to ug476, these attributes contain magic undocumented and reserved wizard values
        // TODO: check values
        write_int_vector("QPLL_CFG[26:0]", 0b11010000000000111000001, 27);
        write_int_vector("QPLL_CLKOUT_CFG[3:0]", 0, 4);
        write_int_vector("QPLL_COARSE_FREQ_OVRD[5:0]", 0b10000, 6);
        auto coarse_freq_ovrd_en = int_or_default(ci->params, ctx->id("QPLL_COARSE_FREQ_OVRD_EN"), 0);
        if (coarse_freq_ovrd_en > 0)
            log_warning("According to UG476, the QPLL_COARSE_FREQ_OVRD_EN attribute must be 0, but it is not. Be sure you know what you are doing.");
        write_bit("QPLL_COARSE_FREQ_OVRD_EN[0]", coarse_freq_ovrd_en >= 1);
        write_int_vector("QPLL_INIT_CFG[23:0]", 0b110, 24);
        write_int_vector("QPLL_LOCK_CFG[15:0]", 0b10000111101000, 16);
        write_int_vector("QPLL_LPF[3:0]", 0b1111, 4);
        write_int_vector("QPLL_CP[9:0]", 0b11111, 10);
        auto cp_monitor_en = int_or_default(ci->params, ctx->id("QPLL_CP_MONITOR_EN"), 0);
        if (cp_monitor_en > 0)
            log_warning("According to UG476, the QPLL_CP_MONITOR_EN attribute must be 0, but it is not. Be sure you know what you are doing.");
        write_bit("QPLL_CP_MONITOR_EN[0]", cp_monitor_en >= 1);
        write_bit("QPLL_DMONITOR_SEL[0]", 0); // TODO: find real vivado default value

        auto qpll_refclk_div = int_or_default(ci->params, ctx->id("QPLL_REFCLK_DIV"), 1);
        if (qpll_refclk_div < 1 || qpll_refclk_div > 4)
            log_error("QPLL_REFCLK_DIV can only range from 1 to 4, but is: %d", qpll_refclk_div);
        auto real_qpll_refclk_div = qpll_refclk_div == 1 ? 16 : qpll_refclk_div - 2;
        write_int_vector("QPLL_REFCLK_DIV[4:0]", real_qpll_refclk_div, 5);

        auto qpll_fbdiv = int_or_default(ci->params, ctx->id("QPLL_FBDIV"), 1);
        write_int_vector("QPLL_FBDIV[9:0]", qpll_fbdiv, 10);

        auto fbdiv_monitor_en = int_or_default(ci->params, ctx->id("FBDIV_MONITOR_EN"), 0);
        if (fbdiv_monitor_en > 0)
            log_warning("According to UG476, the QPLL_FBDIV_MONITOR_EN attribute must be 0, but it is 1. Be sure you know what you are doing.");
        write_bit("QPLL_FBDIV_MONITOR_EN[0]", fbdiv_monitor_en >= 1);

        auto qpll_fbdiv_ratio = int_or_default(ci->params, ctx->id("QPLL_FBDIV_RATIO"), 1);
        if (qpll_fbdiv_ratio > 1)
            log_error("QPLL_FBDIV_ratio can only be 0 or 1, but is: %d", qpll_fbdiv_ratio);
        write_bit("QPLL_FBDIV_RATIO[0]", qpll_fbdiv_ratio == 1);

        auto pll_clkout_cfg = int_or_default(ci->params, ctx->id("PLL_CLKOUT_CFG"), 0);
        write_int_vector("PLL_CLKOUT_CFG[3:0]", pll_clkout_cfg, 4);

        pop(); // GTXE2_COMMON
        pop(); // tile name
    }
    void write_gtx_channel(CellInfo *ci)
    {
        push(uarch->tile_name(ci->bel.tile));
        push("GTXE2_CHANNEL");

        write_bit("IN_USE");

        auto write_str_bool = [&](std::string attribute, std::string bit, std::string deflt = "FALSE") {
            auto val = str_or_default(ci->params, ctx->id(attribute), deflt);
            boost::algorithm::to_upper(val);
            write_bit(bit, val == "TRUE");
        };

        write_str_bool("ALIGN_COMMA_DOUBLE", "ALIGN_COMMA_DOUBLE");

        auto align_comma_enable = int_or_default(ci->params, ctx->id("ALIGN_COMMA_ENABLE"), 0b1111111111);
        write_int_vector("ALIGN_COMMA_ENABLE[9:0]", align_comma_enable, 10);

        auto align_comma_word = int_or_default(ci->params, ctx->id("ALIGN_COMMA_WORD"), 1);
        if (!(align_comma_word == 1 || align_comma_word == 2 || align_comma_word == 4))
            log_error("ALIGN_COMMA_WORD may only be 1, 2 or 4 but is: %d\n", align_comma_word);
        write_int_vector("ALIGN_COMMA_WORD[2:0]", align_comma_word, 3);

        write_str_bool("ALIGN_MCOMMA_DET", "ALIGN_MCOMMA_DET");
        auto align_mcomma_value = int_or_default(ci->params, ctx->id("ALIGN_MCOMMA_VALUE"), 0b1010000011);
        write_int_vector("ALIGN_MCOMMA_VALUE[9:0]", align_mcomma_value, 10);

        write_str_bool("ALIGN_PCOMMA_DET", "ALIGN_PCOMMA_DET");
        auto align_pcomma_value = int_or_default(ci->params, ctx->id("ALIGN_PCOMMA_VALUE"), 0b0101111100);
        write_int_vector("ALIGN_PCOMMA_VALUE[9:0]", align_pcomma_value, 10);

        auto cbcc_data_source_sel = str_or_default(ci->params, ctx->id("CBCC_DATA_SOURCE_SEL"), "DECODED");
        if (cbcc_data_source_sel == "DECODED") write_bit("CBCC_DATA_SOURCE_SEL.DECODED");

        write_str_bool("CHAN_BOND_KEEP_ALIGN", "CHAN_BOND_KEEP_ALIGN");
        auto chan_bond_max_skew = int_or_default(ci->params, ctx->id("CHAN_BOND_MAX_SKEW"), 0);
        if (chan_bond_max_skew < 1 || 14 < chan_bond_max_skew)
            log_error("CHAN_BOND_MAX_SKEW may only range from 1 to 14, but is: %d\n", chan_bond_max_skew);
        write_int_vector("CHAN_BOND_MAX_SKEW[3:0]", chan_bond_max_skew, 4);

        auto chan_bond_seq_1_enable = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_1_ENABLE"), 0);
        write_int_vector("CHAN_BOND_SEQ_1_ENABLE[3:0]", chan_bond_seq_1_enable, 4);
        auto chan_bond_seq_1_1 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_1_1"), 0);
        write_int_vector("CHAN_BOND_SEQ_1_1[9:0]", chan_bond_seq_1_1, 10);
        auto chan_bond_seq_1_2 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_1_2"), 0);
        write_int_vector("CHAN_BOND_SEQ_1_2[9:0]", chan_bond_seq_1_2, 10);
        auto chan_bond_seq_1_3 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_1_3"), 0);
        write_int_vector("CHAN_BOND_SEQ_1_3[9:0]", chan_bond_seq_1_3, 10);
        auto chan_bond_seq_1_4 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_1_4"), 0);
        write_int_vector("CHAN_BOND_SEQ_1_4[9:0]", chan_bond_seq_1_4, 10);

        write_str_bool("CHAN_BOND_SEQ_2_USE", "CHAN_BOND_SEQ_2_USE");
        auto chan_bond_seq_2_enable = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_2_ENABLE"), 0);
        write_int_vector("CHAN_BOND_SEQ_2_ENABLE[3:0]", chan_bond_seq_2_enable, 4);
        auto chan_bond_seq_2_1 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_2_1"), 0);
        write_int_vector("CHAN_BOND_SEQ_2_1[9:0]", chan_bond_seq_2_1, 10);
        auto chan_bond_seq_2_2 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_2_2"), 0);
        write_int_vector("CHAN_BOND_SEQ_2_2[9:0]", chan_bond_seq_2_2, 10);
        auto chan_bond_seq_2_3 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_2_3"), 0);
        write_int_vector("CHAN_BOND_SEQ_2_3[9:0]", chan_bond_seq_2_3, 10);
        auto chan_bond_seq_2_4 = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_2_4"), 0);
        write_int_vector("CHAN_BOND_SEQ_2_4[9:0]", chan_bond_seq_2_4, 10);

        auto chan_bond_seq_len = int_or_default(ci->params, ctx->id("CHAN_BOND_SEQ_LEN"), 0);
        if (!(chan_bond_seq_len == 1 || chan_bond_seq_len == 2 || chan_bond_seq_len == 4))
            log_error("CHAN_BOND_SEQ_LEN may only be 1, 2 or 4, but is: %d\n", chan_bond_seq_len);
        write_int_vector("CHAN_BOND_SEQ_LEN[1:0]", chan_bond_seq_len - 1, 2);

        write_str_bool("CLK_COR_KEEP_IDLE", "CLK_COR_KEEP_IDLE");
        auto clk_cor_max_lat = int_or_default(ci->params, ctx->id("CLK_COR_MAX_LAT"), 0);
        write_int_vector("CLK_COR_MAX_LAT[5:0]", clk_cor_max_lat, 6);
        auto clk_cor_min_lat = int_or_default(ci->params, ctx->id("CLK_COR_MIN_LAT"), 0);
        write_int_vector("CLK_COR_MIN_LAT[5:0]", clk_cor_min_lat, 6);
        write_str_bool("CLK_COR_PRECEDENCE", "CLK_COR_PRECEDENCE");
        auto clk_cor_repeat_wait = int_or_default(ci->params, ctx->id("CLK_COR_REPEAT_WAIT"), 0);
        write_int_vector("CLK_COR_REPEAT_WAIT[4:0]", clk_cor_repeat_wait, 5);

        auto clk_cor_seq_1_enable = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_1_ENABLE"), 0);
        write_int_vector("CLK_COR_SEQ_1_ENABLE[3:0]", clk_cor_seq_1_enable, 4);
        auto clk_cor_seq_1_1 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_1_1"), 0);
        write_int_vector("CLK_COR_SEQ_1_1[9:0]", clk_cor_seq_1_1, 10);
        auto clk_cor_seq_1_2 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_1_2"), 0);
        write_int_vector("CLK_COR_SEQ_1_2[9:0]", clk_cor_seq_1_2, 10);
        auto clk_cor_seq_1_3 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_1_3"), 0);
        write_int_vector("CLK_COR_SEQ_1_3[9:0]", clk_cor_seq_1_3, 10);
        auto clk_cor_seq_1_4 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_1_4"), 0);
        write_int_vector("CLK_COR_SEQ_1_4[9:0]", clk_cor_seq_1_4, 10);

        write_str_bool("CLK_COR_SEQ_2_USE", "CLK_COR_SEQ_2_USE");
        auto clk_cor_seq_2_enable = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_2_ENABLE"), 0);
        write_int_vector("CLK_COR_SEQ_2_ENABLE[3:0]", clk_cor_seq_2_enable, 4);
        auto clk_cor_seq_2_1 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_2_1"), 0);
        write_int_vector("CLK_COR_SEQ_2_1[9:0]", clk_cor_seq_2_1, 10);
        auto clk_cor_seq_2_2 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_2_2"), 0);
        write_int_vector("CLK_COR_SEQ_2_2[9:0]", clk_cor_seq_2_2, 10);
        auto clk_cor_seq_2_3 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_2_3"), 0);
        write_int_vector("CLK_COR_SEQ_2_3[9:0]", clk_cor_seq_2_3, 10);
        auto clk_cor_seq_2_4 = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_2_4"), 0);
        write_int_vector("CLK_COR_SEQ_2_4[9:0]", clk_cor_seq_2_4, 10);

        auto clk_cor_seq_len = int_or_default(ci->params, ctx->id("CLK_COR_SEQ_LEN"), 0);
        if (clk_cor_seq_len < 1 || 4 < clk_cor_seq_len)
            log_error("CLK_COR_SEQ_LEN may only range from 1 to 4, but is: %d\n", clk_cor_seq_len);
        write_int_vector("CLK_COR_SEQ_LEN[1:0]", clk_cor_seq_len - 1, 2);

        write_str_bool("CLK_CORRECT_USE", "CLK_CORRECT_USE");

        auto cpll_cfg = int_or_default(ci->params, ctx->id("CPLL_CFG"), 0b101111000000011111011100);
        write_int_vector("CPLL_CFG[23:0]", cpll_cfg, 24);

        auto cpll_fbdiv = int_or_default(ci->params, ctx->id("CPLL_FBDIV"), 1);
        switch (cpll_fbdiv) {
            case 1: cpll_fbdiv  = 16; break;
            case 2: cpll_fbdiv  =  0; break;
            case 3: cpll_fbdiv  =  1; break;
            case 4: cpll_fbdiv  =  2; break;
            case 5: cpll_fbdiv  =  3; break;
            case 6: cpll_fbdiv  =  5; break;
            case 8: cpll_fbdiv  =  6; break;
            case 10: cpll_fbdiv =  7; break;
            case 12: cpll_fbdiv = 13; break;
            case 16: cpll_fbdiv = 14; break;
            case 20: cpll_fbdiv = 15; break;
            default:
                log_error("CPLL_FBDIV can only be 1, 2, 3, 4, 5, 6, 8, 10, 12, 16 or 20, but is: %d", cpll_fbdiv);
                break;
        }
        write_int_vector("CPLL_FBDIV[4:0]", cpll_fbdiv, 5);

        auto cpll_fbdiv_45 = int_or_default(ci->params, ctx->id("CPLL_FBDIV_45"), 4);
        if (cpll_fbdiv_45 < 4 || cpll_fbdiv_45 > 5)
            log_error("CPLL_FBDIV_45 can only be 4 or 5, but is: %d", cpll_fbdiv);
        write_bit("CPLL_FBDIV_45[0]", cpll_fbdiv_45 == 5);

        auto cpll_init_cfg = int_or_default(ci->params, ctx->id("CPLL_INIT_CFG"), 0b11110);
        write_int_vector("CPLL_INIT_CFG[4:0]", cpll_init_cfg, 5);
        auto cpll_lock_cfg = int_or_default(ci->params, ctx->id("CPLL_LOCK_CFG"), 0b111101000);
        write_int_vector("CPLL_LOCK_CFG[8:0]", cpll_lock_cfg, 9);

        auto cpll_refclk_div = int_or_default(ci->params, ctx->id("CPLL_REFCLK_DIV"), 1);
        switch (cpll_refclk_div) {
            case 1: cpll_refclk_div  = 16; break;
            case 2: cpll_refclk_div  =  0; break;
            case 3: cpll_refclk_div  =  1; break;
            case 4: cpll_refclk_div  =  2; break;
            case 5: cpll_refclk_div  =  3; break;
            case 6: cpll_refclk_div  =  5; break;
            case 8: cpll_refclk_div  =  6; break;
            case 10: cpll_refclk_div =  7; break;
            case 12: cpll_refclk_div = 13; break;
            case 16: cpll_refclk_div = 14; break;
            case 20: cpll_refclk_div = 15; break;
            default:
                log_error("CPLL_REFCLK_DIV can only be 1, 2, 3, 4, 5, 6, 8, 10, 12, 16 or 20, but is: %d", cpll_refclk_div);
                break;
        }
        write_int_vector("CPLL_REFCLK_DIV[4:0]", cpll_refclk_div, 5);

        write_str_bool("DEC_MCOMMA_DETECT", "DEC_MCOMMA_DETECT");
        write_str_bool("DEC_PCOMMA_DETECT", "DEC_PCOMMA_DETECT");
        write_str_bool("DEC_VALID_COMMA_ONLY", "DEC_VALID_COMMA_ONLY");

        auto dmonitor_cfg = int_or_default(ci->params, ctx->id("DMONITOR_CFG"), 0x008101);
        write_int_vector("DMONITOR_CFG[23:0]", dmonitor_cfg, 24);

        auto es_control = int_or_default(ci->params, ctx->id("ES_CONTROL"), 0);
        write_int_vector("ES_CONTROL[5:0]", es_control, 6);
        write_str_bool("ES_ERRDET_EN", "ES_ERRDET_EN");
        write_str_bool("ES_EYE_SCAN_EN", "ES_EYE_SCAN_EN");
        auto es_pma_cfg = int_or_default(ci->params, ctx->id("ES_PMA_CFG"), 0);
        write_int_vector("ES_PMA_CFG[9:0]", es_pma_cfg, 10);
        auto es_prescale = int_or_default(ci->params, ctx->id("ES_PRESCALE"), 0);
        write_int_vector("ES_PRESCALE[4:0]", es_prescale, 5);
        auto es_qual_mask = int_or_default(ci->params, ctx->id("ES_QUAL_MASK"), 0);
        write_int_vector("ES_QUAL_MASK[79:0]", es_qual_mask, 80);
        auto es_qualifier = int_or_default(ci->params, ctx->id("ES_QUALIFIER"), 0);
        write_int_vector("ES_QUALIFIER[79:0]", es_qualifier, 80);
        auto es_sdata_mask = int_or_default(ci->params, ctx->id("ES_SDATA_MASK"), 0);
        write_int_vector("ES_SDATA_MASK[79:0]", es_sdata_mask, 80);
        auto es_vert_offset = int_or_default(ci->params, ctx->id("ES_VERT_OFFSET"), 0);
        write_int_vector("ES_VERT_OFFSET[8:0]", es_vert_offset, 9);
        auto es_horz_offset = int_or_default(ci->params, ctx->id("ES_HORZ_OFFSET"), 0);
        write_int_vector("ES_HORZ_OFFSET[11:0]", es_horz_offset, 12);

        auto fts_deskew_seq_enable = int_or_default(ci->params, ctx->id("FTS_DESKEW_SEQ_ENABLE"), 0b1111);
        write_int_vector("FTS_DESKEW_SEQ_ENABLE[3:0]", fts_deskew_seq_enable, 4);
        auto fts_lane_deskew_cfg = int_or_default(ci->params, ctx->id("FTS_LANE_DESKEW_CFG"), 0);
        write_int_vector("FTS_LANE_DESKEW_CFG[3:0]", fts_lane_deskew_cfg, 4);
        write_str_bool("FTS_LANE_DESKEW_EN", "FTS_LANE_DESKEW_EN");

        auto gearbox_mode = int_or_default(ci->params, ctx->id("GEARBOX_MODE"), 0);
        write_int_vector("GEARBOX_MODE[2:0]", gearbox_mode, 3);

        auto write_inv = [&](std::string name) {
            write_bit("INV_" + name, bool_or_default(ci->params, ctx->id("IS_" + name + "_INVERTED"), false));
        };
        // only these have been fuzzed yet,
        write_inv("DRPCLK");
        write_inv("RXUSRCLK");
        write_inv("RXUSRCLK2");
        write_inv("TXPHDLYTSTCLK");
        write_inv("TXUSRCLK");
        write_inv("TXUSRCLK2");
        write_inv("RXUSRCLK2");
        write_inv("TXUSRCLK2");
        write_inv("CPLLLOCKDETCLK");
        write_bit("GTREFCLK0_USED", bool_or_default(ci->params, ctx->id("_GTREFCLK0_USED"), false));
        write_bit("GTREFCLK1_USED", bool_or_default(ci->params, ctx->id("_GTREFCLK1_USED"), false));

        auto outrefclk_sel_inv = int_or_default(ci->params, ctx->id("OUTREFCLK_SEL_INV"), 0b10);
        write_int_vector("OUTREFCLK_SEL_INV[1:0]", outrefclk_sel_inv, 2);

        write_str_bool("PCS_PCIE_EN", "PCS_PCIE_EN");

        auto rsvd_attr = int_or_default(ci->params, ctx->id("PCS_RSVD_ATTR"), 0);
        write_int_vector("PCS_RSVD_ATTR[47:0]", rsvd_attr, 48);

        auto pd_trans_time_from_p2 = int_or_default(ci->params, ctx->id("PD_TRANS_TIME_FROM_P2"), 0);
        write_int_vector("PD_TRANS_TIME_FROM_P2[11:0]", pd_trans_time_from_p2, 12);
        auto pd_trans_time_none_p2 = int_or_default(ci->params, ctx->id("PD_TRANS_TIME_NONE_P2"), 0);
        write_int_vector("PD_TRANS_TIME_NONE_P2[7:0]", pd_trans_time_none_p2, 8);
        auto pd_trans_time_to_p2 = int_or_default(ci->params, ctx->id("PD_TRANS_TIME_TO_P2"), 0);
        write_int_vector("PD_TRANS_TIME_TO_P2[7:0]", pd_trans_time_to_p2, 8);

        auto pma_rsv = int_or_default(ci->params, ctx->id("PMA_RSV"), 0);
        write_int_vector("PMA_RSV[31:0]", pma_rsv, 32);
        auto pma_rsv2 = int_or_default(ci->params, ctx->id("PMA_RSV2"), 0);
        write_int_vector("PMA_RSV2[15:0]", pma_rsv2, 16);
        auto pma_rsv3 = int_or_default(ci->params, ctx->id("PMA_RSV3"), 0);
        write_int_vector("PMA_RSV3[1:0]", pma_rsv3, 2);
        auto pma_rsv4 = int_or_default(ci->params, ctx->id("PMA_RSV4"), 0);
        write_int_vector("PMA_RSV4[3:0]", pma_rsv4, 4);

        auto rx_bias_cfg = int_or_default(ci->params, ctx->id("RX_BIAS_CFG"), 0);
        write_int_vector("RX_BIAS_CFG[11:0]", rx_bias_cfg, 12);

        auto rx_buffer_cfg = int_or_default(ci->params, ctx->id("RX_BUFFER_CFG"), 0);
        write_int_vector("RX_BUFFER_CFG[5:0]", rx_buffer_cfg, 6);
        auto rx_cm_sel = int_or_default(ci->params, ctx->id("RX_CM_SEL"), 0);
        write_int_vector("RX_CM_SEL[1:0]", rx_cm_sel, 2);
        auto rx_cm_trim = int_or_default(ci->params, ctx->id("RX_CM_TRIM"), 0);
        write_int_vector("RX_CM_TRIM[2:0]", rx_cm_trim, 3);
        auto rx_data_width = int_or_default(ci->params, ctx->id("RX_DATA_WIDTH"), 0);
        switch (rx_data_width) {
            case 16:
                rx_data_width = 2; break;
            case 20:
                rx_data_width = 3; break;
            case 32:
                rx_data_width = 4; break;
            case 40:
                rx_data_width = 5; break;
            case 64:
                rx_data_width = 6; break;
            case 80:
                rx_data_width = 7; break;
            default:
                log_error("Invalid RX_DATA_WIDTH parameter '%d' for GTXE2_CHANNEL instance %s\n", rx_data_width, ci->name.c_str(ctx));
        }
        write_int_vector("RX_DATA_WIDTH[2:0]", rx_data_width, 3);
        auto rx_int_datawidth = bool_or_default(ci->params, ctx->id("RX_INT_DATAWIDTH"), false);
        write_bit("RX_INT_DATAWIDTH[0]", rx_int_datawidth);
        auto rx_ddi_sel = int_or_default(ci->params, ctx->id("RX_DDI_SEL"), 0);
        write_int_vector("RX_DDI_SEL[5:0]", rx_ddi_sel, 6);
        auto rx_debug_cfg = int_or_default(ci->params, ctx->id("RX_DEBUG_CFG"), 0);
        write_int_vector("RX_DEBUG_CFG[10:0]", rx_debug_cfg, 11);
        write_str_bool("RX_DEFER_RESET_BUF_EN", "RX_DEFER_RESET_BUF_EN");

        // default values, see ug476, default values from wizards not mentioned in the handbook taken from LiteX
        auto rx_dfe_gain_cfg = int_or_default(ci->params, ctx->id("RX_DFE_GAIN_CFG"), 0x020FEA);
        write_int_vector("RX_DFE_GAIN_CFG[17:0]", rx_dfe_gain_cfg, 18);
        auto rx_dfe_h2_cfg = int_or_default(ci->params, ctx->id("RX_DFE_H2_CFG"), 0x000);
        write_int_vector("RX_DFE_H2_CFG[11:0]", rx_dfe_h2_cfg, 12);
        auto rx_dfe_h3_cfg = int_or_default(ci->params, ctx->id("RX_DFE_H3_CFG"), 0x040);
        write_int_vector("RX_DFE_H3_CFG[11:0]", rx_dfe_h3_cfg, 12);
        auto rx_dfe_h4_cfg = int_or_default(ci->params, ctx->id("RX_DFE_H4_CFG"), 0x0f0);
        write_int_vector("RX_DFE_H4_CFG[10:0]", rx_dfe_h4_cfg, 11);
        auto rx_dfe_h5_cfg = int_or_default(ci->params, ctx->id("RX_DFE_H5_CFG"), 0x0e0);
        write_int_vector("RX_DFE_H5_CFG[10:0]", rx_dfe_h5_cfg, 11);
        auto rx_dfe_kl_cfg = int_or_default(ci->params, ctx->id("RX_DFE_KL_CFG"), 0b11111110);
        write_int_vector("RX_DFE_KL_CFG[12:0]", rx_dfe_kl_cfg, 13);
        auto rx_dfe_kl_cfg2 = int_or_default(ci->params, ctx->id("RX_DFE_KL_CFG2"), 0b110000000100010100100010101100);
        write_int_vector("RX_DFE_KL_CFG2[31:0]", rx_dfe_kl_cfg2, 32);
        auto rx_dfe_lpm_cfg = int_or_default(ci->params, ctx->id("RX_DFE_LPM_CFG"), 0b100101010100);
        write_int_vector("RX_DFE_LPM_CFG[11:0]", rx_dfe_lpm_cfg, 12);
        auto rx_dfe_lpm_hold_during_eidle = bool_or_default(ci->params, ctx->id("RX_DFE_LPM_HOLD_DURING_EIDLE"), false);
        write_bit("RX_DFE_LPM_HOLD_DURING_EIDLE[0]", rx_dfe_lpm_hold_during_eidle);
        auto rx_dfe_ut_cfg = int_or_default(ci->params, ctx->id("RX_DFE_UT_CFG"), 0x11E00);
        write_int_vector("RX_DFE_UT_CFG[16:0]", rx_dfe_ut_cfg, 17);
        auto rx_dfe_vp_cfg = int_or_default(ci->params, ctx->id("RX_DFE_VP_CFG"), 0x03F03);
        write_int_vector("RX_DFE_VP_CFG[16:0]", rx_dfe_vp_cfg, 17);
        auto rx_dfe_xyd_cfg = int_or_default(ci->params, ctx->id("RX_DFE_XYD_CFG"), 0);
        write_int_vector("RX_DFE_XYD_CFG[12:0]", rx_dfe_xyd_cfg, 13);

        write_str_bool("RX_DISPERR_SEQ_MATCH", "RX_DISPERR_SEQ_MATCH");
        auto rx_os_cfg = int_or_default(ci->params, ctx->id("RX_OS_CFG"), 0);
        write_int_vector("RX_OS_CFG[12:0]", rx_os_cfg, 13);
        auto rx_sig_valid_dly = int_or_default(ci->params, ctx->id("RX_SIG_VALID_DLY"), 0) - 1;
        write_int_vector("RX_SIG_VALID_DLY[4:0]", rx_sig_valid_dly, 5);
        auto rx_xclk_sel = str_or_default(ci->params, ctx->id("RX_XCLK_SEL"), "RXUSR");
        if (rx_xclk_sel != "RXUSR" && rx_xclk_sel != "RXREC")
            log_error("RX_XCLK_SEL may only have values 'RXREC' or 'RXUSR' but is: '%s'\n", rx_xclk_sel.c_str());
        write_bit("RX_XCLK_SEL.RXUSR", rx_xclk_sel == "RXUSR");
        auto rx_clk25_div = int_or_default(ci->params, ctx->id("RX_CLK25_DIV"), 0) - 1;
        write_int_vector("RX_CLK25_DIV[4:0]", rx_clk25_div, 5);

        auto rxbuf_addr_mode = str_or_default(ci->params, ctx->id("RXBUF_ADDR_MODE"), "PMA");
        if (rxbuf_addr_mode != "FULL" && rxbuf_addr_mode != "FAST")
            log_error("RXBUF_ADDR_MODE may only have values 'FULL' or 'FAST' but is: '%s'\n", rxbuf_addr_mode.c_str());
        write_bit("RXBUF_ADDR_MODE.FAST", rxbuf_addr_mode == "FAST");
        auto rxbuf_eidle_hi_cnt = int_or_default(ci->params, ctx->id("RXBUF_EIDLE_HI_CNT"), 0);
        write_int_vector("RXBUF_EIDLE_HI_CNT[3:0]", rxbuf_eidle_hi_cnt, 4);
        auto rxbuf_eidle_lo_cnt = int_or_default(ci->params, ctx->id("RXBUF_EIDLE_LO_CNT"), 0);
        write_int_vector("RXBUF_EIDLE_LO_CNT[3:0]", rxbuf_eidle_lo_cnt, 4);
        write_str_bool("RXBUF_EN", "RXBUF_EN", "TRUE");
        write_str_bool("RXBUF_RESET_ON_CB_CHANGE", "RXBUF_RESET_ON_CB_CHANGE", "TRUE");
        write_str_bool("RXBUF_RESET_ON_COMMAALIGN", "RXBUF_RESET_ON_COMMAALIGN");
        write_str_bool("RXBUF_RESET_ON_EIDLE", "RXBUF_RESET_ON_EIDLE");
        write_str_bool("RXBUF_RESET_ON_RATE_CHANGE", "RXBUF_RESET_ON_RATE_CHANGE", "TRUE");
        write_str_bool("RXBUF_THRESH_OVRD", "RXBUF_THRESH_OVRD");
        auto rxbuf_thresh_ovflw = int_or_default(ci->params, ctx->id("RXBUF_THRESH_OVFLW"), 0);
        write_int_vector("RXBUF_THRESH_OVFLW[5:0]", rxbuf_thresh_ovflw, 6);
        auto rxbuf_thresh_undflw = int_or_default(ci->params, ctx->id("RXBUF_THRESH_UNDFLW"), 0);
        write_int_vector("RXBUF_THRESH_UNDFLW[5:0]", rxbuf_thresh_undflw, 6);
        auto rxbufreset_time = int_or_default(ci->params, ctx->id("RXBUFRESET_TIME"), 0);
        write_int_vector("RXBUFRESET_TIME[4:0]", rxbufreset_time, 5);

        auto rxcdr_cfg = get_or_default(ci->params, ctx->id("RXCDR_CFG"),
            Property("00000000000000000000000000000000000000000000000000000000000000000000000000000000000"));
        write_vector("RXCDR_CFG[71:0]", bits_from_string_default(rxcdr_cfg, 72));
        auto rxcdr_fr_reset_on_eidle = bool_or_default(ci->params, ctx->id("RXCDR_FR_RESET_ON_EIDLE"), false);
        write_bit("RXCDR_FR_RESET_ON_EIDLE[0]", rxcdr_fr_reset_on_eidle);
        auto rxcdr_ph_reset_on_eidle = bool_or_default(ci->params, ctx->id("RXCDR_PH_RESET_ON_EIDLE"), false);
        write_bit("RXCDR_PH_RESET_ON_EIDLE[0]", rxcdr_ph_reset_on_eidle);
        auto rxcdr_hold_during_eidle = bool_or_default(ci->params, ctx->id("RXCDR_HOLD_DURING_EIDLE"), false);
        write_bit("RXCDR_HOLD_DURING_EIDLE[0]", rxcdr_hold_during_eidle);
        auto rx_clkmux_pd = bool_or_default(ci->params, ctx->id("RX_CLKMUX_PD"), true);
        write_bit("RX_CLKMUX_PD[0]", rx_clkmux_pd);
        auto rxcdr_lock_cfg = int_or_default(ci->params, ctx->id("RXCDR_LOCK_CFG"), 0);
        write_int_vector("RXCDR_LOCK_CFG[5:0]", rxcdr_lock_cfg, 6);
        auto rxcdrfreqreset_time = int_or_default(ci->params, ctx->id("RXCDRFREQRESET_TIME"), 0);
        write_int_vector("RXCDRFREQRESET_TIME[4:0]", rxcdrfreqreset_time, 5);
        auto rxcdrphreset_time = int_or_default(ci->params, ctx->id("RXCDRPHRESET_TIME"), 0);
        write_int_vector("RXCDRPHRESET_TIME[4:0]", rxcdrphreset_time, 5);

        auto rxdfelpmreset_time = int_or_default(ci->params, ctx->id("RXDFELPMRESET_TIME"), 0);
        write_int_vector("RXDFELPMRESET_TIME[6:0]", rxdfelpmreset_time, 7);

        auto rxdly_cfg = int_or_default(ci->params, ctx->id("RXDLY_CFG"), 0);
        write_int_vector("RXDLY_CFG[15:0]", rxdly_cfg, 16);
        auto rxdly_lcfg = int_or_default(ci->params, ctx->id("RXDLY_LCFG"), 0);
        write_int_vector("RXDLY_LCFG[8:0]", rxdly_lcfg, 9);
        auto rxdly_tap_cfg = int_or_default(ci->params, ctx->id("RXDLY_TAP_CFG"), 0);
        write_int_vector("RXDLY_TAP_CFG[15:0]", rxdly_tap_cfg, 16);

        write_str_bool("RXGEARBOX_EN", "RXGEARBOX_EN");

        auto rxiscanreset_time = int_or_default(ci->params, ctx->id("RXISCANRESET_TIME"), 0);
        write_int_vector("RXISCANRESET_TIME[4:0]", rxiscanreset_time, 5);

        auto rxlpm_hf_cfg = int_or_default(ci->params, ctx->id("RXLPM_HF_CFG"), 0);
        write_int_vector("RXLPM_HF_CFG[13:0]", rxlpm_hf_cfg, 14);
        auto rxlpm_lf_cfg = int_or_default(ci->params, ctx->id("RXLPM_LF_CFG"), 0);
        write_int_vector("RXLPM_LF_CFG[13:0]", rxlpm_lf_cfg, 14);

        auto rxoob_cfg = int_or_default(ci->params, ctx->id("RXOOB_CFG"), 0);
        write_int_vector("RXOOB_CFG[6:0]", rxoob_cfg, 7);

        auto rxout_div = std::log2(int_or_default(ci->params, ctx->id("RXOUT_DIV"), 1));
        write_int_vector("RXOUT_DIV[1:0]", rxout_div, 2);

        auto rxpcsreset_time = int_or_default(ci->params, ctx->id("RXPCSRESET_TIME"), 0);
        write_int_vector("RXPCSRESET_TIME[4:0]", rxpcsreset_time, 5);

        auto rxph_cfg = int_or_default(ci->params, ctx->id("RXPH_CFG"), 0);
        write_int_vector("RXPH_CFG[23:0]", rxph_cfg, 24);
        auto rxph_monitor_sel = int_or_default(ci->params, ctx->id("RXPH_MONITOR_SEL"), 0);
        write_int_vector("RXPH_MONITOR_SEL[4:0]", rxph_monitor_sel, 5);
        auto rxphdly_cfg = int_or_default(ci->params, ctx->id("RXPHDLY_CFG"), 0);
        write_int_vector("RXPHDLY_CFG[23:0]", rxphdly_cfg, 24);

        auto rxpmareset_time = int_or_default(ci->params, ctx->id("RXPMARESET_TIME"), 0);
        write_int_vector("RXPMARESET_TIME[4:0]", rxpmareset_time, 5);

        auto rxprbs_err_loopback = bool_or_default(ci->params, ctx->id("RXPRBS_ERR_LOOPBACK"), false);
        write_bit("RXPRBS_ERR_LOOPBACK[0]", rxprbs_err_loopback);

        auto rxslide_auto_wait = int_or_default(ci->params, ctx->id("RXSLIDE_AUTO_WAIT"), 7);
        write_int_vector("RXSLIDE_AUTO_WAIT[3:0]", rxslide_auto_wait, 4);
        auto rxslide_mode = str_or_default(ci->params, ctx->id("RXSLIDE_MODE"), "OFF");
        if (rxslide_mode != "OFF" && rxslide_mode != "AUTO" && rxslide_mode != "PCS" && rxslide_mode != "PMA")
            log_error("RXSLIDE_MODE may only have values 'OFF', 'AUTO', 'PCS' or 'PMA' but is: '%s'\n", rxslide_mode.c_str());
        write_bit("RXSLIDE_MODE.AUTO", rxslide_mode == "AUTO");
        write_bit("RXSLIDE_MODE.PCS",  rxslide_mode == "PCS");
        write_bit("RXSLIDE_MODE.PMA",  rxslide_mode == "PMA");

        auto sas_max_com = int_or_default(ci->params, ctx->id("SAS_MAX_COM"), 0);
        write_int_vector("SAS_MAX_COM[6:0]", sas_max_com, 7);
        auto sas_min_com = int_or_default(ci->params, ctx->id("SAS_MIN_COM"), 0);
        write_int_vector("SAS_MIN_COM[5:0]", sas_min_com, 6);

        auto sata_burst_seq_len = int_or_default(ci->params, ctx->id("SATA_BURST_SEQ_LEN"), 0);
        write_int_vector("SATA_BURST_SEQ_LEN[3:0]", sata_burst_seq_len, 4);
        auto sata_burst_val = int_or_default(ci->params, ctx->id("SATA_BURST_VAL"), 0);
        write_int_vector("SATA_BURST_VAL[2:0]", sata_burst_val, 3);
        auto sata_eidle_val = int_or_default(ci->params, ctx->id("SATA_EIDLE_VAL"), 0);
        write_int_vector("SATA_EIDLE_VAL[2:0]", sata_eidle_val, 3);
        auto sata_max_burst = int_or_default(ci->params, ctx->id("SATA_MAX_BURST"), 0);
        write_int_vector("SATA_MAX_BURST[5:0]", sata_max_burst, 6);
        auto sata_max_init = int_or_default(ci->params, ctx->id("SATA_MAX_INIT"), 0);
        write_int_vector("SATA_MAX_INIT[5:0]", sata_max_init, 6);
        auto sata_max_wake = int_or_default(ci->params, ctx->id("SATA_MAX_WAKE"), 0);
        write_int_vector("SATA_MAX_WAKE[5:0]", sata_max_wake, 6);
        auto sata_min_burst = int_or_default(ci->params, ctx->id("SATA_MIN_BURST"), 0);
        write_int_vector("SATA_MIN_BURST[5:0]", sata_min_burst, 6);
        auto sata_min_init = int_or_default(ci->params, ctx->id("SATA_MIN_INIT"), 0);
        write_int_vector("SATA_MIN_INIT[5:0]", sata_min_init, 6);
        auto sata_min_wake = int_or_default(ci->params, ctx->id("SATA_MIN_WAKE"), 0);
        write_int_vector("SATA_MIN_WAKE[5:0]", sata_min_wake, 6);
        auto sata_cpll_cfg = str_or_default(ci->params, ctx->id("SATA_CPLL_CFG"), "VCO_3000MHZ");
        if (sata_cpll_cfg != "VCO_3000MHZ" && sata_cpll_cfg != "VCO_1500MHZ" && sata_cpll_cfg != "VCO_750MHZ")
            log_error("SATA_CPLL_CFG may only have values 'VCO_3000MHZ', 'VCO_1500MHZ' or 'VCO_750MHZ' but is: '%s'\n", sata_cpll_cfg.c_str());
        write_bit("SATA_CPLL_CFG.VCO_1500MHZ", sata_cpll_cfg == "VCO_1500MHZ");
        write_bit("SATA_CPLL_CFG.VCO_750MHZ",  sata_cpll_cfg == "VCO_750MHZ");

        write_str_bool("SHOW_REALIGN_COMMA", "SHOW_REALIGN_COMMA");

        auto term_rcal_cfg = int_or_default(ci->params, ctx->id("TERM_RCAL_CFG"), 0);
        write_int_vector("TERM_RCAL_CFG[4:0]", term_rcal_cfg, 5);
        auto term_rcal_ovrd = int_or_default(ci->params, ctx->id("TERM_RCAL_OVRD"), 0);
        write_bit("TERM_RCAL_OVRD[0]", term_rcal_ovrd);

        auto trans_time_rate = int_or_default(ci->params, ctx->id("TRANS_TIME_RATE"), 0);
        write_int_vector("TRANS_TIME_RATE[7:0]", trans_time_rate, 8);

        auto tst_rsv = int_or_default(ci->params, ctx->id("TST_RSV"), 0);
        write_int_vector("TST_RSV[31:0]", tst_rsv, 32);

        auto tx_data_width = int_or_default(ci->params, ctx->id("TX_DATA_WIDTH"), 0);
        switch (tx_data_width) {
            case 16:
                tx_data_width = 2; break;
            case 20:
                tx_data_width = 3; break;
            case 32:
                tx_data_width = 4; break;
            case 40:
                tx_data_width = 5; break;
            case 64:
                tx_data_width = 6; break;
            case 80:
                tx_data_width = 7; break;
            default:
                log_error("Invalid TX_DATA_WIDTH parameter '%d' for GTXE2_CHANNEL instance %s\n", tx_data_width, ci->name.c_str(ctx));
        }
        write_int_vector("TX_DATA_WIDTH[2:0]", tx_data_width, 3);
        auto tx_int_datawidth = bool_or_default(ci->params, ctx->id("TX_INT_DATAWIDTH"), false);
        write_bit("TX_INT_DATAWIDTH[0]", tx_int_datawidth);

        auto tx_drive_mode = str_or_default(ci->params, ctx->id("TX_DRIVE_MODE"), "DIRECT");
        if (tx_drive_mode != "DIRECT" && tx_drive_mode != "PIPE" && tx_drive_mode != "PIPEGEN3")
            log_error("TX_DRIVE_MODE may only have values 'PIPE',  'PIPEGEN3' or 'DIRECT' but is: '%s'\n", tx_drive_mode.c_str());
        write_bit("TX_DRIVE_MODE.PIPE", tx_drive_mode == "PIPE");
        write_bit("TX_DRIVE_MODE.PIPEGEN3", tx_drive_mode == "PIPEGEN3");

        auto tx_eidle_assert_delay = int_or_default(ci->params, ctx->id("TX_EIDLE_ASSERT_DELAY"), 0);
        write_int_vector("TX_EIDLE_ASSERT_DELAY[2:0]", tx_eidle_assert_delay, 3);
        auto tx_eidle_deassert_delay = int_or_default(ci->params, ctx->id("TX_EIDLE_DEASSERT_DELAY"), 0);
        write_int_vector("TX_EIDLE_DEASSERT_DELAY[2:0]", tx_eidle_deassert_delay, 3);

        write_str_bool("LOOPBACK_DRIVE_HIZ", "LOOPBACK_DRIVE_HIZ");

        auto tx_maincursor_sel = bool_or_default(ci->params, ctx->id("TX_MAINCURSOR_SEL"), false);
        write_bit("TX_MAINCURSOR_SEL[0]", tx_maincursor_sel);
        auto tx_margin_full_0 = int_or_default(ci->params, ctx->id("TX_MARGIN_FULL_0"), 0b1001111);
        write_int_vector("TX_MARGIN_FULL_0[6:0]", tx_margin_full_0, 7);
        auto tx_margin_full_1 = int_or_default(ci->params, ctx->id("TX_MARGIN_FULL_1"), 0b1001111);
        write_int_vector("TX_MARGIN_FULL_1[6:0]", tx_margin_full_1, 7);
        auto tx_margin_full_2 = int_or_default(ci->params, ctx->id("TX_MARGIN_FULL_2"), 0b1001111);
        write_int_vector("TX_MARGIN_FULL_2[6:0]", tx_margin_full_2, 7);
        auto tx_margin_full_3 = int_or_default(ci->params, ctx->id("TX_MARGIN_FULL_3"), 0b1000001);
        write_int_vector("TX_MARGIN_FULL_3[6:0]", tx_margin_full_3, 7);
        auto tx_margin_full_4 = int_or_default(ci->params, ctx->id("TX_MARGIN_FULL_4"), 0b1000000);
        write_int_vector("TX_MARGIN_FULL_4[6:0]", tx_margin_full_4, 7);
        auto tx_margin_low_0 = int_or_default(ci->params, ctx->id("TX_MARGIN_LOW_0"), 0b1000111);
        write_int_vector("TX_MARGIN_LOW_0[6:0]", tx_margin_low_0, 7);
        auto tx_margin_low_1 = int_or_default(ci->params, ctx->id("TX_MARGIN_LOW_1"), 0b1000110);
        write_int_vector("TX_MARGIN_LOW_1[6:0]", tx_margin_low_1, 7);
        auto tx_margin_low_2 = int_or_default(ci->params, ctx->id("TX_MARGIN_LOW_2"), 0b1000100);
        write_int_vector("TX_MARGIN_LOW_2[6:0]", tx_margin_low_2, 7);
        auto tx_margin_low_3 = int_or_default(ci->params, ctx->id("TX_MARGIN_LOW_3"), 0b1000000);
        write_int_vector("TX_MARGIN_LOW_3[6:0]", tx_margin_low_3, 7);
        auto tx_margin_low_4 = int_or_default(ci->params, ctx->id("TX_MARGIN_LOW_4"), 0b1000000);
        write_int_vector("TX_MARGIN_LOW_4[6:0]", tx_margin_low_4, 7);
        auto tx_predriver_mode = bool_or_default(ci->params, ctx->id("TX_PREDRIVER_MODE"), false);
        write_bit("TX_PREDRIVER_MODE[0]", tx_predriver_mode);
        auto tx_rxdetect_cfg = int_or_default(ci->params, ctx->id("TX_RXDETECT_CFG"), 0);
        write_int_vector("TX_RXDETECT_CFG[13:0]", tx_rxdetect_cfg, 14);
        auto tx_rxdetect_ref = int_or_default(ci->params, ctx->id("TX_RXDETECT_REF"), 0);
        write_int_vector("TX_RXDETECT_REF[2:0]", tx_rxdetect_ref, 3);
        auto tx_xclk_sel = str_or_default(ci->params, ctx->id("TX_XCLK_SEL"), "TXUSR");
        if (tx_xclk_sel != "TXUSR" && tx_xclk_sel != "TXOUT")
            log_error("TX_XCLK_SEL may only have values 'TXOUT' or 'TXUSR' but is: '%s'\n", tx_xclk_sel.c_str());
        write_bit("TX_XCLK_SEL.TXUSR", tx_xclk_sel == "TXUSR");
        auto tx_clk25_div = int_or_default(ci->params, ctx->id("TX_CLK25_DIV"), 0) - 1;
        write_int_vector("TX_CLK25_DIV[4:0]", tx_clk25_div, 5);
        auto tx_clkmux_pd = bool_or_default(ci->params, ctx->id("TX_CLKMUX_PD"), true);
        write_bit("TX_CLKMUX_PD[0]", tx_clkmux_pd);
        auto tx_deemph0 = int_or_default(ci->params, ctx->id("TX_DEEMPH0"), 0);
        write_int_vector("TX_DEEMPH0[5:0]", tx_deemph0, 6);
        auto tx_deemph1 = int_or_default(ci->params, ctx->id("TX_DEEMPH1"), 0);
        write_int_vector("TX_DEEMPH1[5:0]", tx_deemph1, 6);
        write_str_bool("TXBUF_EN", "TXBUF_EN");
        write_str_bool("TXBUF_RESET_ON_RATE_CHANGE", "TXBUF_RESET_ON_RATE_CHANGE", "TRUE");
        auto txdly_cfg = int_or_default(ci->params, ctx->id("TXDLY_CFG"), 0);
        write_int_vector("TXDLY_CFG[15:0]", txdly_cfg, 16);
        auto txdly_lcfg = int_or_default(ci->params, ctx->id("TXDLY_LCFG"), 0);
        write_int_vector("TXDLY_LCFG[8:0]", txdly_lcfg, 9);
        auto txdly_tap_cfg = int_or_default(ci->params, ctx->id("TXDLY_TAP_CFG"), 0);
        write_int_vector("TXDLY_TAP_CFG[15:0]", txdly_tap_cfg, 16);
        write_str_bool("TXGEARBOX_EN", "TXGEARBOX_EN");
        auto txout_div = std::log2(int_or_default(ci->params, ctx->id("TXOUT_DIV"), 1));
        write_int_vector("TXOUT_DIV[1:0]", txout_div, 2);
        auto txpcsreset_time = int_or_default(ci->params, ctx->id("TXPCSRESET_TIME"), 0);
        write_int_vector("TXPCSRESET_TIME[4:0]", txpcsreset_time, 5);

        auto txph_cfg = int_or_default(ci->params, ctx->id("TXPH_CFG"), 0);
        write_int_vector("TXPH_CFG[15:0]", txph_cfg, 16);
        auto txph_monitor_sel = int_or_default(ci->params, ctx->id("TXPH_MONITOR_SEL"), 0);
        write_int_vector("TXPH_MONITOR_SEL[4:0]", txph_monitor_sel, 5);
        auto txphdly_cfg = int_or_default(ci->params, ctx->id("TXPHDLY_CFG"), 0);
        write_int_vector("TXPHDLY_CFG[23:0]", txphdly_cfg, 24);

        auto txpmareset_time = int_or_default(ci->params, ctx->id("TXPMARESET_TIME"), 0);
        write_int_vector("TXPMARESET_TIME[4:0]", txpmareset_time, 5);

        auto tx_qpi_status_en = bool_or_default(ci->params, ctx->id("TX_QPI_STATUS_EN"), false);
        write_bit("TX_QPI_STATUS_EN[0]", tx_qpi_status_en);

        auto ucodeer_clr = bool_or_default(ci->params, ctx->id("UCODEER_CLR"), false);
        write_bit("UCODEER_CLR[0]", ucodeer_clr);

        pop(); // GTXE2_CHANNEL
        pop(); // tile name
    }

    std::vector<bool> bits_from_string_default(const Property &p, size_t n)
    {
        std::vector<bool> r;
        if (p.is_string) {
            for (char c : p.as_string())
                r.push_back(c == '1');
        } else {
            r = p.as_bits();
        }
        r.resize(n, false);
        return r;
    }

    void write_ip()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_DSP48E1_DSP48E1) {
                write_dsp_cell(ci);
                blank();
            } else if (ci->type == id_BUFR_BUFR && ci->bel != BelId()) {
                write_bufr(ci);
                blank();
            } else if (ci->type == id_GTPE2_COMMON) {
                write_gtp_pll(ci);
                blank();
            } else if (ci->type == id_GTPE2_CHANNEL) {
                write_gtp_channel(ci);
                blank();
            } else if (ci->type == id_GTXE2_COMMON) {
                write_gtx_pll(ci);
                blank();
            } else if (ci->type == id_GTXE2_CHANNEL) {
                write_gtx_channel(ci);
                blank();
            } else if (ci->type == id_IBUFDS_GTE2 && ci->bel != BelId()) {
                write_ibufds_gte2(ci);
                blank();
            }
        }
    }

    void write_fasm()
    {
        // Run-identity header: these comment lines are dropped by fasm2frames,
        // so the resulting bitstream hash is unaffected; they record the exact
        // toolchain/chipdb/placer provenance of this FASM file for
        // reproducibility (port of nextpnr-xilinx 7037c948).
        out << "# nextpnr-himbaechel " << GIT_DESCRIBE_STR << "\n";
        out << "# chipdb " << ctx->chip_info->name.get() << " version " << ctx->chip_info->version << " generator "
            << ctx->chip_info->generator.get() << "\n";
        out << "# placer seed " << ctx->rngstate << "\n";
        get_invertible_pins(ctx, invertible_pins);
        populate_bufgctrl_bound_slots(); // must run before any pip emission
        write_logic();
        write_cfg();
        write_io();
        write_routing();
        write_bram();
        write_clocking();
        write_ip();
    }
};

} // namespace

void XilinxImpl::write_fasm(const std::string &filename)
{
    auto out = open_ofstream_and_log_error(filename, "FASM file");

    FasmBackend be(this->ctx, this, out);
    be.write_fasm();
}

NEXTPNR_NAMESPACE_END
