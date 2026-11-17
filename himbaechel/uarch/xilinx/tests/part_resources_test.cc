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

#include <map>
#include <regex>
#include <sstream>
#include <string>
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

// The JSON report is what apio reads. Cells on a type whose bels are all
// hidden (the constant drivers) must not appear as used with nothing
// available. These two are created and not placed, so they count as their
// type. A placed cell counts on the bel it occupies. PAD stays the
// bonded-pad count, VP and VN included.
TEST_F(XilinxPartResourcesTest, report_used_stays_within_available)
{
    ArchArgs chipArgs;
    chipArgs.device = "xc7a35tcsg324-1";
    Context ctx(chipArgs);
    ctx.uarch->init(&ctx);
    ctx.late_init();
    ctx.createCell(ctx.id("$PACKER_GND_DRV"), id_PSEUDO_GND);
    ctx.createCell(ctx.id("$PACKER_VCC_DRV"), id_PSEUDO_VCC);
    std::ostringstream out;
    ctx.writeJsonReport(out);
    const std::string text = out.str();
    const bool gnd_is_absent = text.find("\"PSEUDO_GND\"") == std::string::npos;
    const bool vcc_is_absent = text.find("\"PSEUDO_VCC\"") == std::string::npos;
    EXPECT_TRUE(gnd_is_absent);
    EXPECT_TRUE(vcc_is_absent);
    // The root object is a std::map, so "fmax" is written before "utilization".
    // Take the utilization object by its braces.
    const size_t util_at = text.find("\"utilization\"");
    ASSERT_NE(util_at, std::string::npos);
    const size_t brace = text.find('{', util_at);
    ASSERT_NE(brace, std::string::npos);
    int depth = 0;
    size_t util_end = std::string::npos;
    for (size_t i = brace; i < text.size(); ++i) {
        const bool opens = text[i] == '{';
        const bool closes = text[i] == '}';
        if (opens)
            depth++;
        if (closes)
            depth--;
        const bool closed_the_object = closes && depth == 0;
        if (closed_the_object) {
            util_end = i;
            break;
        }
    }
    ASSERT_NE(util_end, std::string::npos);
    const std::string util = text.substr(brace, util_end - brace + 1);
    const std::regex row("\"([A-Za-z0-9_]+)\": \\{\"available\": (-?[0-9]+), \"used\": (-?[0-9]+)\\}");
    auto begin = std::sregex_iterator(util.begin(), util.end(), row);
    auto end = std::sregex_iterator();
    int rows = 0;
    bool saw_lut = false;
    bool saw_ff = false;
    bool saw_pad = false;
    bool saw_iob = false;
    for (auto it = begin; it != end; ++it) {
        const std::string name = (*it)[1].str();
        const int available = std::stoi((*it)[2].str());
        const int used = std::stoi((*it)[3].str());
        EXPECT_GE(used, 0) << name;
        EXPECT_LE(used, available) << name;
        if (name == "SLICE_LUTX") {
            EXPECT_EQ(available, 20800);
            saw_lut = true;
        }
        if (name == "SLICE_FFX") {
            EXPECT_EQ(available, 41600);
            saw_ff = true;
        }
        if (name == "PAD") {
            EXPECT_EQ(available, 212);
            saw_pad = true;
        }
        if (name == "IOB33_INBUF_EN") {
            EXPECT_EQ(available, 210);
            saw_iob = true;
        }
        rows++;
    }
    EXPECT_GT(rows, 10);
    EXPECT_TRUE(saw_lut);
    EXPECT_TRUE(saw_ff);
    EXPECT_TRUE(saw_pad);
    EXPECT_TRUE(saw_iob);
}

// A placed cell counts on the bel it occupies. The packer turns an IDDR into an
// ILOGICE3_IFF cell and an ODDR into an OLOGICE3_OUTFF cell and binds each to
// its site's flip-flop bel; in a High Performance bank those bels are
// ILOGICE2_IFF and OLOGICE2_OUTFF. Counted by cell type, the report had them
// on the E3 rows and nothing used on the E2 rows.
TEST_F(XilinxPartResourcesTest, report_counts_a_placed_cell_on_its_bel)
{
    ArchArgs chipArgs;
    chipArgs.device = "xc7k325tffg676-1";
    Context ctx(chipArgs);
    ctx.uarch->init(&ctx);
    ctx.late_init();
    BelId iff_bel;
    BelId outff_bel;
    for (auto bel : ctx.getBels()) {
        const std::string bucket = ctx.getBelBucketName(ctx.getBelBucketForBel(bel)).str(&ctx);
        const bool first_hp_iff = bucket == "ILOGICE2_IFF" && iff_bel == BelId();
        if (first_hp_iff)
            iff_bel = bel;
        const bool first_hp_outff = bucket == "OLOGICE2_OUTFF" && outff_bel == BelId();
        if (first_hp_outff)
            outff_bel = bel;
    }
    ASSERT_NE(iff_bel, BelId());
    ASSERT_NE(outff_bel, BelId());
    ctx.bindBel(iff_bel, ctx.createCell(ctx.id("iddr"), id_ILOGICE3_IFF), STRENGTH_LOCKED);
    ctx.bindBel(outff_bel, ctx.createCell(ctx.id("oddr"), id_OLOGICE3_OUTFF), STRENGTH_LOCKED);
    std::ostringstream out;
    ctx.writeJsonReport(out);
    const std::string text = out.str();
    std::map<std::string, std::pair<int, int>> rows;
    const std::regex row("\"([A-Za-z0-9_]+)\": \\{\"available\": (-?[0-9]+), \"used\": (-?[0-9]+)\\}");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), row); it != std::sregex_iterator(); ++it)
        rows[(*it)[1].str()] = std::make_pair(std::stoi((*it)[2].str()), std::stoi((*it)[3].str()));
    for (const auto &entry : rows)
        EXPECT_LE(entry.second.second, entry.second.first) << entry.first;
    ASSERT_EQ(rows.count("ILOGICE2_IFF"), 1u);
    ASSERT_EQ(rows.count("OLOGICE2_OUTFF"), 1u);
    EXPECT_EQ(rows.at("ILOGICE2_IFF").second, 1);
    EXPECT_EQ(rows.at("OLOGICE2_OUTFF").second, 1);
    const bool hr_iff_listed = rows.count("ILOGICE3_IFF") != 0;
    if (hr_iff_listed)
        EXPECT_EQ(rows.at("ILOGICE3_IFF").second, 0);
    const bool hr_outff_listed = rows.count("OLOGICE3_OUTFF") != 0;
    if (hr_outff_listed)
        EXPECT_EQ(rows.at("OLOGICE3_OUTFF").second, 0);
}
