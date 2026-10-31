#include <gtest/gtest.h>

#include <cppboot/base/version.hpp>
#include <cppboot/cli/cli.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using cppboot::ErrorCode;
using cppboot::cli::createProject;
using cppboot::cli::readProjectName;
using cppboot::cli::validateProjectName;

class CliTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = std::filesystem::temp_directory_path() /
                     (std::string{"cppboot-cli-test-"} + info->name());
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
    }

    void TearDown() override { std::filesystem::remove_all(directory_); }

    std::string readFile(const std::filesystem::path& path) const {
        std::ifstream input{path, std::ios::binary};
        std::ostringstream content;
        content << input.rdbuf();
        return content.str();
    }

    std::filesystem::path directory_;
};

TEST_F(CliTest, AcceptsValidProjectNames) {
    EXPECT_TRUE(validateProjectName("hello"));
    EXPECT_TRUE(validateProjectName("hello-world_2"));
    EXPECT_TRUE(validateProjectName("Banking"));
}

TEST_F(CliTest, RejectsInvalidProjectNames) {
    for (const char* name : {"", "2fast", "-flag", "has space", "../escape", "a/b", "a.b"}) {
        EXPECT_FALSE(validateProjectName(name)) << name;
    }
}

TEST_F(CliTest, RejectsReservedProjectNames) {
    for (const char* name : {"test", "all", "install", "con", "NUL", "Com1"}) {
        EXPECT_FALSE(validateProjectName(name)) << name;
    }
}

TEST_F(CliTest, CreatesAProjectUsingItsName) {
    ASSERT_TRUE(createProject("demo-app", directory_));

    const auto root = directory_ / "demo-app";
    EXPECT_TRUE(std::filesystem::exists(root / "src" / "main.cpp"));
    EXPECT_NE(readFile(root / "CMakeLists.txt").find("project(demo-app "), std::string::npos);
    EXPECT_EQ(readFile(root / "CMakeLists.txt").find("@NAME@"), std::string::npos);

    const auto name = readProjectName(root);
    ASSERT_TRUE(name) << name.error().describe();
    EXPECT_EQ(name.value(), "demo-app");
}

TEST_F(CliTest, PlainProjectHasNoWebParts) {
    ASSERT_TRUE(createProject("plain", directory_));

    const auto root = directory_ / "plain";
    EXPECT_TRUE(std::filesystem::exists(root / "src" / "greeting.hpp"));
    EXPECT_FALSE(std::filesystem::exists(root / "src" / "users.hpp"));
    EXPECT_EQ(readFile(root / "CMakeLists.txt").find("cppboot::web"), std::string::npos);
    EXPECT_EQ(readFile(root / "cppboot.yaml").find("server:"), std::string::npos);
    EXPECT_EQ(readFile(root / "cppboot.yaml").find("@"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(root / "db"));
}

TEST_F(CliTest, SqliteProjectHasAMigrationAndSettings) {
    ASSERT_TRUE(createProject("store", directory_, cppboot::cli::ProjectKind::WebSqlite));

    const auto root = directory_ / "store";
    EXPECT_NE(readFile(root / "cppboot.yaml").find("url: sqlite:store.db"), std::string::npos);
    EXPECT_NE(readFile(root / "cppboot.yaml").find("port: 8080"), std::string::npos);
    EXPECT_NE(readFile(root / "CMakeLists.txt").find("cppboot::data"), std::string::npos);
    EXPECT_NE(readFile(root / "src" / "users.hpp").find("CPPBOOT_ROW"), std::string::npos);
    EXPECT_NE(readFile(root / "db" / "migrations" / "001_create_users.sql").find("INTEGER PRIMARY"),
              std::string::npos);
}

TEST_F(CliTest, PostgresqlProjectUsesItsOwnSettingsAndColumnType) {
    ASSERT_TRUE(createProject("store", directory_, cppboot::cli::ProjectKind::WebPostgresql));

    const auto root = directory_ / "store";
    EXPECT_NE(readFile(root / "cppboot.yaml").find("localhost:5432/store"), std::string::npos);
    EXPECT_NE(readFile(root / "db" / "migrations" / "001_create_users.sql").find("BIGSERIAL"),
              std::string::npos);
}

TEST_F(CliTest, SecuredProjectHasSignInAndNoSecret) {
    ASSERT_TRUE(createProject("safe", directory_, cppboot::cli::ProjectKind::WebSecured));

    const auto root = directory_ / "safe";
    EXPECT_NE(readFile(root / "src" / "auth.hpp").find("CPPBOOT_PUBLIC"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(root / "src" / "users.hpp"));
    EXPECT_NE(readFile(root / "CMakeLists.txt").find("cppboot::security"), std::string::npos);
    EXPECT_NE(readFile(root / "cppboot.yaml").find("issuer: safe"), std::string::npos);
    EXPECT_EQ(readFile(root / "cppboot.yaml").find("secret"), std::string::npos);
}

TEST_F(CliTest, SecurityOptionNeedsWebAndNoDatabase) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"create", "a", "--security"}, out, err), cppboot::cli::kExitUsage);
    EXPECT_EQ(cppboot::cli::run({"create", "a", "--web", "--security", "--sqlite"}, out, err),
              cppboot::cli::kExitUsage);
}

