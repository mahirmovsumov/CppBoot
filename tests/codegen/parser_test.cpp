#include <gtest/gtest.h>

#include <cppboot/codegen/parser.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using cppboot::ErrorCode;
using cppboot::codegen::parseComponents;

namespace {

constexpr const char* kPreamble = R"(
#include <memory>
#define SERVICE [[clang::annotate("cppboot:service")]]
)";

}  // namespace

class ParserTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = std::filesystem::temp_directory_path() /
                     (std::string{"cppboot-parser-test-"} + info->name());
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
    }

    void TearDown() override { std::filesystem::remove_all(directory_); }

    auto parse(const std::string& source, const std::string& name = "input.hpp") const {
        const auto path = directory_ / name;
        std::ofstream{path, std::ios::binary} << kPreamble << source;
        return parseComponents(path, {"-I" + directory_.string()});
    }

    void expectError(const std::string& source, const std::string& expected) const {
        const auto result = parse(source);
        ASSERT_FALSE(result) << source;
        EXPECT_NE(result.error().message().find(expected), std::string::npos)
            << result.error().message();
    }

    std::filesystem::path directory_;
};

TEST_F(ParserTest, ReadsQualifiedNamesAndDependencies) {
    const auto result = parse(R"(
        namespace app {
        struct Repo { virtual ~Repo() = default; virtual void save() = 0; };
        namespace inner {
        class SERVICE SqlRepo : public Repo { public: void save() override {} };
        }
        class SERVICE Service {
        public:
            Service(std::shared_ptr<Repo> a, const std::shared_ptr<const inner::SqlRepo>& b);
        };
        }
    )");
    ASSERT_TRUE(result) << result.error().message();
    ASSERT_EQ(result.value().size(), 2U);

    EXPECT_EQ(result.value()[0].type, "app::inner::SqlRepo");
    EXPECT_EQ(result.value()[0].interfaces, std::vector<std::string>{"app::Repo"});
    EXPECT_TRUE(result.value()[0].dependencies.empty());

    EXPECT_EQ(result.value()[1].type, "app::Service");
    EXPECT_EQ(result.value()[1].dependencies,
              (std::vector<std::string>{"app::Repo", "app::inner::SqlRepo"}));
}

TEST_F(ParserTest, SeesThroughTypeAliases) {
    const auto result = parse(R"(
        namespace app {
        class SERVICE Repo {};
        using RepoPointer = std::shared_ptr<Repo>;
        class SERVICE Service { public: explicit Service(RepoPointer repo); };
        }
    )");
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(result.value()[1].dependencies, std::vector<std::string>{"app::Repo"});
}

TEST_F(ParserTest, IgnoresClassesThatAreNotMarked) {
    const auto result = parse("class Plain {}; class SERVICE Marked {};");
    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_EQ(result.value()[0].type, "Marked");
}

TEST_F(ParserTest, SkipsComponentsInIncludedFiles) {
    std::ofstream{directory_ / "other.hpp"} << kPreamble << "class SERVICE Included {};\n";

    const auto result = parse("#include \"other.hpp\"\nclass SERVICE Local {};");
    ASSERT_TRUE(result) << result.error().message();
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_EQ(result.value()[0].type, "Local");
}

TEST_F(ParserTest, IgnoresCopyAndMoveConstructors) {
    const auto result = parse(R"(
        class SERVICE Copyable {
        public:
            Copyable();
            Copyable(const Copyable&);
            Copyable(Copyable&&);
        };
    )");
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_TRUE(result.value()[0].dependencies.empty());
}

TEST_F(ParserTest, LocationNamesTheFileAndLine) {
    const auto result = parse("\nclass SERVICE Marked {};");
    ASSERT_TRUE(result);
    EXPECT_NE(result.value()[0].location.find("input.hpp:"), std::string::npos);
}

TEST_F(ParserTest, RejectsAbstractComponents) {
    expectError("class SERVICE Pure { public: virtual void f() = 0; };", "abstract");
}

TEST_F(ParserTest, RejectsMultiplePublicConstructors) {
    expectError(R"(
        class SERVICE Two { public: Two(); explicit Two(std::shared_ptr<int> value); };
    )",
                "more than one public constructor");
}

TEST_F(ParserTest, RejectsPrivateConstructors) {
    expectError("class SERVICE Hidden { Hidden(); };", "no public constructor");
}

TEST_F(ParserTest, RejectsParametersThatAreNotSharedPointers) {
    expectError("class SERVICE Takes { public: explicit Takes(int size); };",
                "Constructor parameter 'size' of Takes");
}

TEST_F(ParserTest, RejectsTemplates) {
    expectError("template <typename T> class SERVICE Box {};", "template");
}

TEST_F(ParserTest, RejectsAnonymousNamespaces) {
    expectError("namespace { class SERVICE Hidden {}; }", "can't name it");
}

TEST_F(ParserTest, ReportsCompileErrors) {
    const auto result = parse("class SERVICE Broken { UnknownType value; };");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::ParseError);
    EXPECT_NE(result.error().message().find("UnknownType"), std::string::npos);
}

TEST_F(ParserTest, ReportsAMissingFile) {
    const auto result = parseComponents(directory_ / "absent.hpp", {});
    EXPECT_FALSE(result);
}
