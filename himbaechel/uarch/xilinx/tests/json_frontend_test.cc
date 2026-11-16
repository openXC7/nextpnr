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

// Before the check, a module that instantiates itself was imported until the stack overflowed: the test binary died
TEST_F(XilinxJsonFrontendTest, a_module_that_instantiates_itself_is_an_error)
{
    // top contains u, a "sub", and "sub" contains v, which is a "sub" again
    EXPECT_THROW(load(R"({"modules": {
        "top": {"attributes": {"top": "00000000000000000000000000000001"}, "ports": {},
                "cells": {"u": {"type": "sub", "port_directions": {}, "connections": {}}}, "netnames": {}},
        "sub": {"ports": {}, "cells": {"v": {"type": "sub", "port_directions": {}, "connections": {}}}, "netnames": {}}
    }})"),
                 log_execution_error_exception);
}

TEST_F(XilinxJsonFrontendTest, a_cycle_of_modules_is_an_error)
{
    // top contains u, an "a", which contains x, a "b", which contains y, an "a" again
    EXPECT_THROW(load(R"({"modules": {
        "top": {"attributes": {"top": "00000000000000000000000000000001"}, "ports": {},
                "cells": {"u": {"type": "a", "port_directions": {}, "connections": {}}}, "netnames": {}},
        "a": {"ports": {}, "cells": {"x": {"type": "b", "port_directions": {}, "connections": {}}}, "netnames": {}},
        "b": {"ports": {}, "cells": {"y": {"type": "a", "port_directions": {}, "connections": {}}}, "netnames": {}}
    }})"),
                 log_execution_error_exception);
}

TEST_F(XilinxJsonFrontendTest, a_cycle_at_the_end_of_a_very_deep_hierarchy_is_an_error_and_not_a_stack_overflow)
{
    // top is m0, m0 contains an m1, and so on for far more levels than a call stack holds one frame per level for;
    // the last module contains itself. The check has to reach the end of the chain and report the cycle
    const int depth = 60000;
    std::string netlist = "{\"modules\": {\n";
    for (int i = 0; i <= depth; i++) {
        const std::string next = "m" + std::to_string(i < depth ? i + 1 : depth);
        netlist += "\"m" + std::to_string(i) + "\": {" +
                   (i == 0 ? "\"attributes\": {\"top\": \"00000000000000000000000000000001\"}, " : "") +
                   "\"ports\": {}, \"netnames\": {}, \"cells\": {\"c\": {\"type\": \"" + next +
                   "\", \"port_directions\": {}, \"connections\": {}}}}" + (i < depth ? ",\n" : "\n");
    }
    netlist += "}}";
    EXPECT_THROW(load(netlist), log_execution_error_exception);
}

TEST_F(XilinxJsonFrontendTest, the_names_in_the_error_of_a_recursive_module_are_escaped)
{
    // names with a newline, a terminal escape sequence, a bell, a C1 control (U+0085) and a Unicode line separator
    // (U+2028) must not reach the log as they are: a name could forge a line of the log, or move the cursor of a
    // terminal
    std::string logged;
    log_write_function = [&](std::string text) { logged += text; };
    EXPECT_THROW(load(R"({"modules": {
        "top": {"attributes": {"top": "00000000000000000000000000000001"}, "ports": {}, "netnames": {},
                "cells": {"u": {"type": "evil\nERROR: forged\u001b[2J\u0085\u2028", "port_directions": {},
                               "connections": {}}}},
        "evil\nERROR: forged\u001b[2J\u0085\u2028": {"ports": {}, "netnames": {},
                "cells": {"v\u0007": {"type": "evil\nERROR: forged\u001b[2J\u0085\u2028", "port_directions": {},
                                      "connections": {}}}}
    }})"),
                 log_execution_error_exception);
    log_write_function = nullptr;
    EXPECT_NE(logged.find("instantiates itself"), std::string::npos);
    EXPECT_NE(logged.find("evil\\x0aERROR: forged\\x1b[2J\\xc2\\x85\\xe2\\x80\\xa8"), std::string::npos) << logged;
    EXPECT_NE(logged.find("(cell 'v\\x07'"), std::string::npos) << logged;
    EXPECT_EQ(logged.find('\x1b'), std::string::npos);
    EXPECT_EQ(logged.find("\xc2\x85"), std::string::npos);
    EXPECT_EQ(logged.find("\xe2\x80\xa8"), std::string::npos);
    EXPECT_EQ(logged.find("\nERROR: forged"), std::string::npos);
}

TEST_F(XilinxJsonFrontendTest, a_very_long_name_is_cut_in_the_error_of_a_recursive_module)
{
    const std::string name(5000, 'a');
    std::string logged;
    log_write_function = [&](std::string text) { logged += text; };
    EXPECT_THROW(load(R"({"modules": {
        "top": {"attributes": {"top": "00000000000000000000000000000001"}, "ports": {}, "netnames": {},
                "cells": {"u": {"type": ")" +
                      name + R"(", "port_directions": {}, "connections": {}}}},
        ")" + name + R"(": {"ports": {}, "netnames": {},
                "cells": {"v": {"type": ")" +
                      name + R"(", "port_directions": {}, "connections": {}}}}
    }})"),
                 log_execution_error_exception);
    log_write_function = nullptr;
    EXPECT_NE(logged.find("instantiates itself"), std::string::npos);
    EXPECT_NE(logged.find(std::string(100, 'a') + "..."), std::string::npos);
    EXPECT_LT(logged.size(), 1000u);
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
