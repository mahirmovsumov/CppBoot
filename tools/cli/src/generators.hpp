#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::cli {

/// The kinds of class that cppboot add can create, in the order the help lists them.
std::vector<std::string_view> componentKinds();

/// Writes a new marked class into the project's src folder, and for a repository a migration too.
/// Returns the files it wrote, relative to the project. Fails without writing anything if the name
/// isn't a valid class name, a file already exists, or the project lacks what the kind needs, such
/// as a database for a repository.
Result<std::vector<std::filesystem::path>> addComponent(const std::filesystem::path& project,
                                                        std::string_view kind,
                                                        std::string_view name);

}  // namespace cppboot::cli
