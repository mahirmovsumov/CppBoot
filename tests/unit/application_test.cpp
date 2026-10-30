#include <gtest/gtest.h>

#include <cppboot/cppboot.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace cppboot;

namespace {

using Events = std::vector<std::string>;

enum class StartBehavior { RequestShutdown, Succeed, ReturnError, Throw, SpawnFailingWorker };

class RecordingModule : public Module {
public:
    RecordingModule(std::string name, Events& events, StartBehavior behavior,
                    std::vector<std::string> dependencies = {})
        : name_(std::move(name)),
          events_(events),
          behavior_(behavior),
          dependencies_(std::move(dependencies)) {}

    std::string_view name() const noexcept override { return name_; }
    std::vector<std::string> dependencies() const override { return dependencies_; }

    Result<void> start(ApplicationContext& context) override {
        events_.push_back("start " + name_);
        switch (behavior_) {
            case StartBehavior::RequestShutdown:
                context.requestShutdown();
                return {};
            case StartBehavior::Succeed:
                return {};
            case StartBehavior::ReturnError:
                return Error{ErrorCode::StartupError, "refused"};
            case StartBehavior::Throw:
                throw std::runtime_error("exploded");
            case StartBehavior::SpawnFailingWorker:
                return context.workers().spawn("broken", [](std::stop_token) {
                    throw std::runtime_error("worker exploded");
                });
        }
        return {};
    }

    void stop() noexcept override { events_.push_back("stop " + name_); }

private:
    std::string name_;
    Events& events_;
    StartBehavior behavior_;
    std::vector<std::string> dependencies_;
};

}  // namespace

class ApplicationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = std::filesystem::temp_directory_path() /
                     (std::string{"cppboot-application-test-"} + info->name());
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);

        options_.config_directory = directory_;
        options_.environment = std::map<std::string, std::string>{};
        options_.default_settings = {{"cppboot.logging.level", "off"}};
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    std::filesystem::path directory_;
    ApplicationOptions options_;
    Events events_;
};

TEST_F(ApplicationTest, StartsAndStopsModulesInDependencyOrder) {
    Application app{0, nullptr, options_};
    app.emplaceModule<RecordingModule>("web", events_, StartBehavior::RequestShutdown,
                                       std::vector<std::string>{"data"});
    app.emplaceModule<RecordingModule>("data", events_, StartBehavior::Succeed);

    EXPECT_EQ(app.state(), LifecycleState::Initialized);
    EXPECT_EQ(app.run(), kExitSuccess);
    EXPECT_EQ(app.state(), LifecycleState::Terminated);
    EXPECT_EQ(events_, (Events{"start data", "start web", "stop web", "stop data"}));
}

TEST_F(ApplicationTest, ShutdownRequestedBeforeRunStillStopsCleanly) {
    Application app{0, nullptr, options_};
    app.emplaceModule<RecordingModule>("only", events_, StartBehavior::Succeed);

    app.requestShutdown();
    EXPECT_EQ(app.run(), kExitSuccess);
    EXPECT_EQ(events_, (Events{"start only", "stop only"}));
}

TEST_F(ApplicationTest, ShutdownRequestFromAnotherThreadEndsRun) {
    Application app{0, nullptr, options_};
    app.emplaceModule<RecordingModule>("only", events_, StartBehavior::Succeed);

    std::jthread requester{[&](std::stop_token token) {
        while (!token.stop_requested() && app.state() != LifecycleState::Running) {
            std::this_thread::yield();
        }
        app.requestShutdown();
    }};

    EXPECT_EQ(app.run(), kExitSuccess);
}

TEST_F(ApplicationTest, StartFailureStopsOnlyModulesThatStarted) {
    Application app{0, nullptr, options_};
    app.emplaceModule<RecordingModule>("good", events_, StartBehavior::Succeed);
    app.emplaceModule<RecordingModule>("bad", events_, StartBehavior::ReturnError);
    app.emplaceModule<RecordingModule>("never", events_, StartBehavior::Succeed);

    EXPECT_EQ(app.run(), kExitFailure);
    EXPECT_EQ(app.state(), LifecycleState::Terminated);
    EXPECT_EQ(events_, (Events{"start good", "start bad", "stop good"}));
}

TEST_F(ApplicationTest, ExceptionDuringStartIsContained) {
    Application app{0, nullptr, options_};
    app.emplaceModule<RecordingModule>("bad", events_, StartBehavior::Throw);

    EXPECT_EQ(app.run(), kExitFailure);
    EXPECT_EQ(events_, (Events{"start bad"}));
}

TEST_F(ApplicationTest, DependencyCycleStartsNothing) {
    Application app{0, nullptr, options_};
    app.emplaceModule<RecordingModule>("a", events_, StartBehavior::Succeed,
                                       std::vector<std::string>{"b"});
    app.emplaceModule<RecordingModule>("b", events_, StartBehavior::Succeed,
                                       std::vector<std::string>{"a"});

    EXPECT_EQ(app.run(), kExitFailure);
    EXPECT_TRUE(events_.empty());
}

TEST_F(ApplicationTest, WorkerFailureShutsDownAndFailsTheProcess) {
    Application app{0, nullptr, options_};
    app.emplaceModule<RecordingModule>("worker", events_, StartBehavior::SpawnFailingWorker);

    EXPECT_EQ(app.run(), kExitFailure);
    EXPECT_EQ(events_, (Events{"start worker", "stop worker"}));
}

TEST_F(ApplicationTest, MissingProfileIsAConfigurationError) {
    options_.environment = std::map<std::string, std::string>{{"CPPBOOT_PROFILE", "absent"}};

    Application app{0, nullptr, options_};
    app.emplaceModule<RecordingModule>("only", events_, StartBehavior::Succeed);

    EXPECT_EQ(app.run(), kExitConfigurationError);
    EXPECT_EQ(app.state(), LifecycleState::Terminated);
    EXPECT_TRUE(events_.empty());
}

TEST_F(ApplicationTest, InvalidLogLevelIsAConfigurationError) {
    options_.default_settings = {{"cppboot.logging.level", "loud"}};

    Application app{0, nullptr, options_};
    EXPECT_EQ(app.run(), kExitConfigurationError);
}

TEST_F(ApplicationTest, ShutdownAfterStartSettingStopsTheApplication) {
    options_.default_settings["cppboot.runtime.shutdown_after_start"] = "true";

    Application app{0, nullptr, options_};
    app.emplaceModule<RecordingModule>("only", events_, StartBehavior::Succeed);

    EXPECT_EQ(app.run(), kExitSuccess);
    EXPECT_EQ(events_, (Events{"start only", "stop only"}));
}

TEST_F(ApplicationTest, InvalidShutdownAfterStartIsAConfigurationError) {
    options_.default_settings["cppboot.runtime.shutdown_after_start"] = "maybe";

    Application app{0, nullptr, options_};
    EXPECT_EQ(app.run(), kExitConfigurationError);
}

TEST_F(ApplicationTest, RunCanOnlyHappenOnce) {
    Application app{0, nullptr, options_};
    app.requestShutdown();

    EXPECT_EQ(app.run(), kExitSuccess);
    EXPECT_EQ(app.run(), kExitFailure);
}

TEST_F(ApplicationTest, CommandLineOverridesConfiguration) {
    std::string program = "test";
    std::string argument = "--cppboot.logging.level=loud";
    char* argv[] = {program.data(), argument.data()};

    options_.default_settings = {{"cppboot.logging.level", "off"}};
    Application app{2, argv, options_};
    EXPECT_EQ(app.run(), kExitConfigurationError);
}
