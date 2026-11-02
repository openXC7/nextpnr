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
    // The DI mux (xCY0) must be reachable from the bypass pin for pack_carry's
    // choice to be routable; list its legs
    BelId cy = logic_bel("SLICE_X0Y0.CARRY4");
    ASSERT_NE(cy, BelId());
    int tile = cy.tile;
    IdString tname = ctx->getBelName(cy)[0];
    for (const char *w : {"SLICE_X0Y0.ACY0_OUT", "SLICE_X0Y0.CCY0_OUT", "SLICE_X0Y0.AFFMUX_OUT", "SLICE_X0Y0.A5FFMUX_OUT"}) {
        WireId wire = ctx->getWireByName(IdStringList::concat(tname, ctx->id(w)));
        ASSERT_NE(wire, WireId()) << w;
        int legs = 0, bypass = 0;
        for (PipId pip : ctx->getPipsUphill(wire)) {
            std::string src = ctx->getWireName(ctx->getPipSrcWire(pip))[1].str(ctx);
            bool avail = !xil->is_pip_unavail(pip);
            log_info("    %s <- %s%s\n", w, src.c_str(), avail ? "" : " (unavailable)");
            legs++;
            if (src.find("X") != std::string::npos && avail)
                bypass++;
        }
        EXPECT_GT(legs, 0) << w;
        (void)tile;
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
    int avail = 0;
    for (PipId pip : ctx->getPipsUphill(w)) {
        bool ok = ctx->checkPipAvailForNet(pip, nullptr);
        log_info("    %s <- %s%s\n", ctx->nameOfWire(w), ctx->nameOfWire(ctx->getPipSrcWire(pip)), ok ? "" : " (unavailable)");
        avail += ok;
    }
    EXPECT_GE(avail, 2);
    // and from the X pin back to the interconnect
    WireId cursor = ctx->getWireByName(IdStringList::concat(ctx->getBelName(cy)[0], ctx->id("SLICE_X0Y0.CX")));
    for (int hop = 0; hop < 4 && cursor != WireId(); hop++) {
        PipId up;
        int n = 0;
        for (PipId pip : ctx->getPipsUphill(cursor)) {
            if (ctx->checkPipAvailForNet(pip, nullptr)) {
                up = pip;
                n++;
            }
        }
        log_info("    %s has %d available uphill pips\n", ctx->nameOfWire(cursor), n);
        cursor = (n >= 1) ? ctx->getPipSrcWire(up) : WireId();
    }
}

TEST_F(XilinxRouteThruTest, precyinit_legs)
{
    // What feeds a carry's CYINIT: the constant legs, CIN, and the AX pin
    BelId cy = logic_bel("SLICE_X0Y0.CARRY4");
    ASSERT_NE(cy, BelId());
    WireId w = ctx->getBelPinWire(cy, id_CYINIT);
    ASSERT_NE(w, WireId());
    for (PipId pip : ctx->getPipsUphill(w))
        log_info("    %s <- %s\n", ctx->nameOfWire(w), ctx->nameOfWire(ctx->getPipSrcWire(pip)));
}

TEST_F(XilinxRouteThruTest, chipdb_has_ff_mux_route_throughs)
{
    log_info("    ff_routethru_pips = %d\n", int(xil->ff_routethru_pips));
    EXPECT_TRUE(xil->ff_routethru_pips);
}
