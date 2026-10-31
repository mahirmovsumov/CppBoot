#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::cli {

inline constexpr int kExitUsage = 2;

/// Accepts names that work as a folder, a CMake target and an executable on every platform.
Result<void> validateProjectName(std::string_view name);

enum class ProjectDatabase { None, Sqlite, Postgresql };

struct ProjectOptions {
    bool web{false};
    ProjectDatabase database{ProjectDatabase::None};
    bool security{false};
    bool actuator{false};
};

/// Creates a new project folder inside parent. Never touches a folder that already exists.
Result<void> createProject(std::string_view name, const std::filesystem::path& parent,
                           const ProjectOptions& options = {});

/// Reads the application name of the project in a directory.
Result<std::string> readProjectName(const std::filesystem::path& directory);

/// Runs a command, where arguments excludes the program name, and returns the exit code.
int run(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err);

}  // namespace cppboot::cli
