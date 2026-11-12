#include "cppboot/config/binding.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <system_error>

#include "cppboot/base/exceptions.hpp"

namespace cppboot::config {

namespace {

std::string lowercase(std::string_view text) {
    std::string result{text};
    std::transform(result.begin(), result.end(), result.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return result;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

template <typename T>
std::optional<T> parseWhole(std::string_view text) {
    T value{};
    const char* const end = text.data() + text.size();
    const auto [parsed_end, error] = std::from_chars(text.data(), end, value);
    if (text.empty() || error != std::errc{} || parsed_end != end) {
        return std::nullopt;
    }
    return value;
}

/// How many single-character changes turn one word into the other.
std::size_t editDistance(std::string_view left, std::string_view right) {
    std::vector<std::size_t> previous(right.size() + 1);
    std::vector<std::size_t> current(right.size() + 1);
    for (std::size_t j = 0; j <= right.size(); ++j) {
        previous[j] = j;
    }
    for (std::size_t i = 1; i <= left.size(); ++i) {
        current[0] = i;
        for (std::size_t j = 1; j <= right.size(); ++j) {
            const std::size_t change = previous[j - 1] + (left[i - 1] == right[j - 1] ? 0 : 1);
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, change});
        }
        std::swap(previous, current);
    }
    return previous[right.size()];
}

struct DurationUnit {
    std::string_view suffix;
    std::int64_t nanoseconds;
};

// Longer suffixes first, so that "ms" isn't read as "s".
constexpr std::array<DurationUnit, 7> kDurationUnits = {{
    {"ns", 1},
    {"us", 1'000},
    {"ms", 1'000'000},
    {"s", 1'000'000'000},
    {"m", 60LL * 1'000'000'000},
    {"h", 3600LL * 1'000'000'000},
    {"d", 86400LL * 1'000'000'000},
}};

}  // namespace

namespace detail {

std::optional<bool> parseBool(std::string_view text) {
    const std::string lowered = lowercase(text);
    if (lowered == "true" || lowered == "yes" || lowered == "on" || lowered == "1") {
        return true;
    }
    if (lowered == "false" || lowered == "no" || lowered == "off" || lowered == "0") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::int64_t> parseSigned(std::string_view text) {
    return parseWhole<std::int64_t>(text);
}

std::optional<std::uint64_t> parseUnsigned(std::string_view text) {
    return parseWhole<std::uint64_t>(text);
}

std::optional<double> parseDecimal(std::string_view text) {
    double value = 0;
    const char* const end = text.data() + text.size();
    const auto [parsed_end, error] = std::from_chars(text.data(), end, value);
    if (text.empty() || error != std::errc{} || parsed_end != end || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::chrono::nanoseconds> parseDuration(std::string_view text) {
    for (const DurationUnit& unit : kDurationUnits) {
        if (!text.ends_with(unit.suffix)) {
            continue;
        }
        const auto count = parseWhole<std::int64_t>(text.substr(0, text.size() - unit.suffix.size()));
        if (!count) {
            return std::nullopt;
        }
        if (*count < 0 || *count > std::numeric_limits<std::int64_t>::max() / unit.nanoseconds) {
            return std::nullopt;
        }
        return std::chrono::nanoseconds{*count * unit.nanoseconds};
    }
    return std::nullopt;
}

}  // namespace detail

SettingsBinder::SettingsBinder(const Config& config, std::string prefix, std::string type_name)
    : config_(config), prefix_(std::move(prefix)), type_name_(std::move(type_name)) {}

std::string SettingsBinder::fullKey(std::string_view key) const {
    return prefix_ + "." + std::string{key};
}

std::optional<std::string> SettingsBinder::find(std::string_view key) const {
    return config_.find(fullKey(key));
}

std::optional<std::vector<SettingsBinder::ListItem>> SettingsBinder::listItems(
    std::string_view key) const {
    std::vector<ListItem> items;
    for (std::size_t index = 0;; ++index) {
        std::string item_key = std::string{key} + "." + std::to_string(index);
        auto text = find(item_key);
        if (!text) {
            break;
        }
        items.push_back(ListItem{std::move(item_key), *std::move(text)});
    }
    if (!items.empty()) {
        return items;
    }

    // One value with commas, which is all an environment variable or an argument can give.
    const auto joined = find(key);
    if (!joined) {
        return std::nullopt;
    }
    std::string_view rest = trim(*joined);
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        items.push_back(ListItem{std::string{key}, std::string{trim(rest.substr(0, comma))}});
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    return items;
}

void SettingsBinder::addKnownKey(std::string_view key) {
    known_keys_.emplace_back(key);
}

void SettingsBinder::addKnownGroup(std::string_view key) {
    known_groups_.push_back(std::string{key} + ".");
}

void SettingsBinder::ignoreGroup(std::string_view group) {
    ignored_groups_.push_back(std::string{group} + ".");
}

bool SettingsBinder::hasProblem(std::string_view key) const {
    return std::any_of(problems_.begin(), problems_.end(),
                       [&](const Problem& problem) { return problem.key == key; });
}

void SettingsBinder::wrongValue(std::string_view key, std::string_view text,
                                std::string_view expected) {
    std::string message = "must be " + std::string{expected};
    // What was given helps to find the mistake, but a secret must not reach a log.
    if (!isSensitiveKey(fullKey(key))) {
        message += ", but is '" + std::string{text} + "'";
    }
    problems_.push_back(Problem{std::string{key}, std::move(message)});
}

void SettingsBinder::require(std::string_view key) {
    if (hasProblem(key)) {
        return;
    }
    const bool is_list = std::find(known_groups_.begin(), known_groups_.end(),
                                   std::string{key} + ".") != known_groups_.end();
    const bool is_set = is_list ? listItems(key).has_value() : find(key).has_value();
    if (!is_set) {
        problems_.push_back(Problem{
            std::string{key}, "must be set, in a settings file or with the environment variable " +
                                  environmentVariableFor(fullKey(key))});
    }
}

void SettingsBinder::check(std::string_view key, bool passed, std::string_view message) {
    if (!passed && !hasProblem(key)) {
        problems_.push_back(Problem{std::string{key}, std::string{message}});
    }
}

void SettingsBinder::finish() const {
    std::vector<Problem> problems = problems_;

    const std::string group = prefix_ + ".";
    for (const std::string& full_key : config_.keysStartingWith(group)) {
        const std::string_view key = std::string_view{full_key}.substr(group.size());
        const auto starts_key = [&](const std::string& start) { return key.starts_with(start); };
        if (std::find(known_keys_.begin(), known_keys_.end(), key) != known_keys_.end() ||
            std::any_of(known_groups_.begin(), known_groups_.end(), starts_key) ||
            std::any_of(ignored_groups_.begin(), ignored_groups_.end(), starts_key)) {
            continue;
        }

        std::string message = "isn't a setting that " + type_name_ + " reads";
        const auto closest = std::min_element(
            known_keys_.begin(), known_keys_.end(), [&](const std::string& left, const std::string& right) {
                return editDistance(key, left) < editDistance(key, right);
            });
        if (closest != known_keys_.end() && editDistance(key, *closest) <= 2 &&
            editDistance(key, *closest) < closest->size()) {
            message += ". Did you mean " + fullKey(*closest) + "?";
        }
        problems.push_back(Problem{std::string{key}, std::move(message)});
    }

    if (problems.empty()) {
        return;
    }
    std::string text = "Invalid settings for " + type_name_ + ":";
    for (const Problem& problem : problems) {
        text += "\n  " + fullKey(problem.key) + ": " + problem.message;
    }
    throw Exception(Error{ErrorCode::InvalidConfiguration, std::move(text)});
}

}  // namespace cppboot::config
