#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "cppboot/base/result.hpp"
#include "cppboot/codegen/model.hpp"

namespace cppboot::codegen {

/// Returns the folder of Clang's built-in headers: the copy installed with the tool, or the one
/// found at build time when running from a build folder.
std::string clangResourceDirectory();

/// Finds what's marked in a header. Declarations in the files it includes are skipped. Fails on a
/// compile error, or on a marked declaration that generated code couldn't use. If included_files
/// is given, it receives every file the header used, itself included, so that a build can parse it
/// again when any of them changes.
Result<Declarations> parseDeclarations(const std::filesystem::path& header,
                                       const std::vector<std::string>& compiler_arguments,
                                       std::vector<std::filesystem::path>* included_files = nullptr);

}  // namespace cppboot::codegen
