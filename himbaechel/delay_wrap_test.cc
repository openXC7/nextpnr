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

#include <cstdint>
#include <limits>
#include "delay_wrap.h"
#include "gtest/gtest.h"

USING_NEXTPNR_NAMESPACE

// The timing analysis adds delays to, and subtracts them from, the times that it starts at INT_MAX and INT_MIN. The
// result has to be the number modulo 2^N that the analysis has always got, without a signed overflow, which is
// undefined: these tests are meant to be run with the sanitizers (-fsanitize=undefined) as well.
constexpr int32_t kMax = std::numeric_limits<int32_t>::max();
constexpr int32_t kMin = std::numeric_limits<int32_t>::min();

TEST(DelayWrapTest, sums_and_differences_in_range_are_exact)
{
    EXPECT_EQ(wrap_add<int32_t>(1000, 234), 1234);
    EXPECT_EQ(wrap_add<int32_t>(-5, 3), -2);
    EXPECT_EQ(wrap_sub<int32_t>(7, 9), -2);
    EXPECT_EQ(wrap_add<int32_t>(kMax, 0), kMax);
    EXPECT_EQ(wrap_add<int32_t>(kMin, 0), kMin);
    EXPECT_EQ(wrap_add<int32_t>(kMax, kMin), -1);
    EXPECT_EQ(wrap_sub<int32_t>(kMin, kMin), 0);
    EXPECT_EQ(wrap_sub<int32_t>(kMax, kMax), 0);
}

TEST(DelayWrapTest, the_largest_delay_plus_a_delay_wraps_around_to_the_smallest)
{
    // the min half of an arrival time that was never set, plus a route delay
    EXPECT_EQ(wrap_add<int32_t>(kMax, 200), kMin + 199);
    EXPECT_EQ(wrap_add<int32_t>(kMax, 1), kMin);
    EXPECT_EQ(wrap_add<int32_t>(kMax, kMax), -2);
}

TEST(DelayWrapTest, the_smallest_delay_minus_a_delay_wraps_around_to_the_largest)
{
    // the max half of a required time that was never set, minus a route delay
    EXPECT_EQ(wrap_sub<int32_t>(kMin, 256), kMax - 255);
    EXPECT_EQ(wrap_sub<int32_t>(kMin, 1), kMax);
    EXPECT_EQ(wrap_sub<int32_t>(kMin, kMax), 1);
    EXPECT_EQ(wrap_sub<int32_t>(kMax, kMin), -1);
}

TEST(DelayWrapTest, negating_the_smallest_delay_gives_the_smallest_delay)
{
    EXPECT_EQ(wrap_sub<int32_t>(0, kMin), kMin);
    EXPECT_EQ(wrap_sub<int32_t>(0, kMax), kMin + 1);
    EXPECT_EQ(wrap_sub<int32_t>(0, 1234), -1234);
}

TEST(DelayWrapTest, the_other_integer_widths_wrap_in_the_same_way)
{
    EXPECT_EQ(wrap_add<int16_t>(std::numeric_limits<int16_t>::max(), 1), std::numeric_limits<int16_t>::min());
    EXPECT_EQ(wrap_sub<int16_t>(std::numeric_limits<int16_t>::min(), 1), std::numeric_limits<int16_t>::max());
    EXPECT_EQ(wrap_add<int64_t>(std::numeric_limits<int64_t>::max(), 1), std::numeric_limits<int64_t>::min());
    EXPECT_EQ(wrap_sub<int64_t>(std::numeric_limits<int64_t>::min(), 1), std::numeric_limits<int64_t>::max());
    EXPECT_EQ(wrap_sub<int64_t>(0, std::numeric_limits<int64_t>::min()), std::numeric_limits<int64_t>::min());
}

TEST(DelayWrapTest, floating_point_delays_are_added_as_they_are)
{
    EXPECT_FLOAT_EQ(wrap_add<float>(1.5f, 2.25f), 3.75f);
    EXPECT_FLOAT_EQ(wrap_sub<float>(1.0f, 0.25f), 0.75f);
    EXPECT_DOUBLE_EQ(wrap_sub<double>(0.0, 0.5), -0.5);
}
