#pragma once

#include <cstdint>
#include <stdexcept>

namespace huffblock::detail {

inline std::uint64_t checked_output_total(std::uint64_t total, std::uint64_t addition,
                                         std::uint64_t limit) {
    if (total > limit || addition > limit - total) {
        throw std::runtime_error("decoded output exceeds --max-output limit");
    }
    return total + addition;
}

} // namespace huffblock::detail
