#include "toolchain.hpp"

#include <system_error>
#include <utility>

#include "cppboot/platform/environment.hpp"
#include "cppboot/platform/process.hpp"

namespace cppboot::cli {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kPackageFile = "lib/cmake/CppBoot/CppBootConfig.cmake";

}  // namespace

Result<fs::path> findInstallation() {
    const auto environment = platform::environmentVariables();

    fs::path prefix;
    if (const auto home = environment.find("CPPBOOT_HOME"); home != environment.end()) {
        prefix = home->second;
    } else {
        auto executable = platform::executablePath();
        if (!executable) {
            return executable.error();
        }
        prefix = executable.value().parent_path().parent_path();
    }

    std::error_code ignored;
    if (!fs::exists(prefix / kPackageFile, ignored)) {
        return Error{ErrorCode::NotFound,
                     "CppBoot isn't installed at " + prefix.generic_string() +
                         ". Run the installed cppboot, or set CPPBOOT_HOME to the install folder."};
    }
    return prefix;
}

/// Reports a missing cmake clearly, since that's the most common setup problem.
Result<int> runTool(std::vector<std::string> command, const fs::path& directory) {
    const std::string program = command.front();
    auto exit_code = platform::runProcess({std::move(command), directory});
    if (!exit_code && exit_code.error().code() == ErrorCode::NotFound) {
        return Error{ErrorCode::NotFound, "Cannot find " + program + ". Check that it's on PATH."};
    }
    return exit_code;
}

/// Programs that CMake runs need the toolchain on PATH. On Windows, the installer records MSYS2's
/// folder in CPPBOOT_TOOLCHAIN, rather than adding it to everyone's PATH, where its programs could
/// take the place of others.
void applyToolchain() {
    const auto environment = platform::environmentVariables();
    const auto toolchain = environment.find("CPPBOOT_TOOLCHAIN");
    std::error_code ignored;
    if (toolchain != environment.end() && !toolchain->second.empty() &&
        fs::is_directory(toolchain->second, ignored)) {
        platform::prependToPath(toolchain->second);
    }
}

}  // namespace cppboot::cli
