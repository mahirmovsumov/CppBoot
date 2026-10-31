#include <gtest/gtest.h>

#include <cppboot/config/config.hpp>
#include <cppboot/core/module.hpp>
#include <cppboot/di/container.hpp>
#include <cppboot/logging/logger.hpp>
#include <filesystem>
#include <memory>
#include <string>

#include "cppboot_components.generated.hpp"

using namespace cppboot;
using namespace cppboot::web;

namespace {

template <typename T>
std::shared_ptr<T> borrow(const T& value) {
    return std::shared_ptr<T>{const_cast<T*>(&value), [](T*) {}};
}

config::Config makeConfig() {
    config::ConfigSources sources;
    sources.directory = std::filesystem::temp_directory_path() / "cppboot-absent-directory";
    sources.defaults = {{"cppboot.logging.level", "off"}};
    return config::Config::load(sources).value();
}

logging::Logger makeLogger() {
    logging::LoggerOptions options;
    options.level = logging::Level::Off;
    return logging::Logger::create(options).value();
}

}  // namespace

class MonitoredTest : public ::testing::Test {
protected:
    void SetUp() override {
        container_.registerInstance(borrow(config_));
        container_.registerInstance(borrow(logger_));
        cppboot::generated::registerComponents(container_);

        const auto valid = container_.validate();
        ASSERT_TRUE(valid) << valid.error().message();
        router_ = container_.resolve<Router>();
    }

    Response send(Method method, std::string_view target) const {
        return router_->handle(Request::create(method, target).value());
    }

    config::Config config_ = makeConfig();
    logging::Logger logger_ = makeLogger();
    di::Container container_;
    std::shared_ptr<Router> router_;
};

TEST_F(MonitoredTest, RegistersAServerModule) {
    EXPECT_EQ(container_.resolveAll<Module>().size(), 1U);
}

TEST_F(MonitoredTest, HealthIncludesMarkedIndicators) {
    const Response response = send(Method::Get, "/actuator/health");
    EXPECT_EQ(response.status(), 200);
    EXPECT_EQ(response.body(), R"({"status":"UP","checks":{"queue":"UP","disk":"UP"}})");
}

TEST_F(MonitoredTest, BrokenIndicatorFailsHealth) {
    container_.resolve<monitored::QueueHealth>()->broken = true;

    const Response response = send(Method::Get, "/actuator/health");
    EXPECT_EQ(response.status(), 503);
    EXPECT_EQ(response.body().find("The queue is down"), std::string::npos);
}

TEST_F(MonitoredTest, ComponentsCanAddTheirOwnMetrics) {
    EXPECT_EQ(send(Method::Post, "/orders").status(), 204);
    EXPECT_EQ(send(Method::Post, "/orders").status(), 204);

    const std::string metrics = send(Method::Get, "/actuator/metrics").body();
    EXPECT_NE(metrics.find("orders_created_total 2"), std::string::npos);
    EXPECT_NE(metrics.find(R"(http_requests_total{method="POST",route="/orders",status="204"} 2)"),
              std::string::npos)
        << metrics;
}
