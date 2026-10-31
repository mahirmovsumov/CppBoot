#include "cppboot/web/server.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <charconv>
#include <iterator>
#include <list>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cppboot::web {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using asio::ip::tcp;

constexpr auto kAsTuple = asio::as_tuple(asio::use_awaitable);

// One waiting accept takes one connection per turn of the event loop, which is only a few hundred
// per second under load, so new clients would queue for seconds while existing ones are served.
constexpr int kConcurrentAccepts = 64;

http::response<http::string_body> toBeastResponse(const Response& response, unsigned version,
                                                  bool keep_alive, bool head_request) {
    http::response<http::string_body> result{static_cast<http::status>(response.status()), version};
    for (const auto& [name, value] : response.headers()) {
        result.set(name, value);
    }
    result.keep_alive(keep_alive);

    if (head_request) {
        // A HEAD response reports the size of the body it would have sent.
        result.content_length(response.body().size());
    } else {
        result.body() = response.body();
        result.prepare_payload();
    }
    return result;
}

Result<Request> toRequest(http::request<http::string_body>& request) {
    const auto method = parseMethod(request.method_string());
    if (!method) {
        return Error{ErrorCode::InvalidArgument, "Unsupported method"};
    }

    Headers headers;
    for (const auto& field : request) {
        headers.emplace_back(field.name_string(), field.value());
    }
    return Request::create(*method, request.target(), std::move(headers),
                           std::move(request.body()));
}

asio::awaitable<Response> runHandler(const Router& router, Request request) {
    co_return router.handle(std::move(request));
}

/// Keeps per-connection state on the network thread, which is the only thread to touch it.
struct Connection {
    Connection(tcp::socket socket, std::string peer_in, bool peer_is_trusted_in)
        : stream(std::move(socket)),
          peer(std::move(peer_in)),
          peer_is_trusted(peer_is_trusted_in) {}

    beast::tcp_stream stream;
    std::string peer;
    bool peer_is_trusted;
    bool busy{false};
};

/// Writes an IPv4 client as IPv4 even on a dual-stack socket, so that it has one spelling.
asio::ip::address normalized(const asio::ip::address& address) {
    if (!address.is_v6()) {
        return address;
    }
    asio::ip::address_v6 v6 = address.to_v6();
    if (v6.is_v4_mapped()) {
        return asio::ip::make_address_v4(asio::ip::v4_mapped, v6);
    }
    v6.scope_id(0);
    return v6;
}

/// An address range, with IPv4 held in its IPv6-mapped form so that both compare the same way.
struct AddressRange {
    asio::ip::address_v6::bytes_type bytes;
    unsigned prefix_bits;

    bool contains(const asio::ip::address& address) const {
        const asio::ip::address plain = normalized(address);
        const auto other =
            plain.is_v4() ? asio::ip::make_address_v6(asio::ip::v4_mapped, plain.to_v4()).to_bytes()
                          : plain.to_v6().to_bytes();

        for (unsigned bit = 0; bit < prefix_bits; ++bit) {
            const unsigned mask = 0x80U >> (bit % 8);
            if (((bytes[bit / 8] ^ other[bit / 8]) & mask) != 0) {
                return false;
            }
        }
        return true;
    }
};

Result<AddressRange> parseAddressRange(std::string_view text) {
    const auto invalid = [&](std::string_view reason) {
        return Error{ErrorCode::InvalidArgument, "Invalid trusted proxy '" + std::string{text} +
                                                     "': " + std::string{reason}};
    };

    const auto slash = text.find('/');
    boost::system::error_code error;
    const asio::ip::address parsed =
        normalized(asio::ip::make_address(text.substr(0, slash), error));
    if (error) {
        return invalid("it isn't an IP address");
    }

    const unsigned address_bits = parsed.is_v4() ? 32 : 128;
    unsigned prefix_bits = address_bits;
    if (slash != std::string_view::npos) {
        const std::string_view digits = text.substr(slash + 1);
        const char* const end = digits.data() + digits.size();
        const auto [parsed_end, parse_error] = std::from_chars(digits.data(), end, prefix_bits);
        if (parse_error != std::errc{} || parsed_end != end || prefix_bits > address_bits) {
            return invalid("its prefix length is wrong");
        }
    }
    if (prefix_bits == 0) {
        // This would let any client choose the address it appears to come from.
        return invalid("a proxy range can't cover every address");
    }

    if (parsed.is_v4()) {
        return AddressRange{asio::ip::make_address_v6(asio::ip::v4_mapped, parsed.to_v4()).to_bytes(),
                            prefix_bits + 96};
    }
    return AddressRange{parsed.to_v6().to_bytes(), prefix_bits};
}

