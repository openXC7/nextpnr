/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  A reconfigurable window for partial reconfiguration with an open flow.
 *
 *  -o pr-window=x0,y0,x1,y1 names a rectangle of the chipdb tile grid
 *  (inclusive) and -o pr-cells=<prefix> the cells that belong inside it: the
 *  static design is everything else.  Three rules make the window's
 *  configuration frames rewritable on their own, with the static design
 *  live around them:
 *
 *   - a window cell is placed in the window (a Region constraint, which the
 *     analytic placer spreads within) and no other cell is (bel validity);
 *   - a static net (no pin on a window cell) may not use a pip in the window,
 *     and an internal net (every pin on one) may not use a pip outside it;
 *   - after routing the crossings are reported: every node a crossing net
 *     drives from one side and consumes from the other, classified by whether
 *     the driver reaches it without crossing first.  The clean ones are
 *     the window's interface, the contract a window-only build routes from.
 *
 *  See docs/xilinx-partial-reconfiguration.md.
 *
 *  A crossing net's route inside the window belongs to the window (it is
 *  replaced with the window), its route outside to the static design.  A
 *  node that merely passes through the window (an HCLK row line on its way
 *  to columns beyond) costs nothing: the rule is on pips, not wires.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 */

#include <boost/algorithm/string.hpp>
#include <cstring>
#include <fstream>
#include <functional>

#include "log.h"
#include "nextpnr.h"
#include "xilinx.h"

NEXTPNR_NAMESPACE_BEGIN

void XilinxImpl::init_pr_window()
{
    const ArchArgs &args = ctx->args;
    const bool no_window_option = !args.options.count("pr-window");
    if (no_window_option)
        return;
    std::string spec = args.options.at("pr-window").as<std::string>();
    std::vector<std::string> parts;
    boost::split(parts, spec, boost::is_any_of(","));
    const bool spec_is_not_four_numbers = parts.size() != 4;
    if (spec_is_not_four_numbers)
        log_error("-o pr-window wants x0,y0,x1,y1 (tile grid, inclusive), got '%s'\n", spec.c_str());
    prw_x0 = std::stoi(parts[0]);
    prw_y0 = std::stoi(parts[1]);
    prw_x1 = std::stoi(parts[2]);
    prw_y1 = std::stoi(parts[3]);
    const int w = ctx->chip_info->width, h = ctx->chip_info->height;
    const bool rectangle_is_off_the_grid =
            prw_x0 < 0 || prw_y0 < 0 || prw_x1 >= w || prw_y1 >= h || prw_x0 > prw_x1 || prw_y0 > prw_y1;
    if (rectangle_is_off_the_grid)
        log_error("-o pr-window=%s is not a rectangle inside the %dx%d tile grid\n", spec.c_str(), w, h);
    const bool no_cells_prefix = !args.options.count("pr-cells");
    if (no_cells_prefix)
        log_error("-o pr-window needs -o pr-cells=<name prefix of the cells inside>\n");
    prw_prefix = args.options.at("pr-cells").as<std::string>();
    prw_tile.assign(w * h, 0);
    for (int y = prw_y0; y <= prw_y1; y++)
        for (int x = prw_x0; x <= prw_x1; x++)
            prw_tile.at(tile_by_xy(ctx->chip_info, x, y)) = 1;
    prw_active = true;
    log_info("PR window: tiles X%d..%d Y%d..%d (%s .. %s), cells '%s*'\n", prw_x0, prw_x1, prw_y0, prw_y1,
             tile_name(tile_by_xy(ctx->chip_info, prw_x0, prw_y0)).c_str(),
             tile_name(tile_by_xy(ctx->chip_info, prw_x1, prw_y1)).c_str(), prw_prefix.c_str());
}

// The cells derived from a window cell carry its name (replicate's "$dupN",
// hold-fix's "<net>$holdbufN" on a net named after its window driver), so the
// prefix test covers them too.
bool XilinxImpl::pr_window_cell(const CellInfo *ci) const
{
    const bool no_cell = ci == nullptr;
    if (no_cell)
        return false;
    const bool is_listed_window_cell = prw_cells.count(ci->name) != 0;
    if (is_listed_window_cell)
        return true;
    // the placer asks this for every legality check: compare in place, no copy of the name
    return std::strncmp(ci->name.c_str(ctx), prw_prefix.c_str(), prw_prefix.size()) == 0;
}

