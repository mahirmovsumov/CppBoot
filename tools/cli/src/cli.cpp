#include "cppboot/cli/cli.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <map>
#include <fstream>
#include <optional>
#include <ostream>
#include <system_error>
#include <utility>

#include "cppboot/base/version.hpp"
#include "cppboot/config/config.hpp"
#include "cppboot/platform/environment.hpp"
#include "cppboot/platform/process.hpp"
#include "package_files.hpp"

namespace cppboot::cli {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUsage = R"(Usage: cppboot <command> [arguments]

Commands:
  create <name>     Create a new project in a new folder
                    Add --web for a web API with a sample controller
                    Add --sqlite or --postgresql with --web to store its data in a database
                    Add --security with --web for sign-in and protected routes
                    Add --actuator for health and metrics routes
  build             Build the project in the current folder
  run [arguments]   Build and run the project, passing arguments to the application
                    Put --sanitize=address or --sanitize=thread first on build or run to find
                    memory errors or data races, on Linux
  package           Build a release into dist/, with everything needed to run it elsewhere
                    On Linux, this includes a systemd service and a script that installs it
  doctor            Check that everything CppBoot needs is installed, and say how to fix what isn't
                    Add --full to also build and run a sample project
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

enum class ProjectFiles { Always, PlainOnly, WebWithoutDatabase, WithDatabase, WithSecurity };

struct ProjectFile {
    std::string_view path;
    std::string_view content;
    ProjectFiles included{ProjectFiles::Always};
};

constexpr std::string_view kWebSettings = R"(
  server:
    host: 127.0.0.1
    port: 8080
)";

constexpr std::string_view kSqliteSettings = R"(
  database:
    url: sqlite:@NAME@.db
    migrations: db/migrations
)";

// A local development default. Set CPPBOOT_DATABASE_URL to keep real credentials out of this file.
constexpr std::string_view kPostgresqlSettings = R"(
  database:
    url: postgresql://postgres:postgres@localhost:5432/@NAME@
    migrations: db/migrations
)";

// There's no secret here on purpose. It has to come from the environment, and the application won't
// start without one.
constexpr std::string_view kSecuritySettings = R"(
  security:
    jwt:
      issuer: @NAME@
      lifetime_seconds: 3600
)";

