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

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "command.h"
#include "gtest/gtest.h"
#include "nextpnr.h"
#include "uarch/xilinx/pack.h"
#include "uarch/xilinx/xilinx.h"
#define HIMBAECHEL_CONSTIDS "uarch/xilinx/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

class XilinxPackTest : public ::testing::Test
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

    ArchArgs chipArgs;
    Context *ctx;
    XilinxImpl *xil;
};

TEST_F(XilinxPackTest, pack_cfg_startupe2)
{
    // STARTUPE2 must pack to STARTUP_STARTUP and preplace to its single
    // dedicated site (WP6)
    CellInfo *ci = ctx->createCell(ctx->id("startup"), id_STARTUPE2);
    for (auto p : {"CLK", "GSR", "GTS", "KEYCLEARB", "PACK", "USRCCLKO", "USRCCLKTS", "USRDONEO", "USRDONETS"})
        ci->addInput(ctx->id(p));
    for (auto p : {"CFGCLK", "CFGMCLK", "EOS", "PREQ"})
        ci->addOutput(ctx->id(p));

    XC7Packer p(ctx, xil);
    p.pack_cfg();

    EXPECT_EQ(ci->type, id_STARTUP_STARTUP);
    EXPECT_NE(ci->bel, BelId());
    EXPECT_EQ(ctx->getBelType(ci->bel), id_STARTUP_STARTUP);
}

TEST_F(XilinxPackTest, pack_cfg_bscane2_chain)
{
    // BSCANE2 packs to BSCAN, validates JTAG_CHAIN, and is bound to the site
    // its chain selects: USER<n> is served by BSCAN_X0Y<n-1>, so an instance
    // left on another BSCAN site answers another chain's user register (WP6).
    CellInfo *ci = ctx->createCell(ctx->id("bscan"), id_BSCANE2);
    ci->params[id_JTAG_CHAIN] = Property(2);
    XC7Packer p(ctx, xil);
    p.pack_cfg();
    EXPECT_EQ(ci->type, id_BSCAN);
    ASSERT_NE(ci->bel, BelId());
    EXPECT_EQ(xil->get_site_name(xil->get_bel_site(ci->bel)), ctx->id("BSCAN_X0Y1"));
}

TEST_F(XilinxPackTest, pack_cfg_bscane2_two_chains_do_not_share_a_site)
{
    // The design that showed the bug: a JTAG clock controller on one chain
    // and a logic analyser on another.  Each instance must get the site
    // serving its own chain, not whichever BSCAN site is free first.
    CellInfo *user3 = ctx->createCell(ctx->id("user3"), id_BSCANE2);
    user3->params[id_JTAG_CHAIN] = Property(3);
    CellInfo *user1 = ctx->createCell(ctx->id("user1"), id_BSCANE2);
    user1->params[id_JTAG_CHAIN] = Property(1);

    XC7Packer p(ctx, xil);
    p.pack_cfg();

    ASSERT_NE(user3->bel, BelId());
    ASSERT_NE(user1->bel, BelId());
    EXPECT_EQ(xil->get_site_name(xil->get_bel_site(user3->bel)), ctx->id("BSCAN_X0Y2"));
    EXPECT_EQ(xil->get_site_name(xil->get_bel_site(user1->bel)), ctx->id("BSCAN_X0Y0"));
}

TEST_F(XilinxPackTest, pack_cfg_bscane2_duplicate_chain_is_an_error)
{
    // Only one instance can serve a chain -- the chain's site has one BSCAN
    // bel -- so a second instance asking for the same chain is reported
    // rather than quietly bound to another chain's site.
    CellInfo *first = ctx->createCell(ctx->id("first"), id_BSCANE2);
    first->params[id_JTAG_CHAIN] = Property(2);
    CellInfo *second = ctx->createCell(ctx->id("second"), id_BSCANE2);
    second->params[id_JTAG_CHAIN] = Property(2);

    XC7Packer p(ctx, xil);
    EXPECT_THROW(p.pack_cfg(), log_execution_error_exception);

    // Whichever instance got there first holds the site serving USER2.
    CellInfo *placed = first->bel != BelId() ? first : second;
    ASSERT_NE(placed->bel, BelId());
    EXPECT_EQ(xil->get_site_name(xil->get_bel_site(placed->bel)), ctx->id("BSCAN_X0Y1"));
}

TEST_F(XilinxPackTest, clocking_bufh_bufhce)
{
    // BUFH and BUFHCE pack to BUFHCE_BUFHCE with CE tied active (WP3.2)
    XC7Packer p(ctx, xil);
    p.pack_constants();

    CellInfo *bufh = ctx->createCell(ctx->id("bufh"), id_BUFH);
    bufh->addInput(id_I);
    bufh->addOutput(id_O);
    CellInfo *bufhce = ctx->createCell(ctx->id("bufhce"), id_BUFHCE);
    bufhce->addInput(id_I);
    bufhce->addInput(id_CE);
    bufhce->addOutput(id_O);

    p.prepare_clocking();

    EXPECT_EQ(bufh->type, id_BUFHCE_BUFHCE);
    EXPECT_EQ(bufhce->type, id_BUFHCE_BUFHCE);
    for (auto ci : {bufh, bufhce}) {
        NetInfo *ce = ci->getPort(id_CE);
        EXPECT_NE(ce, nullptr);
        EXPECT_EQ(ce->name, ctx->id("$PACKER_VCC_NET"));
    }
}

TEST_F(XilinxPackTest, clocking_bufr)
{
    // BUFR packs to BUFR_BUFR (WP2.6)
    CellInfo *ci = ctx->createCell(ctx->id("bufr"), id_BUFR);
    ci->addInput(id_I);
    ci->addInput(id_CE);
    ci->addInput(id_CLR);
    ci->addOutput(id_O);
    XC7Packer p(ctx, xil);
    p.prepare_clocking();
    EXPECT_EQ(ci->type, id_BUFR_BUFR);
}

TEST_F(XilinxPackTest, cells_ibufgds_alias)
{
    // IBUFGDS is accepted as an alias of IBUFDS (WP3.8)
    XilinxPacker p(ctx, xil);
    CellInfo *ci = p.create_cell(id_IBUFGDS, ctx->id("ibufgds"));
    EXPECT_NE(ci, nullptr);
    EXPECT_NE(ci->ports.count(id_I), size_t(0));
    EXPECT_NE(ci->ports.count(id_IB), size_t(0));
    EXPECT_NE(ci->ports.count(id_O), size_t(0));
}

// An IBUFGDS on two top-level ports, clk_p and clk_n, as yosys leaves them.
static std::pair<CellInfo *, CellInfo *> create_diff_input(Context *ctx)
{
    auto top_port = [&](const char *name) {
        CellInfo *io = ctx->createCell(ctx->id(name), ctx->id("$nextpnr_ibuf"));
        io->addOutput(id_O);
        io->connectPort(id_O, ctx->createNet(ctx->id(name)));
        return io;
    };
    CellInfo *p_port = top_port("clk_p");
    CellInfo *n_port = top_port("clk_n");
    CellInfo *buf = ctx->createCell(ctx->id("ib"), id_IBUFGDS);
    buf->addInput(id_I);
    buf->addInput(id_IB);
    buf->addOutput(id_O);
    buf->connectPort(id_I, ctx->nets.at(ctx->id("clk_p")).get());
    buf->connectPort(id_IB, ctx->nets.at(ctx->id("clk_n")).get());
    return std::make_pair(p_port, n_port);
}

