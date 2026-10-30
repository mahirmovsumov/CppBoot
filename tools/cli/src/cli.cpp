#include "cppboot/cli/cli.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <ostream>
#include <system_error>
#include <utility>

#include "cppboot/base/version.hpp"
#include "cppboot/config/config.hpp"
#include "cppboot/platform/environment.hpp"
#include "cppboot/platform/process.hpp"

namespace cppboot::cli {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUsage = R"(Usage: cppboot <command> [arguments]

Commands:
  create <name>     Create a new project in a new folder
  build             Build the project in the current folder
  run [arguments]   Build and run the project, passing arguments to the application
  info              Show version and installation details
  help              Show this message
)";

constexpr std::string_view kPackageFile = "lib/cmake/CppBoot/CppBootConfig.cmake";

// Names CMake reserves for its own targets, plus device names Windows reserves in any folder.
constexpr std::array<std::string_view, 30> kReservedNames = {
    "all",  "clean", "help", "install", "test", "package", "edit_cache", "rebuild_cache",
    "con",  "prn",   "aux",  "nul",     "com1", "com2",    "com3",       "com4",
    "com5", "com6",  "com7", "com8",    "com9", "lpt1",    "lpt2",       "lpt3",
    "lpt4", "lpt5",  "lpt6", "lpt7",    "lpt8", "lpt9",
};

struct ProjectFile {
    std::string_view path;
    std::string_view content;
};

