// Runs many kinds of client against cppboot-stress-server at once: well-behaved ones that check
// every response, and ones that misbehave. Any response a scenario didn't expect is a failure, so a
// stress run checks correctness as well as survival.
//
//   cppboot-stress-client --seconds 600 --port 18090 [--tls] [--scale 2]
//
// It prints a line every 30 seconds, and exits with 1 if anything unexpected happened.
#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using asio::ip::tcp;
using Clock = std::chrono::steady_clock;
using TlsStream = beast::ssl_stream<tcp::socket>;

constexpr auto kAsTuple = asio::as_tuple(asio::use_awaitable);
constexpr auto kResponseTimeout = std::chrono::seconds{60};
constexpr std::string_view kServerTooBusy = "server-too-busy";

// A server that keeps running but hardly serves anything has still failed. Even a slow sanitizer
// build on a small machine manages hundreds per second.
constexpr std::uint64_t kLeastResultsPerSecond = 50;
constexpr std::uint64_t kMostTooBusyPercent = 10;

struct Options {
    unsigned short port{18090};
    int seconds{60};
    bool tls{false};
    int scale{1};
};

/// Counts results for each scenario. Failures keep their first few messages, to explain a bad run.
class Results {
public:
    void ok(std::string_view scenario) {
        const std::lock_guard lock{mutex_};
        ++counts_[std::string{scenario}].ok;
    }

    void failed(std::string_view scenario, const std::string& message) {
        const std::lock_guard lock{mutex_};
        ++counts_[std::string{scenario}].failed;
        if (messages_.size() < 40) {
            messages_.push_back(std::string{scenario} + ": " + message);
        }
    }

    std::uint64_t totalFailed() const {
        const std::lock_guard lock{mutex_};
        std::uint64_t total = 0;
        for (const auto& [name, count] : counts_) {
            total += count.failed;
        }
        return total;
    }

    /// Returns why the amount of work done is too low, or nothing if it's fine.
    std::optional<std::string> throughputProblem(int seconds) const {
        const std::lock_guard lock{mutex_};
        std::uint64_t total = 0;
        std::uint64_t too_busy = 0;
        for (const auto& [name, count] : counts_) {
            total += count.ok;
            if (name == kServerTooBusy) {
                too_busy = count.ok;
            }
        }
        const std::uint64_t served = total - too_busy;
        if (served < kLeastResultsPerSecond * static_cast<std::uint64_t>(seconds)) {
            return "only " + std::to_string(served) + " results in " + std::to_string(seconds) +
                   " seconds";
        }
        if (too_busy * 100 > total * kMostTooBusyPercent) {
            return "the server turned away " + std::to_string(too_busy) + " of " +
                   std::to_string(total) + " requests";
        }
        return std::nullopt;
    }

    void print(std::ostream& out) const {
        const std::lock_guard lock{mutex_};
        for (const auto& [name, count] : counts_) {
            out << "  " << name << ": ok=" << count.ok << " failed=" << count.failed << '\n';
        }
        for (const std::string& message : messages_) {
            out << "  FAILURE " << message << '\n';
        }
        out.flush();
    }

    void printTotals(std::ostream& out) const {
        const std::lock_guard lock{mutex_};
        std::uint64_t ok_total = 0;
        std::uint64_t failed_total = 0;
        for (const auto& [name, count] : counts_) {
            ok_total += count.ok;
            failed_total += count.failed;
        }
        out << "ok=" << ok_total << " failed=" << failed_total << std::endl;
    }

private:
    struct Count {
        std::uint64_t ok{0};
        std::uint64_t failed{0};
    };

    mutable std::mutex mutex_;
    std::map<std::string, Count, std::less<>> counts_;
    std::vector<std::string> messages_;
};

struct Shared {
    Options options;
    tcp::endpoint server;
    Results results;
    Clock::time_point deadline;
    std::atomic<std::int64_t> next_item{1};
};

/// A connection, plain or TLS, with a timer so that a stuck server fails the scenario.
class Connection {
public:
    Connection(asio::any_io_executor executor, asio::ssl::context* tls, std::uint8_t source_group,
               std::uint8_t source_host)
        : socket_(executor), timer_(executor) {
        if (tls != nullptr) {
            tls_.emplace(tcp::socket{executor}, *tls);
        }
        // Different loopback addresses look like different clients, on systems where any of them
        // can be used. Connecting still works if not.
        source_ = asio::ip::make_address_v4(asio::ip::address_v4::bytes_type{
            127, 1, source_group, static_cast<unsigned char>(source_host | 1U)});
    }