// The two ports of a differential pair share one standard, and the XDC may give
// it to either of them, as Vivado takes it. Here only the N port has it: with
// the P port alone on a package pin, the N port goes on the other pin of the
// pair, and the P port takes the N port's standard (every pad needs one).
TEST_F(XilinxPackTest, diff_pair_takes_the_standard_of_the_port_without_a_pin)
{
    auto ports = create_diff_input(ctx);
    ports.first->attrs[id_PACKAGE_PIN] = std::string("E3");
    ports.second->attrs[id_IOSTANDARD] = std::string("DIFF_SSTL15");
    XC7Packer p(ctx, xil);
    p.pack_constants();
    p.pack_io();
    const CellInfo *pad_p = ctx->cells.at(ctx->id("clk_p")).get();
    const CellInfo *pad_n = ctx->cells.at(ctx->id("clk_n")).get();
    EXPECT_EQ(str_or_default(pad_n->attrs, id_PACKAGE_PIN, ""), "D3");
    EXPECT_EQ(str_or_default(pad_p->attrs, id_IOSTANDARD, ""), "DIFF_SSTL15");
    EXPECT_EQ(str_or_default(pad_n->attrs, id_IOSTANDARD, ""), "DIFF_SSTL15");
}

// The same with both ports on their pins.
TEST_F(XilinxPackTest, diff_pair_on_two_pins_takes_the_standard_of_either_port)
{
    auto ports = create_diff_input(ctx);
    ports.first->attrs[id_PACKAGE_PIN] = std::string("E3");
    ports.second->attrs[id_PACKAGE_PIN] = std::string("D3");
    ports.second->attrs[id_IOSTANDARD] = std::string("DIFF_SSTL15");
    XC7Packer p(ctx, xil);
    p.pack_constants();
    p.pack_io();
    EXPECT_EQ(str_or_default(ctx->cells.at(ctx->id("clk_p"))->attrs, id_IOSTANDARD, ""), "DIFF_SSTL15");
}

// Two different standards on the two ports are an error: decomposing the buffer
// mixed them, and Vivado keeps the one set last, which the netlist cannot say.
TEST_F(XilinxPackTest, diff_pair_with_two_standards_is_an_error)
{
    auto ports = create_diff_input(ctx);
    ports.first->attrs[id_PACKAGE_PIN] = std::string("E3");
    ports.first->attrs[id_IOSTANDARD] = std::string("DIFF_SSTL15");
    ports.second->attrs[id_IOSTANDARD] = std::string("DIFF_HSTL_I");
    XC7Packer p(ctx, xil);
    p.pack_constants();
    EXPECT_THROW(p.pack_io(), log_execution_error_exception);
}

// Helper: create an SRLC32E with the ports a real (yosys) design carries.
static CellInfo *create_srlc32e(Context *ctx, const char *name)
{
    CellInfo *ci = ctx->createCell(ctx->id(name), id_SRLC32E);
    for (auto p : {id_A0, id_A1, id_A2, id_A3, id_A4})
        ci->addInput(p);
    ci->addInput(id_D);
    ci->addInput(id_CLK);
    ci->addInput(id_CE);
    ci->addOutput(id_Q);
    ci->addOutput(id_Q31);
    return ci;
}

TEST_F(XilinxPackTest, pack_srl_cascade_pair)
{
    // A two-deep SRLC32E chain (Q31 -> next D) must pack to SLICE_LUTX and be
    // constrained into one slice, head at D6LUT and member at C6LUT, exactly
    // like a carry chain (the MC31 cascade arc exists only inside a SLICEM).
    XC7Packer p(ctx, xil);
    p.pack_constants();

    CellInfo *srl1 = create_srlc32e(ctx, "srl1");
    CellInfo *srl2 = create_srlc32e(ctx, "srl2");
    NetInfo *q31 = ctx->createNet(ctx->id("q31net"));
    srl1->connectPort(id_Q31, q31);
    srl2->connectPort(id_D, q31);

    p.pack_srls();
    p.constrain_srl_cascades();

    EXPECT_EQ(srl1->type, id_SLICE_LUTX);
    EXPECT_EQ(srl2->type, id_SLICE_LUTX);
    EXPECT_EQ(str_or_default(srl1->attrs, id_X_ORIG_TYPE), "SRLC32E");

    EXPECT_TRUE(srl1->constr_abs_z);
    EXPECT_EQ(srl1->constr_z, (3 << 4) | BEL_6LUT);
    EXPECT_EQ(srl1->cluster, srl1->name);
    EXPECT_EQ(srl1->constr_children.size(), size_t(1));
    EXPECT_EQ(srl1->constr_children.at(0), srl2);

    EXPECT_EQ(srl2->cluster, srl1->name);
    EXPECT_TRUE(srl2->constr_abs_z);
    EXPECT_EQ(srl2->constr_z, (2 << 4) | BEL_6LUT);
}

TEST_F(XilinxPackTest, pack_srl_cascade_offslice)
{
    // A five-deep chain cannot fit in one slice: the link between the 4th and
    // 5th element leaves the slice. Q has no users, so the link moves onto Q
    // with the read address tied to 31, instead of leaving the A LUT through DOUTMUX.
    XC7Packer p(ctx, xil);
    p.pack_constants();

    CellInfo *srls[5];
    NetInfo *links[4];
    for (int i = 0; i < 5; i++) {
        srls[i] = create_srlc32e(ctx, stringf("srl%d", i + 1).c_str());
        if (i > 0)
            srls[i]->connectPort(id_D, links[i - 1]);
        if (i < 4) {
            links[i] = ctx->createNet(ctx->idf("link%d", i));
            srls[i]->connectPort(id_Q31, links[i]);
        }
    }

    p.pack_srls();
    p.constrain_srl_cascades();

    // Elements 1..4 cluster into one slice (D,C,B,A); element 5 does not.
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(srls[i]->cluster, srls[0]->name);
        EXPECT_EQ(srls[i]->constr_z, ((3 - i) << 4) | BEL_6LUT);
    }
    EXPECT_EQ(srls[4]->cluster, ClusterId());

    // The 4th element's Q31 is rewired onto Q, with the address tied to 31.
    EXPECT_EQ(srls[3]->getPort(id_MC31), nullptr);
    EXPECT_EQ(srls[3]->getPort(id_O6), links[3]);
    EXPECT_EQ(srls[4]->getPort(id_DI1), links[3]);
    for (auto a : {id_A2, id_A3, id_A4, id_A5, id_A6})
        EXPECT_EQ(srls[3]->getPort(a), ctx->nets.at(ctx->id("$PACKER_VCC_NET")).get());
}

class XilinxPackTestK325t : public ::testing::Test
{
  protected:
    virtual void SetUp()
    {
        init_share_dirname();
        chipArgs.device = "xc7k325tffg676-1";
        ctx = new Context(chipArgs);
        ctx->uarch->init(ctx);
        ctx->late_init();
        xil = reinterpret_cast<XilinxImpl *>(ctx->uarch.get());
    }

