#include <gtest/gtest.h>

#include <atomic>
#include <cppboot/core/worker_group.hpp>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <string>

using cppboot::WorkerGroup;

TEST(WorkerGroupTest, StopAllStopsAndJoinsWorkers) {
    WorkerGroup group{nullptr};
    std::latch running{1};
    std::atomic<bool> finished{false};

    ASSERT_TRUE(group.spawn("loop", [&](std::stop_token token) {
        running.count_down();
        while (!token.stop_requested()) {
            std::this_thread::yield();
        }
        finished.store(true);
    }));

    running.wait();
    EXPECT_EQ(group.size(), 1U);

    group.stopAll();
    EXPECT_TRUE(finished.load());
    EXPECT_EQ(group.size(), 0U);
}

TEST(WorkerGroupTest, SpawnFailsAfterStopAll) {
    WorkerGroup group{nullptr};
    group.stopAll();
    EXPECT_FALSE(group.spawn("late", [](std::stop_token) {}));
}

TEST(WorkerGroupTest, ReportsExceptionsToFailureHandler) {
    std::mutex mutex;
    std::string worker_name;
    std::string failure_reason;

    {
        WorkerGroup group{[&](std::string_view worker, std::string_view reason) {
            const std::lock_guard lock{mutex};
            worker_name = worker;
            failure_reason = reason;
        }};
        ASSERT_TRUE(group.spawn("broken", [](std::stop_token) {
            throw std::runtime_error("boom");
        }));
    }

    EXPECT_EQ(worker_name, "broken");
    EXPECT_EQ(failure_reason, "boom");
}