TEST_F(CliTest, DatabaseOptionsNeedWebAndCannotBeCombined) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"create", "a", "--sqlite"}, out, err), cppboot::cli::kExitUsage);
    EXPECT_EQ(cppboot::cli::run({"create", "a", "--web", "--sqlite", "--postgresql"}, out, err),
              cppboot::cli::kExitUsage);
}

TEST_F(CliTest, WebProjectHasAControllerAndServerSettings) {
    ASSERT_TRUE(createProject("web-app", directory_, cppboot::cli::ProjectKind::Web));

    const auto root = directory_ / "web-app";
    EXPECT_TRUE(std::filesystem::exists(root / "src" / "users.hpp"));
    EXPECT_FALSE(std::filesystem::exists(root / "src" / "greeting.hpp"));
    EXPECT_NE(readFile(root / "CMakeLists.txt").find("cppboot::web"), std::string::npos);
    EXPECT_NE(readFile(root / "cppboot.yaml").find("port: 8080"), std::string::npos);
}

TEST_F(CliTest, CreateNeedsExactlyOneName) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"create", "--web"}, out, err), cppboot::cli::kExitUsage);
    EXPECT_EQ(cppboot::cli::run({"create", "one", "two"}, out, err), cppboot::cli::kExitUsage);
}

TEST_F(CliTest, CreateLeavesAnExistingFolderUntouched) {
    const auto root = directory_ / "taken";
    std::filesystem::create_directories(root);
    std::ofstream{root / "keep.txt"} << "mine";

    const auto result = createProject("taken", directory_);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
    EXPECT_EQ(readFile(root / "keep.txt"), "mine");
    EXPECT_FALSE(std::filesystem::exists(root / "CMakeLists.txt"));
}

TEST_F(CliTest, ReadingAFolderWithNoProjectIsNotFound) {
    const auto name = readProjectName(directory_);
    ASSERT_FALSE(name);
    EXPECT_EQ(name.error().code(), ErrorCode::NotFound);
}

TEST_F(CliTest, ProjectNameFromYamlIsValidated) {
    std::ofstream{directory_ / "CMakeLists.txt"} << "";
    std::ofstream{directory_ / "cppboot.yaml"} << "cppboot:\n  application:\n    name: ../evil\n";

    EXPECT_FALSE(readProjectName(directory_));
}

TEST_F(CliTest, NoArgumentsPrintsUsageAndFails) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({}, out, err), cppboot::cli::kExitUsage);
    EXPECT_NE(err.str().find("Usage:"), std::string::npos);
}

TEST_F(CliTest, UnknownCommandFails) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"launch"}, out, err), cppboot::cli::kExitUsage);
}

TEST_F(CliTest, InfoPrintsTheVersion) {
    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(cppboot::cli::run({"info"}, out, err), 0);
    EXPECT_NE(out.str().find(std::string{cppboot::kVersionString}), std::string::npos);
}