    tcp::socket& socket() { return tls_ ? beast::get_lowest_layer(*tls_) : socket_; }

    asio::awaitable<boost::system::error_code> connect(const tcp::endpoint& server) {
        tcp::socket& raw = socket();
        boost::system::error_code error;
        raw.open(tcp::v4(), error);
        raw.bind(tcp::endpoint{source_, 0}, error);
        if (error) {
            error = {};
            raw.close(error);
            raw.open(tcp::v4(), error);
        }
        auto [connect_error] = co_await raw.async_connect(server, kAsTuple);
        if (connect_error) {
            co_return connect_error;
        }
        raw.set_option(tcp::no_delay(true), error);
        if (tls_) {
            arm();
            auto [handshake_error] =
                co_await tls_->async_handshake(asio::ssl::stream_base::client, kAsTuple);
            timer_.cancel();
            co_return handshake_error;
        }
        co_return boost::system::error_code{};
    }

    /// Sends a request and returns its response, or an error.
    asio::awaitable<std::pair<boost::system::error_code, http::response<http::string_body>>> send(
        http::request<http::string_body> request) {
        request.set(http::field::host, "localhost");
        request.prepare_payload();
        auto [write_error] = tls_ ? co_await writeTo(*tls_, request) : co_await writeTo(socket_, request);
        if (write_error) {
            co_return std::pair{write_error, http::response<http::string_body>{}};
        }
        co_return co_await read();
    }

    asio::awaitable<std::pair<boost::system::error_code, http::response<http::string_body>>> read() {
        http::response_parser<http::string_body> parser;
        parser.body_limit(4 * 1024 * 1024);
        arm();
        boost::system::error_code error;
        if (tls_) {
            std::tie(error, std::ignore) =
                co_await http::async_read(*tls_, buffer_, parser, kAsTuple);
        } else {
            std::tie(error, std::ignore) =
                co_await http::async_read(socket_, buffer_, parser, kAsTuple);
        }
        timer_.cancel();
        co_return std::pair{error, parser.release()};
    }

    asio::awaitable<boost::system::error_code> sendRaw(std::string_view text) {
        boost::system::error_code error;
        if (tls_) {
            std::tie(error, std::ignore) =
                co_await asio::async_write(*tls_, asio::buffer(text), kAsTuple);
        } else {
            std::tie(error, std::ignore) =
                co_await asio::async_write(socket_, asio::buffer(text), kAsTuple);
        }
        co_return error;
    }

    /// Closes at once, without the wait that would otherwise hold a port for minutes.
    void abort() {
        boost::system::error_code ignored;
        tcp::socket& raw = socket();
        raw.set_option(asio::socket_base::linger(true, 0), ignored);
        raw.close(ignored);
        timer_.cancel();
    }

private:
    template <typename Stream>
    asio::awaitable<std::tuple<boost::system::error_code>> writeTo(
        Stream& stream, http::request<http::string_body>& request) {
        arm();
        auto [error, written] = co_await http::async_write(stream, request, kAsTuple);
        timer_.cancel();
        co_return std::tuple{error};
    }

    void arm() {
        timer_.expires_after(kResponseTimeout);
        timer_.async_wait([this](const boost::system::error_code& error) {
            if (!error) {
                boost::system::error_code ignored;
                socket().close(ignored);
            }
        });
    }

    tcp::socket socket_;
    std::optional<TlsStream> tls_;
    asio::steady_timer timer_;
    asio::ip::address_v4 source_;
    beast::flat_buffer buffer_;
};

class Worker {
public:
    Worker(Shared& shared, asio::ssl::context* tls, std::uint32_t seed)
        : shared_(shared), tls_(tls), random_(seed) {}

    int between(int low, int high) { return std::uniform_int_distribution<int>{low, high}(random_); }

    bool running() const { return Clock::now() < shared_.deadline; }

    asio::awaitable<void> pause(std::chrono::milliseconds length) {
        asio::steady_timer timer{co_await asio::this_coro::executor, length};
        static_cast<void>(co_await timer.async_wait(kAsTuple));
    }

