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
        attrs_before = ctx->attrs.size();
        settings_before = ctx->settings.size();
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

    // An import that fails is not undone step by step, so it must fail before it has changed anything: a caller that
    // keeps the Context (the GUI, the Python API) is left with the Context it had
    void expect_nothing_imported()
    {
        EXPECT_TRUE(ctx->cells.empty());
        EXPECT_TRUE(ctx->nets.empty());
        EXPECT_TRUE(ctx->hierarchy.empty());
        EXPECT_EQ(ctx->attrs.size(), attrs_before);
        EXPECT_EQ(ctx->settings.size(), settings_before);
        EXPECT_TRUE(ctx->top_module == IdString());
        EXPECT_FALSE(ctx->design_loaded);
    }

    ArchArgs chipArgs;
    Context *ctx;
    size_t attrs_before, settings_before;
};

TEST_F(XilinxJsonFrontendTest, a_module_that_instantiates_itself_is_an_error_that_leaves_the_context_as_it_was)
{
    // top has a cell, a net and a submodule that import fine, and then z_rec, a "sub": "sub" contains v, which is a
    // "sub" again. Cells are imported in the order of their names, so everything before z_rec would be in the Context
    // when the recursion is found, if the import were left to find it
    EXPECT_THROW(load(R"({"modules": {
        "top": {"attributes": {"top": "00000000000000000000000000000001"}, "ports": {},
                "cells": {"a_leaf": {"type": "LUT4", "port_directions": {"A": "input"}, "connections": {"A": [2]}},
                          "b_ok": {"type": "ok", "port_directions": {}, "connections": {}},
                          "z_rec": {"type": "sub", "port_directions": {}, "connections": {}}},
                "netnames": {"n": {"bits": [2]}}},
        "ok": {"ports": {}, "cells": {"w": {"type": "LUT4", "port_directions": {}, "connections": {}}}, "netnames": {}},
        "sub": {"ports": {}, "cells": {"v": {"type": "sub", "port_directions": {}, "connections": {}}}, "netnames": {}}
    }})"),
                 log_execution_error_exception);
    expect_nothing_imported();
}

TEST_F(XilinxJsonFrontendTest, a_cycle_of_modules_is_an_error_that_leaves_the_context_as_it_was)
{
    // top contains u, an "a", which contains x, a "b", which contains y, an "a" again
    EXPECT_THROW(load(R"({"modules": {
        "top": {"attributes": {"top": "00000000000000000000000000000001"}, "ports": {},
                "cells": {"a_leaf": {"type": "LUT4", "port_directions": {"A": "input"}, "connections": {"A": [2]}},
                          "u": {"type": "a", "port_directions": {}, "connections": {}}},
                "netnames": {"n": {"bits": [2]}}},
        "a": {"ports": {}, "cells": {"x": {"type": "b", "port_directions": {}, "connections": {}}}, "netnames": {}},
        "b": {"ports": {}, "cells": {"y": {"type": "a", "port_directions": {}, "connections": {}}}, "netnames": {}}
    }})"),
                 log_execution_error_exception);
    expect_nothing_imported();
}

TEST_F(XilinxJsonFrontendTest, a_recursive_module_that_the_design_does_not_reach_is_not_an_error)
{
    // "orphan" instantiates itself but nothing below top uses it, so it is never imported
    ASSERT_TRUE(load(R"({"modules": {
        "top": {"attributes": {"top": "00000000000000000000000000000001"}, "ports": {},
                "cells": {"u": {"type": "sub", "port_directions": {}, "connections": {}}}, "netnames": {}},
        "sub": {"ports": {}, "cells": {}, "netnames": {}},
        "orphan": {"ports": {}, "netnames": {},
                   "cells": {"o": {"type": "orphan", "port_directions": {}, "connections": {}}}}
    }})"));
    EXPECT_TRUE(ctx->hierarchy.count(ctx->id("top")));
    EXPECT_TRUE(ctx->hierarchy.count(ctx->id("top/u")));
    EXPECT_FALSE(ctx->hierarchy.count(ctx->id("orphan")));
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
    // and every submodule is linked from its parent, under the name of the instance
    struct Link
    {
        const char *parent, *instance, *child;
    };
    for (const Link &l : {Link{"top", "u1", "top/u1"}, Link{"top", "u2", "top/u2"}, Link{"top/u1", "w", "top/u1/w"},
                          Link{"top/u2", "w", "top/u2/w"}}) {
        const auto &links = ctx->hierarchy.at(ctx->id(l.parent)).hier_cells;
        ASSERT_TRUE(links.count(ctx->id(l.instance))) << l.parent << " has no link for " << l.instance;
        EXPECT_TRUE(links.at(ctx->id(l.instance)) == ctx->id(l.child)) << l.parent << "/" << l.instance;
    }
}
