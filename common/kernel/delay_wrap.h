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

#include <limits>
#include <type_traits>

#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

// Sums and differences of delays that wrap around instead of overflowing.
//
// The timing analysis starts every arrival and required time at (INT_MAX, INT_MIN), "no time has reached this port
// yet", and goes on adding delays to those, subtracting them from each other and negating the difference: for the
// half of the times that the analysis does not update (the hold half, when only the setup analysis runs), for ports
// that nothing reaches, and for what the sentinels turn into along a combinational loop. The wrapped value is what the
// analysis has always used for these entries, and what the placers and routers read, so it is kept; but it is computed
// without undefined or implementation-defined behaviour: the operation is done modulo 2^N on the unsigned type (a
// signed overflow is undefined), and the residue is mapped back to the signed range explicitly (converting an
// out-of-range unsigned value is implementation-defined before C++20). A floating-point delay has no sentinels to
// overflow, and is taken as it is.
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

NEXTPNR_NAMESPACE_END

#endif
