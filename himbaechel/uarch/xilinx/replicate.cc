/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Post-placement replication of merged flip-flops.
 *
 *  The synthesis merges equivalent flip-flops (same D, clock, enable, reset
 *  and initial value) into one and marks the survivor with MERGED_EQUIV, the
 *  names of those it absorbed.  That is right for area and for a placement
 *  that keeps the sinks together, and wrong when they end up far apart: on a
 *  4-lane kHeavyHash miner the four lanes' identical matrix registers became
 *  one bank fanning out to four compact lanes, and a quarter of the design's
 *  nets then crossed the die centre and routed one at a time.
 *
 *  So after placement each merged flip-flop is offered a split: its placed
 *  sinks are clustered (k-means on the tile grid, k up to one more than the
 *  number merged), and when the clusters' wirelength is well below the single
 *  net's, the flip-flop is copied once per extra cluster -- same D, clock,
 *  enable, reset and INIT, named <original>$dup<n> -- each copy taking its
 *  cluster's sinks and a free flip-flop site next to them.  The LVS checker
 *  knows a $dup copy stands for its original.  -o replicate-merged=off
 *  disables it.
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 */

#include <algorithm>
#include <cmath>

#include "log.h"
#include "nextpnr.h"
#include "xilinx.h"

#define HIMBAECHEL_CONSTIDS "uarch/xilinx/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

struct Pt
{
    int x, y;
};

// HeAP's weighting for this arch: a horizontal tile step counts double
int64_t hpwl(const std::vector<Pt> &pts)
{
    if (pts.empty())
        return 0;
    int x0 = pts[0].x, x1 = pts[0].x, y0 = pts[0].y, y1 = pts[0].y;
    for (auto &p : pts) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    return int64_t(2) * (x1 - x0) + (y1 - y0);
}

// k-means with farthest-point seeding; returns each point's cluster
std::vector<int> kmeans(const std::vector<Pt> &pts, int k, std::vector<Pt> &centres)
{
    centres.clear();
    centres.push_back(pts[0]);
    while (int(centres.size()) < k) {
        int64_t best = -1;
        Pt bp = pts[0];
        for (auto &p : pts) {
            int64_t d = std::numeric_limits<int64_t>::max();
            for (auto &c : centres)
                d = std::min<int64_t>(d, 2 * std::abs(p.x - c.x) + std::abs(p.y - c.y));
            if (d > best) {
                best = d;
                bp = p;
            }
        }
        centres.push_back(bp);
    }
    std::vector<int> assign(pts.size(), 0);
    for (int iter = 0; iter < 12; iter++) {
        bool changed = false;
        for (size_t i = 0; i < pts.size(); i++) {
            int bc = 0;
            int64_t bd = std::numeric_limits<int64_t>::max();
            for (int c = 0; c < k; c++) {
                int64_t d = 2 * std::abs(pts[i].x - centres[c].x) + std::abs(pts[i].y - centres[c].y);
                if (d < bd) {
                    bd = d;
                    bc = c;
                }
            }
            if (assign[i] != bc) {
                assign[i] = bc;
                changed = true;
            }
        }
        std::vector<int64_t> sx(k, 0), sy(k, 0), n(k, 0);
        for (size_t i = 0; i < pts.size(); i++) {
            sx[assign[i]] += pts[i].x;
            sy[assign[i]] += pts[i].y;
            n[assign[i]]++;
        }
        for (int c = 0; c < k; c++)
            if (n[c] > 0)
                centres[c] = Pt{int(sx[c] / n[c]), int(sy[c] / n[c])};
        if (!changed)
            break;
    }
    return assign;
}

} // namespace

