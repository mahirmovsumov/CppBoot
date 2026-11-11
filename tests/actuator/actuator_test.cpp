#include <gtest/gtest.h>

#include <atomic>
#include <cppboot/actuator/actuator.hpp>
#include <cppboot/actuator/metrics.hpp>
#include <cppboot/web/router.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace cppboot::actuator;
using namespace cppboot::web;
using cppboot::Exception;

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST(MetricsTest, CounterRendersInPrometheusFormat) {
    Metrics metrics;
    metrics.counter("orders_total", "Orders created").increment(3);

    EXPECT_EQ(metrics.render(),
              "# HELP orders_total Orders created\n"
              "# TYPE orders_total counter\n"
              "orders_total 3\n");
}

TEST(MetricsTest, SameNameAndLabelsReturnTheSameMetric) {
    Metrics metrics;
    Counter& first = metrics.counter("a_total", "A", {{"kind", "x"}});
    Counter& second = metrics.counter("a_total", "A", {{"kind", "x"}});
    Counter& other = metrics.counter("a_total", "A", {{"kind", "y"}});

    EXPECT_EQ(&first, &second);
    EXPECT_NE(&first, &other);
}

TEST(MetricsTest, LabelsSplitSeries) {
    Metrics metrics;
    metrics.counter("a_total", "A", {{"kind", "x"}}).increment();
    metrics.counter("a_total", "A", {{"kind", "y"}}).increment(2);

    const std::string text = metrics.render();
    EXPECT_TRUE(contains(text, "a_total{kind=\"x\"} 1\n"));
    EXPECT_TRUE(contains(text, "a_total{kind=\"y\"} 2\n"));
}

TEST(MetricsTest, EscapesLabelValuesAndHelp) {
    Metrics metrics;
    metrics.counter("a_total", "Line\nbreak", {{"path", "a\"b\\c\nd"}}).increment();

    const std::string text = metrics.render();
    EXPECT_TRUE(contains(text, "# HELP a_total Line\\nbreak\n"));
    EXPECT_TRUE(contains(text, R"(path="a\"b\\c\nd")"));
}

TEST(MetricsTest, GaugeGoesUpAndDown) {
    Metrics metrics;
    Gauge& gauge = metrics.gauge("queue_size", "Items waiting");
    gauge.set(5);
    gauge.add(-2);

    EXPECT_EQ(gauge.value(), 3);
    EXPECT_TRUE(contains(metrics.render(), "queue_size 3\n"));
}

TEST(MetricsTest, GaugeFunctionIsReadWhenRendering) {
    Metrics metrics;
    int calls = 0;
    metrics.gaugeFunction("live_value", "A value", [&] { return ++calls * 1.5; });

    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(contains(metrics.render(), "live_value 1.5\n"));
    EXPECT_TRUE(contains(metrics.render(), "live_value 3\n"));
}

TEST(MetricsTest, HistogramIsCumulative) {
    Metrics metrics;
    Histogram& histogram = metrics.histogram("size", "Sizes", {}, {1, 5});
    histogram.observe(0.5);
    histogram.observe(1);
    histogram.observe(3);
    histogram.observe(100);

    const std::string text = metrics.render();
    EXPECT_TRUE(contains(text, "size_bucket{le=\"1\"} 2\n"));
    EXPECT_TRUE(contains(text, "size_bucket{le=\"5\"} 3\n"));
    EXPECT_TRUE(contains(text, "size_bucket{le=\"+Inf\"} 4\n"));
    EXPECT_TRUE(contains(text, "size_sum 104.5\n"));
    EXPECT_TRUE(contains(text, "size_count 4\n"));
}

TEST(MetricsTest, HistogramKeepsLabelsWithBuckets) {
    Metrics metrics;
    metrics.histogram("size", "Sizes", {{"kind", "x"}}, {1}).observe(1);
    EXPECT_TRUE(contains(metrics.render(), "size_bucket{kind=\"x\",le=\"1\"} 1\n"));
}

TEST(MetricsTest, RejectsBadNames) {
    Metrics metrics;
    EXPECT_THROW(metrics.counter("", "x"), Exception);
    EXPECT_THROW(metrics.counter("2fast", "x"), Exception);
    EXPECT_THROW(metrics.counter("has space", "x"), Exception);
    EXPECT_THROW(metrics.counter("a", "x", {{"bad label", "y"}}), Exception);
    EXPECT_THROW(metrics.counter("a", "x", {{"a:b", "y"}}), Exception);
}

TEST(MetricsTest, RejectsANameUsedForAnotherKind) {
    Metrics metrics;
    metrics.counter("taken", "x");
    EXPECT_THROW(metrics.gauge("taken", "x"), Exception);
    EXPECT_THROW(metrics.histogram("taken", "x"), Exception);
}

TEST(MetricsTest, RejectsUnsortedBounds) {
    Metrics metrics;
    EXPECT_THROW(metrics.histogram("size", "x", {}, {5, 1}), Exception);
}

TEST(MetricsTest, ReadingAFunctionGaugeAsAGaugeFailsSafely) {
    Metrics metrics;
    metrics.gaugeFunction("live", "x", [] { return 1.0; });
    EXPECT_THROW(metrics.gauge("live", "x"), Exception);
}

TEST(MetricsTest, FailedHistogramLeavesNothingBehind) {
    Metrics metrics;
    EXPECT_THROW(metrics.histogram("size", "x", {}, {5, 1}), Exception);
    EXPECT_NO_THROW(static_cast<void>(metrics.render()));
    EXPECT_NO_THROW(metrics.histogram("size", "x", {}, {1, 5}).observe(1));
}

// This crashed about half the time when Metrics used std::shared_mutex, which isn't reliable on
// MinGW, so a failure here may not show up on every run.
TEST(MetricsTest, CountsCorrectlyFromManyThreads) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 20000;

    Metrics metrics;
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < kPerThread; ++i) {
                    metrics.counter("hits_total", "Hits", {{"kind", "x"}}).increment();
                    metrics.histogram("took", "Took", {}, {1}).observe(0.5);
                    if (i % 1000 == 0) {
                        static_cast<void>(metrics.render());
                    }
                }
            });
        }
    }

    const std::string text = metrics.render();
    EXPECT_TRUE(contains(text, "hits_total{kind=\"x\"} " + std::to_string(kThreads * kPerThread)));
    EXPECT_TRUE(contains(text, "took_count " + std::to_string(kThreads * kPerThread)));
}

