#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "cppboot/base/result.hpp"
#include "cppboot/codegen/model.hpp"

namespace cppboot::codegen {

/// Finds the components declared in a header. Declarations in the files it includes are skipped.
/// Fails on a compile error or on a marked class the generated code couldn't construct.
Result<std::vector<Component>> parseComponents(const std::filesystem::path& header,
                                               const std::vector<std::string>& compiler_arguments);

}  // namespace cppboot::codegen
