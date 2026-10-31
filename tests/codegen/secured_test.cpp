#include <gtest/gtest.h>

#include <cppboot/config/config.hpp>
#include <cppboot/di/container.hpp>
#include <cppboot/logging/logger.hpp>
#include <cppboot/web/binding.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "cppboot_components.generated.hpp"

using namespace cppboot;
using namespace cppboot::web;

namespace {

constexpr const char* kSecret = "a-test-secret-that-is-long-enough-for-hs256";

config::Config makeConfig(std::map<std::string, std::string> settings) {
    config::ConfigSources sources;
    sources.directory = std::filesystem::temp_directory_path() / "cppboot-absent-directory";
    sources.defaults = std::move(settings);
    return config::Config::load(sources).value();
}

template <typename T>
std::shared_ptr<T> borrow(const T& value) {
    return std::shared_ptr<T>{const_cast<T*>(&value), [](T*) {}};
}

}  // namespace

class SecuredTest : public ::testing::Test {
protected:
    void SetUp() override {
        container_.registerInstance(borrow(config_));
        container_.registerInstance(borrow(logger_));
        cppboot::generated::registerComponents(container_);
        const auto valid = container_.validate();
        ASSERT_TRUE(valid) << valid.error().message();

        router_ = container_.resolve<Router>();
        tokens_ = container_.resolve<security::TokenService>();
    }

    Response get(std::string_view target, const std::string& token = {}) const {
        Headers headers;
        if (!token.empty()) {
            headers.emplace_back("Authorization", "Bearer " + token);
        }
        return router_->handle(Request::create(Method::Get, target, std::move(headers)).value());
    }

    Response login(const std::string& body) const {
        return router_->handle(Request::create(Method::Post, "/auth/login", {}, body).value());
    }

    std::string tokenFor(const std::vector<std::string>& roles) const {
        return tokens_->issue("someone", roles);
    }

    config::Config config_ = makeConfig({{"cppboot.security.jwt.secret", kSecret}});
    logging::Logger logger_ = logging::Logger::create({}).value();
    di::Container container_;
    std::shared_ptr<Router> router_;
    std::shared_ptr<security::TokenService> tokens_;
};

TEST_F(SecuredTest, PublicRouteNeedsNoToken) {
    EXPECT_EQ(get("/app/health").status(), 200);
}

TEST_F(SecuredTest, RouteWithNoMarkIsDeniedByDefault) {
    const Response response = get("/app/forgotten");
    EXPECT_EQ(response.status(), 401);
    EXPECT_EQ(response.body().find("secret"), std::string::npos);
    EXPECT_EQ(response.header("WWW-Authenticate"), "Bearer");
}

TEST_F(SecuredTest, ValidTokenOpensAProtectedRoute) {
    EXPECT_EQ(get("/app/forgotten", tokenFor({})).status(), 200);
}

TEST_F(SecuredTest, InvalidTokensAreRejected) {
    const std::string good = tokenFor({"admin"});
    EXPECT_EQ(get("/app/forgotten", "not-a-token").status(), 401);
    EXPECT_EQ(get("/app/forgotten", good + "x").status(), 401);
}

TEST_F(SecuredTest, HandlerReceivesThePrincipal) {
    const Response response = get("/app/me", tokens_->issue("ada", {"admin", "owner"}));
    EXPECT_EQ(response.status(), 200);
    EXPECT_EQ(response.body(), R"({"name":"ada","roles":["admin","owner"]})");
}

TEST_F(SecuredTest, MissingRoleIsForbidden) {
    EXPECT_EQ(get("/app/admin", tokenFor({"editor"})).status(), 403);
    EXPECT_EQ(get("/app/admin", tokenFor({})).status(), 403);
}

TEST_F(SecuredTest, RequiredRoleOpensTheRoute) {
    EXPECT_EQ(get("/app/admin", tokenFor({"admin"})).status(), 200);
}

TEST_F(SecuredTest, RoleCheckComesAfterAuthentication) {
    EXPECT_EQ(get("/app/admin").status(), 401);
}

TEST_F(SecuredTest, AnyOfSeveralRolesIsEnough) {
    EXPECT_EQ(get("/app/audit", tokenFor({"owner"})).status(), 200);
    EXPECT_EQ(get("/app/audit", tokenFor({"auditor"})).status(), 200);
    EXPECT_EQ(get("/app/audit", tokenFor({"admin"})).status(), 403);
}

TEST_F(SecuredTest, ControllerRoleAppliesToItsRoutes) {
    EXPECT_EQ(get("/staff", tokenFor({"other"})).status(), 403);
    EXPECT_EQ(get("/staff", tokenFor({"staff"})).status(), 200);
}

TEST_F(SecuredTest, FunctionRoleReplacesControllerRole) {
    EXPECT_EQ(get("/staff/promote", tokenFor({"staff"})).status(), 403);
    EXPECT_EQ(get("/staff/promote", tokenFor({"admin"})).status(), 200);
    EXPECT_EQ(get("/staff/about").status(), 200);
}

TEST_F(SecuredTest, RoleInsideAPublicControllerStillProtects) {
    EXPECT_EQ(get("/open").status(), 200);
    EXPECT_EQ(get("/open/locked").status(), 401);
    EXPECT_EQ(get("/open/locked", tokenFor({"admin"})).status(), 200);
}

TEST_F(SecuredTest, LoginGivesATokenThatWorks) {
    const Response response = login(R"({"name":"ada","password":"correct horse"})");
    ASSERT_EQ(response.status(), 200) << response.body();

    const std::string token = Json::parse(response.body())["token"];
    EXPECT_EQ(get("/app/admin", token).status(), 200);
    EXPECT_EQ(Json::parse(get("/app/me", token).body())["name"], "ada");
}

TEST_F(SecuredTest, WrongPasswordFails) {
    EXPECT_EQ(login(R"({"name":"ada","password":"wrong"})").status(), 401);
}

TEST_F(SecuredTest, MiddlewareRunsInOrderAndSeesEveryRequest) {
    const Response response = get("/app/health");
    EXPECT_EQ(response.header("X-Middleware"), "ran");

    const auto log = container_.resolve<secured::TraceLog>();
    EXPECT_EQ(log->read(), "outer inner ");

    // It also runs for requests that security rejects.
    EXPECT_EQ(get("/app/forgotten").header("X-Middleware"), "ran");
}

TEST(SecuredStartupTest, MissingSecretStopsTheRouterFromBeingCreated) {
    const config::Config config = makeConfig({});
    const logging::Logger logger = logging::Logger::create({}).value();

    di::Container container;
    container.registerInstance(borrow(config));
    container.registerInstance(borrow(logger));
    cppboot::generated::registerComponents(container);

    EXPECT_THROW(static_cast<void>(container.resolve<Router>()), Exception);
}

TEST(SecuredStartupTest, ShortSecretStopsTheRouterFromBeingCreated) {
    const config::Config config = makeConfig({{"cppboot.security.jwt.secret", "too-short"}});
    const logging::Logger logger = logging::Logger::create({}).value();

    di::Container container;
    container.registerInstance(borrow(config));
    container.registerInstance(borrow(logger));
    cppboot::generated::registerComponents(container);

    EXPECT_THROW(static_cast<void>(container.resolve<Router>()), Exception);
}
