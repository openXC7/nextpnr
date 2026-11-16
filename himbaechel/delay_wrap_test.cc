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

#include <cmath>
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

// The slack that follows from a time that is a wrapped sentinel can be out of the range of delay_t (and of int, in
// picoseconds) even though it is a number: converting it has to give the nearest value in range, not an undefined one.
TEST(DelayWrapTest, a_floating_point_value_in_range_is_truncated_as_in_a_conversion)
{
    EXPECT_EQ(saturating_cast<int32_t>(1234.9), 1234);
    EXPECT_EQ(saturating_cast<int32_t>(-5.5), -5);
    EXPECT_EQ(saturating_cast<int32_t>(0.0f), 0);
    EXPECT_EQ(saturating_cast<int32_t>(static_cast<double>(kMax)), kMax);
    EXPECT_EQ(saturating_cast<int32_t>(static_cast<double>(kMin)), kMin);
    EXPECT_EQ(saturating_cast<int32_t>(2147483520.0f), 2147483520);
}

TEST(DelayWrapTest, a_floating_point_value_out_of_range_is_saturated)
{
    EXPECT_EQ(saturating_cast<int32_t>(3.0e9), kMax);
    EXPECT_EQ(saturating_cast<int32_t>(-3.0e9), kMin);
    EXPECT_EQ(saturating_cast<int32_t>(2147483648.0f), kMax); // float(INT32_MAX) rounds up to this
    EXPECT_EQ(saturating_cast<int32_t>(-2147483904.0f), kMin);
    EXPECT_EQ(saturating_cast<int32_t>(std::numeric_limits<double>::infinity()), kMax);
    EXPECT_EQ(saturating_cast<int32_t>(-std::numeric_limits<double>::infinity()), kMin);
    EXPECT_EQ(saturating_cast<int32_t>(std::numeric_limits<double>::quiet_NaN()), kMin);
    EXPECT_EQ(saturating_cast<int64_t>(1.0e30), std::numeric_limits<int64_t>::max());
    EXPECT_EQ(saturating_cast<int16_t>(40000.0), std::numeric_limits<int16_t>::max());
}

TEST(DelayWrapTest, a_floating_point_target_takes_the_value_as_it_is)
{
    EXPECT_FLOAT_EQ(saturating_cast<float>(1.5), 1.5f);
    EXPECT_DOUBLE_EQ(saturating_cast<double>(-2.25f), -2.25);
}

TEST(DelayWrapTest, wrap_to_signed_maps_the_residues_to_the_two_halves_of_the_range)
{
    EXPECT_EQ(wrap_to_signed<int32_t>(0u), 0);
    EXPECT_EQ(wrap_to_signed<int32_t>(static_cast<uint32_t>(kMax)), kMax);
    EXPECT_EQ(wrap_to_signed<int32_t>(static_cast<uint32_t>(kMax) + 1u), kMin);
    EXPECT_EQ(wrap_to_signed<int32_t>(0xffffffffu), -1);
    EXPECT_EQ(wrap_to_signed<int16_t>(0x8000u), std::numeric_limits<int16_t>::min());
    EXPECT_EQ(wrap_to_signed<int64_t>(std::numeric_limits<uint64_t>::max()), -1);
}

TEST(DelayWrapTest, wrap_neg_negates_and_leaves_the_smallest_delay_alone)
{
    EXPECT_EQ(wrap_neg<int32_t>(1234), -1234);
    EXPECT_EQ(wrap_neg<int32_t>(-1234), 1234);
    EXPECT_EQ(wrap_neg<int32_t>(0), 0);
    EXPECT_EQ(wrap_neg<int32_t>(kMax), kMin + 1);
    EXPECT_EQ(wrap_neg<int32_t>(kMin), kMin);
    EXPECT_FLOAT_EQ(wrap_neg<float>(2.5f), -2.5f);
}

