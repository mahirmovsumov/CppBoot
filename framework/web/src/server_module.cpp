#include "cppboot/web/server_module.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "cppboot/logging/logger.hpp"

namespace cppboot::web {

namespace {

Result<std::int64_t> readInRange(const config::Config& config, std::string_view key,
                                 std::int64_t fallback, std::int64_t minimum, std::int64_t maximum) {
    auto value = config.getInt(key, fallback);
    if (!value) {
        return value.error();
    }
    if (value.value() < minimum || value.value() > maximum) {
        return Error{ErrorCode::InvalidArgument,
                     "Setting " + std::string{key} + " must be between " + std::to_string(minimum) +
                         " and " + std::to_string(maximum)};
    }
    return value.value();
}

}  // namespace

Result<ServerOptions> readServerOptions(const config::Config& config) {
    constexpr std::int64_t kMaxCount = std::numeric_limits<std::int32_t>::max();

    ServerOptions options;
    options.host = config.getString("cppboot.server.host", options.host);

    const auto read = [&](std::string_view key, std::int64_t fallback, std::int64_t minimum,
                          std::int64_t maximum, auto&& apply) -> Result<void> {
        auto value = readInRange(config, key, fallback, minimum, maximum);
        if (!value) {
            return value.error();
        }
        apply(value.value());
        return {};
    };

    if (auto r = read("cppboot.server.port", options.port, 0, 65535,
                      [&](std::int64_t v) { options.port = static_cast<std::uint16_t>(v); });
        !r) {
        return r.error();
    }
    if (auto r = read("cppboot.server.worker_threads", 0, 0, 4096,
                      [&](std::int64_t v) { options.worker_threads = static_cast<std::size_t>(v); });
        !r) {
        return r.error();
    }
    if (auto r = read("cppboot.server.max_body_bytes",
                      static_cast<std::int64_t>(options.max_body_bytes), 0, kMaxCount,
                      [&](std::int64_t v) { options.max_body_bytes = static_cast<std::size_t>(v); });
        !r) {
        return r.error();
    }
    if (auto r = read("cppboot.server.max_connections",
                      static_cast<std::int64_t>(options.max_connections), 1, kMaxCount,
                      [&](std::int64_t v) { options.max_connections = static_cast<std::size_t>(v); });
        !r) {
        return r.error();
    }
    if (auto r = read("cppboot.server.request_timeout_seconds", 30, 1, 86400,
                      [&](std::int64_t v) { options.request_timeout = std::chrono::seconds{v}; });
        !r) {
        return r.error();
    }
    if (auto r = read("cppboot.server.shutdown_timeout_seconds", 30, 0, 86400,
                      [&](std::int64_t v) { options.shutdown_timeout = std::chrono::seconds{v}; });
        !r) {
        return r.error();
    }

    const std::string proxies = config.getString("cppboot.server.trusted_proxies");
    std::string_view rest = proxies;
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        std::string_view entry = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);

        entry.remove_prefix(std::min(entry.find_first_not_of(' '), entry.size()));
        entry = entry.substr(0, entry.find(' '));
        if (!entry.empty()) {
            options.trusted_proxies.emplace_back(entry);
        }
    }

    // Any of these means HTTPS was wanted, so a mistake stops startup rather than serving plain HTTP.
    const std::string certificate = config.getString("cppboot.server.tls.certificate_file");
    const std::string private_key = config.getString("cppboot.server.tls.private_key_file");
    const std::string min_version = config.getString("cppboot.server.tls.min_version");
    if (!certificate.empty() || !private_key.empty() || !min_version.empty()) {
        if (certificate.empty() || private_key.empty()) {
            return Error{ErrorCode::InvalidArgument,
                         "HTTPS needs both cppboot.server.tls.certificate_file and "
                         "cppboot.server.tls.private_key_file"};
        }
        TlsOptions tls{certificate, private_key};
        if (!min_version.empty()) {
            tls.min_version = min_version;
        }
        options.tls = std::move(tls);
    }
    return options;
}

ServerModule::ServerModule(std::shared_ptr<Router> router) : router_(std::move(router)) {}

ServerModule::~ServerModule() = default;

Result<void> ServerModule::start(ApplicationContext& context) {
    auto options = readServerOptions(context.config());
    if (!options) {
        return options.error();
    }

    const logging::Logger& logger = context.logger();
    const bool uses_tls = options.value().tls.has_value();
    if (!uses_tls && !options.value().host.starts_with("127.") && options.value().host != "::1" &&
        options.value().trusted_proxies.empty()) {
        logger.warn("Serving plain HTTP on {}, so anyone on the network can read tokens and "
                    "passwords. Set cppboot.server.tls.certificate_file, or put a TLS proxy in "
                    "front and list it in cppboot.server.trusted_proxies",
                    options.value().host);
    }

    router_->onUnhandledException(
        [&logger](std::string_view what) { logger.error("Request failed: {}", what); });

    server_ = std::make_unique<Server>(*router_, std::move(options).value(),
                                       [&logger](std::string_view what) { logger.error("{}", what); });
    if (auto started = server_->start(); !started) {
        server_.reset();
        return started;
    }

    logger.info("Server listening on port {}{}", server_->port(), uses_tls ? " with HTTPS" : "");
    return {};
}

void ServerModule::stop() noexcept {
    if (server_) {
        server_->stop();
    }
}

std::uint16_t ServerModule::port() const noexcept {
    return server_ ? server_->port() : std::uint16_t{0};
}

}  // namespace cppboot::web
