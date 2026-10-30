// A server for measuring the framework's own cost, with routes that do very little.
#include <cppboot/cppboot.hpp>
#include <cppboot/data/database.hpp>
#include <cppboot/web/server_module.hpp>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <utility>

using namespace cppboot::web;

namespace {

/// Creates a small table, so that /db measures one indexed read through the whole data layer.
std::shared_ptr<cppboot::data::Database> makeDatabase() {
    const auto path = std::filesystem::temp_directory_path() / "cppboot-bench.db";
    for (const char* suffix : {"", "-wal", "-shm"}) {
        std::error_code ignored;
        std::filesystem::remove(path.string() + suffix, ignored);
    }

    cppboot::data::PoolOptions options;
    options.max_size = 4;
    auto database = std::make_shared<cppboot::data::Database>(
        cppboot::data::connectionFactoryFor("sqlite:" + path.generic_string()), options);

    database->execute("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL)");
    database->transaction([](cppboot::data::Transaction& transaction) {
        for (int id = 1; id <= 1000; ++id) {
            transaction.execute("INSERT INTO items (id, name) VALUES ($1, $2)", id,
                                "item-" + std::to_string(id));
        }
    });
    return database;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto router = std::make_shared<Router>();
    const auto database = makeDatabase();

    router->get("/plaintext", [](const Request&) { return Response::text("Hello, World!"); });

    router->get("/json", [](const Request&) {
        const nlohmann::json body = {{"message", "Hello, World!"}};
        return Response::json(body.dump());
    });

    router->get("/users/{id}/posts/{post}", [](const Request& request) {
        return Response::text(*request.param("id") + "/" + *request.param("post"));
    });

    router->get("/db", [database](const Request&) {
        const auto name =
            database->queryValue<std::string>("SELECT name FROM items WHERE id = $1", 500);
        return Response::text(*name);
    });

    cppboot::ApplicationOptions options;
    options.default_settings = {
        {"cppboot.logging.level", "warn"},
        {"cppboot.server.port", "18090"},
        {"cppboot.server.max_connections", "1000000"},
        {"cppboot.server.request_timeout_seconds", "3600"},
    };

    cppboot::Application app(argc, argv, std::move(options));
    app.emplaceModule<ServerModule>(router);
    return app.run();
}
