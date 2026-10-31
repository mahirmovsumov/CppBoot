#include "cppboot/web/rate_limit.hpp"

#include <algorithm>
#include <boost/asio/ip/address_v6.hpp>
#include <limits>
#include <memory>
#include <utility>

namespace cppboot::web {

namespace {

/// One IPv6 customer usually controls a whole /64, so limiting single addresses would let them
/// pick a new one for every request.
std::string clientKey(const std::string& address) {
    if (address.find(':') == std::string::npos) {
        return address;
    }

    boost::system::error_code error;
    const auto parsed = boost::asio::ip::make_address_v6(address, error);
    if (error) {
        return address;
    }
    const auto bytes = parsed.to_bytes();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size() / 2);
}

}  // namespace

RateLimiterOptions RateLimiterOptions::fromSettings(const config::Config& config,
                                                    const logging::Logger& logger) {
    RateLimiterOptions options;

    const auto enabled = config.getBool("cppboot.rate_limit.enabled", options.enabled);
    if (!enabled) {
        throw Exception(enabled.error());
    }
    options.enabled = enabled.value();

    const auto max_clients = config.getInt("cppboot.rate_limit.max_clients",
                                           static_cast<std::int64_t>(options.max_clients));
    if (!max_clients) {
        throw Exception(max_clients.error());
    }
    if (max_clients.value() < 1 || max_clients.value() > std::numeric_limits<std::int32_t>::max()) {
        throw Exception(Error{ErrorCode::InvalidArgument,
                              "Setting cppboot.rate_limit.max_clients must be positive"});
    }
    options.max_clients = static_cast<std::size_t>(max_clients.value());

    if (!options.enabled) {
        logger.warn("Rate limits are off, because cppboot.rate_limit.enabled is false");
    }
    return options;
}

RateLimiter::RateLimiter(RateLimit limit, RateLimiterOptions options, Clock clock)
    : window_(std::chrono::duration_cast<Duration>(limit.window)),
      interval_(limit.requests == 0 ? Duration::zero() : window_ / limit.requests),
      options_(options),
      clock_(std::move(clock)) {
    if (interval_ <= Duration::zero() || options_.max_clients == 0 || !clock_) {
        throw Exception(Error{ErrorCode::InvalidArgument,
                              "A rate limit needs at least one request, a positive window, and "
                              "room for at least one client"});
    }
}

void RateLimiter::forgetIdleClients(TimePoint now) {
    // A client that hasn't been seen for a whole window has its full limit back, so forgetting it
    // changes nothing. The list is in order of last_seen, so these are all at the end.
    while (!clients_.empty() && now - clients_.back().last_seen >= window_) {
        index_.erase(clients_.back().key);
        clients_.pop_back();
    }
}

std::optional<Response> RateLimiter::check(const Request& request) {
    if (!options_.enabled) {
        return std::nullopt;
    }

    std::string key = clientKey(request.clientAddress());
    Duration wait = Duration::zero();
    {
        const std::lock_guard lock{mutex_};
        const TimePoint now = clock_();
        forgetIdleClients(now);

        if (const auto found = index_.find(key); found != index_.end()) {
            clients_.splice(clients_.begin(), clients_, found->second);
        } else {
            if (clients_.size() >= options_.max_clients) {
                index_.erase(clients_.back().key);
                clients_.pop_back();
            }
            clients_.push_front(Client{std::move(key), now, now});
            index_.emplace(clients_.front().key, clients_.begin());
        }

        Client& client = clients_.front();
        client.last_seen = now;

        const TimePoint start = std::max(client.free_at, now);
        const TimePoint earliest = start - (window_ - interval_);
        if (earliest > now) {
            wait = earliest - now;
        } else {
            client.free_at = start + interval_;
        }
    }

    if (wait == Duration::zero()) {
        return std::nullopt;
    }

    Response response = errorResponse(429, statusReason(429));
    response.setHeader("Retry-After",
                       std::to_string(std::chrono::ceil<std::chrono::seconds>(wait).count()));
    return response;
}

std::size_t RateLimiter::trackedClients() const {
    const std::lock_guard lock{mutex_};
    return clients_.size();
}

Middleware rateLimit(RateLimit limit, RateLimiterOptions options) {
    auto limiter = std::make_shared<RateLimiter>(limit, options);
    return [limiter = std::move(limiter)](const Request& request, const Next& next) -> Response {
        if (auto rejected = limiter->check(request)) {
            return *std::move(rejected);
        }
        return next(request);
    };
}

}  // namespace cppboot::web
