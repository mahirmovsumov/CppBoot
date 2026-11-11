#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::cli {

/// The CppBoot installation to build with: CPPBOOT_HOME if set, or else the folder that holds the
/// running cppboot's bin folder.
Result<std::filesystem::path> findInstallation();

/// Runs a tool such as cmake in a folder, and says clearly when it isn't on PATH.
Result<int> runTool(std::vector<std::string> command, const std::filesystem::path& directory);

/// Puts the folder in CPPBOOT_TOOLCHAIN first on PATH, for the programs this process starts.
void applyToolchain();

}  // namespace cppboot::cli
