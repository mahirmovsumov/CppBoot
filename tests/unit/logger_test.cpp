#include <gtest/gtest.h>

#include <cppboot/logging/logger.hpp>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using cppboot::logging::Level;
using cppboot::logging::Logger;
using cppboot::logging::LoggerOptions;

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = std::filesystem::temp_directory_path() /
                     (std::string{"cppboot-logger-test-"} + info->name());
        std::filesystem::remove_all(directory_);
        file_ = directory_ / "logs" / "app.log";
    }

    void TearDown() override { std::filesystem::remove_all(directory_); }

    Logger create(Level level = Level::Info) const {
        auto logger = Logger::create(LoggerOptions{"shop", level, file_});
        EXPECT_TRUE(logger) << logger.error().describe();
        return std::move(logger).value();
    }

    std::vector<std::string> lines() const {
        std::ifstream input{file_, std::ios::binary};
        std::vector<std::string> result;
        for (std::string line; std::getline(input, line);) {
            result.push_back(line);
        }
        return result;
    }

    std::filesystem::path directory_;
    std::filesystem::path file_;
};

TEST_F(LoggerTest, WritesTimeNameLevelAndMessage) {
    {
        const Logger logger = create();
        logger.info("Order {} paid", 42);
        logger.warn("Low stock");
    }

    const auto written = lines();
    ASSERT_EQ(written.size(), 2u);
    const std::regex format{R"(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\] \[shop\] \[info\] Order 42 paid)"};
    EXPECT_TRUE(std::regex_match(written[0], format)) << written[0];
    EXPECT_TRUE(written[1].ends_with("[shop] [warning] Low stock")) << written[1];
}

TEST_F(LoggerTest, LeavesOutLevelsBelowTheSetting) {
    {
        const Logger logger = create(Level::Warn);
        EXPECT_FALSE(logger.enabled(Level::Info));
        EXPECT_TRUE(logger.enabled(Level::Error));
        logger.debug("hidden");
        logger.info("hidden");
        logger.error("shown");
    }
    const auto written = lines();
    ASSERT_EQ(written.size(), 1u);
    EXPECT_TRUE(written[0].ends_with("[error] shown"));
}

TEST_F(LoggerTest, OffWritesNothing) {
    {
        const Logger logger = create(Level::Off);
        EXPECT_FALSE(logger.enabled(Level::Critical));
        EXPECT_FALSE(logger.enabled(Level::Off));
        logger.critical("hidden");
    }
    EXPECT_TRUE(lines().empty());
}

TEST_F(LoggerTest, AddsToAnExistingFile) {
    create().info("first");
    create().info("second");
    const auto written = lines();
    ASSERT_EQ(written.size(), 2u);
    EXPECT_TRUE(written[1].ends_with("second"));
}

TEST_F(LoggerTest, WarningsReachTheFileAtOnce) {
    const Logger logger = create();
    logger.warn("now");
    // Still open, so only a flush on the warning itself could have written it.
    ASSERT_EQ(lines().size(), 1u);
}

TEST_F(LoggerTest, LinesFromManyThreadsStayWhole) {
    constexpr int kThreads = 8;
    constexpr int kLines = 200;
    {
        const Logger logger = create();
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&logger, t] {
                for (int i = 0; i < kLines; ++i) {
                    logger.info("thread {} line {} {}", t, i, std::string(50, 'x'));
                }
            });
        }
    }

    const auto written = lines();
    ASSERT_EQ(written.size(), static_cast<std::size_t>(kThreads * kLines));
    const std::regex whole{R"(\[.{23}\] \[shop\] \[info\] thread \d line \d+ x{50})"};
    for (const std::string& line : written) {
        ASSERT_TRUE(std::regex_match(line, whole)) << line;
    }
}

TEST_F(LoggerTest, AFileThatCannotBeOpenedIsAnError) {
    // A folder where the file should be.
    std::filesystem::create_directories(file_);
    EXPECT_FALSE(Logger::create(LoggerOptions{"shop", Level::Info, file_}));
}

TEST(ParseLevelTest, ReadsEveryLevelAndRejectsOthers) {
    EXPECT_EQ(cppboot::logging::parseLevel("warn").value(), Level::Warn);
    EXPECT_EQ(cppboot::logging::parseLevel("off").value(), Level::Off);
    EXPECT_FALSE(cppboot::logging::parseLevel("loud"));
    EXPECT_FALSE(cppboot::logging::parseLevel("INFO"));
}