class ActuatorTest : public ::testing::Test {
protected:
    void build(std::map<std::string, std::string> settings = {}, bool require_token = false) {
        cppboot::config::ConfigSources sources;
        sources.directory = std::filesystem::temp_directory_path() / "cppboot-absent-directory";
        sources.defaults = std::move(settings);
        config_ = std::make_shared<const cppboot::config::Config>(
            cppboot::config::Config::load(sources).value());

        cppboot::logging::LoggerOptions log_options;
        log_options.level = cppboot::logging::Level::Off;
        logger_ = std::make_shared<const cppboot::logging::Logger>(
            cppboot::logging::Logger::create(log_options).value());

        metrics_ = std::make_shared<Metrics>();
        actuator_ = std::make_shared<Actuator>(metrics_, logger_, config_);
        router_ = std::make_unique<Router>();
        mount(*router_, actuator_, require_token);
    }

    Response send(std::string_view target, Headers headers = {}, Method method = Method::Get,
                  std::string client = "127.0.0.1") const {
        Request request = Request::create(method, target, std::move(headers)).value();
        request.setClientAddress(std::move(client));
        return router_->handle(request);
    }

    Response withToken(std::string_view target, const std::string& token) const {
        return send(target, Headers{{"Authorization", "Bearer " + token}});
    }

    std::shared_ptr<const cppboot::config::Config> config_;
    std::shared_ptr<const cppboot::logging::Logger> logger_;
    std::shared_ptr<Metrics> metrics_;
    std::shared_ptr<Actuator> actuator_;
    std::unique_ptr<Router> router_;
};

TEST_F(ActuatorTest, HealthyWithNoChecks) {
    build();
    const Response response = send("/actuator/health");
    EXPECT_EQ(response.status(), 200);
    EXPECT_EQ(response.body(), R"({"status":"UP","checks":{}})");
}

TEST_F(ActuatorTest, ReportsEachCheck) {
    build();
    actuator_->addCheck("database", [] {});
    actuator_->addCheck("cache", [] {});

    const Response response = send("/actuator/health");
    EXPECT_EQ(response.status(), 200);
    EXPECT_EQ(response.body(), R"({"status":"UP","checks":{"database":"UP","cache":"UP"}})");
}