// 0: a static net (no pin inside), 1: a crossing net (pins on both sides),
// 2: an internal net (every pin inside).  A static net may not use a window
// pip, an internal net may not use a pip outside: a route that left the
// window and came back would leave static pips driving into the window after
// the window is replaced.  A crossing net is the interface and may use both.
//
// This is called from the router's search threads, on its hottest path, so it
// must never write: the table is filled single-threaded by
// pr_window_index_nets() before each router entry and is read-only meanwhile.
// A net not in it (none today, but cheap to tolerate) is computed in place.
static int pr_window_classify(const XilinxImpl *impl, const NetInfo *net)
{
    int inside = 0, outside = 0;
    const bool has_driver = net->driver.cell != nullptr;
    if (has_driver)
        (impl->pr_window_cell(net->driver.cell) ? inside : outside)++;
    for (auto &usr : net->users)
        (impl->pr_window_cell(usr.cell) ? inside : outside)++;
    return inside == 0 ? 0 : (outside == 0 ? 2 : 1);
}

int XilinxImpl::pr_window_net_class(const NetInfo *net) const
{
    const int idx = net->name.index;
    const int known = idx < int(prw_net_class.size()) ? prw_net_class[idx] : -1;
    return known >= 0 ? known : pr_window_classify(this, net);
}

// Single-threaded, before a router runs: connectivity changes between router
// entries (hold-fix buffers move sinks to new nets), so this rebuilds the table.
void XilinxImpl::pr_window_index_nets()
{
    const bool no_window = !prw_active;
    if (no_window)
        return;
    int max_idx = -1;
    for (auto &net : ctx->nets)
        max_idx = std::max(max_idx, net.second->name.index);
    prw_net_class.assign(max_idx + 1, -1);
    for (auto &net : ctx->nets)
        prw_net_class[net.second->name.index] = pr_window_classify(this, net.second.get());
}

bool XilinxImpl::pr_window_net(const NetInfo *net) const { return pr_window_net_class(net) != 0; }

bool XilinxImpl::checkPipAvailForNet(PipId pip, const NetInfo *net) const
{
    const bool pip_unavailable = is_pip_unavail(pip);
    if (pip_unavailable)
        return false;
    const bool window_applies = prw_active && net != nullptr;
    if (window_applies) {
        const bool pip_is_in_the_window = prw_tile[pip.tile] != 0;
        const int cls = pr_window_net_class(net);
        const bool static_net_enters_the_window = pip_is_in_the_window && cls == 0;
        if (static_net_enters_the_window)
            return false;
        const bool internal_net_leaves_the_window = !pip_is_in_the_window && cls == 2;
        if (internal_net_leaves_the_window)
            return false;
    }
    return true;
}

// -o pr-reserve=<file>: one bel name per line; the wire on each bel's pin
// (a PARTPIN bel's node, in a window build) is withheld from every net
// (checkWireAvail) before routing.  A window build reserves the
// nodes the static design drives into the window beyond the ones its own
// partition pins take: they carry static signals and must stay untouched.
void XilinxImpl::pr_window_reserve()
{
    const ArchArgs &args = ctx->args;
    const bool no_reserve_file = !args.options.count("pr-reserve");
    if (no_reserve_file)
        return;
    std::string path = args.options.at("pr-reserve").as<std::string>();
    std::ifstream in(path);
    const bool cannot_open = !in;
    if (cannot_open)
        log_error("cannot read -o pr-reserve=%s\n", path.c_str());
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        const bool blank_or_comment = line.empty() || line[0] == '#';
        if (blank_or_comment)
            continue;
        BelId bel = ctx->getBelByNameStr(line);
        const bool no_such_bel = bel == BelId();
        if (no_such_bel)
            log_error("-o pr-reserve: no bel named '%s'\n", line.c_str());
        WireId w;
        for (IdString pin : ctx->getBelPins(bel)) {
            w = ctx->getBelPinWire(bel, pin);
            break;
        }
        const bool no_pin_wire = w == WireId();
        if (no_pin_wire)
            log_error("-o pr-reserve: bel '%s' has no pin wire\n", line.c_str());
        NetInfo *bound = ctx->getBoundWireNet(w);
        const bool wire_already_bound = bound != nullptr;
        if (wire_already_bound)
            log_error("-o pr-reserve: wire %s of bel '%s' is already used by net '%s'\n", ctx->nameOfWire(w),
                      line.c_str(), bound->name.c_str(ctx));
        // withheld through checkWireAvail rather than bound to a net: a
        // driverless net with wires trips router1's final check
        const bool newly_reserved = prw_reserved_wires.insert(w).second;
        if (newly_reserved)
            n++;
    }
    log_info("PR window: %d wires reserved from %s\n", n, path.c_str());
}

