#include "cppboot/platform/environment.hpp"

#include <string_view>

#include "text.hpp"

namespace cppboot::platform {

std::map<std::string, std::string> environmentVariables() {
    std::map<std::string, std::string> variables;

    wchar_t* block = GetEnvironmentStringsW();
    if (block == nullptr) {
        return variables;
    }

    for (const wchar_t* entry = block; *entry != L'\0';) {
        const std::wstring_view text{entry};
        // Windows keeps hidden per-drive entries such as "=C:=C:\dir", so skip the first character.
        if (const auto separator = text.find(L'=', 1); separator != std::wstring_view::npos) {
            variables.insert_or_assign(detail::toUtf8(text.substr(0, separator)),
                                       detail::toUtf8(text.substr(separator + 1)));
        }
        entry += text.size() + 1;
    }

    FreeEnvironmentStringsW(block);
    return variables;
}

}  // namespace cppboot::platform