bool isTrusted(const std::vector<AddressRange>& ranges, const asio::ip::address& address) {
    return std::any_of(ranges.begin(), ranges.end(),
                       [&](const AddressRange& range) { return range.contains(address); });
}

/// Each proxy adds the address it saw to the end of X-Forwarded-For, and a client can write
/// anything before that. So this reads from the end, and the client is the first address that isn't
/// a trusted proxy. If an entry can't be read, the last address it could believe is used.
std::string forwardedClient(const http::request<http::string_body>& request,
                            const std::vector<AddressRange>& ranges, const std::string& peer) {
    std::string entries;
    const auto fields = request.equal_range("X-Forwarded-For");
    for (auto field = fields.first; field != fields.second; ++field) {
        entries += field->value();
        entries += ',';
    }

    std::string client = peer;
    std::string_view rest = entries;
    while (!rest.empty()) {
        const auto comma = rest.rfind(',');
        std::string_view entry = comma == std::string_view::npos ? rest : rest.substr(comma + 1);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(0, comma);

        entry.remove_prefix(std::min(entry.find_first_not_of(" \t"), entry.size()));
        entry = entry.substr(0, entry.find_first_of(" \t"));
        if (entry.empty()) {
            continue;
        }

        boost::system::error_code error;
        const asio::ip::address address = asio::ip::make_address(entry, error);
        if (error) {
            break;
        }
        client = normalized(address).to_string();
        if (!isTrusted(ranges, address)) {
            break;
        }
    }
    return client;
}

}  // namespace

struct Server::Impl {
    Impl(const Router& router_in, ServerOptions options_in, LogFunction log_error_in)
        : router(router_in),
          options(std::move(options_in)),
          log_error(std::move(log_error_in)),
          acceptor(io),
          drain_timer(io),
          workers(options.worker_threads != 0
                      ? options.worker_threads
                      : std::max<std::size_t>(1, std::thread::hardware_concurrency())) {}

    void logError(std::string_view message) const {
        if (log_error) {
            log_error(message);
        }
    }

    asio::awaitable<void> accept() {
        while (true) {
            auto [error, socket] = co_await acceptor.async_accept(kAsTuple);
            if (stopping) {
                co_return;
            }
            if (error) {
                // Usually a temporary shortage, such as running out of file handles.
                logError("Accepting a connection failed: " + error.message());
                asio::steady_timer pause{io, std::chrono::milliseconds{100}};
                static_cast<void>(co_await pause.async_wait(kAsTuple));
                continue;
            }

            if (connections.size() >= options.max_connections) {
                boost::system::error_code ignored;
                socket.close(ignored);
                continue;
            }

            boost::system::error_code endpoint_error;
            const tcp::endpoint remote = socket.remote_endpoint(endpoint_error);
            if (endpoint_error) {
                // The client has already gone.
                continue;
            }

            connections.emplace_back(std::move(socket),
                                     normalized(remote.address()).to_string(),
                                     isTrusted(trusted_proxies, remote.address()));
            asio::co_spawn(io, serve(std::prev(connections.end())), asio::detached);
        }
    }

    asio::awaitable<void> serve(std::list<Connection>::iterator connection) {
        beast::flat_buffer buffer;

        while (!stopping) {
            http::request_parser<http::string_body> parser;
            parser.body_limit(options.max_body_bytes);

            connection->busy = false;
            connection->stream.expires_after(options.request_timeout);
            auto [read_error, read_size] =
                co_await http::async_read(connection->stream, buffer, parser, kAsTuple);
            connection->busy = true;

            if (read_error) {
                if (read_error == http::error::body_limit) {
                    co_await sendError(*connection, 413);
                } else if (read_error.category() ==
                               beast::error_code{http::error::bad_method}.category() &&
                           read_error != http::error::end_of_stream &&
                           read_error != http::error::partial_message) {
                    co_await sendError(*connection, 400);
                }
                break;
            }

            auto& beast_request = parser.get();
            const unsigned version = beast_request.version();
            const bool wants_keep_alive = beast_request.keep_alive();
            const bool head_request = beast_request.method() == http::verb::head;

            auto request = toRequest(beast_request);
            if (request) {
                request.value().setClientAddress(
                    connection->peer_is_trusted
                        ? forwardedClient(beast_request, trusted_proxies, connection->peer)
                        : connection->peer);
            }
            Response response = !request ? errorResponse(400, statusReason(400))
                                         : co_await asio::co_spawn(
                                               workers,
                                               runHandler(router, std::move(request).value()),
                                               asio::use_awaitable);

            const bool keep_alive = wants_keep_alive && !stopping;
            auto beast_response = toBeastResponse(response, version, keep_alive, head_request);

            connection->stream.expires_after(options.request_timeout);
            auto [write_error, write_size] =
                co_await http::async_write(connection->stream, beast_response, kAsTuple);
            if (write_error || !keep_alive) {
                break;
            }
        }

        boost::system::error_code ignored;
        connection->stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
        connection->stream.socket().close(ignored);
        connections.erase(connection);

        if (stopping && connections.empty()) {
            drain_timer.cancel();
        }
    }

