#include "package.hpp"

#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "cppboot/cli/cli.hpp"
#include "cppboot/platform/process.hpp"
#include "package_files.hpp"
#include "toolchain.hpp"

namespace cppboot::cli {

namespace fs = std::filesystem;

Result<fs::path> packageProject(const fs::path& directory) {
    auto name = readProjectName(directory);
    if (!name) {
        return name.error();
    }
    auto prefix = findInstallation();
    if (!prefix) {
        return prefix.error();
    }

#if defined(_WIN32)
    constexpr bool kForLinux = false;
    constexpr std::string_view kLibraryFolder = "bin";
    const std::string archive = name.value() + "-windows.zip";
    const std::vector<std::string> archive_options{"--format=zip"};
    const std::string linker_flags = "-s";
#else
    constexpr bool kForLinux = true;
    constexpr std::string_view kLibraryFolder = "lib";
    const std::string archive = name.value() + "-linux.tar.gz";
    const std::vector<std::string> archive_options{};
    // RUNPATH wouldn't apply to the libraries' own dependencies, and RPATH does.
    const std::string linker_flags = "-s -Wl,--disable-new-dtags";
#endif

    const std::string folder = "build-release";
    std::error_code error;
    if (!fs::exists(directory / folder / "CMakeCache.txt", error)) {
        auto configured = runTool({"cmake", "-S", ".", "-B", folder, "-G", "Ninja",
                                   "-DCMAKE_BUILD_TYPE=Release",
                                   "-DCMAKE_PREFIX_PATH=" + prefix.value().generic_string(),
                                   "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON",
                                   "-DCMAKE_INSTALL_RPATH=$ORIGIN/../lib",
                                   "-DCMAKE_EXE_LINKER_FLAGS=" + linker_flags},
                                  directory);
        if (!configured) {
            return configured.error();
        }
        if (configured.value() != 0) {
            return Error{ErrorCode::IoError, "Configuring the release build failed"};
        }
    }

    auto built = runTool({"cmake", "--build", folder}, directory);
    if (!built) {
        return built.error();
    }
    if (built.value() != 0) {
        return Error{ErrorCode::IoError, "The release build failed"};
    }

    // Anything left in the folder from an earlier package must not be shipped in this one.
    const fs::path dist = directory / "dist";
    const fs::path stage = dist / name.value();
    fs::remove_all(stage, error);
    fs::remove(dist / archive, error);
    fs::create_directories(stage / "bin", error);
    if (error) {
        return Error{ErrorCode::IoError,
                     "Cannot create " + stage.generic_string() + ": " + error.message()};
    }

    const std::string executable_name = platform::executableFileName(name.value());
    const fs::path executable = directory / folder / executable_name;
    fs::copy_file(executable, stage / "bin" / executable_name, error);
    if (error) {
        return Error{ErrorCode::IoError,
                     "Cannot copy " + executable.generic_string() + ": " + error.message()};
    }

    for (const fs::directory_entry& entry : fs::directory_iterator{directory, error}) {
        const std::string file = entry.path().filename().string();
        if (entry.is_regular_file() && entry.path().extension() == ".yaml" &&
            (file == "cppboot.yaml" || file.starts_with("cppboot-"))) {
            fs::copy_file(entry.path(), stage / file, error);
        }
    }
    if (fs::exists(directory / "db", error)) {
        fs::copy(directory / "db", stage / "db", fs::copy_options::recursive, error);
    }
    if (error) {
        return Error{ErrorCode::IoError, "Cannot copy the settings or migrations: " + error.message()};
    }

    if (auto written = writePackageFiles(stage, name.value(), kForLinux); !written) {
        return written.error();
    }

    const fs::path script = directory / folder / "cppboot-copy-libraries.cmake";
    std::ofstream{script, std::ios::binary} << runtimeLibrariesScript();
    auto copied = runTool({"cmake", "-DEXE=" + executable.generic_string(),
                           "-DLIB_DIR=" + (stage / kLibraryFolder).generic_string(), "-P",
                           script.generic_string()},
                          directory);
    if (!copied) {
        return copied.error();
    }
    if (copied.value() != 0) {
        return Error{ErrorCode::IoError, "Copying the application's libraries failed"};
    }

    std::vector<std::string> archive_command{"cmake", "-E", "tar", kForLinux ? "czf" : "cf", archive};
    archive_command.insert(archive_command.end(), archive_options.begin(), archive_options.end());
    archive_command.push_back(name.value());
    auto archived = runTool(std::move(archive_command), dist);
    if (!archived) {
        return archived.error();
    }
    if (archived.value() != 0) {
        return Error{ErrorCode::IoError, "Creating " + archive + " failed"};
    }
    return dist / archive;
}

}  // namespace cppboot::cli
