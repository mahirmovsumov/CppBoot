#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::platform {

struct ProcessOptions {
    /// The program followed by its arguments. A program with no path is searched for on PATH.
    std::vector<std::string> command;
    /// Leave empty to use the current working directory.
    std::filesystem::path working_directory{};
};

/// Runs a process sharing this console, waits for it and returns its exit code.
///
/// Termination requests while it runs are left for the child to handle, so that its exit code is
/// still reported. A child ended by a Linux signal returns 128 plus the signal number. A program
/// that can't be found returns an ErrorCode::NotFound error.
Result<int> runProcess(const ProcessOptions& options);

/// Returns the full path of the running executable.
Result<std::filesystem::path> executablePath();

/// Adds the platform's executable suffix to a program name, such as ".exe" on Windows.
std::string executableFileName(std::string_view name);

}  // namespace cppboot::platform