TEST(DelayWrapTest, wrap_neg_of_a_floating_point_zero_keeps_the_sign_that_the_unary_minus_gives)
{
    // compute_criticality() divides by the negated worst slack: -(+0.0f) is -0.0f, a division by it gives -infinity and
    // not +infinity, and a negation that came out as +0.0f would turn the criticality of a path with positive slack
    // from the largest into the smallest
    EXPECT_TRUE(std::signbit(wrap_neg<float>(0.0f)));
    EXPECT_FALSE(std::signbit(wrap_neg<float>(-0.0f)));
    EXPECT_EQ(5.0f / wrap_neg<float>(0.0f), -std::numeric_limits<float>::infinity());
    EXPECT_EQ(wrap_neg<int32_t>(0), 0);
}

TEST(DelayWrapTest, scale_delay_is_the_plain_expression_where_that_does_not_overflow)
{
    // (delay + (N - 1) periods) * h / (h + 2 * (N - 1)) for a path of 3000 ps, a period of 10000 ps, N = 2 and h = 2
    EXPECT_EQ(scale_delay<int32_t>(3000, 10000, 2, 4), (3000 + 10000) * 2 / 4);
    EXPECT_EQ(scale_delay<int32_t>(-3000, 10000, 1, 3), (-3000 + 10000) * 1 / 3);
    EXPECT_EQ(scale_delay<int32_t>(0, 0, 2, 3), 0);
    EXPECT_FLOAT_EQ(scale_delay<float>(3.0f, 1.0f, 3, 2), 6.0f);
}

TEST(DelayWrapTest, scale_delay_does_not_overflow_for_a_wrapped_sentinel)
{
    // the plain int expression overflows for both of these; exact in 64 bits, then clamped
    EXPECT_EQ(scale_delay<int32_t>(kMin + 599, 10000, 2, 4),
              static_cast<int32_t>((int64_t{kMin} + 599 + 10000) * 2 / 4));
    EXPECT_EQ(scale_delay<int32_t>(kMax, 10000, 2, 1), kMax);
    EXPECT_EQ(scale_delay<int32_t>(kMin, -10000, 2, 1), kMin);
}

TEST(DelayWrapTest, the_histogram_slack_is_still_subtracted_in_float)
{
    // The slack histogram takes its slack from slack_from_period(): the delay is rounded to float before it is
    // subtracted from the period, as it always was. 16777217 is not a float, so against a period of 16777216 the slack
    // is 0 (a subtraction in double would give -1)
    EXPECT_EQ(slack_from_period<int32_t>(16777216.0f, 16777217), 0);
    EXPECT_EQ(slack_from_period<int32_t>(16777216.0f, 16777218), -2);
    EXPECT_EQ(slack_from_period<int32_t>(10000.0f, 3000), 7000);
    EXPECT_EQ(slack_from_period<int32_t>(5000.5f, 3000), 2000); // truncated, as in a conversion
}

TEST(DelayWrapTest, the_histogram_slack_of_a_wrapped_sentinel_is_in_range)
{
    EXPECT_EQ(slack_from_period<int32_t>(10000.0f, kMin + 599), kMax);
    EXPECT_EQ(slack_from_period<int32_t>(10000.0f, kMin), kMax);
    EXPECT_EQ(slack_from_period<int32_t>(-10000.0f, kMax), kMin);
    EXPECT_FLOAT_EQ(slack_from_period<float>(10000.0f, 3000.0f), 7000.0f);
}

TEST(DelayWrapTest, scale_delay_takes_a_multicycle_count_beyond_the_range_of_int)
{
    // 2 * (N - 1) + h for N = 2^31 does not fit an int, and it is not the period that bounds N when the period is 0
    const int64_t n = int64_t{1} << 31;
    EXPECT_EQ(scale_delay<int32_t>(5000, 0, 2, 2 + 2 * (n - 1)), 0);
    EXPECT_EQ(scale_delay<int32_t>(kMax, 0, 2, 2 + 2 * (n - 1)), 0);
}
