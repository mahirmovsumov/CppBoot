#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::cli {

/// A checker to build with, from --sanitize=.
enum class Sanitizer { None, Address, Thread };

/// The build folder for a checker, so that switching doesn't rebuild everything.
std::string_view buildFolder(Sanitizer sanitizer);

/// Removes a leading --sanitize=... from the arguments. Returns nothing if its value is unknown.
std::optional<Sanitizer> takeSanitizer(std::vector<std::string>& arguments);

/// Builds the application, or the target given instead, configuring the build folder first if
/// needed. Returns CMake's exit code.
Result<int> buildProject(const std::filesystem::path& directory,
                         Sanitizer sanitizer = Sanitizer::None, const std::string& target = {});

/// Builds the project, then runs it from the project folder. Returns the application's exit code.
Result<int> runProject(const std::filesystem::path& directory, std::vector<std::string> arguments,
                       Sanitizer sanitizer);

/// Builds the tests in tests/ and runs them, passing arguments such as --gtest_filter=Order* on.
Result<int> testProject(const std::filesystem::path& directory,
                        const std::vector<std::string>& arguments, Sanitizer sanitizer);

}  // namespace cppboot::cli
