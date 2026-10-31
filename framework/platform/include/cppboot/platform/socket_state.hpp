#pragma once

#include <cstdint>

namespace cppboot::platform {

/// Returns true if the other end of a connected socket has closed or reset it, without reading
/// anything and without waiting. Data still to be read means false, even if a close follows it.
bool peerHasClosed(std::uintptr_t socket_handle) noexcept;

}  // namespace cppboot::platform
