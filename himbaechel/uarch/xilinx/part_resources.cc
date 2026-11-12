/*
 *  nextpnr -- Next Generation Place and Route
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

// What a part offers: what the device utilisation counts, and how much of
// each kind a design may use.
//
// The utilisation (the log and --report) counts the bels of each bucket.
// Three things made those counts larger than the part, compared with what
// Vivado 2026.1 reports as available for every part:
//
//  - every LUT is two bels, a 6-input and a 5-input one, so SLICE_LUTX was
//    twice the LUTs of the data sheets and of Vivado's "Slice LUTs";
//  - the IOB sites, and the ILOGIC/OLOGIC sites next to them, were the die's,
//    not the ones the package bonds (an xc7a35tcsg324 bonds 210 of 250), and
//    the pad of an IOB that can be half of a pair counted twice (512 pads);
//  - a device on a bigger device's die (an xc7a35t is an xc7a50t,
//    init_database) offers fewer LUTs, flip-flops, block RAMs and DSPs than
//    the die has, and had no limit at all: a design that does not fit an
//    xc7a35t placed and routed on it without a word.
//
// Vivado lists every site of the die for such a device, and places on any of
// them: only the four counts are limited.  So placement is not restricted
// here either; the counts are checked after packing, and the utilisation
// leaves out the bels a part does not offer.

#include "log.h"
#include "nextpnr.h"
#include "util.h"
#include "xilinx.h"

#define HIMBAECHEL_CONSTIDS "uarch/xilinx/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

void XilinxImpl::init_part_resources()
{
    // The sites the package bonds: those its pins name.  An ILOGIC or OLOGIC
    // site counts when the IOB at the same X/Y is bonded, as Vivado counts
    // them; the IDELAY sites count whether bonded or not, as in Vivado.
    pool<SiteIndex> bonded_sites;
    pool<std::pair<int, int>> bonded_iob_xy;
    const bool package_is_known = ctx->package_info != nullptr;
    if (package_is_known) {
        for (const auto &pad : ctx->package_info->pads) {
            BelId pad_bel = ctx->getBelByName(IdStringList::concat(IdString(pad.tile), IdString(pad.bel)));
            const bool pin_has_a_bel = pad_bel != BelId();
            if (!pin_has_a_bel)
                continue;
            SiteIndex site = get_bel_site(pad_bel);
            bonded_sites.insert(site);
            const auto &site_data = tile_extra_data(site.tile)->sites[site.site];
            const bool site_is_an_iob = IdString(site_data.name_prefix).str(ctx) == "IOB";
            if (site_is_an_iob)
                bonded_iob_xy.insert({site_data.site_x, site_data.site_y});
        }
    }

    // Leave out the IO sites the package does not bond and, on a device on a
    // bigger die, the LUTs, flip-flops, DSPs and block RAM tiles beyond those
    // it offers (the first ones in bel order count).
    const bool device_has_limits = part_limits.luts != 0;
    int luts = 0, ffs = 0, dsps = 0;
    pool<int> bram_tiles;
    for (auto bel : ctx->getBels()) {
        SiteIndex site = get_bel_site(bel);
        const bool bel_has_site = site.site >= 0 && site.site < tile_extra_data(site.tile)->sites.ssize();
        if (package_is_known && bel_has_site) {
            const auto &site_data = tile_extra_data(site.tile)->sites[site.site];
            const std::string &prefix = IdString(site_data.name_prefix).str(ctx);
            const bool is_pad_site = prefix == "IOB" || prefix == "IPAD" || prefix == "OPAD" || prefix == "IOPAD";
            const bool is_iologic_site = prefix == "ILOGIC" || prefix == "OLOGIC";
            const bool pad_site_not_bonded = is_pad_site && !bonded_sites.count(site);
            const bool iologic_of_an_unbonded_iob =
                    is_iologic_site && !bonded_iob_xy.count({site_data.site_x, site_data.site_y});
            // An IOB site that can be half of a differential pair has a PAD
            // bel in each of its variants (IOB33 and IOB33M, say): one pad.
            const int variant = BelSiteKey::unpack(chip_bel_info(ctx->chip_info, bel).site).site_variant;
            const bool pad_of_a_second_variant = is_pad_site && variant != 0 && ctx->getBelType(bel) == id_PAD;
            if (pad_site_not_bonded || iologic_of_an_unbonded_iob || pad_of_a_second_variant) {
                uncounted_bels.insert(bel);
                continue;
            }
        }
        if (!device_has_limits)
            continue;
        IdString bucket = getBelBucketForBel(bel);
        const bool is_lut6 = bucket == id_SLICE_LUTX && (ctx->getBelLocation(bel).z & 0xF) == BEL_6LUT;
        const bool is_ff = bucket == id_SLICE_FFX;
        const bool is_dsp = bucket == id_DSP48E1;
        const bool is_bram = is_bram_tile(bel);
        luts += is_lut6;
        ffs += is_ff;
        dsps += is_dsp;
        const bool bram_tile_is_new = is_bram && !bram_tiles.count(bel.tile);
        if (bram_tile_is_new)
            bram_tiles.insert(bel.tile);
        const bool beyond_the_part = (is_lut6 && luts > part_limits.luts) || (is_ff && ffs > part_limits.ffs) ||
                                     (is_dsp && dsps > part_limits.dsps) ||
                                     (is_bram && int(bram_tiles.size()) > part_limits.bram36);
        if (beyond_the_part)
            uncounted_bels.insert(bel);
    }
}

bool XilinxImpl::getBelHidden(BelId bel) const
{
    // A LUT is a 6-input LUT bel with a 5-input LUT bel in it: count it once,
    // as the data sheets and Vivado do.
    const bool is_lut5_half = ctx->getBelType(bel) == id_SLICE_LUTX && (ctx->getBelLocation(bel).z & 0xF) == BEL_5LUT;
    if (is_lut5_half)
        return true;
    return uncounted_bels.count(bel) != 0;
}

// After packing: the LUTs, flip-flops, DSPs and block RAMs a design needs are
// its cells, as Vivado counts them (its DRC UTLZ-1 refuses 20801 LUT cells on
// an xc7a35t even when two of them could share a LUT; a RAMB18E1 is half a
// block RAM).
void XilinxImpl::check_part_limits() const
{
    const bool device_has_limits = part_limits.luts != 0;
    if (!device_has_limits)
        return;
    dict<IdString, int> cells_in_bucket;
    for (auto &cell : ctx->cells)
        cells_in_bucket[getBelBucketForCellType(cell.second->type)]++;
    const int ffs = get_or_default(cells_in_bucket, id_SLICE_FFX, 0);
    const int dsps = get_or_default(cells_in_bucket, id_DSP48E1, 0);
    const int ramb36 = get_or_default(cells_in_bucket, id_RAMB36E1, 0);
    const int ramb18 = get_or_default(cells_in_bucket, id_RAMB18E1, 0);
    const int luts = get_or_default(cells_in_bucket, id_SLICE_LUTX, 0);
    const char *device = ctx->args.device.c_str();
    const bool too_many_ffs = ffs > part_limits.ffs;
    if (too_many_ffs)
        log_error("The design needs %d flip-flops; %s has %d.\n", ffs, device, part_limits.ffs);
    const bool too_many_dsps = dsps > part_limits.dsps;
    if (too_many_dsps)
        log_error("The design needs %d DSP48E1; %s has %d.\n", dsps, device, part_limits.dsps);
    const int bram_halves = 2 * ramb36 + ramb18;
    const bool too_many_brams = bram_halves > 2 * part_limits.bram36;
    if (too_many_brams)
        log_error("The design needs %d block RAMs (%d RAMB36E1, %d RAMB18E1); %s has %d.\n", (bram_halves + 1) / 2,
                  ramb36, ramb18, device, part_limits.bram36);
    const bool too_many_luts = luts > part_limits.luts;
    if (too_many_luts)
        log_error("The design needs %d LUTs; %s has %d.\n", luts, device, part_limits.luts);
}

NEXTPNR_NAMESPACE_END
