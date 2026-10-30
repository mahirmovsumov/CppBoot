#include <gtest/gtest.h>

#include <cppboot/config/config.hpp>
#include <filesystem>
#include <fstream>
#include <string>

using cppboot::ErrorCode;
using cppboot::config::Config;
using cppboot::config::ConfigSources;

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = std::filesystem::temp_directory_path() /
                     (std::string{"cppboot-config-test-"} + info->name());
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
        sources_.directory = directory_;
    }

    void TearDown() override { std::filesystem::remove_all(directory_); }

    void writeFile(const std::string& name, const std::string& content) const {
        std::ofstream{directory_ / name} << content;
    }

    std::filesystem::path directory_;
    ConfigSources sources_;
};

TEST_F(ConfigTest, MissingBaseFileIsAllowed) {
    sources_.defaults = {{"cppboot.server.port", "8080"}};

    const auto config = Config::load(sources_);
    ASSERT_TRUE(config);
    EXPECT_EQ(config.value().getString("cppboot.server.port"), "8080");
}

TEST_F(ConfigTest, EachSourceOverridesTheOneBelowIt) {
    sources_.defaults = {{"a", "default"}, {"b", "default"}, {"c", "default"}, {"d", "default"}};
    writeFile("cppboot.yaml", "a: yaml\nb: yaml\nc: yaml\ncppboot:\n  profile: prod\n");
    writeFile("cppboot-prod.yaml", "b: profile\nc: profile\n");
    sources_.environment = {{"CPPBOOT_C", "environment"}};
    sources_.arguments = {"--cppboot.server.port=9090"};

    const auto config = Config::load(sources_);
    ASSERT_TRUE(config) << config.error().describe();
    EXPECT_EQ(config.value().getString("d"), "default");
    EXPECT_EQ(config.value().getString("a"), "yaml");
    EXPECT_EQ(config.value().getString("b"), "profile");
    EXPECT_EQ(config.value().getString("c"), "environment");
    EXPECT_EQ(config.value().getString("cppboot.server.port"), "9090");
    EXPECT_EQ(config.value().profile(), "prod");
}

TEST_F(ConfigTest, ArgumentsOverrideEnvironment) {
    sources_.environment = {{"CPPBOOT_SERVER_PORT", "1"}};
    sources_.arguments = {"--cppboot.server.port=2"};

    const auto config = Config::load(sources_);
    ASSERT_TRUE(config);
    EXPECT_EQ(config.value().getString("cppboot.server.port"), "2");
}

TEST_F(ConfigTest, EnvironmentMatchesKnownKeysContainingUnderscores) {
    writeFile("cppboot.yaml", "cppboot:\n  runtime:\n    graceful_shutdown_timeout: 30\n");
    sources_.environment = {{"CPPBOOT_RUNTIME_GRACEFUL_SHUTDOWN_TIMEOUT", "5"}};

    const auto config = Config::load(sources_);
    ASSERT_TRUE(config);
    const auto timeout = config.value().getInt("cppboot.runtime.graceful_shutdown_timeout", 0);
    ASSERT_TRUE(timeout);
    EXPECT_EQ(timeout.value(), 5);
}

TEST_F(ConfigTest, EnvironmentMatchingTwoKeysIsAnError) {
    sources_.defaults = {{"cppboot.a_b", "1"}, {"cppboot.a.b", "2"}};
    sources_.environment = {{"CPPBOOT_A_B", "3"}};

    const auto config = Config::load(sources_);
    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(ConfigTest, UnrelatedEnvironmentAndArgumentsAreIgnored) {
    sources_.environment = {{"PATH", "/bin"}};
    sources_.arguments = {"--verbose", "input.txt"};

    const auto config = Config::load(sources_);
    ASSERT_TRUE(config);
    EXPECT_FALSE(config.value().contains("path"));
    EXPECT_FALSE(config.value().contains("verbose"));
}

TEST_F(ConfigTest, EnvironmentOverridesApplicationKeysThatExist) {
    writeFile("cppboot.yaml", "myapp:\n  request_timeout: 30\n");
    sources_.environment = {{"CPPBOOT_MYAPP_REQUEST_TIMEOUT", "5"}};

    const auto config = Config::load(sources_);
    ASSERT_TRUE(config);
    EXPECT_EQ(config.value().getString("myapp.request_timeout"), "5");
}

TEST_F(ConfigTest, ArgumentsOverrideApplicationKeysThatExist) {
    writeFile("cppboot.yaml", "myapp:\n  timeout: 30\n");
    sources_.environment = {{"CPPBOOT_MYAPP_TIMEOUT", "5"}};
    sources_.arguments = {"--myapp.timeout=7"};

    const auto config = Config::load(sources_);
    ASSERT_TRUE(config);
    EXPECT_EQ(config.value().getString("myapp.timeout"), "7");
}

TEST_F(ConfigTest, ArgumentsForUnknownApplicationKeysAreLeftAlone) {
    sources_.arguments = {"--output=report.txt", "--flag"};

    const auto config = Config::load(sources_);
    ASSERT_TRUE(config);
    EXPECT_FALSE(config.value().contains("output"));
}

TEST_F(ConfigTest, ArgumentWithoutValueIsAnError) {
    sources_.arguments = {"--cppboot.server.port"};
    EXPECT_FALSE(Config::load(sources_));
}

TEST_F(ConfigTest, SelectedProfileMustExist) {
    sources_.environment = {{"CPPBOOT_PROFILE", "missing"}};

    const auto config = Config::load(sources_);
    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code(), ErrorCode::IoError);
}

TEST_F(ConfigTest, ProfileNameCannotEscapeTheDirectory) {
    sources_.environment = {{"CPPBOOT_PROFILE", "../secret"}};

    const auto config = Config::load(sources_);
    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(ConfigTest, InvalidYamlIsAnError) {
    writeFile("cppboot.yaml", "a: [unterminated\n");

    const auto config = Config::load(sources_);
    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code(), ErrorCode::ParseError);
}

TEST_F(ConfigTest, SequencesAreFlattenedWithIndexes) {
    writeFile("cppboot.yaml", "hosts:\n  - one\n  - two\n");

    const auto config = Config::load(sources_);
    ASSERT_TRUE(config);
    EXPECT_EQ(config.value().getString("hosts.0"), "one");
    EXPECT_EQ(config.value().getString("hosts.1"), "two");
}

TEST_F(ConfigTest, TypedGettersValidateValues) {
    sources_.defaults = {{"number", "12"}, {"word", "abc"}, {"flag", "yes"}};

    const auto config = Config::load(sources_);
    ASSERT_TRUE(config);
    EXPECT_EQ(config.value().getInt("number", 0).value(), 12);
    EXPECT_EQ(config.value().getInt("absent", 3).value(), 3);
    EXPECT_FALSE(config.value().getInt("word", 0));
    EXPECT_TRUE(config.value().getBool("flag", false).value());
    EXPECT_FALSE(config.value().getBool("word", false));
}

TEST_F(ConfigTest, DescribeHidesSensitiveValues) {
    sources_.defaults = {{"database.password", "hunter2"}, {"server.port", "8080"}};

    const auto config = Config::load(sources_);
    ASSERT_TRUE(config);
    const std::string text = config.value().describe();
    EXPECT_EQ(text.find("hunter2"), std::string::npos);
    EXPECT_NE(text.find("server.port=8080"), std::string::npos);
}
