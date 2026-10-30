#include <gtest/gtest.h>

#include <cppboot/boot.hpp>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cppboot;
using di::Deps;

namespace {

using Events = std::vector<std::string>;

class Log {
public:
    explicit Log(Events& events) : events_(events) {}
    void add(std::string event) { events_.push_back(std::move(event)); }

private:
    Events& events_;
};

class Worker : public Module {
public:
    Worker(std::string name, std::shared_ptr<Log> log, std::vector<std::string> dependencies = {})
        : name_(std::move(name)), log_(std::move(log)), dependencies_(std::move(dependencies)) {}

    std::string_view name() const noexcept override { return name_; }
    std::vector<std::string> dependencies() const override { return dependencies_; }

    Result<void> start(ApplicationContext&) override {
        log_->add("start " + name_);
        return {};
    }

    void stop() noexcept override { log_->add("stop " + name_); }

private:
    std::string name_;
    std::shared_ptr<Log> log_;
    std::vector<std::string> dependencies_;
};

struct FirstWorker : Worker {
    explicit FirstWorker(std::shared_ptr<Log> log) : Worker("first", std::move(log)) {}
};

struct SecondWorker : Worker {
    explicit SecondWorker(std::shared_ptr<Log> log)
        : Worker("second", std::move(log), {"first"}) {}
};

class Unbuildable : public Module {
public:
    Unbuildable() { throw std::runtime_error("cannot build"); }
    std::string_view name() const noexcept override { return "unbuildable"; }
    Result<void> start(ApplicationContext&) override { return {}; }
};

}  // namespace

class BootTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = std::filesystem::temp_directory_path() /
                     (std::string{"cppboot-boot-test-"} + info->name());
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);

        options_.config_directory = directory_;
        options_.environment = std::map<std::string, std::string>{};
        options_.default_settings = {{"cppboot.logging.level", "off"},
                                     {"cppboot.runtime.shutdown_after_start", "true"}};
    }

    void TearDown() override { std::filesystem::remove_all(directory_); }

    std::filesystem::path directory_;
    ApplicationOptions options_;
    Events events_;
};

TEST_F(BootTest, RunsModuleComponentsWithTheirDependencies) {
    const int exit_code = runApplication(
        0, nullptr,
        [&](di::Container& container) {
            container.registerInstance(std::make_shared<Log>(events_));
            // Registered in reverse, so that only the modules' own dependencies set the order.
            container.registerSingleton<SecondWorker>(di::Deps<Log>{});
            container.bindAll<Module, SecondWorker>();
            container.registerSingleton<FirstWorker>(di::Deps<Log>{});
            container.bindAll<Module, FirstWorker>();
        },
        options_);

    EXPECT_EQ(exit_code, kExitSuccess);
    EXPECT_EQ(events_, (Events{"start first", "start second", "stop second", "stop first"}));
}

TEST_F(BootTest, ComponentsCanTakeSettingsAndLoggerInTheirConstructors) {
    struct Configured {
        Configured(std::shared_ptr<const config::Config> config,
                   std::shared_ptr<const logging::Logger> logger)
            : logger_(std::move(logger)), port_(config->getString("myapp.port")) {
            logger_->info("Configured for port {}", port_);
        }
        std::shared_ptr<const logging::Logger> logger_;
        std::string port_;
    };

    struct UsesConfigured : Worker {
        UsesConfigured(std::shared_ptr<Log> log, std::shared_ptr<Configured> configured,
                       std::shared_ptr<std::string> seen_port)
            : Worker("uses-configured", std::move(log)) {
            *seen_port = configured->port_;
        }
    };

    options_.default_settings["myapp.port"] = "8081";
    const auto seen_port = std::make_shared<std::string>();

    const int exit_code = runApplication(
        0, nullptr,
        [&](di::Container& container) {
            container.registerInstance(std::make_shared<Log>(events_));
            container.registerInstance(seen_port);
            container.registerSingleton<Configured>(Deps<config::Config, logging::Logger>{});
            container.registerSingleton<UsesConfigured>(Deps<Log, Configured, std::string>{});
            container.bindAll<Module, UsesConfigured>();
        },
        options_);

    EXPECT_EQ(exit_code, kExitSuccess);
    EXPECT_EQ(*seen_port, "8081");
}

TEST_F(BootTest, RunsWithNoComponents) {
    EXPECT_EQ(runApplication(0, nullptr, [](di::Container&) {}, options_), kExitSuccess);
}

TEST_F(BootTest, InvalidComponentsStopBeforeAnythingStarts) {
    const int exit_code = runApplication(
        0, nullptr,
        [&](di::Container& container) {
            container.registerSingleton<FirstWorker>(di::Deps<Log>{});
            container.bindAll<Module, FirstWorker>();
        },
        options_);

    EXPECT_EQ(exit_code, kExitFailure);
    EXPECT_TRUE(events_.empty());
}

TEST_F(BootTest, RegistrationExceptionIsContained) {
    const int exit_code = runApplication(
        0, nullptr, [](di::Container&) { throw std::runtime_error("bad setup"); }, options_);
    EXPECT_EQ(exit_code, kExitFailure);
}

TEST_F(BootTest, ConstructorExceptionIsContained) {
    const int exit_code = runApplication(
        0, nullptr,
        [](di::Container& container) {
            container.registerSingleton<Unbuildable>();
            container.bindAll<Module, Unbuildable>();
        },
        options_);
    EXPECT_EQ(exit_code, kExitFailure);
}
