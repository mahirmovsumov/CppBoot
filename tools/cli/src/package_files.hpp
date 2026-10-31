#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "cppboot/base/result.hpp"

namespace cppboot::cli {

/// Returns the account name for a service: lower case, because Linux tools expect that. Fails if
/// it's too long for an account.
Result<std::string> serviceAccountName(std::string_view project_name);

/// Writes the README for a package, and on Linux the systemd unit, the sample settings and the
/// scripts that install and remove it.
Result<void> writePackageFiles(const std::filesystem::path& stage, std::string_view project_name,
                               bool for_linux);

/// A CMake script that copies an executable's shared libraries, leaving out the ones every system
/// has. It reads EXE and LIB_DIR.
std::string_view runtimeLibrariesScript();

}  // namespace cppboot::cli