// "@NAME@" is replaced with the project name.
constexpr std::array<ProjectFile, 6> kProjectFiles = {{
    {"CMakeLists.txt", R"(cmake_minimum_required(VERSION 3.25)

project(@NAME@ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(CppBoot CONFIG REQUIRED)

file(GLOB_RECURSE sources CONFIGURE_DEPENDS src/*.cpp)
file(GLOB_RECURSE headers CONFIGURE_DEPENDS src/*.hpp)

add_executable(@NAME@ ${sources})
target_link_libraries(@NAME@ PRIVATE cppboot::boot)
cppboot_generate_components(@NAME@ HEADERS ${headers})
)"},
    {"cppboot.yaml", R"(cppboot:
  application:
    name: @NAME@

  logging:
    level: info
)"},
    {"src/main.cpp", R"(#include <cppboot/boot.hpp>

#include "cppboot_components.generated.hpp"

int main(int argc, char* argv[]) {
    return cppboot::runApplication(argc, argv, cppboot::generated::registerComponents);
}
)"},
    {"src/greeting.hpp", R"(#pragma once

#include <cppboot/cppboot.hpp>
#include <memory>
#include <string>
#include <utility>

// CPPBOOT_SERVICE makes a class a component. CppBoot reads its constructor, creates it and passes
// in the components it needs, so there's no registration code to write.
class CPPBOOT_SERVICE GreetingService {
public:
    std::string greeting() const { return "Hello from CppBoot!"; }
};

// A component that derives from cppboot::Module also starts and stops with the application.
class CPPBOOT_SERVICE GreetingModule : public cppboot::Module {
public:
    explicit GreetingModule(std::shared_ptr<GreetingService> greetings)
        : greetings_(std::move(greetings)) {}

    std::string_view name() const noexcept override { return "greeting"; }

    cppboot::Result<void> start(cppboot::ApplicationContext& context) override {
        context.logger().info("{}", greetings_->greeting());
        return {};
    }

private:
    std::shared_ptr<GreetingService> greetings_;
};
)"},
    {".gitignore", R"(build/
)"},
    {"README.md", R"(# @NAME@

A CppBoot application.

```text
cppboot build
cppboot run
```

Settings are in `cppboot.yaml`. Press Ctrl+C to stop the application.
)"},
}};

std::string fillTemplate(std::string_view content, std::string_view name) {
    constexpr std::string_view kPlaceholder = "@NAME@";

    std::string result;
    std::size_t position = 0;
    while (true) {
        const auto found = content.find(kPlaceholder, position);
        if (found == std::string_view::npos) {
            result += content.substr(position);
            return result;
        }
        result += content.substr(position, found - position);
        result += name;
        position = found + kPlaceholder.size();
    }
}

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

Result<int> buildProject(const fs::path& directory) {
    if (auto name = readProjectName(directory); !name) {
        return name.error();
    }

    std::error_code ignored;
    if (!fs::exists(directory / "build" / "CMakeCache.txt", ignored)) {
        auto prefix = findInstallation();
        if (!prefix) {
            return prefix.error();
        }

        auto configured = runTool({"cmake", "-S", ".", "-B", "build", "-G", "Ninja",
                                   "-DCMAKE_BUILD_TYPE=Debug",
                                   "-DCMAKE_PREFIX_PATH=" + prefix.value().generic_string()},
                                  directory);
        if (!configured || configured.value() != 0) {
            return configured;
        }
    }

    return runTool({"cmake", "--build", "build"}, directory);
}

Result<int> runProject(const fs::path& directory, std::vector<std::string> arguments) {
    auto built = buildProject(directory);
    if (!built || built.value() != 0) {
        return built;
    }

    auto name = readProjectName(directory);
    if (!name) {
        return name.error();
    }

    std::vector<std::string> command;
    command.push_back(
        (directory / "build" / platform::executableFileName(name.value())).string());
    const auto first = !arguments.empty() && arguments.front() == "--" ? 1 : 0;
    command.insert(command.end(), arguments.begin() + first, arguments.end());

    // The application reads cppboot.yaml from its working directory.
    return platform::runProcess({std::move(command), directory});
}

void printInfo(std::ostream& out) {
    out << "CppBoot " << kVersionString << '\n';

    if (auto executable = platform::executablePath()) {
        out << "Executable: " << executable.value().generic_string() << '\n';
    }

    if (auto prefix = findInstallation()) {
        out << "Installation: " << prefix.value().generic_string() << '\n';
    } else {
        out << "Installation: not found\n";
    }

    if (auto name = readProjectName(fs::current_path())) {
        out << "Project: " << name.value() << '\n';
    } else {
        out << "Project: none in this folder\n";
    }
}

}  // namespace

Result<void> validateProjectName(std::string_view name) {
    const auto is_allowed = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
    };

    if (name.empty() || std::isalpha(static_cast<unsigned char>(name.front())) == 0 ||
        !std::all_of(name.begin(), name.end(), is_allowed)) {
        return Error{ErrorCode::InvalidArgument,
                     "Invalid project name '" + std::string{name} +
                         "'. Use a letter followed by letters, digits, '-' or '_'."};
    }

    std::string lowered{name};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    if (std::find(kReservedNames.begin(), kReservedNames.end(), lowered) != kReservedNames.end()) {
        return Error{ErrorCode::InvalidArgument,
                     "'" + std::string{name} + "' is a reserved name. Choose another one."};
    }
    return {};
}

Result<void> createProject(std::string_view name, const fs::path& parent) {
    if (auto valid = validateProjectName(name); !valid) {
        return valid;
    }

    const fs::path root = parent / name;
    std::error_code error;
    if (fs::exists(root, error)) {
        return Error{ErrorCode::InvalidState, "'" + root.generic_string() + "' already exists"};
    }

    for (const ProjectFile& file : kProjectFiles) {
        const fs::path path = root / file.path;
        fs::create_directories(path.parent_path(), error);
        if (error) {
            return Error{ErrorCode::IoError, "Cannot create folder " +
                                                 path.parent_path().generic_string() + ": " +
                                                 error.message()};
        }

        // Binary mode keeps line endings the same on every platform.
        std::ofstream output{path, std::ios::binary};
        output << fillTemplate(file.content, name);
        output.close();
        if (!output) {
            return Error{ErrorCode::IoError, "Cannot write " + path.generic_string()};
        }
    }
    return {};
}

Result<std::string> readProjectName(const fs::path& directory) {
    std::error_code ignored;
    if (!fs::exists(directory / "cppboot.yaml", ignored) ||
        !fs::exists(directory / "CMakeLists.txt", ignored)) {
        return Error{ErrorCode::NotFound,
                     "No CppBoot project here. Run this from a folder made by 'cppboot create'."};
    }

    config::ConfigSources sources;
    sources.directory = directory;
    auto loaded = config::Config::load(sources);
    if (!loaded) {
        return loaded.error();
    }

    std::string name = loaded.value().getString("cppboot.application.name");
    if (name.empty()) {
        return Error{ErrorCode::NotFound, "cppboot.yaml doesn't set cppboot.application.name"};
    }
    if (auto valid = validateProjectName(name); !valid) {
        return valid.error();
    }
    return name;
}

int run(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err) {
    if (arguments.empty()) {
        err << kUsage;
        return kExitUsage;
    }

    const std::string& command = arguments.front();
    const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());

    Result<int> result{0};
    if (command == "help" || command == "--help" || command == "-h") {
        out << kUsage;
    } else if (command == "info" && rest.empty()) {
        printInfo(out);
    } else if (command == "create" && rest.size() == 1) {
        if (auto created = createProject(rest.front(), fs::current_path()); !created) {
            result = created.error();
        } else {
            out << "Created " << rest.front() << ". Next:\n  cd " << rest.front()
                << "\n  cppboot run\n";
        }
    } else if (command == "build" && rest.empty()) {
        out.flush();
        result = buildProject(fs::current_path());
    } else if (command == "run") {
        out.flush();
        result = runProject(fs::current_path(), rest);
    } else {
        err << kUsage;
        return kExitUsage;
    }

    if (!result) {
        err << "cppboot: " << result.error().message() << '\n';
        return 1;
    }
    return result.value();
}

}  // namespace cppboot::cli