    virtual void TearDown() { delete ctx; }

    ArchArgs chipArgs;
    Context *ctx;
    XilinxImpl *xil;
};

TEST_F(XilinxPackTestK325t, pack_io_pcie_retype_preplace)
{
    // PCIE_2_1 must retype to PCIE_2_1_PCIE_2_1 in pack_io and then bind to
    // the unique PCIE hard-block site in pack_gbs (mirrors PS7 handling).
    CellInfo *ci = ctx->createCell(ctx->id("pcie"), id_PCIE_2_1);
    XC7Packer p(ctx, xil);
    p.pack_io();
    EXPECT_EQ(ci->type, id_PCIE_2_1_PCIE_2_1);
    p.pack_gbs();
    EXPECT_NE(ci->bel, BelId());
    EXPECT_EQ(ctx->getBelType(ci->bel), id_PCIE_2_1_PCIE_2_1);
}

// Helper: a RAMB36E1 as yosys emits it for a single-port-width memory.
static CellInfo *create_ramb36e1(Context *ctx, const char *name, int write_width_a, int read_width_b)
{
    CellInfo *ci = ctx->createCell(ctx->id(name), id_RAMB36E1);
    for (int i = 0; i < 16; i++) {
        ci->addInput(ctx->idf("ADDRARDADDR[%d]", i));
        ci->addInput(ctx->idf("ADDRBWRADDR[%d]", i));
    }
    for (int i = 0; i < 32; i++) {
        ci->addInput(ctx->idf("DIADI[%d]", i));
        ci->addInput(ctx->idf("DIBDI[%d]", i));
        ci->addOutput(ctx->idf("DOADO[%d]", i));
        ci->addOutput(ctx->idf("DOBDO[%d]", i));
    }
    for (int i = 0; i < 4; i++) {
        ci->addInput(ctx->idf("DIPADIP[%d]", i));
        ci->addInput(ctx->idf("DIPBDIP[%d]", i));
        ci->addOutput(ctx->idf("DOPADOP[%d]", i));
        ci->addOutput(ctx->idf("DOPBDOP[%d]", i));
        ci->addInput(ctx->idf("WEA[%d]", i));
    }
    for (int i = 0; i < 8; i++)
        ci->addInput(ctx->idf("WEBWE[%d]", i));
    for (auto p : {"CLKARDCLK", "CLKBWRCLK", "ENARDEN", "ENBWREN", "REGCEAREGCE", "REGCEB", "RSTRAMARSTRAM",
                   "RSTRAMB", "RSTREGARSTREG", "RSTREGB"})
        ci->addInput(ctx->id(p));
    ci->params[ctx->id("RAM_MODE")] = Property("TDP");
    ci->params[id_WRITE_WIDTH_A] = Property(write_width_a);
    ci->params[ctx->id("READ_WIDTH_B")] = Property(read_width_b);
    return ci;
}

TEST_F(XilinxPackTest, pack_bram_ramb36_width9_parity_on_both_halves)
{
    // At width 9 a RAMB36E1 keeps the ninth bit of a word in the lower
    // RAMB18 at even addresses and in the upper one at odd addresses, and the
    // upper half reads it from DIPADIP1. Left unconnected (yosys drives only
    // DIPADIP[0]), every odd address reads its parity bit back as 0.
    CellInfo *ci = create_ramb36e1(ctx, "ram_x9", 9, 9);
    NetInfo *parity = ctx->createNet(ctx->id("parity"));
    ci->connectPort(ctx->id("DIPADIP[0]"), parity);

    XC7Packer p(ctx, xil);
    p.pack_bram();

    EXPECT_EQ(ci->type, id_RAMB36E1_RAMB36E1);
    EXPECT_EQ(ci->getPort(id_DIPADIP0), parity);
    EXPECT_EQ(ci->getPort(id_DIPADIP1), parity);
}

TEST_F(XilinxPackTest, pack_bram_ramb36_width18_parity_untouched)
{
    // At width 18 the two parity pins carry two different bits of the word.
    CellInfo *ci = create_ramb36e1(ctx, "ram_x18", 18, 18);
    NetInfo *p0 = ctx->createNet(ctx->id("p0"));
    NetInfo *p1 = ctx->createNet(ctx->id("p1"));
    ci->connectPort(ctx->id("DIPADIP[0]"), p0);
    ci->connectPort(ctx->id("DIPADIP[1]"), p1);

    XC7Packer p(ctx, xil);
    p.pack_bram();

    EXPECT_EQ(ci->getPort(id_DIPADIP0), p0);
    EXPECT_EQ(ci->getPort(id_DIPADIP1), p1);
}

// A BEL attribute may name a bel the way NEXTPNR_BEL has it in a routed JSON,
// "<site>/<bel>", where an IOB's bel has dots of its own (IOB33M.PAD). Each
// such name resolves to the bel it names: here every site bel of the first
// tile of each tile type. A bel of an alternative site type has the same name
// in the site as its primary one (CE1USED and CE1USED~ILOGICE2), so that form
// cannot tell the two apart: those names are left out.
TEST_F(XilinxPackTest, user_bel_in_site_form_resolves_to_its_bel)
{
    pool<IdString> tile_types_seen;
    std::vector<std::pair<std::string, BelId>> names;
    dict<std::string, int> uses;
    int last_tile = -1;
    bool tile_is_taken = false;
    for (auto bel : ctx->getBels()) {
        const bool new_tile = bel.tile != last_tile;
        if (new_tile) {
            last_tile = bel.tile;
            const IdString type = ctx->get_tile_type(bel.tile);
            tile_is_taken = tile_types_seen.count(type) == 0;
            tile_types_seen.insert(type);
        }
        if (!tile_is_taken)
            continue;
        const SiteIndex site = xil->get_bel_site(bel);
        const bool bel_has_a_site = site.site >= 0 && site.site < xil->tile_extra_data(site.tile)->sites.ssize();
        if (!bel_has_a_site)
            continue;
        const std::string site_form = xil->get_site_bel_name(bel).str(ctx);
        names.emplace_back(site_form, bel);
        uses[site_form]++;
    }
    std::vector<std::pair<CellInfo *, BelId>> expected;
    int dotted = 0;
    for (const auto &name : names) {
        const bool name_is_shared = uses.at(name.first) > 1;
        if (name_is_shared)
            continue;
        const bool bel_part_has_a_dot = name.first.find('.') != std::string::npos;
        if (bel_part_has_a_dot)
            dotted++;
        CellInfo *ci = ctx->createCell(ctx->idf("c%d", int(expected.size())), id_SLICE_FFX);
        ci->attrs[id_BEL] = name.first;
        expected.emplace_back(ci, name.second);
    }
    xil->resolve_user_bels();
    for (const auto &entry : expected)
        EXPECT_EQ(entry.first->attrs.at(id_BEL).as_string(), ctx->getBelName(entry.second).str(ctx))
                << xil->get_site_bel_name(entry.second).str(ctx);
    EXPECT_GT(int(expected.size()), 100);
    EXPECT_GT(dotted, 0);
}

