#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

/// The checks behind CPPBOOT_NOT_EMPTY, CPPBOOT_LENGTH, CPPBOOT_MIN, CPPBOOT_MAX and CPPBOOT_RANGE.
/// Generated code calls them, and they're ordinary functions that other code can call too. Each
/// one passes an empty std::optional, because a value that isn't there has nothing to check.
namespace cppboot::validation {

namespace detail {

template <typename T>
struct IsOptional : std::false_type {};
template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

/// Compares numbers of any two types by value, so that -1 is less than an unsigned 1.
template <typename Left, typename Right>
constexpr bool less(const Left& left, const Right& right) {
    if constexpr (std::is_integral_v<Left> && std::is_integral_v<Right>) {
        return std::cmp_less(left, right);
    } else {
        return static_cast<long double>(left) < static_cast<long double>(right);
    }
}

/// Characters in UTF-8 text, which is what a person would count, rather than bytes.
constexpr std::size_t countCharacters(std::string_view text) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

template <typename T>
constexpr std::size_t lengthOf(const T& value) {
    if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        return countCharacters(value);
    } else {
        return value.size();
    }
}

}  // namespace detail

template <typename T, typename Min>
constexpr bool atLeast(const T& value, const Min& min) {
    if constexpr (detail::IsOptional<T>::value) {
        return !value || atLeast(*value, min);
    } else {
        return !detail::less(value, min);
    }
}

template <typename T, typename Max>
constexpr bool atMost(const T& value, const Max& max) {
    if constexpr (detail::IsOptional<T>::value) {
        return !value || atMost(*value, max);
    } else {
        return !detail::less(max, value);
    }
}

template <typename T, typename Min, typename Max>
constexpr bool inRange(const T& value, const Min& min, const Max& max) {
    return atLeast(value, min) && atMost(value, max);
}

/// For text and containers.
template <typename T>
constexpr bool notEmpty(const T& value) {
    if constexpr (detail::IsOptional<T>::value) {
        return !value || notEmpty(*value);
    } else {
        return !value.empty();
    }
}

/// Counts characters in text, and items in a container.
template <typename T>
constexpr bool lengthInRange(const T& value, std::size_t min, std::size_t max) {
    if constexpr (detail::IsOptional<T>::value) {
        return !value || lengthInRange(*value, min, max);
    } else {
        const std::size_t length = detail::lengthOf(value);
        return length >= min && length <= max;
    }
}

}  // namespace cppboot::validation
