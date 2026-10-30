#include <algorithm>
#include <cctype>
#include <chrono>
#include <cppboot/cppboot.hpp>
#include <cppboot/web/server_module.hpp>
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
        const std::string id = *request.param("id");
        if (!std::all_of(id.begin(), id.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
            throw HttpError(400, "The user ID must be a number");
        }
        return Response::json("{\"id\":" + id + ",\"name\":\"user-" + id + "\"}");
    });

    router->post("/api/echo",
                 [](const Request& request) { return Response::text(request.body(), 201); });

    cppboot::Application app(argc, argv);
    app.emplaceModule<ServerModule>(router);
    return app.run();
}