// A coordinate with a byte over 0x7f is not a site name: the BEL is left as
// written (and the placer reports "No Bel named").
TEST_F(XilinxPackTest, user_bel_with_a_non_ascii_coordinate_is_left_as_written)
{
    const std::string name = "SLICE_X\xe9"
                             "0Y0/AFF";
    CellInfo *ci = ctx->createCell(ctx->id("ff"), id_SLICE_FFX);
    ci->attrs[id_BEL] = name;
    xil->resolve_user_bels();
    EXPECT_EQ(ci->attrs.at(id_BEL).as_string(), name);
}

// -o preplaced reads what scripts/routing_dump.py writes, NEXTPNR_BEL as the
// routed JSON has it: a site's bel is "<site>/<bel>". It is resolved like a
// BEL attribute and recorded for the check after placement.
TEST_F(XilinxPackTest, preplaced_bel_in_site_form_is_resolved)
{
    BelId ff_bel;
    BelId pad_bel;
    for (auto bel : ctx->getBels()) {
        const std::string in_site = xil->bel_name_in_site(bel).str(ctx);
        const bool first_aff = in_site == "AFF" && ff_bel == BelId();
        if (first_aff)
            ff_bel = bel;
        const bool first_pad = in_site == "IOB33M.PAD" && pad_bel == BelId();
        if (first_pad)
            pad_bel = bel;
    }
    ASSERT_NE(ff_bel, BelId());
    ASSERT_NE(pad_bel, BelId());
    const std::string path = ::testing::TempDir() + "xilinx_preplaced_test.txt";
    {
        std::ofstream out(path);
        out << "ff\t" << xil->get_site_bel_name(ff_bel).str(ctx) << "\n";
        out << "pad\t" << xil->get_site_bel_name(pad_bel).str(ctx) << "\n";
    }
    CellInfo *ff = ctx->createCell(ctx->id("ff"), id_SLICE_FFX);
    CellInfo *pad = ctx->createCell(ctx->id("pad"), id_PAD);
    ctx->args.options.insert(std::make_pair("preplaced", po::variable_value(path, false)));
    xil->apply_preplaced(false);
    std::remove(path.c_str());
    EXPECT_EQ(ff->attrs.at(id_BEL).as_string(), ctx->getBelName(ff_bel).str(ctx));
    EXPECT_EQ(pad->attrs.at(id_BEL).as_string(), ctx->getBelName(pad_bel).str(ctx));
    EXPECT_EQ(xil->user_bels.count(ff->name), 1u);
    EXPECT_EQ(xil->user_bels.count(pad->name), 1u);
}

// A LUT6_2 with a BEL and neither output used is split into no cell at all,
// which would take the BEL away unchecked: that is an error, as a BEL with
// both outputs used is.
TEST_F(XilinxPackTest, lut6_2_with_a_bel_and_no_output_is_an_error)
{
    CellInfo *ci = ctx->createCell(ctx->id("l"), id_LUT6_2);
    for (auto p : {"I0", "I1", "I2", "I3", "I4", "I5"})
        ci->addInput(ctx->id(p));
    ci->addOutput(id_O5);
    ci->addOutput(id_O6);
    ci->attrs[id_BEL] = std::string("X0Y0/SLICE_X0Y0.C6LUT");
    XC7Packer p(ctx, xil);
    EXPECT_THROW(p.split_lut6_2(), log_execution_error_exception);
}

// ---------------------------------------------------------------------------
// Carry DI inputs: the bypass pin instead of a buffer LUT (pack_carry.cc)

// A flip-flop whose Q drives the given net: a fabric source as the packer
// sees one (anything that is not a LUT).
static NetInfo *fabric_net(Context *ctx, const char *name)
{
    CellInfo *ff = ctx->createCell(ctx->idf("%s_src", name), id_FDRE);
    for (auto p : {id_C, id_CE, id_D, id_R})
        ff->addInput(p);
    ff->addOutput(id_Q);
    NetInfo *net = ctx->createNet(ctx->id(name));
    ff->connectPort(id_Q, net);
    return net;
}

// A CARRY4 with the ports a mapped netlist carries: every S from its own
// two-input LUT, DI[i] from di[i] (nullptr: unconnected), CYINIT as given.
static CellInfo *create_carry4(Context *ctx, const char *name, NetInfo *di[4], NetInfo *cyinit)
{
    CellInfo *c4 = ctx->createCell(ctx->id(name), id_CARRY4);
    c4->addInput(id_CYINIT);
    c4->addInput(id_CI);
    for (int i = 0; i < 4; i++) {
        c4->addInput(ctx->idf("S[%d]", i));
        c4->addInput(ctx->idf("DI[%d]", i));
        c4->addOutput(ctx->idf("O[%d]", i));
        c4->addOutput(ctx->idf("CO[%d]", i));
        CellInfo *lut = ctx->createCell(ctx->idf("%s_s%d", name, i), id_LUT2);
        lut->addInput(id_I0);
        lut->addInput(id_I1);
        lut->addOutput(id_O);
        lut->params[id_INIT] = Property(6, 4);
        lut->connectPort(id_I0, fabric_net(ctx, stringf("%s_a%d", name, i).c_str()));
        lut->connectPort(id_I1, fabric_net(ctx, stringf("%s_b%d", name, i).c_str()));
        NetInfo *s = ctx->createNet(ctx->idf("%s_sum%d", name, i));
        lut->connectPort(id_O, s);
        c4->connectPort(ctx->idf("S[%d]", i), s);
        if (di[i] != nullptr)
            c4->connectPort(ctx->idf("DI[%d]", i), di[i]);
        NetInfo *o = ctx->createNet(ctx->idf("%s_o%d", name, i));
        c4->connectPort(ctx->idf("O[%d]", i), o);
        CellInfo *sink = ctx->createCell(ctx->idf("%s_q%d", name, i), id_FDRE);
        for (auto p : {id_C, id_CE, id_D, id_R})
            sink->addInput(p);
        sink->addOutput(id_Q);
        sink->connectPort(id_D, o);
    }
    if (cyinit != nullptr)
        c4->connectPort(id_CYINIT, cyinit);
    return c4;
}

// The CARRY4 pack_carries made from the chain
static CellInfo *packed_carry4(Context *ctx)
{
    CellInfo *found = nullptr;
    for (auto &cell : ctx->cells)
        if (cell.second->type == id_CARRY4) {
            if (found == nullptr)
                found = cell.second.get();
        }
    return found;
}

static int di_lut_count(Context *ctx)
{
    int n = 0;
    for (auto &cell : ctx->cells)
        if (cell.second->name.str(ctx).find("$DI[") != std::string::npos)
            n++;
    return n;
}

