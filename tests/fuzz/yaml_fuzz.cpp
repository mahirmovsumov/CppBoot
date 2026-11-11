// Feeds random bytes to the settings file parser. Any crash, hang, sanitizer report or uncaught
// exception is a bug. Built with -DCPPBOOT_BUILD_FUZZERS=ON and Clang, and run in CI.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "yaml.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    static_cast<void>(cppboot::config::detail::parseYaml(text));
    return 0;
}
