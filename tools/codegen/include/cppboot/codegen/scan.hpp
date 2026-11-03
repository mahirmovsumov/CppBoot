#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/base/result.hpp"
#include "cppboot/codegen/model.hpp"

namespace cppboot::codegen {

/// What one header contains, saved between builds so that an unchanged header isn't parsed again.
struct ScannedHeader {
    /// The header's absolute path, which the generated file includes.
    std::string header;
    Declarations declarations;
};

std::string toJson(const ScannedHeader& scanned);
Result<ScannedHeader> scannedHeaderFromJson(std::string_view json);

/// Keeps the compiler arguments that change what a header declares, such as definitions, include
/// folders and forced includes. A target's options are for GCC or Clang, and libclang rejects the
/// ones it doesn't know, such as some warnings, so everything else is left out.
std::vector<std::string> usableCompilerArguments(const std::vector<std::string>& arguments);

/// Returns a dependency file, in the Makefile form that Ninja and Make read, saying that a target
/// depends on these files.
std::string makeDepfile(const std::filesystem::path& target,
                        const std::vector<std::filesystem::path>& dependencies);

}  // namespace cppboot::codegen
