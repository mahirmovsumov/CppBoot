#include <unistd.h>

#include <cstdlib>
#include <string_view>

#include "cppboot/platform/console.hpp"

namespace cppboot::platform {

bool consoleSupportsColor() noexcept {
    if (std::getenv("NO_COLOR") != nullptr || isatty(STDOUT_FILENO) == 0) {
        return false;
    }
    const char* const terminal = std::getenv("TERM");
    return terminal != nullptr && std::string_view{terminal} != "dumb";
}

std::tm localTime(std::time_t time) noexcept {
    std::tm result{};
    localtime_r(&time, &result);
    return result;
}

}  // namespace cppboot::platform
