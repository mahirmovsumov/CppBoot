#pragma once

#include <map>
#include <string>

namespace cppboot::platform {

/// Returns the environment variables of the current process, encoded as UTF-8.
std::map<std::string, std::string> environmentVariables();

}  // namespace cppboot::platform
