#include "cppboot/web/server.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <stop_token>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "cppboot/platform/socket_state.hpp"
#include "tls.hpp"

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

// A handshake costs the server real work before it knows anything about the client, so it gets
// less time than a request does.
constexpr std::chrono::milliseconds kMostHandshakeTime = std::chrono::seconds{10};

// Closing cleanly waits for the client to agree, and a client that doesn't must not hold it open.
constexpr std::chrono::milliseconds kTlsCloseTime = std::chrono::seconds{2};

// Checking on a client costs a system call, so a busy server that's keeping up doesn't pay for it. A
// queue built by clients that leave at once soon passes this.
constexpr std::chrono::milliseconds kLongEnoughToCheckClient{10};

// Rejected requests come in floods, so this is how often the log mentions them.
constexpr std::chrono::seconds kShedLogInterval{10};

using PlainStream = beast::tcp_stream;
using TlsStream = beast::ssl_stream<beast::tcp_stream>;

/// Memory for the header fields of the request and the response that a connection is working on.
/// Beast allocates each field separately, and this makes one allocation of them all. It belongs to
/// one connection and only that connection's network thread uses it, and what a handler receives is
/// copied out of it first, so no request can see another's data through it.
class FieldArena {
public:
    void* allocate(std::size_t size, std::size_t alignment) {
        const std::size_t start = (used_ + alignment - 1) & ~(alignment - 1);
        if (start + size > kSize) {
            // More headers than usual. They're still limited by the parser's header limit.
            return ::operator new(size, std::align_val_t{alignment});
        }
        if (!memory_) {
            memory_ = std::make_unique_for_overwrite<std::byte[]>(kSize);
        }
        used_ = start + size;
        ++live_;
        return memory_.get() + start;
    }

    void deallocate(void* pointer, std::size_t alignment) noexcept {
        const auto* const address = static_cast<const std::byte*>(pointer);
        if (!memory_ || std::less<>{}(address, memory_.get()) ||
            !std::less<>{}(address, memory_.get() + kSize)) {
            ::operator delete(pointer, std::align_val_t{alignment});
            return;
        }
        // Everything is given back once a response has been written. The memory goes then too, so
        // that a connection waiting for its next request holds none of it.
        if (--live_ == 0) {
            used_ = 0;
            memory_.reset();
        }
    }

private:
    /// Room for the headers of an ordinary request and its response.
    static constexpr std::size_t kSize = 4096;

    std::unique_ptr<std::byte[]> memory_;
    std::size_t used_{0};
    std::size_t live_{0};
};

template <typename T>
class FieldAllocator {
public:
    using value_type = T;

    explicit FieldAllocator(FieldArena& arena) noexcept : arena_(&arena) {}
    template <typename Other>
    FieldAllocator(const FieldAllocator<Other>& other) noexcept : arena_(other.arena_) {}

    T* allocate(std::size_t count) {
        return static_cast<T*>(arena_->allocate(count * sizeof(T), alignof(T)));
    }
    void deallocate(T* pointer, std::size_t) noexcept { arena_->deallocate(pointer, alignof(T)); }

    template <typename Other>
    bool operator==(const FieldAllocator<Other>& other) const noexcept {
        return arena_ == other.arena_;
    }

private:
    template <typename Other>
    friend class FieldAllocator;

    FieldArena* arena_;
};

using BeastFields = http::basic_fields<FieldAllocator<char>>;
using BeastRequest = http::request<http::string_body, BeastFields>;
using BeastResponse = http::response<http::string_body, BeastFields>;
using BeastRequestParser = http::request_parser<http::string_body, FieldAllocator<char>>;

/// Takes the response's body rather than copying it.
BeastResponse toBeastResponse(Response response, unsigned version, bool keep_alive,
                              bool head_request, FieldArena& arena) {
    BeastResponse result{std::piecewise_construct, std::make_tuple(),
                         std::make_tuple(FieldAllocator<char>{arena})};
    result.result(static_cast<http::status>(response.status()));
    result.version(version);
    for (const auto& [name, value] : response.headers()) {
        result.set(name, value);
    }
    result.keep_alive(keep_alive);

    if (head_request) {
        // A HEAD response reports the size of the body it would have sent.
        result.content_length(response.body().size());
    } else {
        result.body() = std::move(response).body();
        result.prepare_payload();
    }
    return result;
}