    /// Opens a connection, from one loopback address unless many_sources is set.
    asio::awaitable<std::unique_ptr<Connection>> open(std::string_view scenario,
                                                      bool many_sources = false) {
        auto connection = std::make_unique<Connection>(
            co_await asio::this_coro::executor, tls_,
            static_cast<std::uint8_t>(many_sources ? between(1, 200) : 0),
            static_cast<std::uint8_t>(many_sources ? between(1, 254) : 1));
        const auto error = co_await connection->connect(shared_.server);
        if (error) {
            shared_.results.failed(scenario, "connect: " + error.message());
            co_await pause(std::chrono::milliseconds{50});
            co_return nullptr;
        }
        co_return connection;
    }

    static http::request<http::string_body> request(http::verb method, std::string target,
                                                    std::string body = {}) {
        http::request<http::string_body> result{method, target, 11};
        result.body() = std::move(body);
        return result;
    }

    /// Checks a response, and returns false if the connection can't be used again.
    bool check(std::string_view scenario, const boost::system::error_code& error,
               const http::response<http::string_body>& response, int status,
               std::optional<std::string_view> body = std::nullopt) {
        if (error) {
            shared_.results.failed(scenario, "request: " + error.message());
            return false;
        }
        // The server is allowed to turn work away when it's overloaded, if it says so properly.
        if (response.result_int() == 503 && !response[http::field::retry_after].empty()) {
            shared_.results.ok(kServerTooBusy);
            turned_away_ = true;
            return true;
        }
        if (response.result_int() != static_cast<unsigned>(status)) {
            shared_.results.failed(scenario, "status " + std::to_string(response.result_int()) +
                                                 ", expected " + std::to_string(status));
            return true;
        }
        if (body && response.body() != *body) {
            shared_.results.failed(scenario, "wrong body, of " +
                                                 std::to_string(response.body().size()) + " bytes");
            return true;
        }
        shared_.results.ok(scenario);
        return true;
    }

    asio::awaitable<void> keepAlive() {
        while (running()) {
            auto connection = co_await open("keep-alive");
            if (!connection) {
                continue;
            }
            for (int i = 0; running() && i < between(1, 2000); ++i) {
                auto [error, response] =
                    co_await connection->send(request(http::verb::get, "/plaintext"));
                if (!check("keep-alive", error, response, 200, "Hello, World!")) {
                    break;
                }
                if (response[http::field::server] != "") {
                    shared_.results.failed("keep-alive", "unexpected Server header");
                }
            }
            connection->abort();
        }
    }

    asio::awaitable<void> routes() {
        while (running()) {
            auto connection = co_await open("routes");
            if (!connection) {
                continue;
            }
            for (int i = 0; running() && i < 200; ++i) {
                const std::string id = std::to_string(between(1, 1000000));
                const std::string post = std::to_string(between(1, 1000000));
                auto [error, response] = co_await connection->send(
                    request(http::verb::get, "/users/" + id + "/posts/" + post));
                if (!check("routes", error, response, 200, id + "/" + post)) {
                    break;
                }

                auto [json_error, json] =
                    co_await connection->send(request(http::verb::get, "/json?page=" + id));
                if (!check("routes", json_error, json, 200)) {
                    break;
                }
                if (json.result_int() == 200 &&
                    json.body().find("\"page\":\"" + id + "\"") == std::string::npos) {
                    shared_.results.failed("routes", "JSON from another request: " + json.body());
                }

                // Paths nobody registered. A leak per path would show in the server's memory.
                auto [missing_error, missing] = co_await connection->send(
                    request(http::verb::get, "/invented/" + id + "/" + post));
                if (!check("routes", missing_error, missing, 404)) {
                    break;
                }
            }
            connection->abort();
        }
    }

