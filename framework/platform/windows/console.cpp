#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>

#include "cppboot/platform/console.hpp"

namespace cppboot::platform {

bool consoleSupportsColor() noexcept {
    if (std::getenv("NO_COLOR") != nullptr) {
        return false;
    }
    // Fails for a pipe or a file, which shouldn't get color codes.
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output == INVALID_HANDLE_VALUE || GetConsoleMode(output, &mode) == FALSE) {
        return false;
    }
    return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 ||
           SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != FALSE;
}

std::tm localTime(std::time_t time) noexcept {
    std::tm result{};
    localtime_s(&result, &time);
    return result;
}

}  // namespace cppboot::platform
