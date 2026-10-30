#include <gtest/gtest.h>

#include <cppboot/core/module.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using cppboot::ErrorCode;
using cppboot::Module;
using cppboot::ModuleManager;
using cppboot::Result;

namespace {

class FakeModule : public Module {
public:
    explicit FakeModule(std::string name, std::vector<std::string> dependencies = {})
        : name_(std::move(name)), dependencies_(std::move(dependencies)) {}

    std::string_view name() const noexcept override { return name_; }
    std::vector<std::string> dependencies() const override { return dependencies_; }
    Result<void> start(cppboot::ApplicationContext&) override { return {}; }

private:
    std::string name_;
    std::vector<std::string> dependencies_;
};

}  // namespace

TEST(ModuleManagerTest, OrdersDependenciesFirstAndKeepsRegistrationOrderOtherwise) {
    ModuleManager manager;
    manager.add(std::make_unique<FakeModule>("web", std::vector<std::string>{"data"}));
    manager.add(std::make_unique<FakeModule>("logging"));
    manager.add(std::make_unique<FakeModule>("data", std::vector<std::string>{"logging"}));
    manager.add(std::make_unique<FakeModule>("metrics"));

    ASSERT_TRUE(manager.resolveOrder());
    EXPECT_EQ(manager.orderedNames(),
              (std::vector<std::string>{"logging", "data", "web", "metrics"}));
}

TEST(ModuleManagerTest, RejectsDuplicateNames) {
    ModuleManager manager;
    manager.add(std::make_unique<FakeModule>("same"));
    manager.add(std::make_unique<FakeModule>("same"));

    const auto result = manager.resolveOrder();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::DependencyError);
}

TEST(ModuleManagerTest, RejectsUnknownDependencies) {
    ModuleManager manager;
    manager.add(std::make_unique<FakeModule>("web", std::vector<std::string>{"absent"}));

    const auto result = manager.resolveOrder();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("absent"), std::string::npos);
}

TEST(ModuleManagerTest, RejectsCycles) {
    ModuleManager manager;
    manager.add(std::make_unique<FakeModule>("a", std::vector<std::string>{"b"}));
    manager.add(std::make_unique<FakeModule>("b", std::vector<std::string>{"a"}));
    manager.add(std::make_unique<FakeModule>("free"));

    const auto result = manager.resolveOrder();
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("Circular"), std::string::npos);
    EXPECT_EQ(result.error().message().find("free"), std::string::npos);
}

TEST(ModuleManagerTest, RejectsSelfDependency) {
    ModuleManager manager;
    manager.add(std::make_unique<FakeModule>("a", std::vector<std::string>{"a"}));
    EXPECT_FALSE(manager.resolveOrder());
}

TEST(ModuleManagerTest, RejectsNullModules) {
    ModuleManager manager;
    manager.add(nullptr);
    EXPECT_FALSE(manager.resolveOrder());
}
