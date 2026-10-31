#include "cppboot/platform/system_info.hpp"

#include <unistd.h>

#include <cstdio>

namespace cppboot::platform {

std::optional<std::uint64_t> residentMemoryBytes() noexcept {
    std::FILE* const file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return std::nullopt;
    }

    unsigned long long total_pages = 0;
    unsigned long long resident_pages = 0;
    const int fields = std::fscanf(file, "%llu %llu", &total_pages, &resident_pages);
    std::fclose(file);

    const long page_size = sysconf(_SC_PAGESIZE);
    if (fields != 2 || page_size <= 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(resident_pages) * static_cast<std::uint64_t>(page_size);
}

}  // namespace cppboot::platform
