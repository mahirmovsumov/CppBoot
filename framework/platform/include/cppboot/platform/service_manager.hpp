#pragma once

#include <string_view>

namespace cppboot::platform {

/// Tells systemd about a change of state, such as "READY=1". This does nothing unless systemd started
/// the process and asked to be told, and nothing on Windows. A failure is ignored, because the
/// application can't do anything useful about it.
void notifyServiceManager(std::string_view state) noexcept;

}  // namespace cppboot::platform
