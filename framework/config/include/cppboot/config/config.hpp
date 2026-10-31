#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::config {

/// The inputs to Config::load, listed from lowest to highest precedence.
struct ConfigSources {
    std::map<std::string, std::string> defaults;
    /// Holds cppboot.yaml and cppboot-<profile>.yaml.
    std::filesystem::path directory{"."};
    /// Only variables starting with CPPBOOT_ are read.
    std::map<std::string, std::string> environment;
    /// Reads --cppboot.some.key=value, and --other.key=value if other.key is already set.
    /// Any other argument is left for the application.
    std::vector<std::string> arguments;
};

/// Immutable application configuration, stored as flat keys such as "cppboot.server.port".
class Config {
public:
    using Values = std::map<std::string, std::string, std::less<>>;

    static Result<Config> load(const ConfigSources& sources);

    bool contains(std::string_view key) const;
    std::optional<std::string> find(std::string_view key) const;

    std::string getString(std::string_view key, std::string_view fallback = {}) const;
    Result<std::int64_t> getInt(std::string_view key, std::int64_t fallback) const;
    Result<bool> getBool(std::string_view key, bool fallback) const;

    /// The active profile, or an empty string when no profile is selected.
    const std::string& profile() const noexcept { return profile_; }

    /// Returns every setting, one per line, with the values of sensitive keys hidden.
    std::string describe() const;

private:
    Config(Values values, Values environment, std::string profile);

    /// Returns a setting's value, or nothing. A key that wasn't otherwise set can still come from
    /// its environment variable, which has no way to spell the difference between . and _ in a key.
    const std::string* lookup(std::string_view key) const;

    Values values_;
    Values environment_;
    std::string profile_;
};

}  // namespace cppboot::config
