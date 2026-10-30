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
                    Add --web for a web API with a sample controller
                    Add --sqlite or --postgresql with --web to store its data in a database
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

enum class ProjectFiles { Always, PlainOnly, WebWithoutDatabase, WithDatabase };

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

// "@NAME@" is replaced with the project name. "@LIBRARIES@", "@SETTINGS@" and "@ID_COLUMN@" depend
// on the project kind.
constexpr std::array<ProjectFile, 9> kProjectFiles = {{
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
cppboot_generate_components(@NAME@ HEADERS ${headers})
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
)",
     ProjectFiles::PlainOnly},
    {"src/users.hpp", R"(#pragma once

#include <cppboot/cppboot.hpp>
#include <memory>
#include <string>
#include <utility>

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
    explicit UserController(std::shared_ptr<UserService> users) : users_(std::move(users)) {}

    // Handles GET /api/users/7. The id comes from the path, and the result is sent as JSON.
    CPPBOOT_GET("/{id}") User get(int id) const { return users_->find(id); }

private:
    std::shared_ptr<UserService> users_;
};
)",
     ProjectFiles::WebWithoutDatabase},
    {"src/users.hpp", R"(#pragma once

#include <cppboot/cppboot.hpp>
#include <cppboot/data/database.hpp>
#include <cppboot/web/binding.hpp>
#include <cppboot/web/http.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
    explicit UserRepository(std::shared_ptr<cppboot::data::Database> database)
        : database_(std::move(database)) {}

    User add(const NewUser& details) {
        return *database_->queryOne<User>("INSERT INTO users (name) VALUES ($1) RETURNING id, name",
                                          details.name);
    }

    std::optional<User> find(std::int64_t id) {
        return database_->queryOne<User>("SELECT id, name FROM users WHERE id = $1", id);
    }

    std::vector<User> all() {
        return database_->queryAs<User>("SELECT id, name FROM users ORDER BY id");
    }

private:
    std::shared_ptr<cppboot::data::Database> database_;
};

class CPPBOOT_CONTROLLER("/api/users") UserController {
public:
    explicit UserController(std::shared_ptr<UserRepository> users) : users_(std::move(users)) {}

    CPPBOOT_GET("/") std::vector<User> list() { return users_->all(); }

    CPPBOOT_GET("/{id}") User get(std::int64_t id) {
        auto user = users_->find(id);
        if (!user) {
            throw cppboot::web::HttpError(404, "No user has that ID");
        }
        return *user;
    }

    CPPBOOT_POST("/") cppboot::web::WithStatus<User> create(const NewUser& details) {
        return {201, users_->add(details)};
    }

private:
    std::shared_ptr<UserRepository> users_;
};
)",
     ProjectFiles::WithDatabase},
    {"db/migrations/001_create_users.sql", R"(CREATE TABLE users (
    id @ID_COLUMN@,
    name TEXT NOT NULL
);
)",
     ProjectFiles::WithDatabase},
    {".gitignore", R"(build/
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

bool hasDatabase(ProjectKind kind) {
    return kind == ProjectKind::WebSqlite || kind == ProjectKind::WebPostgresql;
}

std::string fillTemplate(std::string_view content, std::string_view name, ProjectKind kind) {
    std::string libraries = "cppboot::boot";
    std::string settings;
    if (kind != ProjectKind::Plain) {
        libraries += " cppboot::web";
        settings += kWebSettings;
    }
    if (hasDatabase(kind)) {
        libraries += " cppboot::data";
        settings += kind == ProjectKind::WebSqlite ? kSqliteSettings : kPostgresqlSettings;
    }

    // Settings go first, because they contain the project name.
    std::string result = replaceAll(content, "@SETTINGS@", settings);
    result = replaceAll(result, "@NAME@", name);
    result = replaceAll(result, "@LIBRARIES@", libraries);
    return replaceAll(result, "@ID_COLUMN@",
                      kind == ProjectKind::WebPostgresql ? "BIGSERIAL PRIMARY KEY"
                                                         : "INTEGER PRIMARY KEY");
}

bool isIncluded(ProjectFiles included, ProjectKind kind) {
    switch (included) {
        case ProjectFiles::Always:
            return true;
        case ProjectFiles::PlainOnly:
            return kind == ProjectKind::Plain;
        case ProjectFiles::WebWithoutDatabase:
            return kind == ProjectKind::Web;
        case ProjectFiles::WithDatabase:
            return hasDatabase(kind);
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

Result<void> createProject(std::string_view name, const fs::path& parent, ProjectKind kind) {
    if (auto valid = validateProjectName(name); !valid) {
        return valid;
    }

    const fs::path root = parent / name;
    std::error_code error;
    if (fs::exists(root, error)) {
        return Error{ErrorCode::InvalidState, "'" + root.generic_string() + "' already exists"};
    }

    for (const ProjectFile& file : kProjectFiles) {
        if (!isIncluded(file.included, kind)) {
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
        output << fillTemplate(file.content, name, kind);
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
    } else if (command == "create") {
        std::vector<std::string> names;
        bool web = false;
        bool sqlite = false;
        bool postgresql = false;
        for (const std::string& argument : rest) {
            if (argument == "--web") {
                web = true;
            } else if (argument == "--sqlite") {
                sqlite = true;
            } else if (argument == "--postgresql") {
                postgresql = true;
            } else {
                names.push_back(argument);
            }
        }
        if (names.size() != 1 || (sqlite && postgresql)) {
            err << kUsage;
            return kExitUsage;
        }
        if ((sqlite || postgresql) && !web) {
            err << "cppboot: --sqlite and --postgresql need --web, because the sample that uses "
                   "the database is a web API\n";
            return kExitUsage;
        }

        ProjectKind kind = ProjectKind::Plain;
        if (sqlite) {
            kind = ProjectKind::WebSqlite;
        } else if (postgresql) {
            kind = ProjectKind::WebPostgresql;
        } else if (web) {
            kind = ProjectKind::Web;
        }

        if (auto created = createProject(names.front(), fs::current_path(), kind); !created) {
            result = created.error();
        } else {
            out << "Created " << names.front() << ". Next:\n  cd " << names.front()
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