TEST_F(ActuatorTest, FailingCheckReturnsServiceUnavailable) {
    build();
    actuator_->addCheck("good", [] {});
    actuator_->addCheck("bad", [] { throw std::runtime_error("password=hunter2 at 10.0.0.5"); });

    const Response response = send("/actuator/health");
    EXPECT_EQ(response.status(), 503);
    EXPECT_EQ(response.body(), R"({"status":"DOWN","checks":{"good":"UP","bad":"DOWN"}})");
    EXPECT_FALSE(contains(response.body(), "hunter2"));
}

TEST_F(ActuatorTest, UnknownExceptionCountsAsDown) {
    build();
    actuator_->addCheck("bad", [] { throw 5; });
    EXPECT_EQ(send("/actuator/health").status(), 503);
}

TEST_F(ActuatorTest, LivenessIgnoresChecks) {
    build();
    actuator_->addCheck("bad", [] { throw std::runtime_error("down"); });

    // A broken database shouldn't make an orchestrator restart the process.
    EXPECT_EQ(send("/actuator/health/live").status(), 200);
}

TEST_F(ActuatorTest, UsesIndicators) {
    class Indicator : public HealthIndicator {
    public:
        std::string name() const override { return "disk"; }
        void check() override { throw std::runtime_error("full"); }
    };

    build();
    actuator_->addIndicator(std::make_shared<Indicator>());
    EXPECT_EQ(send("/actuator/health").body(), R"({"status":"DOWN","checks":{"disk":"DOWN"}})");
}

TEST_F(ActuatorTest, MetricsHavePrometheusContentType) {
    build();
    const Response response = send("/actuator/metrics");
    EXPECT_EQ(response.status(), 200);
    EXPECT_EQ(response.header("Content-Type"), "text/plain; version=0.0.4; charset=utf-8");
    EXPECT_TRUE(contains(response.body(), "process_uptime_seconds"));
    EXPECT_TRUE(contains(response.body(), "process_resident_memory_bytes"));
    EXPECT_TRUE(contains(response.body(), "http_requests_in_progress"));
}

TEST_F(ActuatorTest, RecordsRequestsByRoutePattern) {
    build();
    router_->get("/users/{id}", [](const Request&) { return Response::text("ok"); });

    static_cast<void>(send("/users/1"));
    static_cast<void>(send("/users/2"));

    const std::string text = metrics_->render();
    EXPECT_TRUE(contains(
        text, R"(http_requests_total{method="GET",route="/users/{id}",status="200"} 2)"));
    EXPECT_TRUE(contains(text, R"(http_request_duration_seconds_count{method="GET",route="/users/{id}"} 2)"));
    EXPECT_FALSE(contains(text, "/users/1"));
}

TEST_F(ActuatorTest, InventedPathsCannotCreateNewSeries) {
    build();
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(send("/attack/" + std::to_string(i)).status(), 404);
    }

    const std::string text = metrics_->render();
    EXPECT_TRUE(contains(
        text, R"(http_requests_total{method="GET",route="unmatched",status="404"} 100)"));
    EXPECT_FALSE(contains(text, "/attack"));
}

TEST_F(ActuatorTest, RecordsFailuresWithTheirStatus) {
    build();
    router_->get("/broken", [](const Request&) -> Response { throw std::runtime_error("boom"); });
    router_->get("/refused", [](const Request&) -> Response { throw HttpError(409, "No"); });

    static_cast<void>(send("/broken"));
    static_cast<void>(send("/refused"));

    const std::string text = metrics_->render();
    EXPECT_TRUE(contains(text, R"(route="/broken",status="500"} 1)"));
    EXPECT_TRUE(contains(text, R"(route="/refused",status="409"} 1)"));
}

TEST_F(ActuatorTest, InProgressReturnsToZero) {
    build();
    double seen = -1;
    router_->get("/check", [&](const Request&) {
        seen = metrics_->gauge("http_requests_in_progress", "").value();
        return Response{};
    });

    static_cast<void>(send("/check"));
    EXPECT_EQ(seen, 1);
    EXPECT_EQ(metrics_->gauge("http_requests_in_progress", "").value(), 0);
}

