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
 *  knows a $dup copy stands for its original.  -o replicate=off
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
    const bool no_points = pts.empty();
    if (no_points)
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
            const bool farther_than_best = d > best;
            if (farther_than_best) {
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
                const bool closer_than_best = d < bd;
                if (closer_than_best) {
                    bd = d;
                    bc = c;
                }
            }
            const bool moved_cluster = assign[i] != bc;
            if (moved_cluster) {
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
        for (int c = 0; c < k; c++) {
            const bool has_members = n[c] > 0;
            if (has_members)
                centres[c] = Pt{int(sx[c] / n[c]), int(sy[c] / n[c])};
        }
        const bool converged = !changed;
        if (converged)
            break;
    }
    return assign;
}

} // namespace

void XilinxImpl::replicate_merged_ffs()
{
    const bool replication_off = ctx->args.options.count("replicate") &&
                                 ctx->args.options.at("replicate").as<std::string>() == "off";
    if (replication_off)
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
        const bool not_a_placed_ff = ci->type != id_SLICE_FFX || ci->bel == BelId();
        if (not_a_placed_ff)
            continue;
        const bool fixed_or_clustered = ci->belStrength > STRENGTH_STRONG || ci->cluster != ClusterId();
        if (fixed_or_clustered)
            continue;
        const bool merged_by_synthesis = ci->attrs.count(id_MERGED_EQUIV);
        if (merged_by_synthesis)
            flagged++;
        merged.push_back(ci);
    }
    const bool no_candidates = merged.empty();
    if (no_candidates)
        return;
    std::sort(merged.begin(), merged.end(), [](CellInfo *a, CellInfo *b) { return a->name < b->name; });

    struct Copy
    {
        CellInfo *cell;
        Pt centre;
        NetInfo *original_q;              // where its sinks came from, for rolling the split back
        std::vector<PortRef> taken_sinks; // the sinks moved onto the copy
    };
    std::vector<Copy> copies;
    int64_t saved = 0;
    int split = 0;
    for (CellInfo *ci : merged) {
        NetInfo *q = ci->getPort(id_Q);
        const bool no_q_net = q == nullptr;
        if (no_q_net)
            continue;
        int absorbed = 2;
        const bool was_merged = ci->attrs.count(id_MERGED_EQUIV);
        if (was_merged) {
            const std::string names = ci->attrs.at(id_MERGED_EQUIV).as_string();
            absorbed = int(std::count(names.begin(), names.end(), ';')) + 1;
        }
        std::vector<PortRef> sinks;
        std::vector<Pt> pts;
        for (auto &usr : q->users) {
            const bool unplaced_sink = usr.cell == nullptr || usr.cell->bel == BelId();
            if (unplaced_sink)
                continue;
            Loc l = ctx->getBelLocation(usr.cell->bel);
            sinks.push_back(usr);
            pts.push_back(Pt{l.x, l.y});
        }
        const bool too_few_sinks = sinks.size() < min_sinks;
        if (too_few_sinks)
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
                for (size_t i = 0; i < pts.size(); i++) {
                    const bool in_this_cluster = assign[i] == c;
                    if (in_this_cluster)
                        members.push_back(pts[i]);
                }
                const bool empty_cluster = members.empty();
                if (empty_cluster)
                    continue;
                members.push_back(centres[c]); // the copy sits by its cluster
                cost += hpwl(members);
            }
            const bool cheaper_than_best = cost < best_cost;
            if (cheaper_than_best) {
                best_cost = cost;
                best_k = k;
                best_assign = assign;
                best_centres = centres;
            }
        }
        const bool not_worth_splitting = best_k == 1;
        if (not_worth_splitting)
            continue;
        best_cost = std::min(best_cost, single); // the saving below is against the whole net
        // the biggest cluster keeps the original
        std::vector<int> size(best_k, 0);
        for (int a : best_assign)
            size[a]++;
        int keep = int(std::max_element(size.begin(), size.end()) - size.begin());
        int n = 0;
        for (int c = 0; c < best_k; c++) {
            const bool keeps_original_or_empty = c == keep || size[c] == 0;
            if (keeps_original_or_empty)
                continue;
            n++;
            CellInfo *copy = ctx->createCell(ctx->idf("%s$dup%d", ci->name.c_str(ctx), n), id_SLICE_FFX);
            copy->params = ci->params;
            copy->attrs = ci->attrs;
            copy->attrs.erase(id_MERGED_EQUIV);
            copy->region = ci->region; // a copy keeps to the original's floorplan
            for (auto &port : ci->ports) {
                const bool is_output = port.second.type == PORT_OUT;
                if (is_output)
                    copy->addOutput(port.first);
                else
                    copy->addInput(port.first);
                const bool connected_input = port.second.type != PORT_OUT && port.second.net != nullptr;
                if (connected_input)
                    copy->connectPort(port.first, port.second.net);
            }
            NetInfo *nq = ctx->createNet(ctx->idf("%s$dup%d$Q", ci->name.c_str(ctx), n));
            copy->connectPort(id_Q, nq);
            Copy cp{copy, best_centres[c], q, {}};
            for (size_t i = 0; i < sinks.size(); i++) {
                const bool in_another_cluster = best_assign[i] != c;
                if (in_another_cluster)
                    continue;
                sinks[i].cell->disconnectPort(sinks[i].port);
                sinks[i].cell->connectPort(sinks[i].port, nq);
                cp.taken_sinks.push_back(sinks[i]);
            }
            copies.push_back(cp);
        }
        split++;
        saved += single - best_cost;
    }
    const bool nothing_split = copies.empty();
    if (nothing_split) {
        log_info("replicate: %zu flip-flops (%zu merged by the synthesis), none worth splitting\n", merged.size(),
                 flagged);
        return;
    }
    // The new cells need their indices and legality tags before they can be placed
    ctx->assignArchInfo();
    assign_cell_tags();

    // A copy reads the original's input nets, so each of their drivers gains
    // a user.  A driver inside a slice may have fed only its own position's
    // flip-flop -- a LUT's O5, a carry's O, an F7/F8 mux output -- and now
    // has to leave through the position's single output mux, which can
    // already be taken (an O5 on its way to the fabric; the OUTMUX budget in
    // xc7_logic_tile_valid).  The tile's legality cache refreshes on bel
    // changes, not net changes, so mark the driver's tile dirty and ask.
    // (vc707-cdc6000: a BFF reading its slice's F8MUX was split towards far
    // sinks while the B5LUT's O5 held BMUX, and the F8MUX had no way out:
    // "Failed to route arc ... F8MUX_OUT to ... B5FFMUX_OUT".)
    auto driver_tiles_dirty = [&](const CellInfo *copy) {
        std::vector<BelId> drivers;
        for (auto &port : copy->ports) {
            const bool not_a_driven_input = port.second.type == PORT_OUT || port.second.net == nullptr;
            if (not_a_driven_input)
                continue;
            const CellInfo *drv = port.second.net->driver.cell;
            const bool unplaced_driver = drv == nullptr || drv->bel == BelId();
            if (unplaced_driver)
                continue;
            auto &lts = tile_status.at(drv->bel.tile).lts;
            const bool no_logic_tile_status = !lts;
            if (no_logic_tile_status)
                continue;
            for (auto &eight : lts->eights)
                eight.dirty = true;
            for (auto &half : lts->halfs)
                half.dirty = true;
            drivers.push_back(drv->bel);
        }
        return drivers;
    };
    auto drivers_stay_legal = [&](const CellInfo *copy) {
        for (BelId bel : driver_tiles_dirty(copy)) {
            const bool driver_now_illegal = !isBelLocationValid(bel);
            if (driver_now_illegal)
                return false;
        }
        return true;
    };

    int placed = 0, undone = 0;
    for (auto &cp : copies) {
        const Region *region = cp.cell->region;
        BelId found;
        const bool drivers_can_reach_it = drivers_stay_legal(cp.cell);
        for (int r = 0; r <= search_radius && found == BelId() && drivers_can_reach_it; r++) {
            for (int dx = -r; dx <= r && found == BelId(); dx++) {
                for (int dy = -r; dy <= r && found == BelId(); dy++) {
                    const bool inside_the_ring = std::max(std::abs(dx), std::abs(dy)) != r;
                    if (inside_the_ring)
                        continue;
                    int x = cp.centre.x + dx, y = cp.centre.y + dy;
                    const bool off_the_grid = x < 0 || y < 0 || x >= ctx->getGridDimX() || y >= ctx->getGridDimY();
                    if (off_the_grid)
                        continue;
                    for (BelId bel : ctx->getBelsByTile(x, y)) {
                        const bool not_a_free_ff = ctx->getBelType(bel) != id_SLICE_FFX || !ctx->checkBelAvail(bel);
                        if (not_a_free_ff)
                            continue;
                        const bool outside_region = region != nullptr && region->constr_bels &&
                                                    !region->bels.count(bel);
                        if (outside_region)
                            continue;
                        ctx->bindBel(bel, cp.cell, STRENGTH_WEAK);
                        const bool legal_here = ctx->isBelLocationValid(bel);
                        if (legal_here) {
                            found = bel;
                            break;
                        }
                        ctx->unbindBel(bel);
                    }
                }
            }
        }
        const bool not_placed = found == BelId();
        if (not_placed) {
            // No site near the cluster, or a driver that cannot reach one: this
            // copy was not worth having.  Its sinks go back to the original,
            // whose placement is still legal, and the copy and its net
            // disappear.
            const bool no_site_within_radius = drivers_can_reach_it;
            if (no_site_within_radius)
                log_warning("replicate: no free flip-flop site within %d tiles of (%d,%d) for '%s'; keeping the "
                            "original\n",
                            search_radius, cp.centre.x, cp.centre.y, ctx->nameOf(cp.cell));
            else
                log_warning("replicate: a driver of '%s' has no free output mux to reach a copy; keeping the "
                            "original\n",
                            ctx->nameOf(cp.cell));
            for (auto &sink : cp.taken_sinks) {
                sink.cell->disconnectPort(sink.port);
                sink.cell->connectPort(sink.port, cp.original_q);
            }
            driver_tiles_dirty(cp.cell); // the drivers' legality is cached with the copy among their users
            IdString copy_name = cp.cell->name;
            NetInfo *nq = cp.cell->getPort(id_Q);
            for (auto &port : cp.cell->ports) {
                const bool is_connected = port.second.net != nullptr;
                if (is_connected)
                    cp.cell->disconnectPort(port.first);
            }
            ctx->nets.erase(nq->name);
            ctx->cells.erase(copy_name);
            undone++;
            continue;
        }
        placed++;
    }
    log_info("replicate: %d of %zu flip-flops (%zu merged by the synthesis) split into %d copies%s, wirelength down "
             "by %lld\n",
             split, merged.size(), flagged, placed, undone ? stringf(" (%d undone)", undone).c_str() : "",
             (long long)saved);
}

NEXTPNR_NAMESPACE_END