void XilinxImpl::pr_window_constrain_cells()
{
    const bool no_window = !prw_active;
    if (no_window)
        return;
    IdString rname = ctx->id("$PR_WINDOW");
    ctx->createRectangularRegion(rname, prw_x0, prw_y0, prw_x1, prw_y1);
    // A cluster (a carry chain with its LUTs and flip-flops, say) is placed as
    // one: if any of it is a window cell, all of it is, or no location is legal.
    dict<ClusterId, std::vector<CellInfo *>> clusters;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        const bool is_window_cell = pr_window_cell(ci);
        if (is_window_cell)
            prw_cells.insert(ci->name);
        const bool in_a_cluster = ci->cluster != ClusterId();
        if (in_a_cluster)
            clusters[ci->cluster].push_back(ci);
    }
    int pulled_in = 0;
    for (auto &cl : clusters) {
        bool any_inside = false;
        for (CellInfo *ci : cl.second)
            any_inside |= prw_cells.count(ci->name) != 0;
        const bool cluster_is_outside = !any_inside;
        if (cluster_is_outside)
            continue;
        for (CellInfo *ci : cl.second) {
            const bool already_a_window_cell = prw_cells.count(ci->name) != 0;
            if (already_a_window_cell)
                continue;
            prw_cells.insert(ci->name);
            pulled_in++;
            log_info("PR window: '%s' joins the window with its cluster\n", ci->name.c_str(ctx));
        }
    }
    int n = 0;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        const bool not_a_window_cell = !prw_cells.count(ci->name);
        if (not_a_window_cell)
            continue;
        ctx->constrainCellToRegion(ci->name, rname);
        n++;
    }
    const bool clusters_pulled_cells_in = pulled_in != 0;
    if (clusters_pulled_cells_in)
        log_warning("PR window: %d cells outside the prefix were pulled into the window by their clusters\n", pulled_in);
    const bool nothing_matched_the_prefix = n == 0;
    if (nothing_matched_the_prefix)
        log_error("-o pr-cells=%s: no cell has that name prefix\n", prw_prefix.c_str());
    log_info("PR window: %d cells constrained inside, %zu bels available there\n", n, ctx->region.at(rname)->bels.size());
}

