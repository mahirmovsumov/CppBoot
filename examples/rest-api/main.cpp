#include <chrono>
#include <cppboot/cppboot.hpp>
#include <cppboot/web/binding.hpp>
#include <cppboot/web/server_module.hpp>
#include <cstdint>
#include <memory>
#include <string>

using namespace cppboot::web;

int main(int argc, char* argv[]) {
    auto router = std::make_shared<Router>();

    // Middleware runs around every request. This adds a header to every response.
    router->use([](const Request& request, const Next& next) {
        const auto start = std::chrono::steady_clock::now();
        Response response = next(request);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        response.setHeader(
            "X-Elapsed-Microseconds",
            std::to_string(
                std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
        return response;
    });

    router->get("/api/users/{id}", [](const Request& request) {
        // Anything but a whole number is answered with a 400 that says so.
        const auto id = pathParam<std::int64_t>(request, "id");

        // Building JSON as a value, rather than joining text, keeps it valid whatever goes in.
        cppboot::Json user = cppboot::Json::object();
        user["id"] = id;
        user["name"] = "user-" + std::to_string(id);
        return Response::json(user.dump());
    });

    router->post("/api/echo",
                 [](const Request& request) { return Response::text(request.body(), 201); });

    cppboot::Application app(argc, argv);
    app.emplaceModule<ServerModule>(router);
    return app.run();
}
