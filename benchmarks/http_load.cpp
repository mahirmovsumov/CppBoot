// A small HTTP load generator. Each connection sends a request, waits for the response and repeats,
// so concurrency equals the number of connections.
//
//   cppboot-load --connections 100 --seconds 10 --target /plaintext
//   cppboot-load --connections 10000 --hold-idle --seconds 20
//
// With --hold-idle, it opens every connection, prints a HOLDING line, waits, then sends one request
// on each to check that it still works.
#include <algorithm>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using asio::ip::tcp;
using Clock = std::chrono::steady_clock;

constexpr auto kAsTuple = asio::as_tuple(asio::use_awaitable);

// Binding to more loopback addresses gets past the limit on ports per address.
constexpr std::size_t kConnectionsPerSource = 20000;

struct Options {
    std::string host{"127.0.0.1"};
    unsigned short port{18090};
    std::size_t connections{100};
    int seconds{10};
    std::string target{"/plaintext"};
    std::size_t threads{0};
    bool hold_idle{false};
    bool many_sources{false};
    int connects_per_second{5000};
};

/// Only one thread writes each of these, so they need no locks.
struct Stats {
    std::uint64_t connect_errors{0};
    std::uint64_t request_errors{0};
    std::uint64_t bad_statuses{0};
    std::uint64_t connected{0};
    std::uint64_t verified{0};
    std::vector<std::uint32_t> latencies_us;
};

struct Shared {
    Options options;
    std::string request;
    tcp::endpoint server;
};

asio::awaitable<bool> connectSocket(tcp::socket& socket, const Shared& shared, std::size_t index) {
    boost::system::error_code error;
    if (shared.options.many_sources) {
        const auto source_index = static_cast<unsigned>(index / kConnectionsPerSource);
        socket.open(tcp::v4(), error);
        socket.bind(tcp::endpoint{asio::ip::make_address_v4(0x7F000002U + source_index), 0}, error);
        if (error) {
            co_return false;
        }
    }

    auto [connect_error] = co_await socket.async_connect(shared.server, kAsTuple);
    if (connect_error) {
        co_return false;
    }
    socket.set_option(tcp::no_delay(true), error);
    co_return true;
}

/// Sends one request and reads its response. Returns false if the connection failed.
asio::awaitable<bool> exchange(tcp::socket& socket, beast::flat_buffer& buffer,
                               const std::string& request, Stats& stats) {
    auto [write_error, written] =
        co_await asio::async_write(socket, asio::buffer(request), kAsTuple);
    if (write_error) {
        ++stats.request_errors;
        co_return false;
    }

    http::response_parser<http::string_body> parser;
    auto [read_error, read] = co_await http::async_read(socket, buffer, parser, kAsTuple);
    if (read_error) {
        ++stats.request_errors;
        co_return false;
    }
    if (parser.get().result_int() != 200) {
        ++stats.bad_statuses;
    }
    co_return true;
}

asio::awaitable<void> runActive(const Shared& shared, std::size_t index, Clock::time_point start,
                                Clock::time_point deadline, Stats& stats) {
    auto executor = co_await asio::this_coro::executor;

    asio::steady_timer timer{executor, start};
    static_cast<void>(co_await timer.async_wait(kAsTuple));

    tcp::socket socket{executor};
    if (!co_await connectSocket(socket, shared, index)) {
        ++stats.connect_errors;
        co_return;
    }

    beast::flat_buffer buffer;
    while (Clock::now() < deadline) {
        const auto before = Clock::now();
        if (!co_await exchange(socket, buffer, shared.request, stats)) {
            co_return;
        }
        stats.latencies_us.push_back(static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - before).count()));
    }
}

