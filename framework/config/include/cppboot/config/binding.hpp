#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "cppboot/config/config.hpp"

namespace cppboot::config {

namespace detail {

template <typename T>
struct IsOptional : std::false_type {};
template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

template <typename T>
struct IsVector : std::false_type {};
template <typename T>
struct IsVector<std::vector<T>> : std::true_type {};

template <typename T>
struct IsDuration : std::false_type {};
template <typename Rep, typename Period>
struct IsDuration<std::chrono::duration<Rep, Period>> : std::true_type {};

std::optional<bool> parseBool(std::string_view text);
std::optional<std::int64_t> parseSigned(std::string_view text);
std::optional<std::uint64_t> parseUnsigned(std::string_view text);
/// A number that isn't infinite or NaN.
std::optional<double> parseDecimal(std::string_view text);
/// A whole number and a unit, such as 500ms, 30s, 5m, 2h or 1d.
std::optional<std::chrono::nanoseconds> parseDuration(std::string_view text);

}  // namespace detail

/// Fills the fields of a struct from the settings under a prefix, and collects everything that's
/// wrong, so that one run reports all of it. Generated code uses this for CPPBOOT_CONFIG structs:
///
///   SettingsBinder binder{config, "mail", "MailSettings"};
///   binder.bind("host", settings.host);
///   binder.bind("port", settings.port);
///   binder.finish();
///
/// Fields can be bool, numbers, std::string, std::chrono durations, and std::optional or
/// std::vector of those. A list is read from mail.hosts.0, mail.hosts.1 and so on, as a YAML list
/// gives, or from one value with commas, which is how an environment variable can set it.
class SettingsBinder {
public:
    /// The type's name is only used in messages.
    SettingsBinder(const Config& config, std::string prefix, std::string type_name);

    /// Sets a value from its setting. If the setting isn't set, the value stays as it is. If it
    /// can't be read as a T, the value also stays, and finish() reports it.
    template <typename T>
    void bind(std::string_view key, T& value);

    /// Reports a setting that isn't set. Call it after bind() for the same key.
    void require(std::string_view key);

    /// Reports a setting that failed a check, unless that setting already has a problem.
    void check(std::string_view key, bool passed, std::string_view message);

    /// Leaves the settings under prefix.group to another struct, which reads them.
    void ignoreGroup(std::string_view group);

    /// Throws cppboot::Exception with ErrorCode::InvalidConfiguration if anything was reported, or
    /// if a settings file, default or argument sets a key under the prefix that no field reads.
    /// That's usually a spelling mistake, which would otherwise leave a setting quietly unused.
    void finish() const;

private:
    struct Problem {
        std::string key;
        std::string message;
    };
    struct ListItem {
        /// Relative to the prefix, such as hosts.0.
        std::string key;
        std::string text;
    };

    std::string fullKey(std::string_view key) const;
    std::optional<std::string> find(std::string_view key) const;
    /// The items of a list, or nothing if it isn't set.
    std::optional<std::vector<ListItem>> listItems(std::string_view key) const;
    void addKnownKey(std::string_view key);
    void addKnownGroup(std::string_view key);
    bool hasProblem(std::string_view key) const;
    /// Reports "must be <expected>, but is '<text>'", leaving the text out for a sensitive key.
    void wrongValue(std::string_view key, std::string_view text, std::string_view expected);

    template <typename T>
    bool convert(std::string_view key, std::string_view text, T& value);

    const Config& config_;
    std::string prefix_;
    std::string type_name_;
    std::vector<std::string> known_keys_;
    std::vector<std::string> known_groups_;
    std::vector<std::string> ignored_groups_;
    std::vector<Problem> problems_;
};

template <typename T>
void SettingsBinder::bind(std::string_view key, T& value) {
    if constexpr (detail::IsVector<T>::value) {
        addKnownKey(key);
        addKnownGroup(key);
        const auto items = listItems(key);
        if (!items) {
            return;
        }
        T list;
        for (const ListItem& item : *items) {
            typename T::value_type converted{};
            if (!convert(item.key, item.text, converted)) {
                return;
            }
            list.push_back(std::move(converted));
        }
        value = std::move(list);
    } else {
        addKnownKey(key);
        if (const auto text = find(key)) {
            convert(key, *text, value);
        }
    }
}

template <typename T>
bool SettingsBinder::convert(std::string_view key, std::string_view text, T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        const auto parsed = detail::parseBool(text);
        if (!parsed) {
            wrongValue(key, text, "true or false");
            return false;
        }
        value = *parsed;
    } else if constexpr (std::is_integral_v<T>) {
        const std::string range = "a whole number from " +
                                  std::to_string(std::numeric_limits<T>::min()) + " to " +
                                  std::to_string(std::numeric_limits<T>::max());
        if constexpr (std::is_signed_v<T>) {
            const auto parsed = detail::parseSigned(text);
            if (!parsed || *parsed < std::numeric_limits<T>::min() ||
                *parsed > std::numeric_limits<T>::max()) {
                wrongValue(key, text, range);
                return false;
            }
            value = static_cast<T>(*parsed);
        } else {
            const auto parsed = detail::parseUnsigned(text);
            if (!parsed || *parsed > std::numeric_limits<T>::max()) {
                wrongValue(key, text, range);
                return false;
            }
            value = static_cast<T>(*parsed);
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        const auto parsed = detail::parseDecimal(text);
        if (!parsed) {
            wrongValue(key, text, "a number");
            return false;
        }
        value = static_cast<T>(*parsed);
    } else if constexpr (std::is_same_v<T, std::string>) {
        value = std::string{text};
    } else if constexpr (detail::IsOptional<T>::value) {
        typename T::value_type inner{};
        if (!convert(key, text, inner)) {
            return false;
        }
        value = std::move(inner);
    } else if constexpr (detail::IsDuration<T>::value) {
        const auto parsed = detail::parseDuration(text);
        if (!parsed) {
            wrongValue(key, text, "a time with its unit, such as 500ms, 30s, 5m or 2h");
            return false;
        }
        const T converted = std::chrono::duration_cast<T>(*parsed);
        if (std::chrono::duration_cast<std::chrono::nanoseconds>(converted) != *parsed) {
            wrongValue(key, text, "a time in the unit that this setting counts in");
            return false;
        }
        value = converted;
    } else {
        static_assert(!sizeof(T), "A setting must be a bool, a number, a std::string, a "
                                  "std::chrono duration, or a std::optional or std::vector of those");
    }
    return true;
}

}  // namespace cppboot::config
