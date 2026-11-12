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

#include "command.h"
#include "gtest/gtest.h"
#include "nextpnr.h"
#include "uarch/xilinx/xilinx.h"
#define HIMBAECHEL_CONSTIDS "uarch/xilinx/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

// The device utilisation counts the bels of a bucket that are not hidden.
// An xc7a35t is the xc7a50t die: the same chipdb, fewer LUTs, flip-flops,
// block RAMs and DSPs.  The expected numbers are what Vivado 2026.1 reports
// as available for each part (report_utilization of an empty design).
class XilinxPartResourcesTest : public ::testing::Test
{
  protected:
    virtual void SetUp() { init_share_dirname(); }

    dict<IdString, int> counted(const std::string &device)
    {
        ArchArgs chipArgs;
        chipArgs.device = device;
        Context ctx(chipArgs);
        ctx.uarch->init(&ctx);
        ctx.late_init();
        dict<IdString, int> result;
        for (auto bel : ctx.getBels()) {
            if (!ctx.getBelHidden(bel))
                result[ctx.getBelBucketForBel(bel)]++;
        }
        return result;
    }
};

TEST_F(XilinxPartResourcesTest, xc7a50t_counts_its_luts_once)
{
    auto n = counted("xc7a50tcsg324-1");
    EXPECT_EQ(n[id_SLICE_LUTX], 32600);
    EXPECT_EQ(n[id_SLICE_FFX], 65200);
    EXPECT_EQ(n[id_CARRY4], 8150);
    EXPECT_EQ(n[id_DSP48E1], 120);
    EXPECT_EQ(n[id_RAMB36E1], 75);
    EXPECT_EQ(n[id_RAMB18E1], 150);
}

TEST_F(XilinxPartResourcesTest, xc7a35t_counts_what_it_offers)
{
    auto n = counted("xc7a35tcsg324-1");
    EXPECT_EQ(n[id_SLICE_LUTX], 20800);
    EXPECT_EQ(n[id_SLICE_FFX], 41600);
    EXPECT_EQ(n[id_DSP48E1], 90);
    EXPECT_EQ(n[id_RAMB36E1], 50);
    EXPECT_EQ(n[id_RAMB18E1], 100);
    // Vivado limits these four only: the slices (one CARRY4 each) are the die's
    EXPECT_EQ(n[id_CARRY4], 8150);
}

TEST_F(XilinxPartResourcesTest, csg324_counts_its_bonded_io)
{
    for (auto device : {"xc7a35tcsg324-1", "xc7a50tcsg324-1"}) {
        auto n = counted(device);
        // 210 bonded IOBs, plus the XADC's VP and VN pads
        EXPECT_EQ(n[id_IOB33_INBUF_EN], 210) << device;
        EXPECT_EQ(n[id_IOB33_OUTBUF], 210) << device;
        EXPECT_EQ(n[id_ILOGICE3_IFF], 210) << device;
        EXPECT_EQ(n[id_OLOGICE3_OUTFF], 210) << device;
        EXPECT_EQ(n[id_PAD], 212) << device;
        // every IDELAY counts, bonded or not, as in Vivado
        EXPECT_EQ(n[id_IDELAYE2], 250) << device;
    }
}
