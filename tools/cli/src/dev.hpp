#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::cli {

/// Builds and runs the project, then does it again whenever a watched file changes, until Ctrl+C
/// or q. Reads r and q from in.
Result<int> developProject(const std::filesystem::path& directory,
                           const std::vector<std::string>& arguments, std::istream& in,
                           std::ostream& out);

}  // namespace cppboot::cli
