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

#include <sstream>
#include "command.h"
#include "gtest/gtest.h"
#include "json_frontend.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

// The JSON frontend flattens the modules of a netlist into the Context and records the module tree in
// Context::hierarchy. These tests use a Context of this uarch, the only one the gtests have.
class XilinxJsonFrontendTest : public ::testing::Test
{
  protected:
    virtual void SetUp()
    {
        init_share_dirname();
        chipArgs.device = "xc7a50tcsg324-1";
        ctx = new Context(chipArgs);
        ctx->uarch->init(ctx);
        ctx->late_init();
    }

    virtual void TearDown() { delete ctx; }

    bool load(const std::string &json)
    {
        std::istringstream in(json);
        return parse_json(in, "test.json", ctx);
    }

    // Every link from a hierarchical cell to a submodule has to lead to an entry of the hierarchy: the walks over the
    // hierarchy (Context::fixupHierarchy() for one) look the entry up and throw if it is missing
    void expect_links_resolve()
    {
        for (auto &h : ctx->hierarchy)
            for (auto &sub : h.second.hier_cells)
                EXPECT_TRUE(ctx->hierarchy.count(sub.second))
                        << h.first.str(ctx) << " links to " << sub.second.str(ctx) << ", which does not exist";
    }

    ArchArgs chipArgs;
    Context *ctx;
};

TEST_F(XilinxJsonFrontendTest, a_module_that_instantiates_itself_is_an_error_and_leaves_the_hierarchy_consistent)
{
    // top contains u (a "sub"), and "sub" contains v, which is a "sub" again
    EXPECT_THROW(load(R"({"modules": {
        "top": {"attributes": {"top": "00000000000000000000000000000001"}, "ports": {},
                "cells": {"u": {"type": "sub", "port_directions": {}, "connections": {}}}, "netnames": {}},
        "sub": {"ports": {}, "cells": {"v": {"type": "sub", "port_directions": {}, "connections": {}}}, "netnames": {}}
    }})"),
                 log_execution_error_exception);
    expect_links_resolve();
}

TEST_F(XilinxJsonFrontendTest, a_module_instantiated_more_than_once_is_not_recursion)
{
    // "sub" is used twice, by two siblings, and each copy contains a "leaf": a type that is repeated but is never its
    // own ancestor
    ASSERT_TRUE(load(R"({"modules": {
        "top": {"attributes": {"top": "00000000000000000000000000000001"}, "ports": {},
                "cells": {"u1": {"type": "sub", "port_directions": {}, "connections": {}},
                          "u2": {"type": "sub", "port_directions": {}, "connections": {}}}, "netnames": {}},
        "sub": {"ports": {}, "cells": {"w": {"type": "leaf", "port_directions": {}, "connections": {}}}, "netnames": {}},
        "leaf": {"ports": {}, "cells": {}, "netnames": {}}
    }})"));
    for (const char *path : {"top", "top/u1", "top/u1/w", "top/u2", "top/u2/w"})
        EXPECT_TRUE(ctx->hierarchy.count(ctx->id(path))) << path;
    expect_links_resolve();
}