TEST_F(XilinxPackTest, carry_di_from_fabric_takes_the_bypass_pin)
{
    XC7Packer p(ctx, xil);
    p.pack_constants();
    NetInfo *gnd = ctx->nets.at(ctx->id("$PACKER_GND_NET")).get();
    NetInfo *di[4] = {gnd, fabric_net(ctx, "x1"), fabric_net(ctx, "x2"), gnd};
    create_carry4(ctx, "add", di, gnd);

    p.pack_carries();
    ctx->assignArchInfo();
    xil->assign_cell_tags();
    xil->cell_tags_set = true; // bel changes update the tile status only once the tags exist

    CellInfo *c4 = packed_carry4(ctx);
    ASSERT_NE(c4, nullptr);
    // positions 1 and 2 bypass; the constant positions keep a LUT (an INIT, not a route)
    EXPECT_EQ(int_or_default(c4->params, id_DI_BYPASS, 0), (1 << 1) | (1 << 2));
    EXPECT_EQ(c4->getPort(ctx->id("DI1")), di[1]); // ports carry the bel pin names once packed
    EXPECT_EQ(c4->getPort(ctx->id("DI2")), di[2]);
    EXPECT_EQ(di_lut_count(ctx), 2);
    // the legality model sees the bypassed nets on the position's X pin
    auto tags = xil->get_tags(c4);
    ASSERT_NE(tags, nullptr);
    EXPECT_EQ(tags->carry.x_sigs[1], di[1]);
    EXPECT_EQ(tags->carry.x_sigs[2], di[2]);
    EXPECT_EQ(tags->carry.x_sigs[3], nullptr);
}

TEST_F(XilinxPackTest, carry_di_from_a_single_load_lut_packs_the_lut)
{
    // A LUT whose only load is DI goes into the position's 5LUT as before
    XC7Packer p(ctx, xil);
    p.pack_constants();
    NetInfo *gnd = ctx->nets.at(ctx->id("$PACKER_GND_NET")).get();
    CellInfo *lut = ctx->createCell(ctx->id("dilut"), id_LUT1);
    lut->addInput(id_I0);
    lut->addOutput(id_O);
    lut->params[id_INIT] = Property(2, 2);
    lut->connectPort(id_I0, fabric_net(ctx, "y"));
    NetInfo *dinet = ctx->createNet(ctx->id("dinet"));
    lut->connectPort(id_O, dinet);
    NetInfo *di[4] = {gnd, dinet, gnd, gnd};
    create_carry4(ctx, "add", di, gnd);

    p.pack_carries();

    CellInfo *c4 = packed_carry4(ctx);
    ASSERT_NE(c4, nullptr);
    EXPECT_EQ(int_or_default(c4->params, id_DI_BYPASS, 0), 0);
    EXPECT_EQ(lut->cluster, c4->cluster);
    EXPECT_TRUE(lut->constr_abs_z);
    EXPECT_EQ(lut->constr_z, (1 << 4) | BEL_5LUT);
}

TEST_F(XilinxPackTest, carry_di_lut_counts_distinct_nets_beside_an_s_feedthrough)
{
    // YosysHQ/nextpnr#1816: S[1] comes from the fabric, so it gets a
    // one-input feed-through LUT, and DI[1] from a LUT5 using five pins on
    // four nets.  The pair shares A1-A5, and the slice model counts the
    // distinct nets the two read (#187): 4 + 1 = 5, so the LUT5 fits beside
    // the feed-through, with fixup_placement giving the net on two pins one
    // physical pin.  The packer counts the same way; counted as pins, 5 + 1
    // with none shared, it sent DI to the bypass for nothing.
    XC7Packer p(ctx, xil);
    p.pack_constants();
    NetInfo *gnd = ctx->nets.at(ctx->id("$PACKER_GND_NET")).get();
    CellInfo *lut = ctx->createCell(ctx->id("dilut5"), id_LUT5);
    NetInfo *pnet = fabric_net(ctx, "p");
    NetInfo *ins[5] = {pnet, pnet, fabric_net(ctx, "q"), fabric_net(ctx, "r"), fabric_net(ctx, "t")};
    for (int j = 0; j < 5; j++) {
        lut->addInput(ctx->idf("I%d", j));
        lut->connectPort(ctx->idf("I%d", j), ins[j]);
    }
    lut->addOutput(id_O);
    lut->params[id_INIT] = Property(0x96969696, 32);
    NetInfo *dinet = ctx->createNet(ctx->id("dinet"));
    lut->connectPort(id_O, dinet);
    NetInfo *di[4] = {gnd, dinet, gnd, gnd};
    CellInfo *c4_in = create_carry4(ctx, "add", di, gnd);
    // S[1] from the fabric rather than the helper's LUT2
    CellInfo *s1_lut = ctx->cells.at(ctx->id("add_s1")).get();
    NetInfo *s1 = c4_in->getPort(ctx->id("S[1]"));
    s1_lut->disconnectPort(id_O);
    c4_in->disconnectPort(ctx->id("S[1]"));
    ctx->nets.erase(s1->name);
    c4_in->connectPort(ctx->id("S[1]"), fabric_net(ctx, "s1_fabric"));

    p.pack_carries();

    CellInfo *c4 = packed_carry4(ctx);
    ASSERT_NE(c4, nullptr);
    // the five-pin, four-net LUT fits beside the feed-through: DI[1] keeps its LUT
    EXPECT_EQ(int_or_default(c4->params, id_DI_BYPASS, 0), 0);
    EXPECT_EQ(lut->cluster, c4->cluster);
    // and the position's nets add up: every clustered LUT pair reads at most five distinct nets
    for (CellInfo *child : c4->constr_children) {
        if (!child->constr_abs_z || (child->constr_z & 0xf) != BEL_5LUT)
            continue;
        int pos = child->constr_z >> 4;
        std::set<NetInfo *> nets;
        for (CellInfo *other : c4->constr_children)
            if (other->constr_abs_z && other->constr_z == ((pos << 4) | BEL_6LUT))
                for (int j = 0; j < 6; j++)
                    if (NetInfo *n = other->getPort(ctx->idf("I%d", j)))
                        nets.insert(n);
        for (int j = 0; j < 5; j++)
            if (NetInfo *n = child->getPort(ctx->idf("I%d", j)))
                nets.insert(n);
        EXPECT_LE(int(nets.size()), 5) << "position " << pos;
    }
}

TEST_F(XilinxPackTest, carry_di0_keeps_a_lut_when_cyinit_has_the_pin)
{
    // CYINIT from the fabric takes position 0's bypass pin, so DI[0] cannot
    XC7Packer p(ctx, xil);
    p.pack_constants();
    NetInfo *gnd = ctx->nets.at(ctx->id("$PACKER_GND_NET")).get();
    NetInfo *di[4] = {fabric_net(ctx, "x0"), gnd, gnd, gnd};
    create_carry4(ctx, "add", di, fabric_net(ctx, "cin"));

    p.pack_carries();

    CellInfo *c4 = packed_carry4(ctx);
    ASSERT_NE(c4, nullptr);
    EXPECT_EQ(int_or_default(c4->params, id_DI_BYPASS, 0), 0);
    EXPECT_EQ(di_lut_count(ctx), 4);
}

TEST_F(XilinxPackTest, carry_di_bypass_can_be_switched_off)
{
    ctx->args.options.insert(std::make_pair(std::string("carry-di-bypass"),
                                            po::variable_value(boost::any(std::string("off")), false)));
    XC7Packer p(ctx, xil);
    p.pack_constants();
    NetInfo *gnd = ctx->nets.at(ctx->id("$PACKER_GND_NET")).get();
    NetInfo *di[4] = {gnd, fabric_net(ctx, "x1"), fabric_net(ctx, "x2"), gnd};
    create_carry4(ctx, "add", di, gnd);

    p.pack_carries();

    CellInfo *c4 = packed_carry4(ctx);
    ASSERT_NE(c4, nullptr);
    EXPECT_EQ(int_or_default(c4->params, id_DI_BYPASS, 0), 0);
    EXPECT_EQ(di_lut_count(ctx), 4);
}

