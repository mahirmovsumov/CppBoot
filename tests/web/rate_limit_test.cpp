#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cppboot/config/config.hpp>
#include <cppboot/logging/logger.hpp>
#include <cppboot/web/rate_limit.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace cppboot;
using namespace cppboot::web;
using namespace std::chrono_literals;

namespace {

Request requestFrom(const std::string& address) {
    Request request = Request::create(Method::Get, "/").value();
    request.setClientAddress(address);
    return request;
}

}  // namespace

class RateLimiterTest : public ::testing::Test {
protected:
    RateLimiter makeLimiter(RateLimit limit, RateLimiterOptions options = {}) {
        return RateLimiter{limit, options, [this] { return now_; }};
    }

    std::chrono::steady_clock::time_point now_{std::chrono::hours{1}};
};

TEST_F(RateLimiterTest, AllowsTheWholeLimitAtOnceThenRejects) {
    auto limiter = makeLimiter({5, 60s});
    const Request request = requestFrom("192.0.2.1");

    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(limiter.check(request).has_value()) << "request " << i;
    }

    const auto rejected = limiter.check(request);
    ASSERT_TRUE(rejected.has_value());
    EXPECT_EQ(rejected->status(), 429);
    EXPECT_EQ(rejected->header("Retry-After"), "12");
}

TEST_F(RateLimiterTest, GivesRequestsBackEvenlyOverTheWindow) {
    auto limiter = makeLimiter({5, 60s});
    const Request request = requestFrom("192.0.2.1");
    for (int i = 0; i < 5; ++i) {
        ASSERT_FALSE(limiter.check(request).has_value());
    }

    now_ += 11s;
    EXPECT_TRUE(limiter.check(request).has_value());

    now_ += 1s;
    EXPECT_FALSE(limiter.check(request).has_value());
    EXPECT_TRUE(limiter.check(request).has_value());
}

TEST_F(RateLimiterTest, AnIdleClientGetsItsWholeLimitBack) {
    auto limiter = makeLimiter({3, 60s});
    const Request request = requestFrom("192.0.2.1");
    for (int i = 0; i < 3; ++i) {
        ASSERT_FALSE(limiter.check(request).has_value());
    }

    now_ += 10min;
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(limiter.check(request).has_value());
    }
    EXPECT_TRUE(limiter.check(request).has_value());
}

TEST_F(RateLimiterTest, RejectedRequestsDontPushBackTheNextOne) {
    auto limiter = makeLimiter({1, 10s});
    const Request request = requestFrom("192.0.2.1");
    ASSERT_FALSE(limiter.check(request).has_value());

    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(limiter.check(request).has_value());
    }

    now_ += 10s;
    EXPECT_FALSE(limiter.check(request).has_value());
}

TEST_F(RateLimiterTest, RetryAfterRoundsUpSoClientsDontComeBackEarly) {
    auto limiter = makeLimiter({1, 10s});
    const Request request = requestFrom("192.0.2.1");
    ASSERT_FALSE(limiter.check(request).has_value());

    now_ += 8500ms;
    EXPECT_EQ(limiter.check(request)->header("Retry-After"), "2");

    now_ += 1499ms;
    EXPECT_EQ(limiter.check(request)->header("Retry-After"), "1");
}

TEST_F(RateLimiterTest, ClientsHaveSeparateLimits) {
    auto limiter = makeLimiter({1, 60s});
    EXPECT_FALSE(limiter.check(requestFrom("192.0.2.1")).has_value());
    EXPECT_FALSE(limiter.check(requestFrom("192.0.2.2")).has_value());
    EXPECT_TRUE(limiter.check(requestFrom("192.0.2.1")).has_value());
}

TEST_F(RateLimiterTest, OneIpv6NetworkSharesALimit) {
    auto limiter = makeLimiter({1, 60s});
    EXPECT_FALSE(limiter.check(requestFrom("2001:db8:1:2::1")).has_value());
    EXPECT_TRUE(limiter.check(requestFrom("2001:db8:1:2:ffff:ffff:ffff:ffff")).has_value());
    EXPECT_FALSE(limiter.check(requestFrom("2001:db8:1:3::1")).has_value());
}

TEST_F(RateLimiterTest, RequestsWithNoAddressShareALimit) {
    auto limiter = makeLimiter({1, 60s});
    EXPECT_FALSE(limiter.check(requestFrom("")).has_value());
    EXPECT_TRUE(limiter.check(requestFrom("")).has_value());
}

TEST_F(RateLimiterTest, MemoryStaysWithinTheLimitUnderManyAddresses) {
    RateLimiterOptions options;
    options.max_clients = 100;
    auto limiter = makeLimiter({1, 60s}, options);

    for (int i = 0; i < 10000; ++i) {
        EXPECT_FALSE(limiter.check(requestFrom("10.0." + std::to_string(i / 256) + "." +
                                               std::to_string(i % 256)))
                         .has_value());
    }
    EXPECT_EQ(limiter.trackedClients(), 100U);
}

