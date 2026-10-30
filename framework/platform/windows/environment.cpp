#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string_view>

#include "cppboot/platform/environment.hpp"

namespace cppboot::platform {

namespace {

std::string toUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), size, nullptr, nullptr);
    return result;
}

}  // namespace

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
            variables.insert_or_assign(toUtf8(text.substr(0, separator)),
                                       toUtf8(text.substr(separator + 1)));
        }
        entry += text.size() + 1;
    }

    FreeEnvironmentStringsW(block);
    return variables;
}

}  // namespace cppboot::platform