    /// Each worker sends bodies of its own, so a response mixed up between connections shows.
    asio::awaitable<void> echo() {
        while (running()) {
            auto connection = co_await open("echo");
            if (!connection) {
                continue;
            }
            for (int i = 0; running() && i < 100; ++i) {
                const auto size = static_cast<std::size_t>(between(0, 3) == 0 ? between(0, 512 * 1024)
                                                                              : between(0, 4096));
                std::string body(size, static_cast<char>('a' + between(0, 25)));
                if (!body.empty()) {
                    body.front() = '<';
                    body.back() = '>';
                }
                const auto started = Clock::now();
                auto [error, response] =
                    co_await connection->send(request(http::verb::post, "/echo", body));
                if (error) {
                    // How long it took shows whether the server's request timeout ended it.
                    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now() - started);
                    shared_.results.failed("echo", "request: " + error.message() + ", after " +
                                                       std::to_string(took.count()) + " ms, with " +
                                                       std::to_string(size) + " bytes");
                    break;
                }
                if (!check("echo", error, response, 200, body)) {
                    break;
                }
            }
            connection->abort();
        }
    }

    asio::awaitable<void> database() {
        while (running()) {
            auto connection = co_await open("database");
            if (!connection) {
                continue;
            }
            for (int i = 0; running() && i < 100; ++i) {
                const std::string id = std::to_string(shared_.next_item.fetch_add(1));
                const std::string body = "body of item " + id;
                const std::string path = "/items/" + id;

                // Each step relies on the one before. A step the server turned away didn't happen, so
                // the sequence starts again with a new item.
                auto [add_error, added] =
                    co_await connection->send(request(http::verb::post, path, body));
                if (!check("database", add_error, added, 201)) {
                    break;
                }
                if (wasTurnedAway()) {
                    continue;
                }
                auto [read_error, read] = co_await connection->send(request(http::verb::get, path));
                if (!check("database", read_error, read, 200, body)) {
                    break;
                }
                auto [again_error, again] =
                    co_await connection->send(request(http::verb::post, path, body));
                if (!check("database-duplicate", again_error, again, 409)) {
                    break;
                }
                auto [gone_error, gone] = co_await connection->send(request(http::verb::delete_, path));
                if (!check("database", gone_error, gone, 204)) {
                    break;
                }
                if (wasTurnedAway()) {
                    continue;
                }

                // A rolled-back insert must leave nothing behind.
                auto [back_error, back] =
                    co_await connection->send(request(http::verb::post, path + "/rollback", body));
                if (!check("database-rollback", back_error, back, 422)) {
                    break;
                }
                auto [none_error, none] = co_await connection->send(request(http::verb::get, path));
                if (!check("database-rollback", none_error, none, 404)) {
                    break;
                }
                wasTurnedAway();
            }
            connection->abort();
        }
    }

    asio::awaitable<void> failures() {
        while (running()) {
            auto connection = co_await open("failures");
            if (!connection) {
                continue;
            }
            for (int i = 0; running() && i < 200; ++i) {
                auto [boom_error, boom] = co_await connection->send(request(http::verb::get, "/boom"));
                if (!check("failures", boom_error, boom, 500)) {
                    break;
                }
                if (boom.body().find("on purpose") != std::string::npos) {
                    shared_.results.failed("failures", "a 500 showed internal details");
                }
                auto [pot_error, pot] = co_await connection->send(request(http::verb::get, "/teapot"));
                if (!check("failures", pot_error, pot, 418)) {
                    break;
                }
                auto [method_error, method] =
                    co_await connection->send(request(http::verb::post, "/plaintext"));
                if (!check("failures", method_error, method, 405)) {
                    break;
                }
                auto [bad_error, bad] =
                    co_await connection->send(request(http::verb::get, "/users/%zz/posts/1"));
                if (!check("failures", bad_error, bad, 400)) {
                    break;
                }
            }
            connection->abort();
        }
    }

    asio::awaitable<void> newConnections() {
        while (running()) {
            auto connection = co_await open("new-connections", true);
            if (!connection) {
                continue;
            }
            auto [error, response] = co_await connection->send(request(http::verb::get, "/plaintext"));
            check("new-connections", error, response, 200, "Hello, World!");
            connection->abort();
            if (shared_.options.tls) {
                co_await pause(std::chrono::milliseconds{between(0, 20)});
            }
        }
    }

    /// From many addresses, so that the limiter has to keep and then forget many clients.
    asio::awaitable<void> rateLimited() {
        while (running()) {
            auto connection = co_await open("rate-limited", true);
            if (!connection) {
                continue;
            }
            auto [error, response] = co_await connection->send(request(http::verb::get, "/limited"));
            if (error) {
                shared_.results.failed("rate-limited", error.message());
            } else if (response.result_int() == 200 ||
                       ((response.result_int() == 429 || response.result_int() == 503) &&
                        !response[http::field::retry_after].empty())) {
                shared_.results.ok("rate-limited");
            } else {
                shared_.results.failed("rate-limited",
                                       "status " + std::to_string(response.result_int()));
            }
            connection->abort();
            co_await pause(std::chrono::milliseconds{between(1, 10)});
        }
    }

    asio::awaitable<void> metrics() {
        while (running()) {
            auto connection = co_await open("metrics");
            if (connection) {
                auto [error, response] =
                    co_await connection->send(request(http::verb::get, "/actuator/metrics"));
                check("metrics", error, response, 200);
                if (!error && response.result_int() == 200 &&
                    response.body().find("stress_middleware_total") == std::string::npos) {
                    shared_.results.failed("metrics", "a metric is missing");
                }
                // Invented paths must all count as one route, or this would grow forever.
                if (response.body().find("/invented") != std::string::npos) {
                    shared_.results.failed("metrics", "metrics contain a path a client chose");
                }
                auto [health_error, health] =
                    co_await connection->send(request(http::verb::get, "/actuator/health"));
                check("metrics", health_error, health, 200);
                connection->abort();
            }
            co_await pause(std::chrono::seconds{1});
        }
    }

    /// Goes away at every awkward moment. All that matters is that the server keeps working.
    asio::awaitable<void> abandon() {
        while (running()) {
            auto connection = co_await open("abandon", true);
            if (!connection) {
                continue;
            }
            switch (between(0, 3)) {
                case 0:  // Half a request.
                    co_await connection->sendRaw("GET /plaintext HTTP/1.1\r\nHost: loc");
                    break;
                case 1:  // Half a body.
                    co_await connection->sendRaw(
                        "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1000\r\n\r\nabc");
                    break;
                case 2:  // Leaves while its handler is running.
                    co_await connection->sendRaw("GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n");
                    co_await pause(std::chrono::milliseconds{between(0, 30)});
                    break;
                default:  // Connects, and says nothing.
                    break;
            }
            connection->abort();
            shared_.results.ok("abandon");
            co_await pause(std::chrono::milliseconds{between(1, 20)});
        }
    }

    /// Holds a connection until the server's request timeout closes it.
    asio::awaitable<void> slowClient() {
        while (running()) {
            auto connection = co_await open("slow-client");
            if (!connection) {
                continue;
            }
            co_await connection->sendRaw("GET /plaintext HTTP/1.1\r\nX-Slow: ");
            const auto start = Clock::now();
            auto [error, response] = co_await connection->read();
            // The server closes it. Only waiting far longer than its timeout is wrong.
            if (Clock::now() - start > std::chrono::seconds{45}) {
                shared_.results.failed("slow-client", "the server didn't time it out");
            } else {
                shared_.results.ok("slow-client");
            }
            connection->abort();
        }
    }

    asio::awaitable<void> malformed() {
        static constexpr std::array<std::string_view, 7> kRequests = {
            "NOT HTTP AT ALL\r\n\r\n",
            "GET\r\n\r\n",
            "GET /plaintext HTTP/9.9\r\nHost: x\r\n\r\n",
            "GET /plaintext HTTP/1.1\r\nBad Header\r\n\r\n",
            "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: nonsense\r\n\r\n",
            "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999\r\n\r\n",
            "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n",
        };
        while (running()) {
            auto connection = co_await open("malformed", true);
            if (!connection) {
                continue;
            }
            co_await connection->sendRaw(
                kRequests[static_cast<std::size_t>(between(0, kRequests.size() - 1))]);
            auto [error, response] = co_await connection->read();
            // Either way is fine, except a success: garbage must not reach a handler.
            if (!error && response.result_int() == 200) {
                shared_.results.failed("malformed", "a malformed request succeeded");
            } else {
                shared_.results.ok("malformed");
            }
            connection->abort();
            co_await pause(std::chrono::milliseconds{between(1, 20)});
        }
    }

    asio::awaitable<void> oversized() {
        while (running()) {
            auto connection = co_await open("oversized");
            if (!connection) {
                continue;
            }
            auto [error, response] = co_await connection->send(
                request(http::verb::post, "/echo", std::string(2 * 1024 * 1024, 'x')));
            // The server may close before it has read everything, which the client sees as an error.
            if (!error && response.result_int() != 413 && response.result_int() != 503) {
                shared_.results.failed("oversized",
                                       "status " + std::to_string(response.result_int()));
            } else {
                shared_.results.ok("oversized");
            }
            connection->abort();
            co_await pause(std::chrono::milliseconds{between(10, 100)});
        }
    }

    /// Bad handshakes, for HTTPS runs: bytes that aren't TLS, or a client that goes away.
    asio::awaitable<void> badHandshakes() {
        auto executor = co_await asio::this_coro::executor;
        while (running()) {
            tcp::socket socket{executor};
            auto [connect_error] = co_await socket.async_connect(shared_.server, kAsTuple);
            if (connect_error) {
                shared_.results.failed("bad-handshakes", connect_error.message());
                continue;
            }
            if (between(0, 1) == 0) {
                static_cast<void>(co_await asio::async_write(
                    socket, asio::buffer(std::string_view{"GET / HTTP/1.1\r\n\r\n"}), kAsTuple));
                std::array<char, 256> bytes{};
                static_cast<void>(co_await socket.async_read_some(asio::buffer(bytes), kAsTuple));
            }
            boost::system::error_code ignored;
            socket.set_option(asio::socket_base::linger(true, 0), ignored);
            socket.close(ignored);
            shared_.results.ok("bad-handshakes");
            co_await pause(std::chrono::milliseconds{between(1, 30)});
        }
    }

