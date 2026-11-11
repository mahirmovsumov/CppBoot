#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/actuator/metrics.hpp"
#include "cppboot/config/config.hpp"
#include "cppboot/logging/logger.hpp"
#include "cppboot/web/router.hpp"

namespace cppboot::actuator {

/// Checks one thing the application depends on, for /actuator/health. Mark a class that derives from
/// this with CPPBOOT_SERVICE, and it's added automatically.
class HealthIndicator {
public:
    virtual ~HealthIndicator() = default;

    virtual std::string name() const = 0;

    /// Throw to report a problem. The message goes to the log, never to clients. Keep this quick,
    /// because load balancers call it often.
    virtual void check() = 0;
};

struct HealthCheckResult {
    std::string name;
    bool healthy;
};

struct HealthReport {
    bool healthy{true};
    std::vector<HealthCheckResult> checks;
};

/// Holds what the /actuator routes report. It's safe to use from any thread.
class Actuator {
public:
    using Check = std::function<void()>;

    Actuator(std::shared_ptr<Metrics> metrics, std::shared_ptr<const logging::Logger> logger,
             std::shared_ptr<const config::Config> config);

    Metrics& metrics() noexcept { return *metrics_; }

    /// Adds a health check. It's healthy unless the function throws.
    void addCheck(std::string name, Check check);
    void addIndicator(std::shared_ptr<HealthIndicator> indicator);

    /// Runs every check.
    HealthReport health() const;

    /// Checks a token against cppboot.actuator.metrics_token, which is the same length whatever
    /// the guess, so that timing can't reveal it. Without a configured token, nothing matches.
    bool matchesMetricsToken(std::string_view token) const noexcept;
    bool hasMetricsToken() const noexcept { return !metrics_token_.empty(); }

    const std::string& applicationName() const noexcept { return application_name_; }
    std::chrono::seconds uptime() const;

private:
    struct NamedCheck {
        std::string name;
        Check check;
    };

    std::shared_ptr<Metrics> metrics_;
    std::shared_ptr<const logging::Logger> logger_;
    std::string metrics_token_;
    std::string application_name_;
    std::chrono::steady_clock::time_point started_;

    mutable std::mutex mutex_;
    std::vector<NamedCheck> checks_;
};

/// Adds the /actuator routes, and middleware that records every request. Call this before adding
/// other middleware, so that it measures them too.
///
/// /actuator/health and /actuator/health/live are always public, and report status only. Metrics and
/// info need cppboot.actuator.metrics_token if one is set. Without one, they're locked if
/// require_token is true, and otherwise open only to clients on this machine. Generated code passes
/// true for a project that uses security.
void mount(web::Router& router, std::shared_ptr<Actuator> actuator, bool require_token = false);

}  // namespace cppboot::actuator
