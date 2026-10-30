#include <gtest/gtest.h>

#include <cppboot/platform/environment.hpp>
#include <cppboot/platform/signals.hpp>
#include <csignal>
#include <thread>

using cppboot::ErrorCode;
using cppboot::platform::ShutdownSignal;

TEST(ShutdownSignalTest, RequestWakesWaiterAndStaysSet) {
    auto signal = ShutdownSignal::install();
    ASSERT_TRUE(signal);
    EXPECT_FALSE(signal.value().requested());

    std::jthread requester{[&] { signal.value().request(); }};
    signal.value().wait();

    EXPECT_TRUE(signal.value().requested());
    signal.value().wait();
}

TEST(ShutdownSignalTest, OnlyOneInstanceCanBeInstalled) {
    auto first = ShutdownSignal::install();
    ASSERT_TRUE(first);

    const auto second = ShutdownSignal::install();
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code(), ErrorCode::InvalidState);
}

TEST(ShutdownSignalTest, CanBeInstalledAgainAfterDestruction) {
    {
        auto first = ShutdownSignal::install();
        ASSERT_TRUE(first);
        first.value().request();
    }

    auto second = ShutdownSignal::install();
    ASSERT_TRUE(second);
    EXPECT_FALSE(second.value().requested());
}

#if !defined(_WIN32)
TEST(ShutdownSignalTest, RaisedTerminationSignalIsObserved) {
    auto signal = ShutdownSignal::install();
    ASSERT_TRUE(signal);

    ASSERT_EQ(std::raise(SIGTERM), 0);
    signal.value().wait();
    EXPECT_TRUE(signal.value().requested());
}
#endif

TEST(EnvironmentTest, ReturnsVariablesOfTheProcess) {
    EXPECT_FALSE(cppboot::platform::environmentVariables().empty());
}
