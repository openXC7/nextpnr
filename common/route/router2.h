/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019  gatecat <gatecat@ds0.me>
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

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

inline float default_base_cost(Context *ctx, WireId wire, PipId pip, float crit_weight)
{
    (void)crit_weight; // unused
    return ctx->getDelayNS(ctx->getPipDelay(pip).maxDelay() + ctx->getWireDelay(wire).maxDelay() +
                           ctx->getDelayEpsilon());
}

struct Router2Cfg
{
    Router2Cfg(Context *ctx);

    // Maximum iterations for backwards routing attempt
    int backwards_max_iter;
    // Maximum iterations for backwards routing attempt for global nets
    int global_backwards_max_iter;
    // Padding added to bounding boxes to account for imperfect routing,
    // congestion, etc
    int bb_margin_x, bb_margin_y;
    // Cost factor added to input pin wires; effectively reduces the
    // benefit of sharing interconnect
    float ipin_cost_adder;
    // Cost factor for "bias" towards center location of net
    float bias_cost_factor;
    // Starting current and historical congestion cost factor
    float init_curr_cong_weight, hist_cong_weight;
    // Current congestion cost multiplier
    float curr_cong_mult;

    // Weight given to delay estimate in A*. Higher values
    // mean faster and more directed routing, at the risk
    // of choosing a less congestion/delay-optimal route
    float estimate_weight;

    // Congestion smoothing: a post-legality rip-up and re-route that makes
    // tile DENSITY a cost.  router2's own congestion cost is driven by
    // overuse -- nets illegally sharing a wire -- so once routing is legal
    // its gradient is flat, and a tile carrying four times its neighbours'
    // traffic is invisible to it.  Off by default; see
    // docs/routing-smoothing-pass.md.
    int smooth_iters = 0;
    float smooth_weight;
    float smooth_percentile;
    float smooth_max_crit;
    int smooth_max_fanout;
    float smooth_cap_frac;
    int smooth_min_wires;
    int smooth_stagnant;

    // Order the route queue by worst absolute slack rather than by criticality.
    bool slack_order;

    // Bound on bounding-box growth for congested nets; 0 = unbounded (the
    // historical behaviour).
    int bb_expand_max;

    // Size each net's box from timing criticality (tight for critical, loose
    // for slack) instead of growing it on congestion; bb_budget_max is the
    // loosest margin, given to a fully-slack net.
    bool bb_budget;
    int bb_budget_max;

    // Print additional performance profiling information
    bool perf_profile = false;

    std::string heatmap;
    std::function<float(Context *ctx, WireId wire, PipId pip, float crit_weight)> get_base_cost = default_base_cost;
};

void router2(Context *ctx, const Router2Cfg &cfg);

NEXTPNR_NAMESPACE_END
