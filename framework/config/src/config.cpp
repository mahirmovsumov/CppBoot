#include "cppboot/config/config.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <system_error>
#include <utility>

namespace cppboot::config {

namespace {

constexpr std::string_view kEnvironmentPrefix = "CPPBOOT_";
constexpr std::string_view kArgumentPrefix = "--cppboot.";
constexpr std::string_view kProfileKey = "cppboot.profile";
constexpr std::string_view kProfileVariable = "CPPBOOT_PROFILE";

// A database URL can hold a password, so it counts as sensitive too.
constexpr std::array<std::string_view, 7> kSensitiveWords = {
    "password", "secret", "token", "credential", "private_key", "api_key", "database.url",
};

char toUpper(char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

char toLower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string lowercase(std::string_view text) {
    std::string result{text};
    std::transform(result.begin(), result.end(), result.begin(), toLower);
    return result;
}

void flatten(const YAML::Node& node, const std::string& prefix, Config::Values& out) {
    switch (node.Type()) {
        case YAML::NodeType::Map:
            for (const auto& entry : node) {
                const auto key = entry.first.as<std::string>();
                flatten(entry.second, prefix.empty() ? key : prefix + "." + key, out);
            }
            break;
        case YAML::NodeType::Sequence: {
            std::size_t index = 0;
            for (const auto& entry : node) {
                flatten(entry, prefix + "." + std::to_string(index), out);
                ++index;
            }
            break;
        }
        case YAML::NodeType::Scalar:
            out.insert_or_assign(prefix, node.Scalar());
            break;
        case YAML::NodeType::Null:
        case YAML::NodeType::Undefined:
            break;
    }
}

Result<void> mergeYamlFile(const std::filesystem::path& path, Config::Values& values) {
    std::ifstream input{path};
    if (!input) {
        return Error{ErrorCode::IoError, "Cannot open configuration file " + path.generic_string()};
    }

    try {
        const YAML::Node root = YAML::Load(input);
        if (!root.IsNull() && !root.IsMap()) {
            return Error{ErrorCode::ParseError,
                         "Configuration file must contain a map: " + path.generic_string()};
        }
        flatten(root, {}, values);
    } catch (const YAML::Exception& exception) {
        return Error{ErrorCode::ParseError,
                     "Invalid YAML in " + path.generic_string() + ": " + exception.what()};
    }
    return {};
}

Result<Config::Values> parseArguments(const std::vector<std::string>& arguments) {
    Config::Values values;
    for (const std::string& argument : arguments) {
        if (!argument.starts_with(kArgumentPrefix)) {
            continue;
        }

        const auto separator = argument.find('=');
        if (separator == std::string::npos) {
            return Error{ErrorCode::InvalidArgument,
                         "Configuration argument needs a value: " + argument};
        }
        values.insert_or_assign(argument.substr(2, separator - 2), argument.substr(separator + 1));
    }
    return values;
}

/// Overrides keys outside the cppboot namespace, such as --myapp.timeout=5. Only keys that already
/// exist are matched, so that other arguments stay available to the application.
void applyKnownArguments(const std::vector<std::string>& arguments, Config::Values& values) {
    for (const std::string& argument : arguments) {
        const auto separator = argument.find('=');
        if (!argument.starts_with("--") || separator == std::string::npos) {
            continue;
        }

        const auto entry = values.find(std::string_view{argument}.substr(2, separator - 2));
        if (entry != values.end()) {
            entry->second = argument.substr(separator + 1);
        }
    }
}

/// Returns the variable that sets a key: cppboot.server.max_connections and myapp.timeout are set by
/// CPPBOOT_SERVER_MAX_CONNECTIONS and CPPBOOT_MYAPP_TIMEOUT.
std::string environmentVariableFor(std::string_view key) {
    std::string variable{key};
    std::transform(variable.begin(), variable.end(), variable.begin(), [](char c) {
        return (c == '.' || c == '-') ? '_' : toUpper(c);
    });
    if (!variable.starts_with(kEnvironmentPrefix)) {
        variable.insert(0, kEnvironmentPrefix);
    }
    return variable;
}

/// Applies each variable to the key it matches. A variable matching no key yet is also stored with
/// each underscore as a dot, for code that lists settings, and Config::lookup() finds any other
/// spelling when a key is read.
Result<void> applyEnvironment(const std::map<std::string, std::string>& environment,
                              Config::Values& values) {
    std::map<std::string, std::vector<std::string>> known_keys;
    for (const auto& [key, value] : values) {
        known_keys[environmentVariableFor(key)].push_back(key);
    }

    Config::Values overrides;
    for (const auto& [variable, value] : environment) {
        if (!variable.starts_with(kEnvironmentPrefix)) {
            continue;
        }

        const auto known = known_keys.find(variable);
        if (known == known_keys.end()) {
            std::string key = lowercase(variable);
            std::replace(key.begin(), key.end(), '_', '.');
            overrides.insert_or_assign(std::move(key), value);
        } else if (known->second.size() == 1) {
            overrides.insert_or_assign(known->second.front(), value);
        } else {
            return Error{ErrorCode::InvalidArgument,
                         "Environment variable " + variable + " matches more than one key"};
        }
    }

    for (auto& [key, value] : overrides) {
        values.insert_or_assign(key, std::move(value));
    }
    return {};
}

bool isValidProfile(std::string_view profile) {
    return std::all_of(profile.begin(), profile.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
    });
}

}  // namespace

Config::Config(Values values, Values environment, std::string profile)
    : values_(std::move(values)),
      environment_(std::move(environment)),
      profile_(std::move(profile)) {}

const std::string* Config::lookup(std::string_view key) const {
    if (const auto entry = values_.find(key); entry != values_.end()) {
        return &entry->second;
    }

    const auto variable = environment_.find(environmentVariableFor(key));
    return variable == environment_.end() ? nullptr : &variable->second;
}

Result<Config> Config::load(const ConfigSources& sources) {
    Values values;
    for (const auto& [key, value] : sources.defaults) {
        values.insert_or_assign(key, value);
    }

    const auto base_file = sources.directory / "cppboot.yaml";
    std::error_code ignored;
    if (std::filesystem::exists(base_file, ignored)) {
        if (auto merged = mergeYamlFile(base_file, values); !merged) {
            return merged.error();
        }
    }

    auto arguments = parseArguments(sources.arguments);
    if (!arguments) {
        return arguments.error();
    }

    std::string profile;
    if (const auto argument = arguments.value().find(kProfileKey);
        argument != arguments.value().end()) {
        profile = argument->second;
    } else if (const auto variable = sources.environment.find(std::string{kProfileVariable});
               variable != sources.environment.end()) {
        profile = variable->second;
    } else if (const auto value = values.find(kProfileKey); value != values.end()) {
        profile = value->second;
    }

    if (!isValidProfile(profile)) {
        return Error{ErrorCode::InvalidArgument, "Invalid profile name: " + profile};
    }

    if (!profile.empty()) {
        // A selected profile must exist. Running without it could silently use the wrong settings.
        const auto profile_file = sources.directory / ("cppboot-" + profile + ".yaml");
        if (auto merged = mergeYamlFile(profile_file, values); !merged) {
            return merged.error();
        }
    }

    if (auto applied = applyEnvironment(sources.environment, values); !applied) {
        return applied.error();
    }

    for (auto& [key, value] : arguments.value()) {
        values.insert_or_assign(key, value);
    }
    applyKnownArguments(sources.arguments, values);

    Values environment;
    for (const auto& [variable, value] : sources.environment) {
        if (variable.starts_with(kEnvironmentPrefix)) {
            environment.insert_or_assign(variable, value);
        }
    }

    return Config{std::move(values), std::move(environment), std::move(profile)};
}

bool Config::contains(std::string_view key) const {
    return lookup(key) != nullptr;
}

std::optional<std::string> Config::find(std::string_view key) const {
    const std::string* const value = lookup(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    return *value;
}

std::string Config::getString(std::string_view key, std::string_view fallback) const {
    const std::string* const value = lookup(key);
    return value == nullptr ? std::string{fallback} : *value;
}

Result<std::int64_t> Config::getInt(std::string_view key, std::int64_t fallback) const {
    const std::string* const found = lookup(key);
    if (found == nullptr) {
        return fallback;
    }

    const std::string& text = *found;
    std::int64_t value = 0;
    const char* const end = text.data() + text.size();
    const auto [parsed_end, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || parsed_end != end) {
        return Error{ErrorCode::ParseError,
                     "Setting " + std::string{key} + " is not an integer: " + text};
    }
    return value;
}

Result<bool> Config::getBool(std::string_view key, bool fallback) const {
    const std::string* const found = lookup(key);
    if (found == nullptr) {
        return fallback;
    }

    const std::string text = lowercase(*found);
    if (text == "true" || text == "yes" || text == "on" || text == "1") {
        return true;
    }
    if (text == "false" || text == "no" || text == "off" || text == "0") {
        return false;
    }
    return Error{ErrorCode::ParseError,
                 "Setting " + std::string{key} + " is not a boolean: " + *found};
}

std::string Config::describe() const {
    std::string text;
    for (const auto& [key, value] : values_) {
        const std::string lowered = lowercase(key);
        const bool sensitive =
            std::any_of(kSensitiveWords.begin(), kSensitiveWords.end(), [&](std::string_view word) {
                return lowered.find(word) != std::string::npos;
            });

        text += key;
        text += '=';
        text += sensitive ? std::string{"******"} : value;
        text += '\n';
    }
    return text;
}

}  // namespace cppboot::config
