#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace timers {
inline constexpr std::uint64_t DefaultIdleTimeoutMS = 60 * 1000;
inline constexpr std::size_t ExpirationBudget = 2000;

inline std::uint64_t monotonicMS() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// poll takes a signed int. Clamp distant deadlines instead of wrapping into a
// negative timeout (which would accidentally disable the timer).
inline int pollTimeoutMS(std::optional<std::uint64_t> deadline, std::uint64_t now) {
    if (!deadline) return -1;
    if (*deadline <= now) return 0;
    const auto remaining = *deadline - now;
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    return remaining > maximum ? static_cast<int>(maximum) : static_cast<int>(remaining);
}
} // namespace timers
