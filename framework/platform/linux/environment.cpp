#include <string_view>

#include "cppboot/platform/environment.hpp"

extern char** environ;

namespace cppboot::platform {

std::map<std::string, std::string> environmentVariables() {
    std::map<std::string, std::string> variables;

    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view text{*entry};
        if (const auto separator = text.find('='); separator != std::string_view::npos) {
            variables.insert_or_assign(std::string{text.substr(0, separator)},
                                       std::string{text.substr(separator + 1)});
        }
    }
    return variables;
}

}  // namespace cppboot::platform
