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

#ifndef DELAY_WRAP_H
#define DELAY_WRAP_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

// Arithmetic on delays that can be derived from the sentinels of the timing analysis.
//
// The contract. TimingAnalyser::reset_times() starts every arrival and required time at (INT_MAX, INT_MIN), "no time
// has reached this port yet". The analysis goes on combining those with real times, and with each other: for the half
// of the times that it does not update (the hold half, when only the setup analysis runs), for ports that nothing
// reaches, and for what the sentinels turn into along a combinational loop (nextpnr times one with --force only).
// What comes out is a number without a meaning, but it is what the placers, the routers and the reports have always
// been given, so it is kept. What is required is that computing it is defined, and that is done in one way:
//  - a sum, a difference or a negation of delays is wrap_add(), wrap_sub() or wrap_neg(): modulo 2^N on the unsigned
//    type (a signed overflow is undefined), with the residue mapped back to the signed range explicitly by
//    wrap_to_signed() (converting an out-of-range unsigned value is implementation-defined before C++20);
//  - a delay scaled by a ratio, as the multicycle paths do, is scale_delay(): exact in 64 bits, clamped to the type;
//  - a floating-point value converted to an integer, as the slack histogram does, is saturating_cast(): out of range
//    becomes the smallest or the largest value (a plain conversion of those is undefined).
// A floating-point delay (the generic architecture) has no sentinels to overflow, and is taken as it is.
template <typename T> T wrap_to_signed(std::make_unsigned_t<T> value)
{
    using U = std::make_unsigned_t<T>;
    // The wrapped values only exist if the signed type has as many values as the unsigned one, which is two's
    // complement (the only one that C++20 allows, and the only one in use). Other representations do not compile
    static_assert(std::numeric_limits<T>::min() == -std::numeric_limits<T>::max() - 1,
                  "delay_t must be two's complement");
    constexpr U max = static_cast<U>(std::numeric_limits<T>::max());
    // [0, max] is the same number; [max + 1, 2^N - 1] stands for the negative numbers min .. -1
    if (value <= max)
        return static_cast<T>(value);
    return static_cast<T>(value - max - 1) + std::numeric_limits<T>::min();
}

template <typename T> T wrap_add(T a, T b)
{
    if constexpr (std::is_integral_v<T>) {
        using U = std::make_unsigned_t<T>;
        return wrap_to_signed<T>(static_cast<U>(static_cast<U>(a) + static_cast<U>(b)));
    } else {
        return a + b;
    }
}

template <typename T> T wrap_sub(T a, T b)
{
    if constexpr (std::is_integral_v<T>) {
        using U = std::make_unsigned_t<T>;
        return wrap_to_signed<T>(static_cast<U>(static_cast<U>(a) - static_cast<U>(b)));
    } else {
        return a - b;
    }
}

// The negation, as the unary minus does it: modulo 2^N for an integer, and -a for a floating-point value, where the
// sign of a zero matters (-(+0.0f) is -0.0f, and a division by it is not the same as a division by 0.0f - 0.0f)
template <typename T> T wrap_neg(T a)
{
    if constexpr (std::is_integral_v<T>) {
        return wrap_sub(T(0), a);
    } else {
        return -a;
    }
}

// (value + offset) * numerator / denominator, for scaling a delay by a ratio. Exact in 64 bits and clamped to the
// range of T, where the plain expression overflows for a delay that is a wrapped sentinel. The values of an integer T
// have to be narrower than 64 bits, and the numerator small (|value + offset| * |numerator| has to fit 64 bits).
template <typename T> T scale_delay(T value, T offset, int64_t numerator, int64_t denominator)
{
    if constexpr (std::is_integral_v<T>) {
        static_assert(sizeof(T) < sizeof(int64_t), "scale_delay() computes in 64 bits");
        const int64_t scaled = (int64_t(value) + int64_t(offset)) * numerator / denominator;
        return T(std::clamp<int64_t>(scaled, std::numeric_limits<T>::lowest(), std::numeric_limits<T>::max()));
    } else {
        return (value + offset) * numerator / denominator;
    }
}

// A floating-point value converted to an integer type. Converting a value that is out of the range of the integer type
// is undefined, and the slack that follows from a time that is a wrapped sentinel can be anywhere in the range of
// delay_t and beyond: such a value becomes the smallest or the largest value of the type (NaN the smallest), and any
// other value is truncated, as in a plain conversion. A floating-point target type takes the value as it is.
template <typename T, typename F> T saturating_cast(F value)
{
    if constexpr (std::is_integral_v<T>) {
        if (!(value > static_cast<F>(std::numeric_limits<T>::min())))
            return std::numeric_limits<T>::min();
        if (value >= static_cast<F>(std::numeric_limits<T>::max()))
            return std::numeric_limits<T>::max();
        return static_cast<T>(value);
    } else {
        return static_cast<T>(value);
    }
}

// The slack of a path against a period, as the slack histogram has always taken it: the delay is subtracted from the
// period in float (a delay of more than 2^24 ticks is rounded first), and the result is converted to the delay type,
// to its smallest or largest value where it is out of range.
template <typename T> T slack_from_period(float period, T delay) { return saturating_cast<T>(period - delay); }

NEXTPNR_NAMESPACE_END

#endif
