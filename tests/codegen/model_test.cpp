#include <gtest/gtest.h>

#include <cppboot/codegen/model.hpp>
#include <string>
#include <vector>

using cppboot::ErrorCode;
using cppboot::codegen::Component;
using cppboot::codegen::renderRegistration;
using cppboot::codegen::validateComponents;

TEST(ValidateComponentsTest, AcceptsResolvedDependencies) {
    const std::vector<Component> components = {
        {"Service", "a.hpp:1", {}, {"Repository"}},
        {"SqlRepository", "a.hpp:2", {"Repository"}, {}},
    };
    EXPECT_TRUE(validateComponents(components));
}

TEST(ValidateComponentsTest, RejectsMissingDependency) {
    const auto result = validateComponents({{"Service", "a.hpp:1", {}, {"Absent"}}});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::DependencyError);
    EXPECT_NE(result.error().message().find("Service (a.hpp:1) needs Absent"), std::string::npos);
}

TEST(ValidateComponentsTest, RejectsTwoProvidersOfOneInterface) {
    const auto result = validateComponents({
        {"SqlRepository", "a.hpp:1", {"Repository"}, {}},
        {"MemoryRepository", "a.hpp:2", {"Repository"}, {}},
    });
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("provided by both"), std::string::npos);
}

TEST(ValidateComponentsTest, RejectsTheSameTypeTwice) {
    EXPECT_FALSE(validateComponents({{"A", "a.hpp:1", {}, {}}, {"A", "b.hpp:1", {}, {}}}));
}

TEST(ValidateComponentsTest, ReportsTheCyclePath) {
    const auto result = validateComponents({
        {"Start", "a.hpp:1", {}, {"A"}},
        {"A", "a.hpp:2", {}, {"B"}},
        {"B", "a.hpp:3", {}, {"A"}},
    });
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("A -> B -> A"), std::string::npos);
    EXPECT_EQ(result.error().message().find("Start"), std::string::npos);
}

TEST(ValidateComponentsTest, RejectsADependencyOnItself) {
    EXPECT_FALSE(validateComponents({{"A", "a.hpp:1", {}, {"A"}}}));
}

TEST(ValidateComponentsTest, AllowsSharedDependencies) {
    EXPECT_TRUE(validateComponents({
        {"Top", "a.hpp:1", {}, {"Left", "Right"}},
        {"Left", "a.hpp:2", {}, {"Base"}},
        {"Right", "a.hpp:3", {}, {"Base"}},
        {"Base", "a.hpp:4", {}, {}},
    }));
}

TEST(RenderRegistrationTest, WritesIncludesInterfacesAndArguments) {
    const std::string text = renderRegistration(
        {
            {"ns::Service", "a.hpp:1", {"ns::IService"}, {"ns::A", "ns::B"}},
            {"ns::Leaf", "a.hpp:2", {}, {}},
        },
        {"D:/app/a.hpp"});

    EXPECT_NE(text.find("#include \"D:/app/a.hpp\""), std::string::npos);
    EXPECT_NE(text.find("container.registerSingleton<ns::Service>("
                        "cppboot::di::Deps<ns::A, ns::B>{});"),
              std::string::npos);
    EXPECT_NE(text.find("container.bind<ns::IService, ns::Service>();"), std::string::npos);
    EXPECT_NE(text.find("container.registerSingleton<ns::Leaf>(cppboot::di::Deps<>{});"),
              std::string::npos);
}