// ---------------------------------------------------------------------------
// Two fabric-fed flip-flops in one position: the 5FF on the bypass pin, the
// main FF through the idle LUT as a route-through (xilinx_place.cc)

class XilinxRouteThruTest : public XilinxPackTest
{
  protected:
    // A packed flip-flop fed by a fabric net
    CellInfo *packed_ff(const char *name, NetInfo *d)
    {
        CellInfo *ff = ctx->createCell(ctx->id(name), id_SLICE_FFX);
        for (auto p : {id_D, id_CK, id_CE, id_SR})
            ff->addInput(p);
        ff->addOutput(id_Q);
        ff->attrs[id_X_ORIG_TYPE] = Property("FDRE");
        ff->connectPort(id_D, d);
        ff->connectPort(id_CK, clk);
        ff->connectPort(id_Q, ctx->createNet(ctx->idf("%s_q", name)));
        return ff;
    }
    CellInfo *packed_lut(const char *name)
    {
        CellInfo *lut = ctx->createCell(ctx->id(name), id_SLICE_LUTX);
        lut->addInput(id_A1);
        lut->addOutput(id_O6);
        lut->connectPort(id_A1, fabric_net(ctx, stringf("%s_in", name).c_str()));
        lut->connectPort(id_O6, ctx->createNet(ctx->idf("%s_o", name)));
        return lut;
    }
    // The bel of the given name in the first logic tile
    BelId logic_bel(const char *name)
    {
        IdString want = ctx->id(name);
        for (BelId bel : ctx->getBels())
            if (xil->is_logic_tile(bel) && ctx->getBelName(bel)[1] == want)
                return bel;
        return BelId();
    }
    void setup_nets()
    {
        clk = ctx->createNet(ctx->id("clk"));
        n1 = fabric_net(ctx, "n1");
        n2 = fabric_net(ctx, "n2");
    }
    NetInfo *clk = nullptr, *n1 = nullptr, *n2 = nullptr;
};

TEST_F(XilinxRouteThruTest, main_ff_routes_through_the_idle_lut_beside_a_bypass_fed_5ff)
{
    setup_nets();
    CellInfo *ff1 = packed_ff("ff1", n1);
    CellInfo *ff2 = packed_ff("ff2", n2);
    ctx->assignArchInfo();
    xil->assign_cell_tags();
    xil->cell_tags_set = true; // bel changes update the tile status only once the tags exist
    BelId bff = logic_bel("SLICE_X0Y0.BFF"), b5ff = logic_bel("SLICE_X0Y0.B5FF");
    ASSERT_NE(bff, BelId());
    ASSERT_NE(b5ff, BelId());
    ctx->bindBel(bff, ff1, STRENGTH_USER);
    ctx->bindBel(b5ff, ff2, STRENGTH_USER);
    // legal exactly when the chipdb can route into the FF mux through the LUT
    EXPECT_EQ(ctx->isBelLocationValid(bff), xil->ff_routethru_pips);

    // a LUT in the position takes the route-through away
    CellInfo *lut = packed_lut("lut");
    ctx->assignArchInfo();
    xil->assign_cell_tags();
    xil->cell_tags_set = true; // bel changes update the tile status only once the tags exist
    ctx->bindBel(logic_bel("SLICE_X0Y0.B6LUT"), lut, STRENGTH_USER);
    EXPECT_FALSE(ctx->isBelLocationValid(bff));
}

TEST_F(XilinxRouteThruTest, position_a_has_no_route_through)
{
    // Its LUT serves the ground driver (is_pip_unavail)
    setup_nets();
    CellInfo *ff1 = packed_ff("ff1", n1);
    CellInfo *ff2 = packed_ff("ff2", n2);
    ctx->assignArchInfo();
    xil->assign_cell_tags();
    xil->cell_tags_set = true; // bel changes update the tile status only once the tags exist
    BelId aff = logic_bel("SLICE_X0Y0.AFF");
    ASSERT_NE(aff, BelId());
    ctx->bindBel(aff, ff1, STRENGTH_USER);
    ctx->bindBel(logic_bel("SLICE_X0Y0.A5FF"), ff2, STRENGTH_USER);
    EXPECT_FALSE(ctx->isBelLocationValid(aff));
}

TEST_F(XilinxRouteThruTest, one_fabric_fed_ff_per_position_stays_legal)
{
    setup_nets();
    CellInfo *ff2 = packed_ff("ff2", n2);
    ctx->assignArchInfo();
    xil->assign_cell_tags();
    xil->cell_tags_set = true; // bel changes update the tile status only once the tags exist
    BelId b5ff = logic_bel("SLICE_X0Y0.B5FF");
    ctx->bindBel(b5ff, ff2, STRENGTH_USER);
    EXPECT_TRUE(ctx->isBelLocationValid(b5ff));
}

TEST_F(XilinxRouteThruTest, carry_di_mux_has_a_bypass_leg)
{
    // The DI mux (xCY0) and the flip-flop muxes must be reachable from the
    // position's bypass pin (AX for position A, CX for C) for pack_carry's
    // choice and the FF route-through to be routable: the leg from that pin
    // has to exist and be available, not merely some leg.
    BelId cy = logic_bel("SLICE_X0Y0.CARRY4");
    ASSERT_NE(cy, BelId());
    IdString tname = ctx->getBelName(cy)[0];
    for (const char *w : {"SLICE_X0Y0.ACY0_OUT", "SLICE_X0Y0.CCY0_OUT", "SLICE_X0Y0.AFFMUX_OUT", "SLICE_X0Y0.A5FFMUX_OUT"}) {
        WireId wire = ctx->getWireByName(IdStringList::concat(tname, ctx->id(w)));
        ASSERT_NE(wire, WireId()) << w;
        const std::string bypass_pin = std::string("SLICE_X0Y0.") + std::string(w).at(11) + "X";
        int legs = 0, bypass = 0;
        for (PipId pip : ctx->getPipsUphill(wire)) {
            std::string src = ctx->getWireName(ctx->getPipSrcWire(pip))[1].str(ctx);
            bool avail = !xil->is_pip_unavail(pip);
            log_info("    %s <- %s%s\n", w, src.c_str(), avail ? "" : " (unavailable)");
            legs++;
            if (src == bypass_pin && avail)
                bypass++;
        }
        EXPECT_GT(legs, 0) << w;
        EXPECT_EQ(bypass, 1) << w << " has no available leg from " << bypass_pin;
    }
}

