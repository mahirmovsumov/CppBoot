#include "cppboot/platform/system_info.hpp"

#include "text.hpp"

// Must come after windows.h.
#include <psapi.h>

namespace cppboot::platform {

std::optional<std::uint64_t> residentMemoryBytes() noexcept {
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == FALSE) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
}

}  // namespace cppboot::platform