// "@NAME@" is replaced with the project name. "@LIBRARIES@", "@GENERATE_OPTIONS@", "@SETTINGS@" and
// "@ID_COLUMN@" depend on the project's options.
constexpr std::array<ProjectFile, 10> kProjectFiles = {{
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
target_link_libraries(@NAME@ PRIVATE @LIBRARIES@)
cppboot_generate_components(@NAME@ @GENERATE_OPTIONS@HEADERS ${headers})

# _GLIBCXX_ASSERTIONS stops the program on mistakes such as an index past the end of a vector, rather
# than letting it read or write memory it shouldn't. It costs very little, so it's on in every build.
target_compile_definitions(@NAME@ PRIVATE _GLIBCXX_ASSERTIONS)
target_compile_options(@NAME@ PRIVATE -Wall -Wextra)
)"},
    {"cppboot.yaml", R"(cppboot:
  application:
    name: @NAME@

  logging:
    level: info
@SETTINGS@)"},
    {"src/main.cpp", R"(#include <cppboot/boot.hpp>

#include "cppboot_components.generated.hpp"

int main(int argc, char* argv[]) {
    return cppboot::runApplication(argc, argv, cppboot::generated::registerComponents);
}
)"},
    {"src/greeting.hpp", R"(#pragma once

#include <cppboot/cppboot.hpp>
#include <string>

// CPPBOOT_SERVICE makes a class a component. CppBoot reads its constructor, creates it and passes
// in the components it needs, so there's no registration code to write.
class CPPBOOT_SERVICE GreetingService {
public:
    std::string greeting() const { return "Hello from CppBoot!"; }
};

// A component that derives from cppboot::Module also starts and stops with the application. CppBoot
// keeps what it passes by reference alive for as long as this class exists.
class CPPBOOT_SERVICE GreetingModule : public cppboot::Module {
public:
    explicit GreetingModule(GreetingService& greetings) : greetings_(greetings) {}

    std::string_view name() const noexcept override { return "greeting"; }

    cppboot::Result<void> start(cppboot::ApplicationContext& context) override {
        context.logger().info("{}", greetings_.greeting());
        return {};
    }

private:
    GreetingService& greetings_;
};
)",
     ProjectFiles::PlainOnly},
    {"src/users.hpp", R"(#pragma once

#include <cppboot/cppboot.hpp>
#include <string>

// CPPBOOT_JSON generates the conversion to and from JSON.
struct CPPBOOT_JSON User {
    int id{0};
    std::string name;
};

// CPPBOOT_SERVICE makes a class a component, which CppBoot creates and passes to what needs it.
class CPPBOOT_SERVICE UserService {
public:
    User find(int id) const { return User{id, "user-" + std::to_string(id)}; }
};

// CPPBOOT_CONTROLLER turns the marked functions below into routes under this path.
class CPPBOOT_CONTROLLER("/api/users") UserController {
public:
    explicit UserController(UserService& users) : users_(users) {}

    // Handles GET /api/users/7. The id comes from the path, and the result is sent as JSON.
    CPPBOOT_GET("/{id}") User get(int id) const { return users_.find(id); }

private:
    UserService& users_;
};
)",
     ProjectFiles::WebWithoutDatabase},
    {"src/users.hpp", R"(#pragma once

#include <cppboot/cppboot.hpp>
#include <cppboot/data/database.hpp>
#include <cppboot/web/binding.hpp>
#include <cppboot/web/http.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// CPPBOOT_JSON generates the conversion to and from JSON, and CPPBOOT_ROW generates reading it from
// a query result. Fields match columns by name.
struct CPPBOOT_JSON CPPBOOT_ROW User {
    std::int64_t id{0};
    std::string name;
};

struct CPPBOOT_JSON NewUser {
    std::string name;
};

// Values go in as $1, $2 and so on, never as part of the SQL text, which prevents SQL injection.
class CPPBOOT_SERVICE UserRepository {
public:
    explicit UserRepository(cppboot::data::Database& database) : database_(database) {}

    User add(const NewUser& details) {
        return *database_.queryOne<User>("INSERT INTO users (name) VALUES ($1) RETURNING id, name",
                                         details.name);
    }

    std::optional<User> find(std::int64_t id) {
        return database_.queryOne<User>("SELECT id, name FROM users WHERE id = $1", id);
    }

    std::vector<User> all() {
        return database_.queryAs<User>("SELECT id, name FROM users ORDER BY id");
    }

private:
    cppboot::data::Database& database_;
};

class CPPBOOT_CONTROLLER("/api/users") UserController {
public:
    explicit UserController(UserRepository& users) : users_(users) {}

    CPPBOOT_GET("/") std::vector<User> list() { return users_.all(); }

    CPPBOOT_GET("/{id}") User get(std::int64_t id) {
        auto user = users_.find(id);
        if (!user) {
            throw cppboot::web::HttpError(404, "No user has that ID");
        }
        return *user;
    }

    CPPBOOT_POST("/") cppboot::web::WithStatus<User> create(const NewUser& details) {
        return {201, users_.add(details)};
    }

private:
    UserRepository& users_;
};
)",
     ProjectFiles::WithDatabase},
    {"src/auth.hpp", R"(#pragma once

#include <cppboot/cppboot.hpp>
#include <cppboot/security/password.hpp>
#include <cppboot/security/token.hpp>
#include <cppboot/web/http.hpp>
#include <string>
#include <vector>

struct CPPBOOT_JSON Credentials {
    std::string name;
    std::string password;
};

struct CPPBOOT_JSON TokenReply {
    std::string token;
};

struct CPPBOOT_JSON Profile {
    std::string name;
    std::vector<std::string> roles;
};

// A sample with one account, whose password comes from the CPPBOOT_MYAPP_PASSWORD environment
// variable. Store real accounts in a database, with hashes from hashPassword().
class CPPBOOT_SERVICE Accounts {
public:
    explicit Accounts(const cppboot::config::Config& config) {
        const std::string password = config.getString("cppboot.myapp.password");
        if (password.empty()) {
            throw cppboot::Exception(cppboot::Error{
                cppboot::ErrorCode::InvalidArgument,
                "Set the sample account's password with CPPBOOT_MYAPP_PASSWORD"});
        }
        hash_ = cppboot::security::hashPassword(password);
    }

    bool check(const Credentials& credentials) const {
        // Checks the password whatever the name is, so that timing doesn't reveal valid names.
        const bool password_matches =
            cppboot::security::verifyPassword(credentials.password, hash_);
        return password_matches && credentials.name == "admin";
    }

private:
    std::string hash_;
};

class CPPBOOT_CONTROLLER("/api") ApiController {
public:
    ApiController(Accounts& accounts, cppboot::security::TokenService& tokens)
        : accounts_(accounts), tokens_(tokens) {}

    // Every route in a project with security needs a token, unless it's marked public.
    // Checking a password is slow on purpose, so this stops one client from using up the server.
    CPPBOOT_PUBLIC CPPBOOT_RATE_LIMIT("10/minute") CPPBOOT_POST("/login")
    TokenReply login(const Credentials& credentials) const {
        if (!accounts_.check(credentials)) {
            throw cppboot::web::HttpError(401, "Wrong name or password");
        }
        return {tokens_.issue(credentials.name, {"admin"})};
    }

    // This has no mark, so it needs a token. A Principal parameter says who sent the request.
    CPPBOOT_GET("/me") Profile me(const cppboot::security::Principal& who) const {
        return {who.subject, who.roles};
    }

    // This needs a token from a user with the admin role.
    CPPBOOT_ROLE("admin") CPPBOOT_GET("/report") Profile report(
        const cppboot::security::Principal& who) const {
        return {who.subject, who.roles};
    }

private:
    Accounts& accounts_;
    cppboot::security::TokenService& tokens_;
};
)",
     ProjectFiles::WithSecurity},
    {"db/migrations/001_create_users.sql", R"(CREATE TABLE users (
    id @ID_COLUMN@,
    name TEXT NOT NULL
);
)",
     ProjectFiles::WithDatabase},
    {".gitignore", R"(build/
build-*/
dist/
*.db
*.db-shm
*.db-wal
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

std::string replaceAll(std::string_view content, std::string_view placeholder,
                       std::string_view value) {
    std::string result;
    std::size_t position = 0;
    while (true) {
        const auto found = content.find(placeholder, position);
        if (found == std::string_view::npos) {
            result += content.substr(position);
            return result;
        }
        result += content.substr(position, found - position);
        result += value;
        position = found + placeholder.size();
    }
}

bool hasDatabase(const ProjectOptions& options) {
    return options.database != ProjectDatabase::None;
}

std::string fillTemplate(std::string_view content, std::string_view name,
                         const ProjectOptions& options) {
    std::string libraries = "cppboot::boot";
    std::string settings;
    if (options.web || options.actuator) {
        settings += kWebSettings;
    }
    if (options.web) {
        libraries += " cppboot::web";
    }
    if (hasDatabase(options)) {
        libraries += " cppboot::data";
        settings +=
            options.database == ProjectDatabase::Sqlite ? kSqliteSettings : kPostgresqlSettings;
    }
    if (options.security) {
        libraries += " cppboot::security";
        settings += kSecuritySettings;
    }

    // Settings go first, because they contain the project name.
    std::string result = replaceAll(content, "@SETTINGS@", settings);
    result = replaceAll(result, "@NAME@", name);
    result = replaceAll(result, "@LIBRARIES@", libraries);
    result = replaceAll(result, "@GENERATE_OPTIONS@", options.actuator ? "ACTUATOR " : "");
    return replaceAll(result, "@ID_COLUMN@",
                      options.database == ProjectDatabase::Postgresql ? "BIGSERIAL PRIMARY KEY"
                                                                      : "INTEGER PRIMARY KEY");
}

bool isIncluded(ProjectFiles included, const ProjectOptions& options) {
    switch (included) {
        case ProjectFiles::Always:
            return true;
        case ProjectFiles::PlainOnly:
            return !options.web;
        case ProjectFiles::WebWithoutDatabase:
            return options.web && !hasDatabase(options) && !options.security;
        case ProjectFiles::WithDatabase:
            return hasDatabase(options);
        case ProjectFiles::WithSecurity:
            return options.security;
    }
    return false;
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

enum class Sanitizer { None, Address, Thread };

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

/// Removes a leading --sanitize=... from the arguments. Returns nothing if its value is unknown.
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

Result<int> buildProject(const fs::path& directory, Sanitizer sanitizer = Sanitizer::None) {
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

        auto configured = runTool(std::move(command), directory);
        if (!configured || configured.value() != 0) {
            return configured;
        }
    }

    return runTool({"cmake", "--build", folder}, directory);
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
#else
constexpr std::string_view kInstallAgain =
    "Run the installer again: curl -fsSL "
    "https://raw.githubusercontent.com/mahirmovsumov/CppBoot/main/scripts/install.sh | bash";
constexpr std::string_view kFixCmake = "Install it: sudo apt-get install cmake";
constexpr std::string_view kFixNinja = "Install it: sudo apt-get install ninja-build";
constexpr std::string_view kFixLibclang = "Install it: sudo apt-get install libclang-dev";
constexpr std::string_view kFixLibraries =
    "Install them: sudo apt-get install g++ libyaml-cpp-dev libspdlog-dev libboost-dev "
    "nlohmann-json3-dev libpq-dev libsqlite3-dev libssl-dev libgtest-dev";
constexpr std::string_view kFromSource = "install.sh --from-source";
#endif

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
file(WRITE ${CMAKE_BINARY_DIR}/doctor.txt
    "cmake=${CMAKE_VERSION}\ncompiler_id=${CMAKE_CXX_COMPILER_ID}\ncompiler_version=${CMAKE_CXX_COMPILER_VERSION}\n")
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

Result<void> createProject(std::string_view name, const fs::path& parent,
                           const ProjectOptions& options) {
    if (auto valid = validateProjectName(name); !valid) {
        return valid;
    }

    const fs::path root = parent / name;
    std::error_code error;
    if (fs::exists(root, error)) {
        return Error{ErrorCode::InvalidState, "'" + root.generic_string() + "' already exists"};
    }

    for (const ProjectFile& file : kProjectFiles) {
        if (!isIncluded(file.included, options)) {
            continue;
        }

        const fs::path path = root / file.path;
        fs::create_directories(path.parent_path(), error);
        if (error) {
            return Error{ErrorCode::IoError, "Cannot create folder " +
                                                 path.parent_path().generic_string() + ": " +
                                                 error.message()};
        }

        // Binary mode keeps line endings the same on every platform.
        std::ofstream output{path, std::ios::binary};
        output << fillTemplate(file.content, name, options);
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

    applyToolchain();

    Result<int> result{0};
    if (command == "doctor" && (rest.empty() || (rest.size() == 1 && rest.front() == "--full"))) {
        out.flush();
        return runDoctor(out, !rest.empty());
    }
    if (command == "help" || command == "--help" || command == "-h") {
        out << kUsage;
    } else if (command == "info" && rest.empty()) {
        printInfo(out);
    } else if (command == "create") {
        std::vector<std::string> names;
        ProjectOptions options;
        bool sqlite = false;
        bool postgresql = false;
        for (const std::string& argument : rest) {
            if (argument == "--web") {
                options.web = true;
            } else if (argument == "--sqlite") {
                sqlite = true;
            } else if (argument == "--postgresql") {
                postgresql = true;
            } else if (argument == "--security") {
                options.security = true;
            } else if (argument == "--actuator") {
                options.actuator = true;
            } else {
                names.push_back(argument);
            }
        }
        if (names.size() != 1 || (sqlite && postgresql)) {
            err << kUsage;
            return kExitUsage;
        }
        if ((sqlite || postgresql || options.security) && !options.web) {
            err << "cppboot: --sqlite, --postgresql and --security need --web, because their "
                   "samples are web APIs\n";
            return kExitUsage;
        }
        if (sqlite) {
            options.database = ProjectDatabase::Sqlite;
        } else if (postgresql) {
            options.database = ProjectDatabase::Postgresql;
        }

        if (auto created = createProject(names.front(), fs::current_path(), options); !created) {
            result = created.error();
        } else {
            out << "Created " << names.front() << ". Next:\n  cd " << names.front()
                << "\n  cppboot run\n";
        }
    } else if (command == "build" || command == "run") {
        std::vector<std::string> remaining = rest;
        const auto sanitizer = takeSanitizer(remaining);
        if (!sanitizer || (command == "build" && !remaining.empty())) {
            err << kUsage;
            return kExitUsage;
        }

        out.flush();
        result = command == "build" ? buildProject(fs::current_path(), *sanitizer)
                                    : runProject(fs::current_path(), std::move(remaining), *sanitizer);
    } else if (command == "package" && rest.empty()) {
        out.flush();
        auto packaged = packageProject(fs::current_path());
        if (!packaged) {
            result = packaged.error();
        } else {
            out << "Packaged " << packaged.value().generic_string() << '\n';
        }
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
