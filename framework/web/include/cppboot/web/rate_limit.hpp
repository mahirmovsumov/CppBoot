#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "cppboot/config/config.hpp"
#include "cppboot/logging/logger.hpp"
#include "cppboot/web/router.hpp"

namespace cppboot::web {

/// How many requests a client can make in a window. It can use them all at once, and then gets
/// them back evenly over the window.
struct RateLimit {
    std::uint32_t requests;
    std::chrono::seconds window;
};

struct RateLimiterOptions {
    /// Turns limits off, such as for a project's own tests.
    bool enabled{true};
    /// The clients remembered at once, which bounds memory. Beyond this, the one seen longest ago
    /// is forgotten, so an attacker would need this many addresses to reset their own limit.
    std::size_t max_clients{100000};

    /// Reads settings under cppboot.rate_limit, and warns if limits are off. An invalid value
    /// throws cppboot::Exception.
    static RateLimiterOptions fromSettings(const config::Config& config,
                                           const logging::Logger& logger);
};

/// Limits requests from each client address. This is safe to use from any number of threads.
class RateLimiter {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /// An impossible limit, such as no requests at all, throws cppboot::Exception.
    explicit RateLimiter(RateLimit limit, RateLimiterOptions options = {},
                         Clock clock = std::chrono::steady_clock::now);

    /// Counts a request. Returns nothing if it can continue, or a 429 response with a Retry-After
    /// header if its client is over the limit. A rejected request doesn't use up the limit.
    std::optional<Response> check(const Request& request);

    std::size_t trackedClients() const;

private:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration = std::chrono::steady_clock::duration;

    struct Client {
        std::string key;
        /// When this client's limit will be fully available again.
        TimePoint free_at;
        TimePoint last_seen;
    };

    void forgetIdleClients(TimePoint now);

    Duration window_;
    Duration interval_;
    RateLimiterOptions options_;
    Clock clock_;

    mutable std::mutex mutex_;
    // Most recently seen first, which makes finding the oldest and the idle ones cheap. The map's
    // keys point into this list, whose elements don't move.
    std::list<Client> clients_;
    std::unordered_map<std::string_view, std::list<Client>::iterator> index_;
};

/// Returns middleware that applies one limit to each client, across every route.
Middleware rateLimit(RateLimit limit, RateLimiterOptions options = {});

}  // namespace cppboot::web