TEST_F(ActuatorTest, InfoReportsTheApplication) {
    build({{"cppboot.application.name", "shop"}});
    const auto body = nlohmann::json::parse(send("/actuator/info").body());
    EXPECT_EQ(body["application"], "shop");
    EXPECT_TRUE(body.contains("cppboot_version"));
    EXPECT_TRUE(body.contains("uptime_seconds"));
}

TEST_F(ActuatorTest, SecuredProjectWithNoTokenStaysLocked) {
    build({}, true);
    EXPECT_EQ(send("/actuator/metrics").status(), 401);
    EXPECT_EQ(send("/actuator/info").status(), 401);
    EXPECT_EQ(withToken("/actuator/metrics", "").status(), 401);
    EXPECT_EQ(withToken("/actuator/metrics", "guess").status(), 401);
}

TEST_F(ActuatorTest, HealthStaysPublicInASecuredProject) {
    build({}, true);
    EXPECT_EQ(send("/actuator/health").status(), 200);
    EXPECT_EQ(send("/actuator/health/live").status(), 200);
}

TEST_F(ActuatorTest, ConfiguredTokenOpensMetrics) {
    build({{"cppboot.actuator.metrics_token", "scrape-token"}}, true);

    EXPECT_EQ(send("/actuator/metrics").status(), 401);
    EXPECT_EQ(withToken("/actuator/metrics", "wrong-token!").status(), 401);
    EXPECT_EQ(withToken("/actuator/metrics", "scrape-toke").status(), 401);
    EXPECT_EQ(withToken("/actuator/metrics", "scrape-token").status(), 200);
    EXPECT_EQ(withToken("/actuator/info", "scrape-token").status(), 200);
}

TEST_F(ActuatorTest, WithNoTokenOnlyThisMachineSeesMetrics) {
    build();
    for (const char* local : {"127.0.0.1", "127.0.0.2", "::1"}) {
        EXPECT_EQ(send("/actuator/metrics", {}, Method::Get, local).status(), 200) << local;
        EXPECT_EQ(send("/actuator/info", {}, Method::Get, local).status(), 200) << local;
    }
    for (const char* remote : {"10.0.0.5", "192.168.1.20", "2001:db8::1", "::ffff:127.0.0.1", ""}) {
        EXPECT_EQ(send("/actuator/metrics", {}, Method::Get, remote).status(), 401) << remote;
        EXPECT_EQ(send("/actuator/info", {}, Method::Get, remote).status(), 401) << remote;
    }
    // A proxy on this machine connects locally, but passes on requests from anywhere.
    EXPECT_EQ(send("/actuator/metrics", {{"X-Forwarded-For", "203.0.113.9"}}, Method::Get,
                   "127.0.0.1")
                  .status(),
              401);
    EXPECT_EQ(send("/actuator/metrics", {{"Forwarded", "for=203.0.113.9"}}, Method::Get, "127.0.0.1")
                  .status(),
              401);
    // Health stays public, because load balancers on other machines call it.
    EXPECT_EQ(send("/actuator/health", {}, Method::Get, "10.0.0.5").status(), 200);
}

TEST_F(ActuatorTest, ConfiguredTokenAppliesFromAnywhere) {
    build({{"cppboot.actuator.metrics_token", "scrape-token"}}, false);
    EXPECT_EQ(send("/actuator/metrics", {{"Authorization", "Bearer scrape-token"}}, Method::Get,
                   "10.0.0.5")
                  .status(),
              200);
    // A token replaces the rule for this machine, rather than adding to it.
    EXPECT_EQ(send("/actuator/metrics", {}, Method::Get, "127.0.0.1").status(), 401);
}

TEST_F(ActuatorTest, ConfiguredTokenAppliesWithoutSecurityToo) {
    build({{"cppboot.actuator.metrics_token", "scrape-token"}}, false);
    EXPECT_EQ(send("/actuator/metrics").status(), 401);
    EXPECT_EQ(withToken("/actuator/metrics", "scrape-token").status(), 200);
}

TEST_F(ActuatorTest, MetricsTokenIsHiddenInConfigurationLogs) {
    build({{"cppboot.actuator.metrics_token", "scrape-token"}});
    EXPECT_FALSE(contains(config_->describe(), "scrape-token"));
}

TEST_F(ActuatorTest, RejectedRequestsCountInMetrics) {
    build({}, true);
    static_cast<void>(send("/actuator/metrics"));
    EXPECT_TRUE(contains(metrics_->render(),
                         R"(route="/actuator/metrics",status="401"} 1)"));
}