TEST_F(RateLimiterTest, FullTableForgetsTheClientSeenLongestAgo) {
    RateLimiterOptions options;
    options.max_clients = 2;
    auto limiter = makeLimiter({1, 60s}, options);

    ASSERT_FALSE(limiter.check(requestFrom("192.0.2.1")).has_value());
    now_ += 1s;
    ASSERT_FALSE(limiter.check(requestFrom("192.0.2.2")).has_value());
    now_ += 1s;

    // Being rejected counts as being seen, so a client under attack isn't forgotten first.
    ASSERT_TRUE(limiter.check(requestFrom("192.0.2.1")).has_value());
    now_ += 1s;
    ASSERT_FALSE(limiter.check(requestFrom("192.0.2.3")).has_value());

    EXPECT_TRUE(limiter.check(requestFrom("192.0.2.1")).has_value());
    EXPECT_FALSE(limiter.check(requestFrom("192.0.2.2")).has_value());
}

TEST_F(RateLimiterTest, IdleClientsAreForgotten) {
    auto limiter = makeLimiter({1, 60s});
    for (int i = 0; i < 50; ++i) {
        ASSERT_FALSE(limiter.check(requestFrom("10.0.0." + std::to_string(i))).has_value());
    }
    EXPECT_EQ(limiter.trackedClients(), 50U);

    now_ += 60s;
    ASSERT_FALSE(limiter.check(requestFrom("192.0.2.1")).has_value());
    EXPECT_EQ(limiter.trackedClients(), 1U);
}

TEST_F(RateLimiterTest, CanBeTurnedOff) {
    RateLimiterOptions options;
    options.enabled = false;
    auto limiter = makeLimiter({1, 60s}, options);

    const Request request = requestFrom("192.0.2.1");
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(limiter.check(request).has_value());
    }
    EXPECT_EQ(limiter.trackedClients(), 0U);
}

TEST_F(RateLimiterTest, RejectsImpossibleLimits) {
    EXPECT_THROW(makeLimiter({0, 60s}), Exception);
    EXPECT_THROW(makeLimiter({1, 0s}), Exception);
    EXPECT_THROW(makeLimiter({1, -1s}), Exception);

    RateLimiterOptions options;
    options.max_clients = 0;
    EXPECT_THROW(makeLimiter({1, 60s}, options), Exception);
}

TEST(RateLimiterThreadsTest, ManyThreadsNeverGetMoreThanTheLimit) {
    constexpr int kLimit = 1000;
    RateLimiter limiter{{kLimit, 1h}};

    std::atomic<int> allowed{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                const Request request = requestFrom("192.0.2.1");
                for (int i = 0; i < 1000; ++i) {
                    if (!limiter.check(request)) {
                        allowed.fetch_add(1);
                    }
                }
            });
        }
    }
    EXPECT_EQ(allowed.load(), kLimit);
}

TEST(RateLimiterThreadsTest, ManyThreadsWithManyClientsStayConsistent) {
    RateLimiterOptions options;
    options.max_clients = 16;
    RateLimiter limiter{{1, 1h}, options};

    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 5000; ++i) {
                    const Request request =
                        requestFrom("10.0." + std::to_string(t) + "." + std::to_string(i % 200));
                    static_cast<void>(limiter.check(request));
                }
            });
        }
    }
    EXPECT_EQ(limiter.trackedClients(), 16U);
}

TEST(RateLimitMiddlewareTest, StopsRequestsBeforeTheHandler) {
    int calls = 0;
    Router router;
    router.use(rateLimit({1, 60s}));
    router.get("/", [&](const Request&) {
        ++calls;
        return Response::text("ok");
    });

    EXPECT_EQ(router.handle(requestFrom("192.0.2.1")).status(), 200);
    EXPECT_EQ(router.handle(requestFrom("192.0.2.1")).status(), 429);
    EXPECT_EQ(calls, 1);
}

TEST(RateLimiterSettingsTest, ReadsSettingsAndRejectsInvalidOnes) {
    const auto load = [](std::map<std::string, std::string> settings) {
        config::ConfigSources sources;
        sources.directory = std::filesystem::temp_directory_path() / "cppboot-absent-directory";
        sources.defaults = std::move(settings);
        return config::Config::load(sources).value();
    };
    const logging::Logger logger = logging::Logger::create({}).value();

    const auto defaults = RateLimiterOptions::fromSettings(load({}), logger);
    EXPECT_TRUE(defaults.enabled);
    EXPECT_EQ(defaults.max_clients, 100000U);

    const auto custom = RateLimiterOptions::fromSettings(
        load({{"cppboot.rate_limit.enabled", "false"}, {"cppboot.rate_limit.max_clients", "7"}}),
        logger);
    EXPECT_FALSE(custom.enabled);
    EXPECT_EQ(custom.max_clients, 7U);

    EXPECT_THROW(RateLimiterOptions::fromSettings(
                     load({{"cppboot.rate_limit.max_clients", "0"}}), logger),
                 Exception);
    EXPECT_THROW(RateLimiterOptions::fromSettings(
                     load({{"cppboot.rate_limit.enabled", "perhaps"}}), logger),
                 Exception);
}