// After routing: no static net may have used a window pip, and every
// crossing net's boundary nodes are reported (and written to -o pr-interface
// as "<net>\t<in|out|reentry|reexit>\t<tile>/<wire>" with the real tile
// names).  "in" is a node driven outside and consumed inside, reached from
// the driver without passing through the window; "out" the reverse.  A
// "reentry"/"reexit" is reached through the other side first (the route
// wandered), so it floats once the window is replaced: never a pin.  A net that
// crosses more than once (enters, leaves, enters again) is reported as such:
// the window build must then drive or consume every one of its nodes.
void XilinxImpl::pr_window_report()
{
    const bool no_window = !prw_active;
    if (no_window)
        return;
    pr_window_index_nets(); // the verdict below is on the final connectivity
    auto real_wire_name = [&](WireId w) {
        IdStringList n = ctx->getWireName(w);
        return tile_name(w.tile) + "/" + n[n.size() - 1].str(ctx);
    };
    int static_pips_inside = 0;
    std::vector<std::string> lines;
    pool<std::string> seen_lines;
    int crossing_nets = 0;
    for (auto &net : ctx->nets) {
        NetInfo *ni = net.second.get();
        const int cls = pr_window_net_class(ni);
        const bool is_static_or_internal = cls == 0 || cls == 2;
        if (is_static_or_internal) {
            for (auto &w : ni->wires) {
                PipId p = w.second.pip;
                const bool no_pip = p == PipId();
                if (no_pip)
                    continue;
                const bool pip_is_in_the_window = prw_tile[p.tile] != 0;
                const bool internal_pip_outside = cls == 2 && !pip_is_in_the_window;
                const bool static_pip_inside = cls == 0 && pip_is_in_the_window;
                if (static_pip_inside) {
                    log_warning("PR window: static net '%s' uses pip %s inside the window\n", ni->name.c_str(ctx),
                                ctx->nameOfPip(p));
                    static_pips_inside++;
                } else if (internal_pip_outside) {
                    log_warning("PR window: internal net '%s' uses pip %s outside the window\n", ni->name.c_str(ctx),
                                ctx->nameOfPip(p));
                    static_pips_inside++;
                }
            }
            continue;
        }
        // the constant nets reach the window through its own tie-off wires, not a boundary
        const bool is_constant_net = ni->constant_value != IdString();
        if (is_constant_net)
            continue;
        // the side a bound wire is driven from: its pip's tile, or the driver's bel for the source
        auto driven_inside = [&](WireId w) -> int {
            auto it = ni->wires.find(w);
            const bool wire_not_on_the_net = it == ni->wires.end();
            if (wire_not_on_the_net)
                return -1;
            PipId p = it->second.pip;
            const bool driven_by_a_pip = p != PipId();
            if (driven_by_a_pip)
                return prw_tile[p.tile] != 0;
            const bool driver_has_a_bel = ni->driver.cell != nullptr && ni->driver.cell->bel != BelId();
            return driver_has_a_bel ? (prw_tile[ni->driver.cell->bel.tile] != 0) : -1;
        };
        // A wire is "clean" when every pip between the driver and it is on the
        // driver's side of the boundary.  An entry reached through the window
        // (out and back in along a wire the window drove) is a re-entry: once
        // the window is replaced nothing drives it, so it cannot be a pin.
        dict<WireId, int8_t> clean_memo;
        std::function<bool(WireId)> clean = [&](WireId w) -> bool {
            auto m = clean_memo.find(w);
            const bool memoised = m != clean_memo.end();
            if (memoised)
                return m->second != 0;
            clean_memo[w] = 1; // against cycles
            auto it = ni->wires.find(w);
            bool ok = true;
            const bool reached_by_a_pip = it != ni->wires.end() && it->second.pip != PipId();
            if (reached_by_a_pip) {
                PipId p = it->second.pip;
                const bool driver_inside =
                        ni->driver.cell != nullptr && ni->driver.cell->bel != BelId() && prw_tile[ni->driver.cell->bel.tile] != 0;
                const bool pip_inside = prw_tile[p.tile] != 0;
                ok = (pip_inside == driver_inside) && clean(ctx->getPipSrcWire(p));
            }
            clean_memo[w] = ok ? 1 : 0;
            return ok;
        };
        int ins = 0, outs = 0;
        for (auto &w : ni->wires) {
            PipId p = w.second.pip;
            const bool no_pip = p == PipId();
            if (no_pip)
                continue;
            WireId s = ctx->getPipSrcWire(p);
            const int src_inside = driven_inside(s);
            const bool source_side_unknown = src_inside < 0;
            if (source_side_unknown)
                continue;
            const bool dst_inside = prw_tile[p.tile] != 0;
            if (bool(src_inside) == dst_inside)
                continue;
            const char *dir = dst_inside ? (clean(s) ? "in" : "reentry") : (clean(s) ? "out" : "reexit");
            (dst_inside ? ins : outs)++;
            std::string name = real_wire_name(s);
            std::string line = stringf("%s\t%s\t%s", ni->name.c_str(ctx), dir, name.c_str());
            const bool line_already_reported = seen_lines.count(line) != 0;
            if (line_already_reported)
                continue;
            seen_lines.insert(line);
            log_info("PR window: net '%s' crosses %s at %s (consumed by %s)\n", ni->name.c_str(ctx), dir, name.c_str(),
                     ctx->nameOfPip(p));
            lines.push_back(line);
        }
        const bool net_crosses = ins + outs > 0;
        if (net_crosses)
            crossing_nets++;
        const bool net_wanders = (ins > 0 && outs > 0) || ins > 1 || outs > 1;
        if (net_wanders)
            log_warning("PR window: net '%s' crosses the boundary %d times (%d in, %d out)\n", ni->name.c_str(ctx),
                        ins + outs, ins, outs);
    }
    log_info("PR window: %d crossing nets, %zu boundary nodes\n", crossing_nets, lines.size());
    const bool window_is_not_reconfigurable = static_pips_inside != 0;
    if (window_is_not_reconfigurable)
        log_error("PR window: %d pip(s) of static nets inside the window or of internal nets outside it; the window "
                  "is not reconfigurable\n", static_pips_inside);
    const bool wants_interface_file = ctx->args.options.count("pr-interface") != 0;
    if (wants_interface_file) {
        std::string path = ctx->args.options.at("pr-interface").as<std::string>();
        std::ofstream out(path);
        const bool cannot_write = !out;
        if (cannot_write)
            log_error("cannot write -o pr-interface=%s\n", path.c_str());
        for (auto &l : lines)
            out << l << "\n";
        log_info("PR window: interface written to %s\n", path.c_str());
    }
}

NEXTPNR_NAMESPACE_END
