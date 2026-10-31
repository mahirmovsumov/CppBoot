#pragma once

#include <cstdint>
#include <optional>

namespace cppboot::platform {

/// Returns the physical memory this process is using, or nothing if the system won't say.
std::optional<std::uint64_t> residentMemoryBytes() noexcept;

}  // namespace cppboot::platform