    asio::awaitable<void> sendError(Connection& connection, int status) {
        auto response = toBeastResponse(errorResponse(status, statusReason(status)), 11, false, false);
        connection.stream.expires_after(options.request_timeout);
        static_cast<void>(co_await http::async_write(connection.stream, response, kAsTuple));
    }

    /// Runs on the network thread. Idle connections close now, and busy ones after their response.
    void beginStop() {
        stopping = true;

        boost::system::error_code ignored;
        acceptor.close(ignored);

        for (Connection& connection : connections) {
            if (!connection.busy) {
                connection.stream.cancel();
            }
        }

        if (!connections.empty()) {
            drain_timer.expires_after(options.shutdown_timeout);
            drain_timer.async_wait([this](const boost::system::error_code& error) {
                if (!error) {
                    for (Connection& connection : connections) {
                        connection.stream.cancel();
                    }
                }
            });
        }
    }

    const Router& router;
    ServerOptions options;
    LogFunction log_error;
    std::vector<AddressRange> trusted_proxies;

    asio::io_context io;
    tcp::acceptor acceptor;
    asio::steady_timer drain_timer;
    asio::thread_pool workers;
    std::list<Connection> connections;
    // Only read and written on the network thread.
    bool stopping{false};

    std::jthread network_thread;
    std::atomic<std::uint16_t> bound_port{0};
    std::atomic<bool> started{false};
    std::atomic<bool> stop_called{false};
};

Server::Server(const Router& router, ServerOptions options, LogFunction log_error)
    : impl_(std::make_unique<Impl>(router, std::move(options), std::move(log_error))) {}

Server::~Server() {
    stop();
}

Result<void> Server::start() {
    Impl& impl = *impl_;
    if (impl.started.exchange(true)) {
        return Error{ErrorCode::InvalidState, "The server has already been started"};
    }

    const auto fail = [&](const std::string& action, const boost::system::error_code& error) {
        return Error{ErrorCode::IoError, "Cannot " + action + " " + impl.options.host + ":" +
                                             std::to_string(impl.options.port) + ": " +
                                             error.message()};
    };

    for (const std::string& proxy : impl.options.trusted_proxies) {
        auto range = parseAddressRange(proxy);
        if (!range) {
            return range.error();
        }
        impl.trusted_proxies.push_back(std::move(range).value());
    }

    boost::system::error_code error;
    const auto address = asio::ip::make_address(impl.options.host, error);
    if (error) {
        return fail("use the address", error);
    }

    const tcp::endpoint endpoint{address, impl.options.port};
    impl.acceptor.open(endpoint.protocol(), error);
    if (error) {
        return fail("open a socket for", error);
    }

#if !defined(_WIN32)
    // Lets a restarted server reuse its port. On Windows, this option would instead let another
    // process take over a port that's in use.
    impl.acceptor.set_option(tcp::acceptor::reuse_address(true), error);
    if (error) {
        return fail("configure a socket for", error);
    }
#endif

    impl.acceptor.bind(endpoint, error);
    if (error) {
        return fail("bind", error);
    }
    impl.acceptor.listen(asio::socket_base::max_listen_connections, error);
    if (error) {
        return fail("listen on", error);
    }

    const tcp::endpoint local = impl.acceptor.local_endpoint(error);
    if (error) {
        return fail("read the port of", error);
    }
    impl.bound_port.store(local.port());

    for (int i = 0; i < kConcurrentAccepts; ++i) {
        asio::co_spawn(impl.io, impl.accept(), asio::detached);
    }
    impl.network_thread = std::jthread{[&impl] { impl.io.run(); }};
    return {};
}

void Server::stop() noexcept {
    Impl& impl = *impl_;
    if (impl.stop_called.exchange(true)) {
        return;
    }

    if (impl.started.load() && impl.network_thread.joinable()) {
        asio::post(impl.io, [&impl] { impl.beginStop(); });
        impl.network_thread.join();
    }
    impl.workers.join();
}

std::uint16_t Server::port() const noexcept {
    return impl_->bound_port.load() != 0 ? impl_->bound_port.load() : impl_->options.port;
}

}  // namespace cppboot::web
