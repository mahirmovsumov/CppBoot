#include <gtest/gtest.h>

#include <cppboot/di/container.hpp>

#include "cppboot_components.generated.hpp"

using cppboot::Exception;
using cppboot::di::Container;

class GeneratedRegistrationTest : public ::testing::Test {
protected:
    void SetUp() override { cppboot::generated::registerComponents(container_); }

    Container container_;
};

TEST_F(GeneratedRegistrationTest, ProducesAValidGraph) {
    const auto result = container_.validate();
    EXPECT_TRUE(result) << (result ? "" : result.error().message());
}

TEST_F(GeneratedRegistrationTest, WiresConstructorDependencies) {
    const auto controller = container_.resolve<demo::web::UserController>();
    EXPECT_EQ(controller->handle(7), "Hello, user-7 / user-7");
}

TEST_F(GeneratedRegistrationTest, ComponentsCanBeFoundByTheirInterface) {
    EXPECT_EQ(container_.resolve<demo::UserRepository>()->findName(1), "user-1");
}

TEST_F(GeneratedRegistrationTest, InterfaceAndComponentShareOneInstance) {
    EXPECT_EQ(container_.resolve<demo::UserRepository>().get(),
              container_.resolve<demo::MemoryUserRepository>().get());
}

TEST_F(GeneratedRegistrationTest, SharesOneInstanceOfEachComponent) {
    const auto service = container_.resolve<demo::UserService>();
    EXPECT_EQ(container_.resolve<demo::web::UserController>()->users(), service.get());
}

TEST_F(GeneratedRegistrationTest, WiresDependenciesTakenByReference) {
    EXPECT_EQ(container_.resolve<demo::ReportService>()->report(7), "user-7/Hello, user-7");
}

TEST_F(GeneratedRegistrationTest, ReferencesAreToTheSharedInstance) {
    const auto service = container_.resolve<demo::UserService>();
    EXPECT_EQ(container_.resolve<demo::ReportService>()->users(), service.get());
}

TEST_F(GeneratedRegistrationTest, MixesReferencesAndSharedPointers) {
    EXPECT_EQ(container_.resolve<demo::MixedService>()->mixed(1),
              "user-1/Hello, user-1/Hello, user-1");
}

TEST_F(GeneratedRegistrationTest, LeavesOutClassesThatAreNotMarked) {
    EXPECT_THROW(static_cast<void>(container_.resolve<demo::HelperWithoutAnnotation>()), Exception);
}
