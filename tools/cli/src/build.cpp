#include "build.hpp"

#include <system_error>
#include <utility>

#include "cppboot/cli/cli.hpp"
#include "cppboot/platform/process.hpp"
#include "toolchain.hpp"

namespace cppboot::cli {

namespace {

namespace fs = std::filesystem;

std::string_view sanitizerFlags(Sanitizer sanitizer) {
    switch (sanitizer) {
        case Sanitizer::Address:
            return "-fsanitize=address,undefined -fno-sanitize-recover=undefined "
                   "-fno-omit-frame-pointer";
        case Sanitizer::Thread:
            return "-fsanitize=thread -fno-omit-frame-pointer";
        case Sanitizer::None:
            break;
    }
    return "";
}

Result<void> checkSanitizerAvailable(Sanitizer sanitizer) {
#if defined(_WIN32)
    if (sanitizer != Sanitizer::None) {
        return Error{ErrorCode::InvalidArgument,
                     "--sanitize needs Linux, because GCC on Windows doesn't include these checkers"};
    }
#else
    static_cast<void>(sanitizer);
#endif
    return {};
}

}  // namespace

std::string_view buildFolder(Sanitizer sanitizer) {
    switch (sanitizer) {
        case Sanitizer::Address:
            return "build-address";
        case Sanitizer::Thread:
            return "build-thread";
        case Sanitizer::None:
            break;
    }
    return "build";
}

std::optional<Sanitizer> takeSanitizer(std::vector<std::string>& arguments) {
    constexpr std::string_view kOption = "--sanitize=";
    if (arguments.empty() || !arguments.front().starts_with(kOption)) {
        return Sanitizer::None;
    }

    const std::string_view value = std::string_view{arguments.front()}.substr(kOption.size());
    std::optional<Sanitizer> sanitizer;
    if (value == "address") {
        sanitizer = Sanitizer::Address;
    } else if (value == "thread") {
        sanitizer = Sanitizer::Thread;
    }
    arguments.erase(arguments.begin());
    return sanitizer;
}

Result<int> buildProject(const fs::path& directory, Sanitizer sanitizer, const std::string& target,
                         Input input) {
    const bool share_input = input == Input::Shared;
    if (auto available = checkSanitizerAvailable(sanitizer); !available) {
        return available.error();
    }
    if (auto name = readProjectName(directory); !name) {
        return name.error();
    }

    const std::string folder{buildFolder(sanitizer)};
    std::error_code ignored;
    if (!fs::exists(directory / folder / "CMakeCache.txt", ignored)) {
        auto prefix = findInstallation();
        if (!prefix) {
            return prefix.error();
        }

        std::vector<std::string> command{"cmake", "-S", ".", "-B", folder, "-G", "Ninja",
                                         "-DCMAKE_BUILD_TYPE=Debug",
                                         "-DCMAKE_PREFIX_PATH=" + prefix.value().generic_string()};
        if (sanitizer != Sanitizer::None) {
            command.push_back("-DCMAKE_CXX_FLAGS=" + std::string{sanitizerFlags(sanitizer)});
        }

        auto configured = runTool(std::move(command), directory, share_input);
        if (!configured || configured.value() != 0) {
            return configured;
        }
    }

    std::vector<std::string> command{"cmake", "--build", folder};
    if (!target.empty()) {
        command.insert(command.end(), {"--target", target});
    }
    return runTool(std::move(command), directory, share_input);
}

Result<int> testProject(const fs::path& directory, const std::vector<std::string>& arguments,
                        Sanitizer sanitizer) {
    auto name = readProjectName(directory);
    if (!name) {
        return name.error();
    }
    std::error_code ignored;
    if (!fs::exists(directory / "tests", ignored)) {
        return Error{ErrorCode::NotFound,
                     "This project has no tests folder. Projects made with cppboot create have one."};
    }

    const std::string target = name.value() + "-tests";
    auto built = buildProject(directory, sanitizer, target);
    if (!built || built.value() != 0) {
        return built;
    }

    std::vector<std::string> command{
        (directory / buildFolder(sanitizer) / platform::executableFileName(target)).string()};
    command.insert(command.end(), arguments.begin(), arguments.end());
    // From the project folder, as the application runs, so the tests read the same cppboot.yaml.
    return platform::runProcess({std::move(command), directory});
}

Result<int> runProject(const fs::path& directory, std::vector<std::string> arguments,
                       Sanitizer sanitizer) {
    auto built = buildProject(directory, sanitizer);
    if (!built || built.value() != 0) {
        return built;
    }

    auto name = readProjectName(directory);
    if (!name) {
        return name.error();
    }

    std::vector<std::string> command;
    command.push_back((directory / buildFolder(sanitizer) /
                       platform::executableFileName(name.value()))
                          .string());
    const auto first = !arguments.empty() && arguments.front() == "--" ? 1 : 0;
    command.insert(command.end(), arguments.begin() + first, arguments.end());

    // The application reads cppboot.yaml from its working directory.
    return platform::runProcess({std::move(command), directory});
}

}  // namespace cppboot::cli
