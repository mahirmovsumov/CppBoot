#include "doctor.hpp"

#include <chrono>
#include <fstream>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "cppboot/base/version.hpp"
#include "cppboot/cli/cli.hpp"
#include "cppboot/platform/process.hpp"
#include "toolchain.hpp"

namespace cppboot::cli {

namespace {

namespace fs = std::filesystem;

#if defined(_WIN32)
constexpr std::string_view kInstallAgain =
    "Run the installer again in PowerShell: irm "
    "https://raw.githubusercontent.com/mahirmovsumov/CppBoot/main/scripts/install.ps1 | iex";
constexpr std::string_view kFixCmake =
    "Run the installer again, or set CPPBOOT_TOOLCHAIN to the ucrt64\\bin folder of MSYS2";
constexpr std::string_view kFixNinja = kFixCmake;
constexpr std::string_view kFixLibclang =
    "Install libclang in MSYS2: pacman -S mingw-w64-ucrt-x86_64-clang";
constexpr std::string_view kFixLibraries =
    "Run the installer again, which installs the libraries CppBoot needs";
constexpr std::string_view kFromSource = "install.ps1 -FromSource";
constexpr std::string_view kFixGoogleTest =
    "Install it in MSYS2: pacman -S mingw-w64-ucrt-x86_64-gtest";
constexpr std::string_view kFixDebugger = "Install it in MSYS2: pacman -S mingw-w64-ucrt-x86_64-gdb";
#else
constexpr std::string_view kInstallAgain =
    "Run the installer again: curl -fsSL "
    "https://raw.githubusercontent.com/mahirmovsumov/CppBoot/main/scripts/install.sh | bash";
constexpr std::string_view kFixCmake = "Install it: sudo apt-get install cmake";
constexpr std::string_view kFixNinja = "Install it: sudo apt-get install ninja-build";
constexpr std::string_view kFixLibclang = "Install it: sudo apt-get install libclang-dev";
constexpr std::string_view kFixLibraries =
    "Install them: sudo apt-get install g++ libboost-dev "
    "nlohmann-json3-dev libpq-dev libsqlite3-dev libssl-dev libgtest-dev";
constexpr std::string_view kFromSource = "install.sh --from-source";
constexpr std::string_view kFixGoogleTest = "Install it: sudo apt-get install libgtest-dev";
constexpr std::string_view kFixDebugger = "Install it: sudo apt-get install gdb";
#endif

std::map<std::string, std::string> readSettingsFile(const fs::path& path) {
    std::map<std::string, std::string> values;
    std::ifstream input{path};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (const auto equals = line.find('='); equals != std::string::npos) {
            values[line.substr(0, equals)] = line.substr(equals + 1);
        }
    }
    return values;
}

std::string firstLine(std::string_view text) {
    const auto end = text.find_first_of("\r\n");
    return std::string{text.substr(0, end)};
}

std::string lastLines(const std::string& text, std::size_t count) {
    std::size_t position = text.size();
    for (std::size_t found = 0; found <= count && position > 0; ++found) {
        position = text.rfind('\n', position - 1);
        if (position == std::string::npos) {
            return text;
        }
    }
    return text.substr(position + 1);
}

std::string majorVersion(const std::string& version) {
    return version.substr(0, version.find('.'));
}

/// Runs a command and returns what it wrote, or nothing if it couldn't start or failed.
std::optional<platform::ProcessOutput> capture(std::vector<std::string> command,
                                               const fs::path& directory = {}) {
    auto result = platform::runProcessCapturingOutput({std::move(command), directory});
    if (!result) {
        return std::nullopt;
    }
    return std::move(result).value();
}

class DoctorReport {
public:
    explicit DoctorReport(std::ostream& out) : out_(out) {}

    void ok(std::string_view check, std::string_view detail) {
        out_ << "  ok       " << check << ": " << detail << '\n';
    }
    void warn(std::string_view check, std::string_view detail, std::string_view fix) {
        out_ << "  warning  " << check << ": " << detail << "\n           " << fix << '\n';
        ++warnings_;
    }
    void fail(std::string_view check, std::string_view detail, std::string_view fix) {
        out_ << "  problem  " << check << ": " << detail << "\n           " << fix << '\n';
        ++problems_;
    }

    int finish() {
        out_ << '\n';
        if (problems_ == 0) {
            out_ << (warnings_ == 0 ? "Everything CppBoot needs is ready.\n"
                                    : "CppBoot should work, but read the warnings above.\n");
            return 0;
        }
        out_ << problems_ << (problems_ == 1 ? " problem" : " problems")
             << " to fix. Run cppboot doctor again afterwards.\n";
        return 1;
    }

private:
    std::ostream& out_;
    int problems_{0};
    int warnings_{0};
};

/// A temporary folder that's deleted when this goes away.
class ScratchFolder {
public:
    ScratchFolder() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("cppboot-doctor-" + std::to_string(stamp));
        fs::create_directories(path_);
    }
    ~ScratchFolder() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

constexpr std::string_view kProbeProject = R"(cmake_minimum_required(VERSION 3.25)
project(cppboot_doctor LANGUAGES CXX)
find_package(CppBoot CONFIG REQUIRED)
find_package(GTest QUIET)
file(WRITE ${CMAKE_BINARY_DIR}/doctor.txt
    "cmake=${CMAKE_VERSION}\ncompiler_id=${CMAKE_CXX_COMPILER_ID}\ncompiler_version=${CMAKE_CXX_COMPILER_VERSION}\ngtest=${GTest_FOUND}\n")
)";