asio::awaitable<void> runIdle(const Shared& shared, std::size_t index, Clock::time_point start,
                              Clock::time_point verify_at, Stats& stats) {
    auto executor = co_await asio::this_coro::executor;

    asio::steady_timer timer{executor, start};
    static_cast<void>(co_await timer.async_wait(kAsTuple));

    tcp::socket socket{executor};
    if (!co_await connectSocket(socket, shared, index)) {
        ++stats.connect_errors;
        co_return;
    }
    ++stats.connected;

    timer.expires_at(verify_at);
    static_cast<void>(co_await timer.async_wait(kAsTuple));

    beast::flat_buffer buffer;
    if (co_await exchange(socket, buffer, shared.request, stats)) {
        ++stats.verified;
    }
}

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

        if (argument == "--host") {
            options.host = next();
        } else if (argument == "--port") {
            options.port = static_cast<unsigned short>(std::stoi(next()));
        } else if (argument == "--connections") {
            options.connections = std::stoull(next());
        } else if (argument == "--seconds") {
            options.seconds = std::stoi(next());
        } else if (argument == "--target") {
            options.target = next();
        } else if (argument == "--threads") {
            options.threads = std::stoull(next());
        } else if (argument == "--connects-per-second") {
            options.connects_per_second = std::stoi(next());
        } else if (argument == "--hold-idle") {
            options.hold_idle = true;
        } else if (argument == "--many-sources") {
            options.many_sources = true;
        } else {
            std::cerr << "Unknown option " << argument << '\n';
            std::exit(2);
        }
    }
    return options;
}

std::uint32_t percentile(const std::vector<std::uint32_t>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0;
    }
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

}  // namespace

int main(int argc, char* argv[]) {
    Shared shared;
    shared.options = parseOptions(argc, argv);
    const Options& options = shared.options;

    shared.request = "GET " + options.target + " HTTP/1.1\r\nHost: " + options.host + "\r\n\r\n";
    shared.server = tcp::endpoint{asio::ip::make_address(options.host), options.port};

    const std::size_t thread_count =
        options.threads != 0 ? options.threads
                             : std::max<std::size_t>(1, std::thread::hardware_concurrency());

    // One event loop per thread, so that each connection and its stats stay on one thread.
    std::vector<std::unique_ptr<asio::io_context>> contexts;
    std::vector<Stats> stats(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        contexts.push_back(std::make_unique<asio::io_context>(1));
    }

    const auto ramp = std::chrono::milliseconds{
        static_cast<std::int64_t>(options.connections) * 1000 / options.connects_per_second};
    const auto begin = Clock::now() + std::chrono::milliseconds{50};
    const auto hold_starts = begin + ramp + std::chrono::seconds{2};
    const auto duration = std::chrono::seconds{options.seconds};
    // An active run measures only after every connection has opened.
    const auto deadline = (options.hold_idle ? hold_starts : begin + ramp) + duration;

    for (std::size_t i = 0; i < options.connections; ++i) {
        const std::size_t owner = i % thread_count;
        const auto start = begin + ramp * static_cast<std::int64_t>(i) /
                                       static_cast<std::int64_t>(options.connections);
        asio::io_context& context = *contexts[owner];
        if (options.hold_idle) {
            asio::co_spawn(context, runIdle(shared, i, start, deadline, stats[owner]),
                           asio::detached);
        } else {
            asio::co_spawn(context, runActive(shared, i, start, deadline, stats[owner]),
                           asio::detached);
        }
    }

    std::vector<std::jthread> threads;
    for (std::size_t i = 0; i < thread_count; ++i) {
        threads.emplace_back([&context = *contexts[i]] { context.run(); });
    }

    if (options.hold_idle) {
        std::this_thread::sleep_until(hold_starts);
        std::cout << "HOLDING" << std::endl;
    }
    threads.clear();

    Stats total;
    for (const Stats& part : stats) {
        total.connect_errors += part.connect_errors;
        total.request_errors += part.request_errors;
        total.bad_statuses += part.bad_statuses;
        total.connected += part.connected;
        total.verified += part.verified;
        total.latencies_us.insert(total.latencies_us.end(), part.latencies_us.begin(),
                                  part.latencies_us.end());
    }

    if (options.hold_idle) {
        std::cout << "connections=" << options.connections << " connected=" << total.connected
                  << " still_working=" << total.verified
                  << " connect_errors=" << total.connect_errors
                  << " request_errors=" << total.request_errors << std::endl;
    } else {
        std::sort(total.latencies_us.begin(), total.latencies_us.end());
        const auto requests = total.latencies_us.size();
        std::cout << "target=" << options.target << " connections=" << options.connections
                  << " requests_per_second=" << requests / static_cast<std::size_t>(options.seconds)
                  << " p50_us=" << percentile(total.latencies_us, 0.50)
                  << " p99_us=" << percentile(total.latencies_us, 0.99)
                  << " max_us=" << percentile(total.latencies_us, 1.0)
                  << " connect_errors=" << total.connect_errors
                  << " request_errors=" << total.request_errors
                  << " bad_statuses=" << total.bad_statuses << std::endl;
    }
    return 0;
}
