// The server for long stress runs. Its routes use every part of the framework together: routing,
// JSON, the database, rate limits, middleware, metrics and every kind of failure.
#include <atomic>
#include <chrono>
#include <cppboot/actuator/actuator.hpp>
#include <cppboot/cppboot.hpp>
#include <cppboot/data/database.hpp>
#include <cppboot/data/database_module.hpp>
#include <cppboot/web/rate_limit.hpp>
#include <cppboot/web/server_module.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <cppboot/base/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using namespace cppboot::web;

namespace {

/// Thrown inside a transaction, to check that it rolls back.
class AbandonTransaction : public std::runtime_error {
public:
    AbandonTransaction() : std::runtime_error("abandoned on purpose") {}
};

std::int64_t idFrom(const Request& request) {
    return std::stoll(*request.param("id"));
}

void addDatabaseRoutes(Router& router, const std::shared_ptr<cppboot::data::Database>& database) {
    router.post("/items/{id}", [database](const Request& request) {
        try {
            database->execute("INSERT INTO stress_items (id, body) VALUES ($1, $2)", idFrom(request),
                              request.body());
        } catch (const cppboot::data::DatabaseError& error) {
            if (error.kind() == cppboot::data::DatabaseError::Kind::UniqueViolation) {
                return errorResponse(409, "That item already exists");
            }
            throw;
        }
        return Response{201};
    });

    router.get("/items/{id}", [database](const Request& request) {
        const auto body = database->queryValue<std::string>(
            "SELECT body FROM stress_items WHERE id = $1", idFrom(request));
        return body ? Response::text(*body) : errorResponse(404, "No such item");
    });

    router.del("/items/{id}", [database](const Request& request) {
        database->execute("DELETE FROM stress_items WHERE id = $1", idFrom(request));
        return Response{204};
    });

    // Inserts, then fails, so the item must never be visible afterwards.
    router.post("/items/{id}/rollback", [database](const Request& request) {
        try {
            database->transaction([&](cppboot::data::Transaction& transaction) {
                transaction.execute("INSERT INTO stress_items (id, body) VALUES ($1, $2)",
                                    idFrom(request), request.body());
                throw AbandonTransaction{};
            });
        } catch (const AbandonTransaction&) {
            return errorResponse(422, "Rolled back");
        }
        return Response{200};
    });
}

}  // namespace

int main(int argc, char* argv[]) {
    cppboot::ApplicationOptions options;
    options.default_settings = {
        {"cppboot.logging.level", "warn"},
        {"cppboot.server.port", "18090"},
        {"cppboot.server.max_connections", "4096"},
        // The default. A shorter one closes idle connections on a client that's still reading the
        // last response, when both are short of CPU, which looks like a failure but isn't.
        {"cppboot.server.request_timeout_seconds", "30"},
        {"cppboot.server.max_body_bytes", "1048576"},
        {"cppboot.database.url", "sqlite:" + (std::filesystem::temp_directory_path() /
                                              "cppboot-stress.db")
                                                 .generic_string()},
        {"cppboot.database.pool.max_size", "8"},
        {"cppboot.rate_limit.max_clients", "2000"},
    };

    cppboot::config::ConfigSources sources;
    sources.directory = std::filesystem::temp_directory_path() / "cppboot-absent-directory";
    sources.defaults = options.default_settings;
    auto config = std::make_shared<const cppboot::config::Config>(
        cppboot::config::Config::load(sources).value());
    auto logger = std::make_shared<const cppboot::logging::Logger>(
        cppboot::logging::Logger::create({}).value());

    auto metrics = std::make_shared<cppboot::actuator::Metrics>();
    auto actuator = std::make_shared<cppboot::actuator::Actuator>(metrics, logger, config);

    auto router = std::make_shared<Router>();
    cppboot::actuator::mount(*router, actuator);

    auto& handled = metrics->counter("stress_middleware_total", "Requests the middleware saw");
    router->use([&handled](const Request& request, const Next& next) {
        handled.increment();
        Response response = next(request);
        response.setHeader("X-Stress", "1");
        return response;
    });

    router->get("/plaintext", [](const Request&) { return Response::text("Hello, World!"); });

    router->get("/json", [](const Request& request) {
        cppboot::Json body = cppboot::Json::object();
        body["message"] = "Hello, World!";
        body["client"] = request.clientAddress();
        body["page"] = request.query("page").value_or("none");
        return Response::json(body.dump());
    });

    router->get("/users/{id}/posts/{post}", [](const Request& request) {
        return Response::text(*request.param("id") + "/" + *request.param("post"));
    });

    router->post("/echo", [](const Request& request) { return Response::text(request.body()); });

    router->get("/boom", [](const Request&) -> Response {
        throw std::runtime_error("failed on purpose, with details a client must not see");
    });

    router->get("/teapot", [](const Request&) -> Response { throw HttpError(418, "I'm a teapot"); });

    // Keeps a worker thread busy, while a client may have gone away.
    router->get("/slow", [](const Request&) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        return Response::text("finally");
    });

    auto limiter = std::make_shared<RateLimiter>(RateLimit{5, std::chrono::seconds{1}},
                                                 RateLimiterOptions::fromSettings(*config, *logger));
    router->get("/limited", [limiter](const Request& request) {
        if (auto rejected = limiter->check(request)) {
            return *std::move(rejected);
        }
        return Response::text("allowed");
    });

    cppboot::Application app(argc, argv, std::move(options));
    router->onUnhandledException([](std::string_view) {});

    std::shared_ptr<cppboot::data::Database> database;
    app.setModuleProvider([&](const cppboot::config::Config& settings, const cppboot::logging::Logger&)
                              -> cppboot::Result<std::vector<std::unique_ptr<cppboot::Module>>> {
        database = cppboot::data::Database::fromSettings(settings);
        database->execute(
            "CREATE TABLE IF NOT EXISTS stress_items (id BIGINT PRIMARY KEY, body TEXT NOT NULL)");
        database->execute("DELETE FROM stress_items");
        actuator->addCheck("database", [database] { database->query("SELECT 1"); });
        addDatabaseRoutes(*router, database);

        std::vector<std::unique_ptr<cppboot::Module>> modules;
        modules.push_back(std::make_unique<ServerModule>(router));
        return modules;
    });
    return app.run();
}