/// Builds and runs a sample web project with a database, which uses most of the SDK.
void checkSampleProject(DoctorReport& report, const fs::path& prefix) {
    const ScratchFolder scratch;
    ProjectOptions options;
    options.web = true;
    options.database = ProjectDatabase::Sqlite;
    if (auto created = createProject("doctor-sample", scratch.path(), options); !created) {
        report.fail("Sample project", created.error().message(), "Check that the folder for "
                                                                 "temporary files can be written");
        return;
    }

    const fs::path project = scratch.path() / "doctor-sample";
    const auto configured = capture({"cmake", "-S", ".", "-B", "build", "-G", "Ninja",
                                     "-DCMAKE_PREFIX_PATH=" + prefix.generic_string()},
                                    project);
    const auto built = configured && configured->exit_code == 0
                           ? capture({"cmake", "--build", "build"}, project)
                           : std::nullopt;
    if (!built || built->exit_code != 0) {
        const std::string output = built ? built->output : configured ? configured->output : "";
        report.fail("Sample project", "it didn't build:\n" + lastLines(output, 15),
                    std::string{kFixLibraries});
        return;
    }

    const auto ran = capture({(project / "build" / platform::executableFileName("doctor-sample"))
                                  .string(),
                              "--cppboot.server.port=0",
                              "--cppboot.runtime.shutdown_after_start=true"},
                             project);
    if (!ran || ran->exit_code != 0 ||
        ran->output.find("Application is running") == std::string::npos) {
        report.fail("Sample project", "it built, but didn't run:\n" +
                                          (ran ? lastLines(ran->output, 15) : std::string{}),
                    "Check that the libraries it needs are on PATH");
        return;
    }
    report.ok("Sample project", "a web project with a database built and ran");
}

}  // namespace

int runDoctor(std::ostream& out, bool full) {
    DoctorReport report{out};
    out << "CppBoot " << kVersionString << " doctor\n\n";

    auto prefix = findInstallation();
    if (!prefix) {
        report.fail("Installation", prefix.error().message(), std::string{kInstallAgain});
        return report.finish();
    }
    report.ok("Installation", prefix.value().generic_string());

    const auto built_with = readSettingsFile(prefix.value() / "lib" / "cppboot" / "build-info.txt");

    const auto cmake = capture({"cmake", "--version"});
    if (!cmake || cmake->exit_code != 0) {
        report.fail("CMake", "not found on PATH", std::string{kFixCmake});
    } else {
        report.ok("CMake", firstLine(cmake->output));
    }

    const auto ninja = capture({"ninja", "--version"});
    if (!ninja || ninja->exit_code != 0) {
        report.fail("Ninja", "not found on PATH", std::string{kFixNinja});
    } else {
        report.ok("Ninja", firstLine(ninja->output));
    }

    // Only debugging needs it, so it's a warning.
    const auto gdb = capture({"gdb", "--version"});
    if (!gdb || gdb->exit_code != 0) {
        report.warn("Debugger", "gdb isn't on PATH, so editors can't debug projects",
                    std::string{kFixDebugger});
    } else {
        report.ok("Debugger", firstLine(gdb->output));
    }

    const auto codegen =
        capture({(prefix.value() / "bin" / platform::executableFileName("cppboot-codegen")).string(),
                 "--print-resource-dir"});
    if (!codegen || codegen->exit_code != 0) {
        report.fail("Code generator", "cppboot-codegen didn't run, which usually means libclang "
                                      "is missing",
                    std::string{kFixLibclang});
    } else {
        report.ok("Code generator", "cppboot-codegen and libclang work");
    }

    // A real project that finds CppBoot shows that the compiler and every library it needs work
    // together, which checking each one alone wouldn't.
    if (cmake && cmake->exit_code == 0 && ninja && ninja->exit_code == 0) {
        const ScratchFolder scratch;
        std::ofstream{scratch.path() / "CMakeLists.txt", std::ios::binary} << kProbeProject;
        const auto configured =
            capture({"cmake", "-S", ".", "-B", "build", "-G", "Ninja",
                     "-DCMAKE_PREFIX_PATH=" + prefix.value().generic_string()},
                    scratch.path());
        if (!configured || configured->exit_code != 0) {
            report.fail("Compiler and libraries",
                        "a test project couldn't use CppBoot:\n" +
                            (configured ? lastLines(configured->output, 12) : std::string{}),
                        std::string{kFixLibraries});
        } else {
            const auto found = readSettingsFile(scratch.path() / "build" / "doctor.txt");
            const std::string compiler = found.count("compiler_id") != 0
                                             ? found.at("compiler_id") + " " +
                                                   found.at("compiler_version")
                                             : "unknown";
            report.ok("Compiler and libraries", "a test project found CppBoot and everything it "
                                                "needs, with " +
                                                    compiler);
            if (found.count("gtest") != 0 && (found.at("gtest") == "TRUE" || found.at("gtest") == "1")) {
                report.ok("GoogleTest", "found, so cppboot test works");
            } else {
                report.warn("GoogleTest", "not found, so cppboot test won't work",
                            std::string{kFixGoogleTest});
            }

            if (built_with.count("compiler_id") != 0 && found.count("compiler_id") != 0) {
                const std::string sdk_compiler =
                    built_with.at("compiler_id") + " " + built_with.at("compiler_version");
                if (built_with.at("compiler_id") != found.at("compiler_id") ||
                    majorVersion(built_with.at("compiler_version")) !=
                        majorVersion(found.at("compiler_version"))) {
                    report.warn("Compiler version",
                                "this SDK was built with " + sdk_compiler +
                                    ", and projects use " + compiler +
                                    ", which may fail to link",
                                "Install the SDK built for your compiler, with " +
                                    std::string{kFromSource});
                }
            }
        }
    }

    if (full) {
        checkSampleProject(report, prefix.value());
    } else {
        out << "  (Run cppboot doctor --full to also build and run a sample project.)\n";
    }
    return report.finish();
}

}  // namespace cppboot::cli
