#include <gtest/gtest.h>

#include <cppboot/platform/process.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using cppboot::ErrorCode;
using cppboot::platform::ProcessOptions;
using cppboot::platform::runProcess;

class ProcessTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = std::filesystem::temp_directory_path() /
                     (std::string{"cppboot-process-test-"} + info->name());
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
        report_ = directory_ / "report.txt";
    }

    void TearDown() override { std::filesystem::remove_all(directory_); }

    std::vector<std::string> readReport() const {
        std::vector<std::string> lines;
        std::ifstream input{report_, std::ios::binary};
        for (std::string line; std::getline(input, line);) {
            lines.push_back(line);
        }
        return lines;
    }

    std::vector<std::string> childCommand(const std::string& exit_code) const {
        return {CPPBOOT_TEST_CHILD, report_.string(), exit_code};
    }

    std::filesystem::path directory_;
    std::filesystem::path report_;
};

TEST_F(ProcessTest, ReturnsTheExitCode) {
    const auto result = runProcess({childCommand("42"), {}});
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result.value(), 42);
}

TEST_F(ProcessTest, PassesArgumentsUnchanged) {
    const std::vector<std::string> arguments = {
        "plain", "with space", "quote\"inside", "trailing\\", "back\\slash", "",
        "ends with space ", "\\\"both\\\"",
    };

    auto command = childCommand("0");
    command.insert(command.end(), arguments.begin(), arguments.end());

    const auto result = runProcess({command, {}});
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result.value(), 0);

    const auto lines = readReport();
    ASSERT_EQ(lines.size(), arguments.size() + 1);
    EXPECT_EQ(std::vector<std::string>(lines.begin() + 1, lines.end()), arguments);
}

TEST_F(ProcessTest, UsesTheWorkingDirectory) {
    const auto result = runProcess({childCommand("0"), directory_});
    ASSERT_TRUE(result) << result.error().describe();

    const auto lines = readReport();
    ASSERT_FALSE(lines.empty());
    EXPECT_TRUE(std::filesystem::equivalent(lines.front(), directory_));
}

TEST_F(ProcessTest, MissingProgramIsNotFound) {
    const auto result = runProcess({{"cppboot-program-that-does-not-exist"}, {}});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

TEST_F(ProcessTest, EmptyCommandIsAnError) {
    const auto result = runProcess({{}, {}});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(ProcessTest, ChildProcessReportsItsExitCode) {
    auto child = cppboot::platform::ChildProcess::start({childCommand("9"), {}});
    ASSERT_TRUE(child) << child.error().describe();
    std::optional<int> code;
    for (int i = 0; i < 500 && !code; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        code = child.value().exitCode();
    }
    EXPECT_EQ(code, 9);
    EXPECT_EQ(child.value().stop(std::chrono::seconds{1}), 9);
}

TEST_F(ProcessTest, ChildProcessStopsAsCtrlCWould) {
    auto child = cppboot::platform::ChildProcess::start({{CPPBOOT_TEST_CHILD, report_.string(), "wait"}, {}});
    ASSERT_TRUE(child) << child.error().describe();
    // It has to be waiting before it's asked, or the request could come before its handler.
    for (int i = 0; i < 500 && readReport() != std::vector<std::string>{"waiting"}; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    EXPECT_FALSE(child.value().exitCode());

    // A clean exit, rather than being ended, shows that it shut down.
    EXPECT_EQ(child.value().stop(std::chrono::seconds{10}), 7);
    EXPECT_EQ(readReport(), std::vector<std::string>{"stopped"});
}

TEST_F(ProcessTest, ChildProcessMissingProgramIsNotFound) {
    const auto child = cppboot::platform::ChildProcess::start({{"cppboot-program-that-does-not-exist"}, {}});
    ASSERT_FALSE(child);
    EXPECT_EQ(child.error().code(), ErrorCode::NotFound);
}

TEST(ExecutablePathTest, NamesTheRunningProgram) {
    const auto path = cppboot::platform::executablePath();
    ASSERT_TRUE(path);
    EXPECT_EQ(path.value().stem().string(), "cppboot-unit-tests");
    EXPECT_TRUE(path.value().is_absolute());
}
