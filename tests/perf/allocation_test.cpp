// Counts the heap allocations that the framework makes for each request, and fails if a kind of
// request takes more than its budget. An allocation is a lock and a cache miss in the making, so
// this number is the one to watch, and unlike a timing it's the same on every run.
//
// It replaces the global operator new, so it's a program of its own, and isn't built with
// sanitizers, which replace it too.
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <charconv>
#include <cppboot/base/json.hpp>
#include <cppboot/web/router.hpp>
#include <cppboot/web/server.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

std::atomic<std::uint64_t> allocations{0};

void* allocate(std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* const memory = std::malloc(size != 0 ? size : 1)) {
        return memory;
    }
    throw std::bad_alloc{};
}

}  // namespace

// The aligned forms aren't replaced, so they keep working with their own matching delete. Nothing
// in the request path uses them.
void* operator new(std::size_t size) {
    return allocate(size);
}
void* operator new[](std::size_t size) {
    return allocate(size);
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete[](void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

using namespace cppboot;
using namespace cppboot::web;
namespace asio = boost::asio;
using asio::ip::tcp;

// A vector that grows moves its items only if moving can't throw, and copies them otherwise.
static_assert(std::is_nothrow_move_constructible_v<Request>);
static_assert(std::is_nothrow_move_assignable_v<Request>);
static_assert(std::is_nothrow_move_constructible_v<Response>);
static_assert(std::is_nothrow_move_assignable_v<Response>);
static_assert(std::is_nothrow_move_constructible_v<Json>);
static_assert(std::is_nothrow_move_assignable_v<Json>);

constexpr int kWarmUp = 200;
constexpr int kMeasured = 2000;

/// What a browser sends, more or less: the headers are most of a request without a body.
constexpr std::string_view kHeaders =
    "Host: localhost:8080\r\n"
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:130.0) Gecko/20100101 Firefox/130.0\r\n"
    "Accept: application/json, text/plain, */*\r\n"
    "Accept-Language: en-US,en;q=0.5\r\n"
    "Accept-Encoding: gzip, deflate, br\r\n"
    "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJhZGEifQ.c2lnbmF0dXJlLXNpZ25hdHVyZQ\r\n"
    "Connection: keep-alive\r\n"
    "Cookie: session=0123456789abcdef0123456789abcdef; theme=dark\r\n";

struct Scenario {
    std::string_view name;
    std::string request;
    /// The most allocations a request may take, with the server and with the router alone.
    double server_budget;
    double router_budget;
    Method method;
    std::string_view target;
    std::string_view body;
};

std::string get(std::string_view target) {
    return "GET " + std::string{target} + " HTTP/1.1\r\n" + std::string{kHeaders} + "\r\n";
}

std::string post(std::string_view target, std::string_view body) {
    return "POST " + std::string{target} + " HTTP/1.1\r\n" + std::string{kHeaders} +
           "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + std::string{body};
}

void addRoutes(Router& router) {
    router.get("/plaintext", [](const Request&) { return Response::text("Hello, World!"); });
    router.get("/users/{id}/posts/{post}", [](const Request& request) {
        return Response::text(request.param("id").has_value() &&
                                      request.param("post").has_value()
                                  ? "found"
                                  : "missing");
    });
    router.post("/echo", [](const Request& request) { return Response::json(request.body()); });
    // Other routes of the same length, as a real API has, which the router has to look past.
    for (const char* other : {"/a/{id}/b/{x}", "/c/{id}/d/{x}", "/e/{id}/f/{x}", "/health",
                              "/metrics", "/items"}) {
        router.get(other, [](const Request&) { return Response{204}; });
    }
    // Middleware that passes every request on, as most do most of the time.
    for (int i = 0; i < 2; ++i) {
        router.use([](const Request& request, const Next& next) { return next(request); });
    }
}

/// Sends one request and reads its whole response, without allocating.
bool exchange(tcp::socket& socket, std::string_view request, std::array<char, 16384>& buffer) {
    boost::system::error_code error;
    asio::write(socket, asio::buffer(request), error);
    if (error) {
        return false;
    }

    std::size_t size = 0;
    while (true) {
        const std::string_view received{buffer.data(), size};
        const auto headers_end = received.find("\r\n\r\n");
        if (headers_end != std::string_view::npos) {
            constexpr std::string_view kLength = "Content-Length: ";
            const auto field = received.find(kLength);
            std::size_t length = 0;
            if (field != std::string_view::npos && field < headers_end) {
                const char* const start = received.data() + field + kLength.size();
                std::from_chars(start, received.data() + headers_end, length);
            }
            if (size >= headers_end + 4 + length) {
                return received.starts_with("HTTP/1.1 200");
            }
        }
        if (size == buffer.size()) {
            return false;
        }
        size += socket.read_some(asio::buffer(buffer.data() + size, buffer.size() - size), error);
        if (error) {
            return false;
        }
    }
}

struct Measurement {
    bool worked;
    double per_request;
};

Measurement measureServer(std::uint16_t port, const Scenario& scenario) {
    asio::io_context io;
    tcp::socket socket{io};
    socket.connect(tcp::endpoint{asio::ip::make_address("127.0.0.1"), port});
    socket.set_option(tcp::no_delay{true});
    std::array<char, 16384> buffer{};

    for (int i = 0; i < kWarmUp; ++i) {
        if (!exchange(socket, scenario.request, buffer)) {
            return {false, 0};
        }
    }
    const auto before = allocations.load();
    for (int i = 0; i < kMeasured; ++i) {
        if (!exchange(socket, scenario.request, buffer)) {
            return {false, 0};
        }
    }
    return {true, static_cast<double>(allocations.load() - before) / kMeasured};
}

/// Routing and handling alone, for a request that already exists: what a worker thread does.
Measurement measureRouter(const Router& router, const Scenario& scenario) {
    std::vector<Request> requests;
    requests.reserve(kMeasured);
    for (int i = 0; i < kMeasured; ++i) {
        auto request = Request::create(scenario.method, scenario.target, {},
                                       std::string{scenario.body});
        if (!request) {
            return {false, 0};
        }
        requests.push_back(std::move(request).value());
    }

    bool worked = true;
    const auto before = allocations.load();
    for (Request& request : requests) {
        worked = router.handle(std::move(request)).status() == 200 && worked;
    }
    return {worked, static_cast<double>(allocations.load() - before) / kMeasured};
}

}  // namespace