/// Copies what the handler needs out of the connection's memory, so that the request owns all of it.
Result<Request> toRequest(BeastRequest& request) {
    const auto method = parseMethod(request.method_string());
    if (!method) {
        return Error{ErrorCode::InvalidArgument, "Unsupported method"};
    }

    std::size_t count = 0;
    std::size_t characters = 0;
    for (const auto& field : request) {
        ++count;
        characters += field.name_string().size() + field.value().size();
    }
    RequestHeaders headers;
    headers.reserve(count, characters);
    for (const auto& field : request) {
        headers.add(field.name_string(), field.value());
    }
    return Request::create(*method, request.target(), std::move(headers),
                           std::move(request.body()));
}

/// Keeps per-connection state on the network thread, which is the only thread to touch it.
struct Connection {
    Connection(tcp::socket socket, asio::ssl::context* tls_context, std::string peer_in,
               bool peer_is_trusted_in)
        : stream(tls_context != nullptr
                     ? std::variant<PlainStream, TlsStream>{std::in_place_type<TlsStream>,
                                                            std::move(socket), *tls_context}
                     : std::variant<PlainStream, TlsStream>{std::in_place_type<PlainStream>,
                                                            std::move(socket)}),
          peer(std::move(peer_in)),
          peer_is_trusted(peer_is_trusted_in) {}

    /// The layer that holds the socket and its timer, whether or not TLS is in use.
    beast::tcp_stream& transport() {
        return std::visit(
            [](auto& layer) -> beast::tcp_stream& { return beast::get_lowest_layer(layer); },
            stream);
    }