private:
    /// Returns whether the server turned away any request since the last time this was called.
    bool wasTurnedAway() { return std::exchange(turned_away_, false); }

    Shared& shared_;
    asio::ssl::context* tls_;
    std::mt19937 random_;
    bool turned_away_{false};
};

Options parseOptions(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing a value for " << argument << '\n';
                std::exit(2);
            }
            return argv[++i];
        };
        if (argument == "--port") {
            options.port = static_cast<unsigned short>(std::stoi(next()));
        } else if (argument == "--seconds") {
            options.seconds = std::stoi(next());
        } else if (argument == "--scale") {
            options.scale = std::stoi(next());
        } else if (argument == "--tls") {
            options.tls = true;
        } else {
            std::cerr << "Unknown option " << argument << '\n';
            std::exit(2);
        }
    }
    return options;
}

}  // namespace

int main(int argc, char* argv[]) {
    Shared shared;
    shared.options = parseOptions(argc, argv);
    shared.server = tcp::endpoint{asio::ip::make_address("127.0.0.1"), shared.options.port};
    shared.deadline = Clock::now() + std::chrono::seconds{shared.options.seconds};

    asio::io_context io{1};
    asio::ssl::context tls{asio::ssl::context::tls_client};
    asio::ssl::context* const tls_pointer = shared.options.tls ? &tls : nullptr;

    using Scenario = asio::awaitable<void> (Worker::*)();
    const struct {
        Scenario scenario;
        int count;
    } mix[] = {
        {&Worker::keepAlive, 8},  {&Worker::routes, 4},        {&Worker::echo, 4},
        {&Worker::database, 8},   {&Worker::failures, 2},      {&Worker::newConnections, 4},
        {&Worker::rateLimited, 2}, {&Worker::metrics, 1},      {&Worker::abandon, 4},
        {&Worker::slowClient, 8}, {&Worker::malformed, 2},     {&Worker::oversized, 1},
        {&Worker::badHandshakes, 2},
    };

    std::vector<std::unique_ptr<Worker>> workers;
    std::uint32_t seed = 1;
    for (const auto& entry : mix) {
        const bool only_for_tls = entry.scenario == &Worker::badHandshakes;
        if (only_for_tls && !shared.options.tls) {
            continue;
        }
        for (int i = 0; i < entry.count * shared.options.scale; ++i) {
            workers.push_back(std::make_unique<Worker>(shared, tls_pointer, seed++));
            Worker* const worker = workers.back().get();
            asio::co_spawn(io, (worker->*entry.scenario)(), [&shared](std::exception_ptr failure) {
                if (failure) {
                    try {
                        std::rethrow_exception(failure);
                    } catch (const std::exception& exception) {
                        shared.results.failed("client", exception.what());
                    }
                }
            });
        }
    }

    // Reports progress from its own thread, so a stuck event loop still shows.
    std::atomic<bool> finished{false};
    std::jthread progress{[&] {
        auto next = Clock::now() + std::chrono::seconds{30};
        while (!finished.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            if (Clock::now() >= next) {
                next += std::chrono::seconds{30};
                shared.results.printTotals(std::cout);
            }
        }
    }};

    io.run();
    finished.store(true);
    progress.join();

    shared.results.print(std::cout);
    const auto failed = shared.results.totalFailed();
    const auto throughput_problem = shared.results.throughputProblem(shared.options.seconds);
    if (throughput_problem) {
        std::cout << "FAILURE too little work: " << *throughput_problem << std::endl;
    }
    const bool passed = failed == 0 && !throughput_problem;
    std::cout << (passed ? "PASSED" : "FAILED") << " with " << failed << " unexpected results"
              << std::endl;
    return passed ? 0 : 1;
}
