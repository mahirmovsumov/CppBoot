#include "project_template.hpp"

#include <array>

namespace cppboot::cli {

namespace {

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
constexpr std::array<ProjectFile, 17> kProjectFiles = {{
    {"CMakeLists.txt", R"(cmake_minimum_required(VERSION 3.25)

project(@NAME@ VERSION 0.1.0 LANGUAGES CXX)

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

# Tests in tests/, which cppboot test builds and runs. They aren't part of a normal build. They're
# built with the application's code, except main.cpp, so they can use its classes.
find_package(GTest QUIET)
if(GTest_FOUND)
    file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS tests/*.cpp)
    set(application_sources ${sources})
    list(FILTER application_sources EXCLUDE REGEX "/src/main\\.cpp$")
    add_executable(@NAME@-tests EXCLUDE_FROM_ALL ${test_sources} ${application_sources})
    target_include_directories(@NAME@-tests PRIVATE src)
    target_link_libraries(@NAME@-tests PRIVATE @LIBRARIES@ GTest::gtest_main)
    cppboot_generate_components(@NAME@-tests @GENERATE_OPTIONS@HEADERS ${headers})
    target_compile_definitions(@NAME@-tests PRIVATE _GLIBCXX_ASSERTIONS)
    target_compile_options(@NAME@-tests PRIVATE -Wall -Wextra)
else()
    add_custom_target(@NAME@-tests
        COMMAND ${CMAKE_COMMAND} -E echo "The tests need GoogleTest. Run cppboot doctor to see how to install it."
        COMMAND ${CMAKE_COMMAND} -E false)
endif()
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
    {"tests/components_test.cpp", R"(#include <gtest/gtest.h>

#include <cppboot/cppboot.hpp>
#include <cppboot/di/container.hpp>
#include <cppboot/platform/environment.hpp>
#include <memory>
#include <utility>

#include "cppboot_components.generated.hpp"

// Checks that every component can get what its constructor asks for, without creating any of them.
// It fails if, for example, a constructor takes a class that isn't marked as a component. It also
// fills each CPPBOOT_CONFIG struct from cppboot.yaml and the environment, as the application does,
// so it fails if a setting doesn't fit.
TEST(ComponentsTest, FitTogether) {
    cppboot::config::ConfigSources sources;
    sources.environment = cppboot::platform::environmentVariables();
    auto config = cppboot::config::Config::load(sources);
    ASSERT_TRUE(config) << config.error().message();
    auto logger = cppboot::logging::Logger::create({});
    ASSERT_TRUE(logger) << logger.error().message();

    cppboot::di::Container container;
    container.registerInstance(std::make_shared<cppboot::config::Config>(std::move(config).value()));
    container.registerInstance(std::make_shared<cppboot::logging::Logger>(std::move(logger).value()));
    try {
        cppboot::generated::registerComponents(container);
    } catch (const cppboot::Exception& exception) {
        FAIL() << exception.error().message();
    }

    const auto valid = container.validate();
    EXPECT_TRUE(valid) << valid.error().message();
}
)"},
    {"tests/greeting_test.cpp", R"(#include <gtest/gtest.h>

#include "greeting.hpp"

// A component is a plain class, so a test can create one and call it.
TEST(GreetingServiceTest, Greets) {
    const GreetingService greetings;
    EXPECT_EQ(greetings.greeting(), "Hello from CppBoot!");
}
)",
     ProjectFiles::PlainOnly},
    {"tests/users_test.cpp", R"(#include <gtest/gtest.h>

#include "users.hpp"

// A component is a plain class, so a test can create one and call it, without a server.
TEST(UserServiceTest, FindsAUser) {
    const UserService users;
    EXPECT_EQ(users.find(7).name, "user-7");
}

TEST(UserControllerTest, ReturnsTheUser) {
    UserService users;
    const UserController controller{users};
    EXPECT_EQ(controller.get(7).id, 7);
}
)",
     ProjectFiles::WebWithoutDatabase},
    {".vscode/extensions.json", R"({
  "recommendations": ["ms-vscode.cpptools", "llvm-vs-code-extensions.vscode-clangd"]
}
)"},
    {".vscode/settings.json", R"({
  // Both read the compiler's settings for each file from the first build, so run cppboot build once.
  "C_Cpp.default.compileCommands": "${workspaceFolder}/build/compile_commands.json",
  "clangd.arguments": ["--compile-commands-dir=${workspaceFolder}/build", "--header-insertion=never"],
  // The cppboot tasks configure the project, so CMake Tools doesn't need to.
  "cmake.configureOnOpen": false,
  "files.exclude": {
    "build*/": true,
    "dist/": true
  }
}
)"},
    // A different delimiter, because the patterns below contain )".
    {".vscode/tasks.json", R"json({
  "version": "2.0.0",
  "tasks": [
    {
      "label": "cppboot: build",
      "type": "shell",
      "command": "cppboot build",
      "group": { "kind": "build", "isDefault": true },
      "problemMatcher": { "base": "$gcc", "fileLocation": ["autoDetect", "${workspaceFolder}/build"] }
    },
    {
      "label": "cppboot: dev",
      "detail": "Build and run, and do it again whenever you save a change",
      "type": "shell",
      "command": "cppboot dev",
      "isBackground": true,
      "problemMatcher": {
        "base": "$gcc",
        "fileLocation": ["autoDetect", "${workspaceFolder}/build"],
        "background": {
          "activeBegins": true,
          "beginsPattern": "cppboot dev: (a file changed|restarting)",
          "endsPattern": "cppboot dev: (running|the build failed)"
        }
      }
    },
    {
      "label": "cppboot: run",
      "type": "shell",
      "command": "cppboot run",
      "problemMatcher": { "base": "$gcc", "fileLocation": ["autoDetect", "${workspaceFolder}/build"] }
    },
    {
      "label": "cppboot: test",
      "type": "shell",
      "command": "cppboot test",
      "group": { "kind": "test", "isDefault": true },
      "problemMatcher": { "base": "$gcc", "fileLocation": ["autoDetect", "${workspaceFolder}/build"] }
    }
  ]
}
)json"},
    {".vscode/launch.json", R"({
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Debug @NAME@",
      "type": "cppdbg",
      "request": "launch",
      "preLaunchTask": "cppboot: build",
      "program": "${workspaceFolder}/build/@NAME@",
      "args": [],
      "cwd": "${workspaceFolder}",
      "MIMode": "gdb",
      "miDebuggerPath": "gdb",
      "setupCommands": [{ "text": "-enable-pretty-printing", "ignoreFailures": true }],
      // On Windows, the debugger and the libraries the application needs are in MSYS2's folder,
      // which the installer records in CPPBOOT_TOOLCHAIN.
      "windows": {
        "program": "${workspaceFolder}/build/@NAME@.exe",
        "miDebuggerPath": "${env:CPPBOOT_TOOLCHAIN}/gdb.exe",
        "environment": [{ "name": "PATH", "value": "${env:CPPBOOT_TOOLCHAIN};${env:PATH}" }]
      }
    }
  ]
}
)"},
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

}  // namespace

std::vector<RenderedFile> renderProjectFiles(std::string_view name, const ProjectOptions& options) {
    std::vector<RenderedFile> files;
    for (const ProjectFile& file : kProjectFiles) {
        if (isIncluded(file.included, options)) {
            files.push_back({file.path, fillTemplate(file.content, name, options)});
        }
    }
    return files;
}

}  // namespace cppboot::cli