TEST_F(XilinxRouteThruTest, carry_di_bypass_pips_stay_available_with_the_carry_placed)
{
    // The site pips a bypassed DI needs, with a CARRY4 bound in the slice
    setup_nets();
    CellInfo *c4 = ctx->createCell(ctx->id("c4"), id_CARRY4);
    for (int i = 0; i < 4; i++) {
        c4->addInput(ctx->idf("S[%d]", i));
        c4->addInput(ctx->idf("DI[%d]", i));
        c4->addOutput(ctx->idf("O[%d]", i));
        c4->addOutput(ctx->idf("CO[%d]", i));
    }
    c4->addInput(id_CYINIT);
    c4->addInput(id_CI);
    c4->addInput(ctx->id("DI2"));
    c4->connectPort(ctx->id("DI2"), n2);
    c4->params[id_DI_BYPASS] = Property(4, 4);
    ctx->assignArchInfo();
    xil->assign_cell_tags();
    xil->cell_tags_set = true; // bel changes update the tile status only once the tags exist
    BelId cy = logic_bel("SLICE_X0Y0.CARRY4");
    ctx->bindBel(cy, c4, STRENGTH_USER);
    EXPECT_TRUE(ctx->isBelLocationValid(cy));
    for (int i = 0; i < 4; i++) {
        WireId w = ctx->getBelPinWire(cy, ctx->idf("DI%d", i));
        log_info("    DI%d pin wire: %s\n", i, w == WireId() ? "-" : ctx->nameOfWire(w));
        if (w == WireId())
            continue;
        for (PipId pip : ctx->getPipsUphill(w))
            log_info("        <- %s%s\n", ctx->nameOfWire(ctx->getPipSrcWire(pip)), xil->is_pip_unavail(pip) ? " (unavailable)" : "");
    }
    // and the X pin itself, from the interconnect
    IdString tname = ctx->getBelName(cy)[0];
    WireId cx = ctx->getWireByName(IdStringList::concat(tname, ctx->id("SLICE_X0Y0.CX")));
    ASSERT_NE(cx, WireId());
    int n = 0;
    for (PipId pip : ctx->getPipsUphill(cx)) {
        log_info("    CX <- %s%s\n", ctx->nameOfWire(ctx->getPipSrcWire(pip)), xil->is_pip_unavail(pip) ? " (unavailable)" : "");
        n++;
    }
    EXPECT_GT(n, 0);
}

TEST_F(XilinxRouteThruTest, carry_di_bypass_pips_in_a_slicem)
{
    // The same legs in the SLICEM of a CLBLM tile, where the quick-set
    // designs saw the DI-mux sink unreachable
    setup_nets();
    CellInfo *c4 = ctx->createCell(ctx->id("c4"), id_CARRY4);
    for (int i = 0; i < 4; i++) {
        c4->addInput(ctx->idf("S%d", i));
        c4->addInput(ctx->idf("DI%d", i));
        c4->addOutput(ctx->idf("O%d", i));
        c4->addOutput(ctx->idf("CO%d", i));
    }
    c4->addInput(id_CYINIT);
    c4->addInput(id_CIN);
    c4->connectPort(ctx->id("DI2"), n2);
    c4->params[id_DI_BYPASS] = Property(4, 4);
    ctx->assignArchInfo();
    xil->assign_cell_tags();
    xil->cell_tags_set = true;
    BelId cy;
    for (BelId bel : ctx->getBels()) {
        if (!xil->is_logic_tile(bel) || ctx->getBelName(bel)[1] != ctx->id("SLICE_X0Y0.CARRY4"))
            continue;
        IdString tt = xil->bel_tile_type(bel);
        if (tt == id_CLBLM_L || tt == id_CLBLM_R) {
            cy = bel;
            break;
        }
    }
    ASSERT_NE(cy, BelId());
    ctx->bindBel(cy, c4, STRENGTH_USER);
    EXPECT_TRUE(ctx->isBelLocationValid(cy));
    WireId w = ctx->getBelPinWire(cy, ctx->id("DI2"));
    ASSERT_NE(w, WireId());
    WireId cx = ctx->getWireByName(IdStringList::concat(ctx->getBelName(cy)[0], ctx->id("SLICE_X0Y0.CX")));
    ASSERT_NE(cx, WireId());
    int avail = 0, from_bypass = 0;
    for (PipId pip : ctx->getPipsUphill(w)) {
        bool ok = ctx->checkPipAvailForNet(pip, nullptr);
        log_info("    %s <- %s%s\n", ctx->nameOfWire(w), ctx->nameOfWire(ctx->getPipSrcWire(pip)), ok ? "" : " (unavailable)");
        avail += ok;
        if (ok && ctx->getPipSrcWire(pip) == cx)
            from_bypass++;
    }
    EXPECT_GE(avail, 2);
    EXPECT_EQ(from_bypass, 1) << "the DI mux has no available leg from the CX bypass pin";
    // and from the X pin back to the interconnect: the walk must leave the
    // site and reach a tile-routing wire within a few hops
    WireId cursor = cx;
    int hops = 0;
    for (; hops < 4 && cursor != WireId(); hops++) {
        PipId up;
        int n = 0;
        for (PipId pip : ctx->getPipsUphill(cursor)) {
            if (ctx->checkPipAvailForNet(pip, nullptr)) {
                up = pip;
                n++;
            }
        }
        log_info("    %s has %d available uphill pips\n", ctx->nameOfWire(cursor), n);
        if (n == 0)
            break;
        cursor = ctx->getPipSrcWire(up);
        if (ctx->getWireName(cursor)[1].str(ctx).find("SLICE_") == std::string::npos)
            break; // out of the site
    }
    ASSERT_NE(cursor, WireId());
    EXPECT_EQ(ctx->getWireName(cursor)[1].str(ctx).find("SLICE_"), std::string::npos)
            << "the bypass pin does not reach the interconnect: stuck at " << ctx->nameOfWire(cursor);
}

TEST_F(XilinxRouteThruTest, precyinit_legs)
{
    // What feeds a carry's CYINIT: the two constant legs, CIN from the slice
    // below, and the AX bypass pin (the leg a fabric CYINIT takes, which is
    // why position A yields its LUTs to it).  All four must be there.
    BelId cy = logic_bel("SLICE_X0Y0.CARRY4");
    ASSERT_NE(cy, BelId());
    WireId w = ctx->getBelPinWire(cy, id_CYINIT);
    ASSERT_NE(w, WireId());
    pool<std::string> legs;
    for (PipId pip : ctx->getPipsUphill(w)) {
        log_info("    %s <- %s\n", ctx->nameOfWire(w), ctx->nameOfWire(ctx->getPipSrcWire(pip)));
        legs.insert(ctx->getWireName(ctx->getPipSrcWire(pip))[1].str(ctx));
    }
    for (const char *leg : {"SLICE_X0Y0.CYINITGND_HARD0", "SLICE_X0Y0.CYINITVCC_HARD1", "SLICE_X0Y0.CIN", "SLICE_X0Y0.AX"})
        EXPECT_TRUE(legs.count(leg)) << "CYINIT has no leg from " << leg;
}

TEST_F(XilinxRouteThruTest, chipdb_has_ff_mux_route_throughs)
{
    log_info("    ff_routethru_pips = %d\n", int(xil->ff_routethru_pips));
    EXPECT_TRUE(xil->ff_routethru_pips);
}

// ---------------------------------------------------------------------------
// Replication of a flip-flop whose sinks the placement spread apart (replicate.cc)

