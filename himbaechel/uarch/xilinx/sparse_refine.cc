/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Post-placement refinement of the cells on sparse bel types: DSP48E1 and
 *  block RAM.
 *
 *  Their bels sit in a few columns five rows apart, so the analytical placer's
 *  spreading, which pushes the dense logic a tile at a time, moves a DSP a
 *  column at a time; the annealer afterwards proposes moves within a shrinking
 *  radius that rarely reaches the next free DSP site.  On a 4-lane kHeavyHash
 *  miner this left each DSP 27-38 tiles from the adder tree reading its
 *  48-bit product, where Vivado's placement of the same netlist has 5-10.
 *
 *  Each such cell is offered every free bel of its type within a radius and
 *  takes the one that most reduces the half-perimeter wirelength of its nets,
 *  with HeAP's x:y weighting; passes repeat while anything moves.  Cells a
 *  user pinned or that belong to a cluster (a cascade) stay.
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 */

#include <algorithm>
#include <map>

#include "log.h"
#include "nextpnr.h"
#include "xilinx.h"

#define HIMBAECHEL_CONSTIDS "uarch/xilinx/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

struct Box
{
    int x0 = std::numeric_limits<int>::max(), y0 = std::numeric_limits<int>::max();
    int x1 = std::numeric_limits<int>::min(), y1 = std::numeric_limits<int>::min();
    bool empty() const { return x0 > x1; }
    void add(int x, int y)
    {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    // HeAP's weighting for this arch: a horizontal tile step counts double
    int64_t hpwl() const { return empty() ? 0 : int64_t(2) * (x1 - x0) + (y1 - y0); }
};

} // namespace

void XilinxImpl::refine_sparse_bels()
{
    if (ctx->args.options.count("sparse-refine") && ctx->args.options.at("sparse-refine").as<std::string>() == "off")
        return;
    const int radius = 80;        // tiles, Manhattan, from the cell's current site
    const int max_passes = 8;
    const size_t big_net = 2000;  // a clock or reset: moving one pin changes nothing worth the time

    pool<IdString> sparse_types;
    sparse_types.insert(id_DSP48E1_DSP48E1);
    sparse_types.insert(id_RAMB36E1_RAMB36E1);
    sparse_types.insert(id_RAMB18E1_RAMB18E1);

    // The movable cells, and the free bels of each type
    std::vector<CellInfo *> cells;
    std::map<IdString, std::vector<BelId>> free_bels;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (!sparse_types.count(ci->type) || ci->bel == BelId())
            continue;
        const bool pinned_or_in_a_cascade = ci->belStrength > STRENGTH_STRONG || ci->cluster != ClusterId();
        if (pinned_or_in_a_cascade)
            continue;
        cells.push_back(ci);
    }
    if (cells.empty())
        return;
    std::sort(cells.begin(), cells.end(), [](CellInfo *a, CellInfo *b) { return a->name < b->name; });
    for (auto bel : ctx->getBels()) {
        IdString type = ctx->getBelType(bel);
        if (sparse_types.count(type) && ctx->checkBelAvail(bel))
            free_bels[type].push_back(bel);
    }

    // The box of a net's pins other than the given cell's, over placed cells
    auto others_box = [&](NetInfo *net, CellInfo *except) {
        Box b;
        auto add = [&](CellInfo *c) {
            if (c == nullptr || c == except || c->bel == BelId())
                return;
            Loc l = ctx->getBelLocation(c->bel);
            b.add(l.x, l.y);
        };
        add(net->driver.cell);
        for (auto &usr : net->users)
            add(usr.cell);
        return b;
    };

    int moved_total = 0;
    int64_t gain_total = 0;
    for (int pass = 0; pass < max_passes; pass++) {
        int moved = 0;
        for (CellInfo *ci : cells) {
            // Each distinct net once, with the box of everything else on it
            std::vector<Box> boxes;
            pool<IdString> seen;
            for (auto &port : ci->ports) {
                NetInfo *net = port.second.net;
                if (net == nullptr || seen.count(net->name) || net->users.entries() > big_net)
                    continue;
                seen.insert(net->name);
                boxes.push_back(others_box(net, ci));
            }
            auto cost_at = [&](Loc l) {
                int64_t c = 0;
                for (auto b : boxes) {
                    b.add(l.x, l.y);
                    c += b.hpwl();
                }
                return c;
            };
            Loc here = ctx->getBelLocation(ci->bel);
            const int64_t cost_here = cost_at(here);
            BelId best;
            int64_t best_cost = cost_here;
            auto &candidates = free_bels[ci->type];
            for (BelId bel : candidates) {
                Loc l = ctx->getBelLocation(bel);
                if (std::abs(l.x - here.x) + std::abs(l.y - here.y) > radius)
                    continue;
                int64_t c = cost_at(l);
                if (c < best_cost) {
                    best_cost = c;
                    best = bel;
                }
            }
            if (best == BelId())
                continue;
            BelId old = ci->bel;
            ctx->unbindBel(old);
            ctx->bindBel(best, ci, STRENGTH_WEAK);
            if (!ctx->isBelLocationValid(best)) {
                ctx->unbindBel(best);
                ctx->bindBel(old, ci, STRENGTH_WEAK);
                continue;
            }
            // the vacated bel becomes a candidate, the taken one stops being one
            candidates.erase(std::find(candidates.begin(), candidates.end(), best));
            candidates.push_back(old);
            moved++;
            gain_total += cost_here - best_cost;
        }
        moved_total += moved;
        if (ctx->verbose)
            log_info("    sparse-refine pass %d: %d cells moved\n", pass, moved);
        if (moved == 0)
            break;
    }
    log_info("sparse-refine: %d of %zu DSP/BRAM cells moved to a nearer free site, wirelength down by %lld\n", moved_total,
             cells.size(), (long long)gain_total);
}

NEXTPNR_NAMESPACE_END
