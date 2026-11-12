#include "generators.hpp"

#include <algorithm>
#include <array>
#include <system_error>

#include "source_files.hpp"

namespace cppboot::cli {

namespace {

namespace fs = std::filesystem;

struct Kind {
    std::string_view name;
    /// Added to the class name unless it already ends with it, as in Order to OrderService.
    std::string_view suffix;
    /// Text that the project's CMakeLists.txt must contain, or empty.
    std::string_view needs;
    std::string_view needs_message;
    std::string_view content;
};

constexpr std::string_view kService = R"(#pragma once

#include <cppboot/cppboot.hpp>
#include <string>

// A component. Other components can take it in their constructors, and CppBoot creates one of it.
// To use another component, take it as a reference: @CLASS@(OtherService& other)
class CPPBOOT_SERVICE @CLASS@ {
public:
    std::string describe() const { return "@CLASS@ is ready"; }
};
)";

constexpr std::string_view kController = R"(#pragma once

#include <cppboot/cppboot.hpp>
#include <cppboot/web/http.hpp>
#include <string>

struct CPPBOOT_JSON @BASE@Reply {
    int id{0};
    std::string name;
};

// Routes under @PATH@. Marked functions become routes, and what they return is sent as JSON.
class CPPBOOT_CONTROLLER("@PATH@") @CLASS@ {
public:
    // GET @PATH@/7
    CPPBOOT_GET("/{id}") @BASE@Reply get(int id) const {
        if (id < 1) {
            throw cppboot::web::HttpError(404, "No @SNAKE@ has that ID");
        }
        return {id, "@SNAKE@-" + std::to_string(id)};
    }
};
)";

constexpr std::string_view kRepository = R"(#pragma once

#include <cppboot/cppboot.hpp>
#include <cppboot/data/database.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A row of the @TABLE@ table, which @MIGRATION@ creates. Fields match columns by name.
struct CPPBOOT_JSON CPPBOOT_ROW @BASE@Record {
    std::int64_t id{0};
    std::string name;
};

// Values go in as $1, $2 and so on, never as part of the SQL text, which prevents SQL injection.
class CPPBOOT_SERVICE @CLASS@ {
public:
    explicit @CLASS@(cppboot::data::Database& database) : database_(database) {}

    @BASE@Record add(const std::string& name) {
        return *database_.queryOne<@BASE@Record>(
            "INSERT INTO @TABLE@ (name) VALUES ($1) RETURNING id, name", name);
    }

    std::optional<@BASE@Record> find(std::int64_t id) {
        return database_.queryOne<@BASE@Record>("SELECT id, name FROM @TABLE@ WHERE id = $1", id);
    }

    std::vector<@BASE@Record> all() {
        return database_.queryAs<@BASE@Record>("SELECT id, name FROM @TABLE@ ORDER BY id");
    }

private:
    cppboot::data::Database& database_;
};
)";

constexpr std::string_view kMiddleware = R"(#pragma once

#include <cppboot/cppboot.hpp>
#include <cppboot/web/router.hpp>

// Runs around every request. Lower numbers run first.
class CPPBOOT_MIDDLEWARE(100) @CLASS@ {
public:
    explicit @CLASS@(const cppboot::logging::Logger& logger) : logger_(logger) {}

    cppboot::web::Response handle(const cppboot::web::Request& request,
                                  const cppboot::web::Next& next) {
        cppboot::web::Response response = next(request);
        logger_.debug("{} {} {}", cppboot::web::toString(request.method()), request.path(),
                      response.status());
        return response;
    }

private:
    const cppboot::logging::Logger& logger_;
};
)";

constexpr std::string_view kModule = R"(#pragma once

#include <cppboot/cppboot.hpp>

// Starts and stops with the application. Don't wait in start(): start a worker for background work.
class CPPBOOT_SERVICE @CLASS@ : public cppboot::Module {
public:
    std::string_view name() const noexcept override { return "@SNAKE@"; }

    cppboot::Result<void> start(cppboot::ApplicationContext& context) override {
        context.logger().info("@CLASS@ started");
        return {};
    }

    void stop() noexcept override {}
};
)";

constexpr std::string_view kHealth = R"(#pragma once

#include <cppboot/actuator/actuator.hpp>
#include <cppboot/cppboot.hpp>
#include <string>

// A check for /actuator/health. Throw from check() to report a problem. Keep it quick, because load
// balancers call it often.
class CPPBOOT_SERVICE @CLASS@ : public cppboot::actuator::HealthIndicator {
public:
    std::string name() const override { return "@SNAKE@"; }

    void check() override {}
};
)";

constexpr std::string_view kConfig = R"(#pragma once

