#include <gtest/gtest.h>

#include <cppboot/core/lifecycle.hpp>

using cppboot::Lifecycle;
using cppboot::LifecycleState;

TEST(LifecycleTest, FollowsTheNormalPath) {
    Lifecycle lifecycle;
    EXPECT_EQ(lifecycle.state(), LifecycleState::Initialized);

    for (const LifecycleState next :
         {LifecycleState::Configuring, LifecycleState::RegisteringModules,
          LifecycleState::CreatingContext, LifecycleState::Starting, LifecycleState::Running,
          LifecycleState::Stopping, LifecycleState::Terminated}) {
        EXPECT_TRUE(lifecycle.transitionTo(next));
        EXPECT_EQ(lifecycle.state(), next);
    }
}

TEST(LifecycleTest, RejectsSkippedStates) {
    Lifecycle lifecycle;
    EXPECT_FALSE(lifecycle.transitionTo(LifecycleState::Running));
    EXPECT_EQ(lifecycle.state(), LifecycleState::Initialized);
}

TEST(LifecycleTest, FailureLeadsOnlyToTermination) {
    Lifecycle lifecycle;
    ASSERT_TRUE(lifecycle.transitionTo(LifecycleState::Configuring));
    ASSERT_TRUE(lifecycle.transitionTo(LifecycleState::Failed));

    EXPECT_FALSE(lifecycle.transitionTo(LifecycleState::Running));
    EXPECT_TRUE(lifecycle.transitionTo(LifecycleState::Terminated));
}

TEST(LifecycleTest, TerminatedIsFinal) {
    EXPECT_FALSE(Lifecycle::isAllowed(LifecycleState::Terminated, LifecycleState::Configuring));
    EXPECT_FALSE(Lifecycle::isAllowed(LifecycleState::Terminated, LifecycleState::Failed));
}
