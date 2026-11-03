#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace cppboot::platform {

/// Returns the environment variables of the current process, encoded as UTF-8.
std::map<std::string, std::string> environmentVariables();

/// Puts a folder first in this process's PATH, which the programs it starts inherit. Nothing
/// outside this process changes.
void prependToPath(const std::filesystem::path& folder);

}  // namespace cppboot::platform
