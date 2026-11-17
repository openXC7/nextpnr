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
