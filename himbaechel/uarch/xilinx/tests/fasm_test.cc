/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  nextpnr contributors
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

#include <sstream>
#include <string>
#include "command.h"
#include "gtest/gtest.h"
#include "nextpnr.h"
#include "uarch/xilinx/extra_data.h"
#include "uarch/xilinx/xilinx.h"
#define HIMBAECHEL_CONSTIDS "uarch/xilinx/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

// The FASM writer on a routed, unplaced fragment: nets bound through the
// chipdb by hand, no cells, the output read back as text.
class XilinxFasmTest : public ::testing::Test
{
  protected:
    virtual void SetUp()
    {
        init_share_dirname();
        chipArgs.device = "xc7a50tcsg324-1";
        ctx = new Context(chipArgs);
        ctx->uarch->init(ctx);
        ctx->late_init();
        xil = reinterpret_cast<XilinxImpl *>(ctx->uarch.get());
    }

    virtual void TearDown() { delete ctx; }

    unsigned pip_type(PipId pip) const { return chip_pip_info(ctx->chip_info, pip).flags; }

    const XlnxPipExtraDataPOD &pip_extra(PipId pip) const
    {
        return *reinterpret_cast<const XlnxPipExtraDataPOD *>(chip_pip_info(ctx->chip_info, pip).extra_data.get());
    }

    // The first tile of the given type, by the chipdb's own tile order.
    int find_tile(const std::string &type_prefix) const
    {
        const int ntiles = ctx->chip_info->width * ctx->chip_info->height;
        for (int tile = 0; tile < ntiles; tile++) {
            const std::string type = IdString(chip_tile_info(ctx->chip_info, tile).type_name).str(ctx);
            if (type.rfind(type_prefix, 0) == 0)
                return tile;
        }
        return -1;
    }

    std::string write_fasm() const
    {
        std::stringstream out;
        xil->write_fasm(out);
        return out.str();
    }

    ArchArgs chipArgs;
    Context *ctx;
    XilinxImpl *xil;
};

TEST_F(XilinxFasmTest, route_through_in_a_tile_with_no_cells_writes_its_outmux)
{
    // A LUT route-through that leaves its slice through the [A-D]MUX output
    // (BMUX <- B6LUT_O6 through BOUTMUX) in a tile holding no cell at all.
    // write_luts_config wrote the route-through's INIT for every tile but the
    // OUTMUX feature only for tiles with cells, so this net was open on the
    // board: the LUT carried the signal and nothing selected it at the exit.
    // The blinky never places a route-through in an empty slice, so the FASM
    // is checked on a fragment bound by hand.
    const int tile = find_tile("CLBLL_L");
    ASSERT_GE(tile, 0);
    const int x = tile % ctx->chip_info->width, y = tile / ctx->chip_info->width;

    // Position B of half 0 (SLICEL_X0): its 6LUT and the O6 site wire.
    const int half = 0, pos = 1;
    BelId lut6 = ctx->getBelByLocation(Loc(x, y, (half << 6) | (pos << 4) | BEL_6LUT));
    ASSERT_NE(lut6, BelId());
    WireId o6 = ctx->getBelPinWire(lut6, id_O6);
    ASSERT_NE(o6, WireId());

    // The route-through pip on B's inputs.  On main's chipdb it ends at the
    // site's output tile wire (CLBLL_LL_B); with #175's generator a route-through
    // ends at the B6LUT_O6 site wire instead and leaves through BOUTMUX, which is
    // the route this fix was found on.  Take the O6 form when the chipdb has it,
    // else the direct one: the writer's job is the same, write what is bound.
    WireId b1_tile_wire;
    for (PipId entry : ctx->getPipsUphill(ctx->getBelPinWire(lut6, id_A1))) {
        if (pip_type(entry) == PIP_LUT_PERMUTATION || pip_type(entry) == PIP_SITE_ENTRY) {
            b1_tile_wire = ctx->getPipSrcWire(entry);
            break;
        }
    }
    ASSERT_NE(b1_tile_wire, WireId());
    PipId route_through;
    for (PipId pip : ctx->getPipsDownhill(b1_tile_wire)) {
        if (pip_type(pip) != PIP_LUT_ROUTETHRU)
            continue;
        const bool ends_at_o6 = ctx->getPipDstWire(pip) == o6;
        if (ends_at_o6 || route_through == PipId())
            route_through = pip;
    }
    ASSERT_NE(route_through, PipId());

    // Out of the site: BOUTMUX's O6 leg onto BMUX, then the site exit pip.
    PipId outmux;
    for (PipId pip : ctx->getPipsDownhill(o6)) {
        if (IdString(pip_extra(pip).bel_name).str(ctx) == "BOUTMUX") {
            outmux = pip;
            break;
        }
    }
    ASSERT_NE(outmux, PipId());
    ASSERT_EQ(IdString(pip_extra(outmux).pip_config).str(ctx), "O6");
    PipId site_exit;
    for (PipId pip : ctx->getPipsDownhill(ctx->getPipDstWire(outmux))) {
        if (pip_type(pip) == PIP_SITE_EXIT) {
            site_exit = pip;
            break;
        }
    }
    ASSERT_NE(site_exit, PipId());

    NetInfo *net = ctx->createNet(ctx->id("through_an_empty_slice"));
    ctx->bindWire(b1_tile_wire, net, STRENGTH_STRONG);
    ctx->bindPip(route_through, net, STRENGTH_STRONG);
    const bool direct_form = ctx->getPipDstWire(route_through) != o6;
    if (direct_form)
        ctx->bindWire(o6, net, STRENGTH_STRONG); // what the LUT carries to the OUTMUX, on this chipdb not a pip
    ctx->bindPip(outmux, net, STRENGTH_STRONG);
    ctx->bindPip(site_exit, net, STRENGTH_STRONG);

    // The condition under test: no cell was ever placed in this tile.
    ASSERT_EQ(xil->tile_status.at(tile).lts, nullptr);

    const std::string fasm = write_fasm();
    const std::string prefix = xil->tile_name(tile) + ".SLICEL_X0.";
    EXPECT_NE(fasm.find(prefix + "BLUT.INIT[63:0]"), std::string::npos) << fasm;
    EXPECT_NE(fasm.find(prefix + "BOUTMUX.O6"), std::string::npos) << fasm;
}

TEST_F(XilinxFasmTest, a_tile_with_nothing_bound_writes_no_outmux)
{
    // The other side of the fix: an idle tile must not gain OUTMUX features
    // just because the writer now looks at tiles without cells.
    const int tile = find_tile("CLBLL_L");
    ASSERT_GE(tile, 0);
    const std::string fasm = write_fasm();
    EXPECT_EQ(fasm.find(xil->tile_name(tile) + ".SLICEL_X0."), std::string::npos) << fasm;
}
