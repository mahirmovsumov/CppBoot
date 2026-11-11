// Feeds random bytes to the JSON parser, which reads request bodies and tokens. Anything that
// parses has to write out as JSON that reads back to the same value.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "cppboot/base/json.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    const auto parsed = cppboot::Json::tryParse(text);
    if (parsed) {
        const auto again = cppboot::Json::tryParse(parsed.value().dump());
        if (!again || !(again.value() == parsed.value())) {
            std::abort();
        }
    }
    return 0;
}