#include <cppboot/cppboot.hpp>
#include <string>

// Filled from the settings under "@SNAKE@" when the application starts, so @SNAKE@.host in
// cppboot.yaml, or the environment variable CPPBOOT_@UPPER@_HOST, sets host. A component takes it
// in its constructor as const @CLASS@&.
//
// A value of the wrong type, a failed check, or a setting under "@SNAKE@" that no field reads
// stops startup with a message that names it.
struct CPPBOOT_CONFIG("@SNAKE@") @CLASS@ {
    // A field keeps its value here unless its setting is set.
    std::string host{"localhost"};

    CPPBOOT_RANGE(1, 65535) int port{8080};

    // Mark a field CPPBOOT_REQUIRED if the application can't start without its setting.
};
)";

constexpr std::array<Kind, 7> kKinds = {{
    {"service", "Service", "", "", kService},
    {"controller", "Controller", "cppboot::web", "a web project, made with --web", kController},
    {"repository", "Repository", "cppboot::data",
     "a project with a database, made with --sqlite or --postgresql", kRepository},
    {"middleware", "Middleware", "cppboot::web", "a web project, made with --web", kMiddleware},
    {"module", "Module", "", "", kModule},
    {"health", "Health", "ACTUATOR", "a project with the actuator, made with --actuator", kHealth},
    {"config", "Settings", "", "", kConfig},
}};

}  // namespace

std::vector<std::string_view> componentKinds() {
    std::vector<std::string_view> names;
    for (const Kind& kind : kKinds) {
        names.push_back(kind.name);
    }
    return names;
}

Result<std::vector<fs::path>> addComponent(const fs::path& project, std::string_view kind_name,
                                           std::string_view name) {
    const auto kind = std::find_if(kKinds.begin(), kKinds.end(),
                                   [&](const Kind& candidate) { return candidate.name == kind_name; });
    if (kind == kKinds.end()) {
        std::string known;
        for (const Kind& candidate : kKinds) {
            known += known.empty() ? "" : ", ";
            known += candidate.name;
        }
        return Error{ErrorCode::InvalidArgument,
                     "Unknown kind '" + std::string{kind_name} + "'. Use one of: " + known +
                         ", resource"};
    }

    if (!isClassName(name)) {
        return Error{ErrorCode::InvalidArgument,
                     "'" + std::string{name} + "' isn't a class name. Use letters and digits, "
                                               "starting with a capital, such as Order"};
    }
    if (auto found = checkProject(project); !found) {
        return found.error();
    }
    if (!kind->needs.empty() && readFile(project / "CMakeLists.txt").find(kind->needs) == std::string::npos) {
        return Error{ErrorCode::InvalidArgument, "A " + std::string{kind->name} + " needs " +
                                                     std::string{kind->needs_message}};
    }

    const std::string class_name =
        name.ends_with(kind->suffix) ? std::string{name} : std::string{name} + std::string{kind->suffix};
    std::string base = std::string{name};
    if (base.ends_with(kind->suffix) && base.size() > kind->suffix.size()) {
        base.resize(base.size() - kind->suffix.size());
    }
    const std::string snake = snakeCase(base);
    const fs::path header = fs::path{"src"} / (snakeCase(class_name) + ".hpp");
    std::error_code error;
    if (fs::exists(project / header, error)) {
        return Error{ErrorCode::InvalidState, header.generic_string() + " already exists"};
    }

    std::vector<fs::path> written;
    std::string content = std::string{kind->content};
    content = replaceAll(content, "@CLASS@", class_name);
    content = replaceAll(content, "@BASE@", base);
    content = replaceAll(content, "@SNAKE@", snake);
    content = replaceAll(content, "@UPPER@", upperCase(snake));
    content = replaceAll(content, "@PATH@", "/api/" + plural(snake));

    if (kind->name == "repository") {
        const std::string table = plural(snake);
        auto migration = newTableMigration(project, table);
        if (!migration) {
            return migration.error();
        }
        const std::string sql = "CREATE TABLE " + table + " (\n    id " +
                                (usesPostgresql(project) ? "BIGSERIAL PRIMARY KEY" : "INTEGER PRIMARY KEY") +
                                ",\n    name TEXT NOT NULL\n);\n";
        content = replaceAll(content, "@TABLE@", table);
        content = replaceAll(content, "@MIGRATION@", migration.value().generic_string());
        if (auto saved = writeNew(project / migration.value(), sql); !saved) {
            return saved.error();
        }
        written.push_back(migration.value());
    }

    if (auto saved = writeNew(project / header, content); !saved) {
        return saved.error();
    }
    written.insert(written.begin(), header);
    return written;
}

}  // namespace cppboot::cli
