#include "cppboot/actuator/actuator.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>

#include "cppboot/base/version.hpp"
#include "cppboot/platform/system_info.hpp"

namespace cppboot::actuator {

namespace {

// Requests with no route share this, so that clients can't create new series by inventing paths.
constexpr std::string_view kUnmatchedRoute = "unmatched";

/// The server gives IPv4-mapped IPv6 addresses in their IPv4 form, so these are all the spellings.
/// An empty address means the request didn't come from the network, which isn't trusted either.
bool isLoopback(std::string_view address) {
    return address.starts_with("127.") || address == "::1";
}

std::optional<std::string_view> bearerToken(const std::string& header) {
    constexpr std::string_view kScheme = "bearer ";
    const std::string_view value = header;
    if (value.size() <= kScheme.size() ||
        !std::equal(kScheme.begin(), kScheme.end(), value.begin(), [](char expected, char actual) {
            return expected == std::tolower(static_cast<unsigned char>(actual));
        })) {
        return std::nullopt;
    }
    return value.substr(kScheme.size());
}

web::Response unauthorizedResponse() {
    web::Response response = web::errorResponse(401, "Sign in first");
    response.setHeader("WWW-Authenticate", "Bearer");
    return response;
}

web::Response healthResponse(const HealthReport& report) {
    nlohmann::ordered_json checks = nlohmann::ordered_json::object();
    for (const HealthCheckResult& result : report.checks) {
        checks[result.name] = result.healthy ? "UP" : "DOWN";
    }

    const nlohmann::ordered_json body = {
        {"status", report.healthy ? "UP" : "DOWN"},
        {"checks", std::move(checks)},
    };
    // A 503 tells load balancers to stop sending traffic here.
    return web::Response::json(body.dump(), report.healthy ? 200 : 503);
}

void addProcessMetrics(Actuator& actuator) {
    Actuator* const raw = &actuator;
    actuator.metrics().gaugeFunction("process_uptime_seconds", "Time since the application started",
                                     [raw] { return static_cast<double>(raw->uptime().count()); });

    if (platform::residentMemoryBytes()) {
        actuator.metrics().gaugeFunction(
            "process_resident_memory_bytes", "Physical memory the process is using", [] {
                const auto bytes = platform::residentMemoryBytes();
                return bytes ? static_cast<double>(*bytes) : 0.0;
            });
    }
}

web::Response recordRequest(Metrics& metrics, Gauge& in_progress, const web::Request& request,
                            const web::Next& next) {
    in_progress.add(1);
    const auto started = std::chrono::steady_clock::now();

    web::Response response = next(request);

    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    in_progress.add(-1);

    const std::string route = request.route().empty() ? std::string{kUnmatchedRoute} : request.route();
    const std::string method{web::toString(request.method())};

    metrics
        .counter("http_requests_total", "Requests handled",
                 {{"method", method},
                  {"route", route},
                  {"status", std::to_string(response.status())}})
        .increment();
    metrics
        .histogram("http_request_duration_seconds", "How long requests took",
                   {{"method", method}, {"route", route}})
        .observe(elapsed.count());
    return response;
}

}  // namespace

Actuator::Actuator(std::shared_ptr<Metrics> metrics, std::shared_ptr<const logging::Logger> logger,
                   std::shared_ptr<const config::Config> config)
    : metrics_(std::move(metrics)),
      logger_(std::move(logger)),
      metrics_token_(config->getString("cppboot.actuator.metrics_token")),
      application_name_(config->getString("cppboot.application.name", "cppboot")),
      started_(std::chrono::steady_clock::now()) {
    addProcessMetrics(*this);
}

void Actuator::addCheck(std::string name, Check check) {
    const std::lock_guard lock{mutex_};
    checks_.push_back(NamedCheck{std::move(name), std::move(check)});
}

void Actuator::addIndicator(std::shared_ptr<HealthIndicator> indicator) {
    std::string name = indicator->name();
    addCheck(std::move(name), [indicator = std::move(indicator)] { indicator->check(); });
}

HealthReport Actuator::health() const {
    std::vector<NamedCheck> checks;
    {
        // Copied, so that a slow check doesn't hold the lock.
        const std::lock_guard lock{mutex_};
        checks = checks_;
    }

    HealthReport report;
    for (const NamedCheck& entry : checks) {
        bool healthy = true;
        try {
            entry.check();
        } catch (const std::exception& exception) {
            healthy = false;
            logger_->warn("Health check {} failed: {}", entry.name, exception.what());
        } catch (...) {
            healthy = false;
            logger_->warn("Health check {} failed", entry.name);
        }
        report.healthy = report.healthy && healthy;
        report.checks.push_back(HealthCheckResult{entry.name, healthy});
    }
    return report;
}

bool Actuator::matchesMetricsToken(std::string_view token) const noexcept {
    if (metrics_token_.empty() || token.size() != metrics_token_.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t i = 0; i < token.size(); ++i) {
        difference = static_cast<unsigned char>(
            difference | (static_cast<unsigned char>(token[i]) ^
                          static_cast<unsigned char>(metrics_token_[i])));
    }
    return difference == 0;
}

std::chrono::seconds Actuator::uptime() const {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                            started_);
}

void mount(web::Router& router, std::shared_ptr<Actuator> actuator, bool require_token) {
    Actuator* const raw = actuator.get();
    Metrics* const metrics = &raw->metrics();
    Gauge* const in_progress =
        &metrics->gauge("http_requests_in_progress", "Requests being handled now");

    router.use([actuator, metrics, in_progress](const web::Request& request, const web::Next& next) {
        return recordRequest(*metrics, *in_progress, request, next);
    });

    router.get("/actuator/health/live", [](const web::Request&) {
        return web::Response::json(R"({"status":"UP"})");
    });

    router.get("/actuator/health", [actuator](const web::Request&) {
        return healthResponse(actuator->health());
    });

    const auto allowed = [actuator, require_token](const web::Request& request) {
        if (!actuator->hasMetricsToken()) {
            // With no token to check, only this machine can see them, so a server that listens on
            // other addresses doesn't show them to the network by mistake.
            // A proxy on this machine also connects from a local address, so a request it passed
            // on, which carries one of these headers, isn't counted as local.
            return !require_token && isLoopback(request.clientAddress()) &&
                   !request.header("X-Forwarded-For") && !request.header("Forwarded");
        }
        const auto header = request.header("Authorization");
        if (!header) {
            return false;
        }
        const auto token = bearerToken(*header);
        return token && actuator->matchesMetricsToken(*token);
    };

    router.get("/actuator/metrics", [actuator, allowed](const web::Request& request) {
        if (!allowed(request)) {
            return unauthorizedResponse();
        }
        web::Response response = web::Response::text(actuator->metrics().render());
        response.setHeader("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
        return response;
    });

    router.get("/actuator/info", [actuator, allowed](const web::Request& request) {
        if (!allowed(request)) {
            return unauthorizedResponse();
        }
        const nlohmann::ordered_json body = {
            {"application", actuator->applicationName()},
            {"cppboot_version", std::string{kVersionString}},
            {"uptime_seconds", actuator->uptime().count()},
        };
        return web::Response::json(body.dump());
    });
}

}  // namespace cppboot::actuator