int main() {
    const std::string body = R"({"name":"Ada","email":"ada@example.com","roles":["admin","user"]})";
    // The router's budgets are what it takes today. The server's have room for two more, because
    // what Asio allocates differs a little between operating systems.
    //
    // What's left is the request's own data, in one piece each: the memory that its header fields
    // are parsed into, its headers, its path segments, and the response's header list and content
    // type. A longer path, a query string, path parameters and a body each add one.
    const std::vector<Scenario> scenarios{
        {"GET /plaintext", get("/plaintext"), 7, 2, Method::Get, "/plaintext", {}},
        {"GET /users/{id}/posts/{post}", get("/users/42/posts/7?page=2&size=20"), 11, 4,
         Method::Get, "/users/42/posts/7?page=2&size=20", {}},
        {"POST /echo", post("/echo", body), 9, 3, Method::Post, "/echo", body},
    };

    Router router;
    addRoutes(router);

    ServerOptions options;
    options.port = 0;
    options.io_threads = 1;
    options.worker_threads = 2;
    Server server{router, options};
    if (auto started = server.start(); !started) {
        std::printf("Cannot start the server: %s\n", started.error().message().c_str());
        return 1;
    }

    bool passed = true;
    std::printf("Allocations for each request\n\n");
    std::printf("%-32s %10s %8s   %10s %8s\n", "Request", "server", "budget", "router", "budget");
    for (const Scenario& scenario : scenarios) {
        const Measurement with_server = measureServer(server.port(), scenario);
        const Measurement with_router = measureRouter(router, scenario);
        const bool within = with_server.worked && with_router.worked &&
                            with_server.per_request <= scenario.server_budget &&
                            with_router.per_request <= scenario.router_budget;
        passed = passed && within;
        std::printf("%-32s %10.1f %8.0f   %10.1f %8.0f   %s\n", scenario.name.data(),
                    with_server.per_request, scenario.server_budget, with_router.per_request,
                    scenario.router_budget,
                    !with_server.worked || !with_router.worked ? "FAILED TO RUN"
                    : within                                    ? "ok"
                                                                : "OVER BUDGET");
    }
    server.stop();

    if (!passed) {
        std::printf("\nA request allocates more than its budget. Find what was added to the "
                    "request path, or raise the budget in tests/perf/allocation_test.cpp if the "
                    "cost is worth it.\n");
    }
    return passed ? 0 : 1;
}
