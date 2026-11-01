#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/base/result.hpp"
#include "cppboot/web/router.hpp"

namespace cppboot::web {

/// Serves HTTPS. Both files are in PEM format, and the key can't have a password.
struct TlsOptions {
    /// The server's certificate, followed by any intermediate certificates.
    std::string certificate_file;
    std::string private_key_file;
    /// The oldest TLS version to accept: "1.2" or "1.3".
    std::string min_version{"1.2"};
};

struct ServerOptions {
    /// Only this machine can connect by default. Use "0.0.0.0" to accept outside connections.
    std::string host{"127.0.0.1"};
    /// Port 0 asks the operating system for a free port. See Server::port().
    std::uint16_t port{8080};
    /// Threads that run handlers. 0 uses one for each processor core.
    std::size_t worker_threads{0};
    /// Threads that move bytes and do TLS work, which handlers don't run on. 0 uses one for every
    /// two processor cores, with at least 1 and at most 4.
    std::size_t io_threads{0};
    std::size_t max_body_bytes{1024 * 1024};
    /// Connections beyond this limit are closed at once.
    std::size_t max_connections{1024};
    /// The longest a client can take to send a request or receive a response.
    std::chrono::milliseconds request_timeout{std::chrono::seconds{30}};
    /// How long stop() lets connections finish before closing them.
    std::chrono::milliseconds shutdown_timeout{std::chrono::seconds{30}};
    /// A request that has waited this long for a worker gets a 503, and its handler doesn't run. Its
    /// client may well have gone, and running old work would make every later request wait too.
    std::chrono::milliseconds queue_timeout{std::chrono::seconds{5}};
    /// How long a handler should take, from when its request arrived. Nothing enforces this, and a
    /// long-running handler should check Request::shouldStop().
    std::chrono::milliseconds handler_deadline{std::chrono::seconds{30}};
    /// Addresses such as "10.0.0.5" or "10.0.0.0/8" of proxies whose X-Forwarded-For header can be
    /// believed. Anyone can send that header, so with none listed, it's ignored.
    std::vector<std::string> trusted_proxies;
    /// With this, every connection uses HTTPS, and start() fails if it can't be set up.
    std::optional<TlsOptions> tls;
};

/// An HTTP/1.1 server. Handlers run on a pool of worker threads, so that a slow handler can't
/// stop the server from serving other connections.
class Server {
public:
    using LogFunction = std::function<void(std::string_view)>;

    /// The router must outlive the server, and must not change while it's running.
    Server(const Router& router, ServerOptions options, LogFunction log_error = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Binds the address and starts serving. Fails if the address can't be used, and can then be
    /// called again. Calling it on a running or stopped server fails.
    Result<void> start();

    /// Stops accepting connections, lets requests finish, and returns when everything has stopped.
    /// After shutdown_timeout, handlers still running are asked to stop through Request::stopToken().
    /// C++ can't safely end a thread from outside, so this waits for them, however long they take.
    void stop() noexcept;

    /// The port in use. This is the real port after start(), even if the option was 0.
    std::uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cppboot::web
