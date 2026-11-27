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
#include "command.h"
#include "gtest/gtest.h"
#include "log.h"
#include "nextpnr.h"
#include "uarch/xilinx/xilinx.h"
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

USING_NEXTPNR_NAMESPACE

static int process_id()
{
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

// create_clock in the XDC reader, on a Context of the xc7a50t with a net clk to constrain
class XilinxXdcTest : public ::testing::Test
{
  protected:
    virtual void SetUp()
    {
        init_share_dirname();
        chipArgs.device = "xc7a50tcsg324-1";
        ctx = new Context(chipArgs);
        ctx->uarch->init(ctx);
        ctx->late_init();
        clk = ctx->createNet(ctx->id("clk"));
    }

    virtual void TearDown()
    {
        std::remove(filename.c_str());
        delete ctx;
    }

    // Reads the constraints from a file, as -o xdc= does
    void parse(const std::string &constraints)
    {
        {
            std::ofstream out(filename);
            out << constraints;
        }
        static_cast<XilinxImpl *>(ctx->uarch.get())->parse_xdc(filename);
    }

    ArchArgs chipArgs;
    Context *ctx;
    NetInfo *clk;
    // one file per process: a test binary of another build may run at the same time
    const std::string filename = ::testing::TempDir() + "nextpnr_xdc_test_" + std::to_string(process_id()) + ".xdc";
};

// A negative period was applied to the net, and the timing analysis overflowed on N-1 periods of a multicycle path
TEST_F(XilinxXdcTest, a_negative_clock_period_is_an_error)
{
    EXPECT_THROW(parse("create_clock -period -10.000 [get_nets clk]\n"), log_execution_error_exception);
    EXPECT_EQ(clk->clkconstr.get(), nullptr);
}

TEST_F(XilinxXdcTest, a_clock_period_of_zero_is_an_error)
{
    EXPECT_THROW(parse("create_clock -period 0 [get_nets clk]\n"), log_execution_error_exception);
    EXPECT_EQ(clk->clkconstr.get(), nullptr);
}

// std::stod reads "nan" and "inf" as numbers, and converting either to a delay was undefined
TEST_F(XilinxXdcTest, a_clock_period_of_nan_is_an_error)
{
    EXPECT_THROW(parse("create_clock -period nan [get_nets clk]\n"), log_execution_error_exception);
    EXPECT_EQ(clk->clkconstr.get(), nullptr);
}

TEST_F(XilinxXdcTest, an_infinite_clock_period_is_an_error)
{
    EXPECT_THROW(parse("create_clock -period inf [get_nets clk]\n"), log_execution_error_exception);
    EXPECT_EQ(clk->clkconstr.get(), nullptr);
}

TEST_F(XilinxXdcTest, a_positive_clock_period_constrains_the_net)
{
    parse("create_clock -period 10.000 [get_nets clk]\n");
    ASSERT_NE(clk->clkconstr.get(), nullptr);
    EXPECT_EQ(clk->clkconstr->period.minDelay(), 10000);
    EXPECT_EQ(clk->clkconstr->high.minDelay(), 5000);
    EXPECT_EQ(clk->clkconstr->low.minDelay(), 5000);
}

// Vivado's PULLUP/PULLDOWN/KEEPER TRUE on a port: the pull resistor the bitstream writer reads
// from PULLTYPE. Without it, an open-drain bus with no external pull-up (I2C) floated.
TEST_F(XilinxXdcTest, pullup_true_selects_the_pull_up)
{
    CellInfo *sda = ctx->createCell(ctx->id("sda"), ctx->id("$nextpnr_iobuf"));
    parse("set_property PULLUP TRUE [get_ports sda]\n");
    ASSERT_EQ(sda->attrs.count(ctx->id("PULLTYPE")), 1U);
    EXPECT_EQ(sda->attrs.at(ctx->id("PULLTYPE")).as_string(), "PULLUP");
}

TEST_F(XilinxXdcTest, pulldown_and_keeper_in_a_dict_select_their_pull)
{
    CellInfo *a = ctx->createCell(ctx->id("a"), ctx->id("$nextpnr_ibuf"));
    CellInfo *b = ctx->createCell(ctx->id("b"), ctx->id("$nextpnr_ibuf"));
    parse("set_property -dict {PULLDOWN true IOSTANDARD LVCMOS33} [get_ports a]\n"
          "set_property KEEPER 1 [get_ports b]\n");
    EXPECT_EQ(a->attrs.at(ctx->id("PULLTYPE")).as_string(), "PULLDOWN");
    EXPECT_EQ(a->attrs.at(ctx->id("IOSTANDARD")).as_string(), "LVCMOS33");
    EXPECT_EQ(b->attrs.at(ctx->id("PULLTYPE")).as_string(), "KEEPER");
}

TEST_F(XilinxXdcTest, pullup_false_leaves_no_pull)
{
    CellInfo *sda = ctx->createCell(ctx->id("sda"), ctx->id("$nextpnr_iobuf"));
    parse("set_property PULLUP FALSE [get_ports sda]\n");
    EXPECT_EQ(sda->attrs.count(ctx->id("PULLTYPE")), 0U);
}

TEST_F(XilinxXdcTest, pullup_true_and_pulltype_pullup_agree)
{
    CellInfo *sda = ctx->createCell(ctx->id("sda"), ctx->id("$nextpnr_iobuf"));
    parse("set_property PULLUP TRUE [get_ports sda]\n"
          "set_property PULLTYPE PULLUP [get_ports sda]\n");
    EXPECT_EQ(sda->attrs.at(ctx->id("PULLTYPE")).as_string(), "PULLUP");
}

TEST_F(XilinxXdcTest, two_different_pulls_are_an_error)
{
    ctx->createCell(ctx->id("sda"), ctx->id("$nextpnr_iobuf"));
    EXPECT_THROW(parse("set_property PULLTYPE PULLDOWN [get_ports sda]\n"
                       "set_property PULLUP TRUE [get_ports sda]\n"),
                 log_execution_error_exception);
}
