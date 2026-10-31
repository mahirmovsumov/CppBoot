#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cppboot::security::detail {

inline std::string base64UrlEncode(std::string_view input) {
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string output;
    output.reserve((input.size() + 2) / 3 * 4);

    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : input) {
        buffer = (buffer << 8) | static_cast<unsigned char>(c);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output += kAlphabet[(buffer >> bits) & 0x3F];
        }
    }
    if (bits > 0) {
        output += kAlphabet[(buffer << (6 - bits)) & 0x3F];
    }
    return output;
}

/// Returns nothing for anything but the one exact encoding of a value, including padding, other
/// characters and unused bits that aren't zero. Otherwise, one token could have several valid
/// spellings.
inline std::optional<std::string> base64UrlDecode(std::string_view input) {
    if (input.size() % 4 == 1) {
        return std::nullopt;
    }

    std::string output;
    output.reserve(input.size() / 4 * 3 + 2);

    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : input) {
        std::uint32_t value = 0;
        if (c >= 'A' && c <= 'Z') {
            value = static_cast<std::uint32_t>(c - 'A');
        } else if (c >= 'a' && c <= 'z') {
            value = static_cast<std::uint32_t>(c - 'a') + 26;
        } else if (c >= '0' && c <= '9') {
            value = static_cast<std::uint32_t>(c - '0') + 52;
        } else if (c == '-') {
            value = 62;
        } else if (c == '_') {
            value = 63;
        } else {
            return std::nullopt;
        }

        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }

    if ((buffer & ((1U << bits) - 1U)) != 0) {
        return std::nullopt;
    }
    return output;
}

}  // namespace cppboot::security::detail