    std::variant<PlainStream, TlsStream> stream;
    std::string peer;
    bool peer_is_trusted;
    bool busy{false};
    FieldArena field_arena;
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
std::string forwardedClient(const BeastRequest& request,
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

enum class ServerState { Created, Running, Stopped };

std::size_t threadCount(std::size_t requested) {
    return requested != 0 ? requested
                          : std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

/// Half the cores leaves the others for handlers. The limit is as far as this has been measured.
std::size_t networkThreadCount(std::size_t requested) {
    constexpr std::size_t kMostByDefault = 4;
    return requested != 0 ? requested
                          : std::clamp<std::size_t>(std::thread::hardware_concurrency() / 2, 1,
                                                    kMostByDefault);
}

/// One network thread and the connections it serves. Only this thread touches them, so they need no
/// locks, and a slow TLS handshake here can't delay a connection on another one.
struct NetworkThread {
    NetworkThread() : work(asio::make_work_guard(io)), drain_timer(io) {}

    asio::io_context io;
    // Keeps run() going while this thread has no connections yet.
    asio::executor_work_guard<asio::io_context::executor_type> work;
    asio::steady_timer drain_timer;
    std::list<Connection> connections;
    std::jthread thread;
};

std::vector<std::unique_ptr<NetworkThread>> makeNetworkThreads(std::size_t count) {
    std::vector<std::unique_ptr<NetworkThread>> threads;
    for (std::size_t i = 0; i < count; ++i) {
        threads.push_back(std::make_unique<NetworkThread>());
    }
    return threads;
}

}  // namespace

struct Server::Impl {
    Impl(const Router& router_in, ServerOptions options_in, LogFunction log_error_in)
        : router(router_in),
          options(std::move(options_in)),
          log_error(std::move(log_error_in)),
          network_threads(makeNetworkThreads(networkThreadCount(options.io_threads))),
          acceptor(network_threads.front()->io),
          workers(threadCount(options.worker_threads)) {}

    void logError(std::string_view message) const {
        if (log_error) {
            log_error(message);
        }
    }

    /// Runs when connections have had their time to finish. It can only ask, because C++ can't safely
    /// end a thread from outside.
    void askHandlersToStop() {
        // Read first, because asking can wake a handler, which then finishes at once.
        const auto running = running_handlers.load();
        if (handler_stop.request_stop() && running > 0) {
            logError("Shutdown timeout reached, with " + std::to_string(running) +
                     " handlers still running. They've been asked to stop, and shutdown waits "
                     "until they do");
        }
    }

    Response runHandlerNow(Request request) {
        running_handlers.fetch_add(1);
        Response response = router.handle(std::move(request));
        running_handlers.fetch_sub(1);
        return response;
    }

    /// Runs on a worker. The workers take requests in order, so once the oldest is too old, this
    /// clears the rest of the backlog quickly rather than making everyone wait behind it.
    asio::awaitable<Response> runHandler(Request request,
                                         std::chrono::steady_clock::time_point queued_at,
                                         std::uintptr_t socket_handle) {
        const auto now = std::chrono::steady_clock::now();
        const auto waited = now - queued_at;

        // A client that sends expensive requests and leaves at once could otherwise keep every
        // worker busy for free. The network thread doesn't close this socket until this returns, so
        // it's safe to look at from here. Nobody will read this response.
        if (waited >= kLongEnoughToCheckClient && platform::peerHasClosed(socket_handle)) {
            co_return Response{503};
        }

        if (waited <= options.queue_timeout) {
            co_return runHandlerNow(std::move(request));
        }

        const auto shed = shed_since_log.fetch_add(1) + 1;
        const auto now_ticks = now.time_since_epoch().count();
        auto next_log = next_shed_log_ticks.load();
        if (now_ticks >= next_log &&
            next_shed_log_ticks.compare_exchange_strong(
                next_log, now_ticks + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                          kShedLogInterval)
                                          .count())) {
            shed_since_log.fetch_sub(shed);
            logError("Too busy: rejected " + std::to_string(shed) +
                     " requests that waited too long for a worker");
        }

        Response response = errorResponse(503, statusReason(503));
        response.setHeader("Retry-After", "1");
        co_return response;
    }

    /// Runs on the first network thread, and shares connections out among all of them.
    asio::awaitable<void> accept() {
        while (true) {
            NetworkThread& owner = *network_threads[next_network_thread];
            next_network_thread = (next_network_thread + 1) % network_threads.size();

            tcp::socket socket{owner.io};
            auto [error] = co_await acceptor.async_accept(socket, kAsTuple);
            if (stopping) {
                co_return;
            }
            if (error) {
                // Usually a temporary shortage, such as running out of file handles.
                logError("Accepting a connection failed: " + error.message());
                asio::steady_timer pause{co_await asio::this_coro::executor,
                                         std::chrono::milliseconds{100}};
                static_cast<void>(co_await pause.async_wait(kAsTuple));
                continue;
            }

            if (connection_count.load() >= options.max_connections) {
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

            // Otherwise a short response can wait for the client to acknowledge earlier data.
            boost::system::error_code ignored;
            socket.set_option(tcp::no_delay(true), ignored);

            connection_count.fetch_add(1);
            asio::post(owner.io, [this, &owner, socket = std::move(socket),
                                  peer = normalized(remote.address()).to_string(),
                                  trusted = isTrusted(trusted_proxies, remote.address())]() mutable {
                owner.connections.emplace_back(std::move(socket), tls_context.get(), std::move(peer),
                                               trusted);
                asio::co_spawn(owner.io, serve(owner, std::prev(owner.connections.end())),
                               asio::detached);
            });
        }
    }

    asio::awaitable<void> serve(NetworkThread& owner, std::list<Connection>::iterator connection) {
        if (auto* const tls = std::get_if<TlsStream>(&connection->stream)) {
            co_await serveTls(*connection, *tls);
        } else {
            static_cast<void>(co_await exchange(*connection, std::get<PlainStream>(connection->stream)));
        }

        boost::system::error_code ignored;
        beast::tcp_stream& transport = connection->transport();
        transport.socket().shutdown(tcp::socket::shutdown_send, ignored);
        transport.socket().close(ignored);
        owner.connections.erase(connection);
        connection_count.fetch_sub(1);

        if (stopping && owner.connections.empty()) {
            owner.drain_timer.cancel();
        }
    }

    asio::awaitable<void> serveTls(Connection& connection, TlsStream& stream) {
        beast::tcp_stream& transport = connection.transport();

        transport.expires_after(std::min(options.request_timeout, kMostHandshakeTime));
        auto [handshake_error] =
            co_await stream.async_handshake(asio::ssl::stream_base::server, kAsTuple);
        if (handshake_error) {
            // Only the client can see this usefully, and logging it would let anyone fill the log.
            co_return;
        }

        if (co_await exchange(connection, stream)) {
            transport.expires_after(kTlsCloseTime);
            static_cast<void>(co_await stream.async_shutdown(kAsTuple));
        }
    }

    /// Handles requests until the connection ends. Returns false if it failed, rather than
    /// finishing normally, which means it can't be closed cleanly.
    template <typename Stream>
    asio::awaitable<bool> exchange(Connection& connection, Stream& stream) {
        beast::flat_buffer buffer;
        beast::tcp_stream& transport = connection.transport();

        while (!stopping) {
            BeastRequestParser parser{std::piecewise_construct, std::make_tuple(),
                                      std::make_tuple(FieldAllocator<char>{connection.field_arena})};
            parser.body_limit(options.max_body_bytes);

            connection.busy = false;
            transport.expires_after(options.request_timeout);
            auto [read_error, read_size] = co_await http::async_read(stream, buffer, parser, kAsTuple);
            connection.busy = true;

            if (read_error) {
                if (read_error == http::error::end_of_stream) {
                    co_return true;
                }
                if (read_error == http::error::body_limit) {
                    co_await sendError(connection, stream, 413);
                    co_return true;
                }
                if (read_error.category() == beast::error_code{http::error::bad_method}.category() &&
                    read_error != http::error::partial_message) {
                    co_await sendError(connection, stream, 400);
                    co_return true;
                }
                co_return false;
            }

            auto& beast_request = parser.get();
            const unsigned version = beast_request.version();
            const bool wants_keep_alive = beast_request.keep_alive();
            const bool head_request = beast_request.method() == http::verb::head;

            auto request = toRequest(beast_request);
            if (request) {
                request.value().setClientAddress(
                    connection.peer_is_trusted
                        ? forwardedClient(beast_request, trusted_proxies, connection.peer)
                        : connection.peer);
                request.value().setStopToken(handler_stop.get_token());
                request.value().setDeadline(std::chrono::steady_clock::now() +
                                            options.handler_deadline);
            }
            Response response = !request ? errorResponse(400, statusReason(400))
                                         : co_await asio::co_spawn(
                                               workers,
                                               runHandler(std::move(request).value(),
                                                          std::chrono::steady_clock::now(),
                                                          static_cast<std::uintptr_t>(
                                                              transport.socket().native_handle())),
                                               asio::use_awaitable);

            const bool keep_alive = wants_keep_alive && !stopping;
            auto beast_response = toBeastResponse(std::move(response), version, keep_alive,
                                                  head_request, connection.field_arena);

            // Writing a message, rather than a serializer, makes Beast allocate one for it.
            http::response_serializer<http::string_body, BeastFields> serializer{beast_response};
            transport.expires_after(options.request_timeout);
            auto [write_error, write_size] =
                co_await http::async_write(stream, serializer, kAsTuple);
            if (write_error) {
                co_return false;
            }
            if (!keep_alive) {
                co_return true;
            }
        }
        co_return true;
    }

    template <typename Stream>
    asio::awaitable<void> sendError(Connection& connection, Stream& stream, int status) {
        auto response = toBeastResponse(errorResponse(status, statusReason(status)), 11, false,
                                        false, connection.field_arena);
        connection.transport().expires_after(options.request_timeout);
        static_cast<void>(co_await http::async_write(stream, response, kAsTuple));
    }

    /// Runs on each network thread. Idle connections close now, and busy ones after their response.
    void beginStop(NetworkThread& owner) {
        if (&owner == network_threads.front().get()) {
            boost::system::error_code ignored;
            acceptor.close(ignored);
        }

        for (Connection& connection : owner.connections) {
            if (!connection.busy) {
                connection.transport().cancel();
            }
        }

        if (!owner.connections.empty()) {
            owner.drain_timer.expires_after(options.shutdown_timeout);
            owner.drain_timer.async_wait([this, &owner](const boost::system::error_code& error) {
                if (!error) {
                    askHandlersToStop();
                    for (Connection& connection : owner.connections) {
                        connection.transport().cancel();
                    }
                }
            });
        }

        owner.work.reset();
    }

    /// Undoes everything start() did before it failed, so that it can be called again.
    void resetAfterFailedStart() {
        boost::system::error_code ignored;
        acceptor.close(ignored);
        trusted_proxies.clear();
        tls_context.reset();
        bound_port.store(0);
    }

    const Router& router;
    ServerOptions options;
    LogFunction log_error;

    // These don't change while the server runs, so every thread can read them.
    std::vector<AddressRange> trusted_proxies;
    std::unique_ptr<asio::ssl::context> tls_context;

    std::vector<std::unique_ptr<NetworkThread>> network_threads;
    tcp::acceptor acceptor;
    asio::thread_pool workers;
    // Only the first network thread uses this, in accept().
    std::size_t next_network_thread{0};

    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> connection_count{0};
    std::stop_source handler_stop;
    std::atomic<std::size_t> running_handlers{0};
    std::atomic<std::uint64_t> shed_since_log{0};
    std::atomic<std::chrono::steady_clock::rep> next_shed_log_ticks{0};
    std::atomic<std::uint16_t> bound_port{0};

    std::mutex lifecycle_mutex;
    ServerState state{ServerState::Created};
};

Server::Server(const Router& router, ServerOptions options, LogFunction log_error)
    : impl_(std::make_unique<Impl>(router, std::move(options), std::move(log_error))) {}

Server::~Server() {
    stop();
}

Result<void> Server::start() {
    Impl& impl = *impl_;
    const std::lock_guard lock{impl.lifecycle_mutex};

    if (impl.state == ServerState::Running) {
        return Error{ErrorCode::InvalidState, "The server has already been started"};
    }
    if (impl.state == ServerState::Stopped) {
        return Error{ErrorCode::InvalidState,
                     "This server has been stopped, so create a new one to serve again"};
    }

    // The state only changes at the end, so a failure leaves a server that can try again.
    const auto fail = [&](const std::string& action, const boost::system::error_code& error) {
        impl.resetAfterFailedStart();
        return Error{ErrorCode::IoError, "Cannot " + action + " " + impl.options.host + ":" +
                                             std::to_string(impl.options.port) + ": " +
                                             error.message()};
    };

    for (const std::string& proxy : impl.options.trusted_proxies) {
        auto range = parseAddressRange(proxy);
        if (!range) {
            impl.resetAfterFailedStart();
            return range.error();
        }
        impl.trusted_proxies.push_back(std::move(range).value());
    }

    if (impl.options.tls) {
        auto context = makeTlsContext(*impl.options.tls, impl.log_error);
        if (!context) {
            impl.resetAfterFailedStart();
            return context.error();
        }
        impl.tls_context = std::move(context).value();
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
        asio::co_spawn(impl.network_threads.front()->io, impl.accept(), asio::detached);
    }
    for (const auto& network_thread : impl.network_threads) {
        network_thread->thread =
            std::jthread{[&io = network_thread->io] { io.run(); }};
    }
    impl.state = ServerState::Running;
    return {};
}

void Server::stop() noexcept {
    Impl& impl = *impl_;
    const std::lock_guard lock{impl.lifecycle_mutex};

    if (impl.state == ServerState::Stopped) {
        return;
    }

    if (impl.state == ServerState::Running) {
        impl.stopping.store(true);
        for (const auto& network_thread : impl.network_threads) {
            asio::post(network_thread->io,
                       [&impl, &owner = *network_thread] { impl.beginStop(owner); });
        }
        for (const auto& network_thread : impl.network_threads) {
            network_thread->thread.join();
        }
    }
    impl.state = ServerState::Stopped;
    impl.workers.join();
}

std::uint16_t Server::port() const noexcept {
    return impl_->bound_port.load() != 0 ? impl_->bound_port.load() : impl_->options.port;
}

}  // namespace cppboot::web
