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

void prependToPath(const std::filesystem::path& folder) {
    const DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring current(size, L'\0');
    if (size > 0) {
        current.resize(GetEnvironmentVariableW(L"PATH", current.data(), size));
    }
    const std::wstring updated = folder.wstring() + (current.empty() ? L"" : L";" + current);
    SetEnvironmentVariableW(L"PATH", updated.c_str());
}

}  // namespace cppboot::platform