TEST_F(XilinxRouteThruTest, spread_flip_flop_is_split_towards_its_far_sinks)
{
    setup_nets();
    // the flip-flop, flagged as a merge of three
    CellInfo *ff = packed_ff("ff", n1);
    ff->attrs[id_MERGED_EQUIV] = Property("ffb;ffc");
    NetInfo *q = ff->getPort(id_Q);
    // eight LUT sinks: four beside the flip-flop, four 120 tiles away
    BelId home = logic_bel("SLICE_X0Y0.AFF");
    ASSERT_NE(home, BelId());
    Loc hl = ctx->getBelLocation(home);
    std::vector<CellInfo *> sinks;
    for (int i = 0; i < 8; i++) {
        CellInfo *lut = ctx->createCell(ctx->idf("sink%d", i), id_SLICE_LUTX);
        lut->addInput(id_A1);
        lut->addOutput(id_O6);
        lut->connectPort(id_A1, q);
        lut->connectPort(id_O6, ctx->createNet(ctx->idf("sink%d_o", i)));
        sinks.push_back(lut);
    }
    ctx->assignArchInfo();
    xil->assign_cell_tags();
    xil->cell_tags_set = true;
    ctx->bindBel(home, ff, STRENGTH_WEAK);
    // a free LUT bel in a logic tile at or after (x, y)
    auto lut_bel_near = [&](int x, int y) {
        for (int dy = 0; dy < 40; dy++)
            for (int dx = 0; dx < 40; dx++) {
                for (BelId bel : ctx->getBelsByTile(x + dx, y + dy)) {
                    if (ctx->getBelType(bel) == id_SLICE_LUTX && ctx->checkBelAvail(bel) && xil->is_logic_tile(bel))
                        return bel;
                }
            }
        return BelId();
    };
    // the far group as far right as this (small) die allows
    const int far_x = std::min(hl.x + 120, ctx->getGridDimX() - 45);
    ASSERT_GT(far_x, hl.x + 40);
    for (int i = 0; i < 8; i++) {
        BelId b = lut_bel_near(i < 4 ? hl.x : far_x, hl.y + 2 * i);
        ASSERT_NE(b, BelId()) << i;
        ctx->bindBel(b, sinks[i], STRENGTH_WEAK);
    }

    xil->replicate_merged_ffs();

    CellInfo *copy = ctx->cells.count(ctx->id("ff$dup1")) ? ctx->cells.at(ctx->id("ff$dup1")).get() : nullptr;
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->type, id_SLICE_FFX);
    EXPECT_EQ(copy->getPort(id_D), n1);
    EXPECT_EQ(copy->getPort(id_CK), clk);
    EXPECT_EQ(copy->attrs.count(id_MERGED_EQUIV), size_t(0));
    ASSERT_NE(copy->bel, BelId());
    Loc cl = ctx->getBelLocation(copy->bel);
    EXPECT_GT(cl.x, (hl.x + far_x) / 2); // placed by the far group
    // the far sinks read the copy, the near ones the original
    int far_on_copy = 0, near_on_orig = 0;
    for (int i = 0; i < 8; i++) {
        NetInfo *d = sinks[i]->getPort(id_A1);
        if (i >= 4 && d == copy->getPort(id_Q))
            far_on_copy++;
        if (i < 4 && d == q)
            near_on_orig++;
    }
    EXPECT_EQ(far_on_copy, 4);
    EXPECT_EQ(near_on_orig, 4);
    EXPECT_TRUE(ctx->isBelLocationValid(copy->bel));
}

TEST_F(XilinxRouteThruTest, a_split_whose_driver_cannot_reach_the_copy_is_undone)
{
    // vc707-cdc6000 on the demos gate: the flip-flop at BFF read its slice's
    // F8MUX, which reached it inside the position, while the B5LUT's O5 left
    // the position through BMUX, the one selectable output.  The copy placed
    // by the far sinks read the F8MUX from outside, so the F8MUX needed BMUX
    // too, and the router had no way out ("Failed to route arc ... F8MUX_OUT
    // to ... B5FFMUX_OUT").  The copy's own site was checked; the driver's
    // position was not.
    setup_nets();
    NetInfo *f8 = ctx->createNet(ctx->id("f8"));
    CellInfo *mux = ctx->createCell(ctx->id("mux"), id_SELMUX2_1);
    mux->addInput(id_S0);
    mux->addOutput(id_OUT);
    mux->connectPort(id_S0, fabric_net(ctx, "sel"));
    mux->connectPort(id_OUT, f8);
    CellInfo *ff = packed_ff("ff", f8);
    ff->attrs[id_MERGED_EQUIV] = Property("ffb;ffc");
    NetInfo *q = ff->getPort(id_Q);
    // the position's 5LUT, whose output goes to the fabric
    CellInfo *lut5 = packed_lut("lut5");
    CellInfo *reader = packed_lut("reader");
    reader->disconnectPort(id_A1);
    reader->connectPort(id_A1, lut5->getPort(id_O6));
    // eight LUT sinks on the flip-flop: four beside it, four far away
    BelId home = logic_bel("SLICE_X0Y0.BFF");
    ASSERT_NE(home, BelId());
    Loc hl = ctx->getBelLocation(home);
    std::vector<CellInfo *> sinks;
    for (int i = 0; i < 8; i++) {
        CellInfo *lut = ctx->createCell(ctx->idf("sink%d", i), id_SLICE_LUTX);
        lut->addInput(id_A1);
        lut->addOutput(id_O6);
        lut->connectPort(id_A1, q);
        lut->connectPort(id_O6, ctx->createNet(ctx->idf("sink%d_o", i)));
        sinks.push_back(lut);
    }
    ctx->assignArchInfo();
    xil->assign_cell_tags();
    xil->cell_tags_set = true;
    ctx->bindBel(home, ff, STRENGTH_WEAK);
    ctx->bindBel(logic_bel("SLICE_X0Y0.F8MUX"), mux, STRENGTH_WEAK);
    ctx->bindBel(logic_bel("SLICE_X0Y0.B5LUT"), lut5, STRENGTH_WEAK);
    auto lut_bel_near = [&](int x, int y) {
        for (int dy = 0; dy < 40; dy++)
            for (int dx = 0; dx < 40; dx++) {
                for (BelId bel : ctx->getBelsByTile(x + dx, y + dy)) {
                    if (ctx->getBelType(bel) == id_SLICE_LUTX && ctx->checkBelAvail(bel) && xil->is_logic_tile(bel))
                        return bel;
                }
            }
        return BelId();
    };
    const int far_x = std::min(hl.x + 120, ctx->getGridDimX() - 45);
    ASSERT_GT(far_x, hl.x + 40);
    for (int i = 0; i < 8; i++) {
        BelId b = lut_bel_near(i < 4 ? hl.x + 2 : far_x, hl.y + 2 * i);
        ASSERT_NE(b, BelId()) << i;
        ctx->bindBel(b, sinks[i], STRENGTH_WEAK);
    }
    // legal as placed: the F8MUX output stays in its position, O5 has BMUX
    ASSERT_TRUE(ctx->isBelLocationValid(home));

    xil->replicate_merged_ffs();

    // the split was worth it by wirelength and is undone all the same
    EXPECT_EQ(ctx->cells.count(ctx->id("ff$dup1")), size_t(0));
    EXPECT_EQ(f8->users.entries(), size_t(1));
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(sinks[i]->getPort(id_A1), q) << i;
    EXPECT_TRUE(ctx->isBelLocationValid(home));
}
