#pragma once

#include <filesystem>

#include "cppboot/base/result.hpp"

namespace cppboot::cli {

/// Builds a release into build-release/, then puts it with everything it needs into dist/, and
/// returns the archive's path.
Result<std::filesystem::path> packageProject(const std::filesystem::path& directory);

}  // namespace cppboot::cli
