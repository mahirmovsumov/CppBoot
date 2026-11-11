#pragma once

#include <ctime>

namespace cppboot::platform {

/// Whether standard output is a terminal that shows ANSI colors. On Windows, this also turns them
/// on for the console. Setting NO_COLOR, to any value, turns them off.
bool consoleSupportsColor() noexcept;

/// The local calendar time of a moment, which the C library's localtime can't give safely from
/// several threads.
std::tm localTime(std::time_t time) noexcept;

}  // namespace cppboot::platform