void XilinxImpl::replicate_merged_ffs()
{
    if (ctx->args.options.count("replicate") &&
        ctx->args.options.at("replicate").as<std::string>() == "off")
        return;
    const int copy_cost = 24;          // HPWL a copy has to save to be worth a flip-flop and a net
    const size_t min_sinks = 4;
    const int search_radius = 40;      // tiles around a cluster's centre to look for a free site

    // Every placed flip-flop is a candidate: one the synthesis never merged
    // can have its sinks spread just as far.  The merge flag only raises how
    // many copies it may become (its pre-merge count); the rest get up to two.
    std::vector<CellInfo *> merged;
    size_t flagged = 0;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type != id_SLICE_FFX || ci->bel == BelId())
            continue;
        if (ci->belStrength > STRENGTH_STRONG || ci->cluster != ClusterId())
            continue;
        if (ci->attrs.count(id_MERGED_EQUIV))
            flagged++;
        merged.push_back(ci);
    }
    if (merged.empty())
        return;
    std::sort(merged.begin(), merged.end(), [](CellInfo *a, CellInfo *b) { return a->name < b->name; });

    struct Copy
    {
        CellInfo *cell;
        Pt centre;
    };
    std::vector<Copy> copies;
    int64_t saved = 0;
    int split = 0;
    for (CellInfo *ci : merged) {
        NetInfo *q = ci->getPort(id_Q);
        if (q == nullptr)
            continue;
        int absorbed = 2;
        if (ci->attrs.count(id_MERGED_EQUIV)) {
            const std::string names = ci->attrs.at(id_MERGED_EQUIV).as_string();
            absorbed = int(std::count(names.begin(), names.end(), ';')) + 1;
        }
        std::vector<PortRef> sinks;
        std::vector<Pt> pts;
        for (auto &usr : q->users) {
            if (usr.cell == nullptr || usr.cell->bel == BelId())
                continue;
            Loc l = ctx->getBelLocation(usr.cell->bel);
            sinks.push_back(usr);
            pts.push_back(Pt{l.x, l.y});
        }
        if (sinks.size() < min_sinks)
            continue;
        Loc here = ctx->getBelLocation(ci->bel);
        std::vector<Pt> all = pts;
        all.push_back(Pt{here.x, here.y});
        const int64_t single = hpwl(all);
        // A split has to buy a quarter of the net's wirelength, not just beat
        // it: clustering always finds something marginally shorter, and a
        // copy is a flip-flop, a site and a net.
        int best_k = 1;
        int64_t best_cost = single * 3 / 4;
        std::vector<int> best_assign;
        std::vector<Pt> best_centres;
        const int kmax = std::min(absorbed + 1, std::min(4, int(sinks.size())));
        for (int k = 2; k <= kmax; k++) {
            std::vector<Pt> centres;
            std::vector<int> assign = kmeans(pts, k, centres);
            int64_t cost = int64_t(k - 1) * copy_cost;
            for (int c = 0; c < k; c++) {
                std::vector<Pt> members;
                for (size_t i = 0; i < pts.size(); i++)
                    if (assign[i] == c)
                        members.push_back(pts[i]);
                if (members.empty())
                    continue;
                members.push_back(centres[c]); // the copy sits by its cluster
                cost += hpwl(members);
            }
            if (cost < best_cost) {
                best_cost = cost;
                best_k = k;
                best_assign = assign;
                best_centres = centres;
            }
        }
        if (best_k == 1)
            continue;
        best_cost = std::min(best_cost, single); // the saving below is against the whole net
        // the biggest cluster keeps the original
        std::vector<int> size(best_k, 0);
        for (int a : best_assign)
            size[a]++;
        int keep = int(std::max_element(size.begin(), size.end()) - size.begin());
        int n = 0;
        for (int c = 0; c < best_k; c++) {
            if (c == keep || size[c] == 0)
                continue;
            n++;
            CellInfo *copy = ctx->createCell(ctx->idf("%s$dup%d", ci->name.c_str(ctx), n), id_SLICE_FFX);
            copy->params = ci->params;
            copy->attrs = ci->attrs;
            copy->attrs.erase(id_MERGED_EQUIV);
            for (auto &port : ci->ports) {
                if (port.second.type == PORT_OUT)
                    copy->addOutput(port.first);
                else
                    copy->addInput(port.first);
                if (port.second.type != PORT_OUT && port.second.net != nullptr)
                    copy->connectPort(port.first, port.second.net);
            }
            NetInfo *nq = ctx->createNet(ctx->idf("%s$dup%d$Q", ci->name.c_str(ctx), n));
            copy->connectPort(id_Q, nq);
            for (size_t i = 0; i < sinks.size(); i++) {
                if (best_assign[i] != c)
                    continue;
                sinks[i].cell->disconnectPort(sinks[i].port);
                sinks[i].cell->connectPort(sinks[i].port, nq);
            }
            copies.push_back(Copy{copy, best_centres[c]});
        }
        split++;
        saved += single - best_cost;
    }
    if (copies.empty()) {
        log_info("replicate: %zu flip-flops (%zu merged by the synthesis), none worth splitting\n", merged.size(),
                 flagged);
        return;
    }
    // The new cells need their indices and legality tags before they can be placed
    ctx->assignArchInfo();
    assign_cell_tags();

    int placed = 0;
    for (auto &cp : copies) {
        BelId found;
        for (int r = 0; r <= search_radius && found == BelId(); r++) {
            for (int dx = -r; dx <= r && found == BelId(); dx++) {
                for (int dy = -r; dy <= r && found == BelId(); dy++) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r)
                        continue;
                    int x = cp.centre.x + dx, y = cp.centre.y + dy;
                    if (x < 0 || y < 0 || x >= ctx->getGridDimX() || y >= ctx->getGridDimY())
                        continue;
                    for (BelId bel : ctx->getBelsByTile(x, y)) {
                        if (ctx->getBelType(bel) != id_SLICE_FFX || !ctx->checkBelAvail(bel))
                            continue;
                        ctx->bindBel(bel, cp.cell, STRENGTH_WEAK);
                        if (ctx->isBelLocationValid(bel)) {
                            found = bel;
                            break;
                        }
                        ctx->unbindBel(bel);
                    }
                }
            }
        }
        if (found == BelId())
            log_error("replicate: no free flip-flop site within %d tiles of (%d,%d) for '%s'\n", search_radius,
                      cp.centre.x, cp.centre.y, ctx->nameOf(cp.cell));
        placed++;
    }
    log_info("replicate: %d of %zu flip-flops (%zu merged by the synthesis) split into %d copies, wirelength down by "
             "%lld\n",
             split, merged.size(), flagged, placed, (long long)saved);
}

NEXTPNR_NAMESPACE_END
