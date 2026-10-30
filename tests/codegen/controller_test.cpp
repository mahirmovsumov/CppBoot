#include <gtest/gtest.h>

#include <cppboot/di/container.hpp>
#include <cppboot/web/binding.hpp>
#include <cppboot/web/router.hpp>
#include <string>

#include "cppboot_components.generated.hpp"

using namespace cppboot::web;
using cppboot::di::Container;

class ControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        cppboot::generated::registerComponents(container_);
        router_ = container_.resolve<Router>();
    }

    Response send(Method method, std::string_view target, std::string body = {}) const {
        return router_->handle(Request::create(method, target, {}, std::move(body)).value());
    }

    Container container_;
    std::shared_ptr<Router> router_;
};

TEST_F(ControllerTest, ProducesAValidGraphWithAServerModule) {
    const auto result = container_.validate();
    EXPECT_TRUE(result) << (result ? "" : result.error().message());
    EXPECT_EQ(container_.resolveAll<cppboot::Module>().size(), 1U);
}

TEST_F(ControllerTest, ReturnsAValueAsJsonWithFieldsInOrder) {
    const Response response = send(Method::Get, "/api/users/7");
    EXPECT_EQ(response.status(), 200);
    EXPECT_EQ(response.header("Content-Type"), "application/json");
    EXPECT_EQ(response.body(),
              R"({"id":7,"name":"user-7","address":{"city":"Baku"},"nickname":null,"scores":[1,2]})");
}

TEST_F(ControllerTest, ConvertsAPathParameter) {
    EXPECT_EQ(send(Method::Get, "/api/users/abc").status(), 400);
    EXPECT_NE(send(Method::Get, "/api/users/abc").body().find("Path parameter 'id'"),
              std::string::npos);
    EXPECT_EQ(send(Method::Get, "/api/users/99999999999999").status(), 400);
}

TEST_F(ControllerTest, ReadsQueryParameters) {
    const Response response = send(Method::Get, "/api/users?limit=2&prefix=x-");
    EXPECT_EQ(response.status(), 200);

    const Json json = Json::parse(response.body());
    ASSERT_EQ(json.size(), 2U);
    EXPECT_EQ(json[1]["name"], "x-user-2");
}

TEST_F(ControllerTest, OptionalQueryParameterMayBeMissing) {
    EXPECT_EQ(send(Method::Get, "/api/users?limit=1").status(), 200);
}

TEST_F(ControllerTest, RequiredQueryParameterMustBePresent) {
    const Response response = send(Method::Get, "/api/users");
    EXPECT_EQ(response.status(), 400);
    EXPECT_NE(response.body().find("Query parameter 'limit' is required"), std::string::npos);
}

TEST_F(ControllerTest, ReadsAJsonBodyAndReturnsAResponse) {
    const Response response =
        send(Method::Post, "/api/users", R"({"name":"Ada","nickname":"A"})");
    EXPECT_EQ(response.status(), 201);
    EXPECT_EQ(response.body(), "Ada/A");
    EXPECT_EQ(response.header("Location"), "/api/users/1");
}

TEST_F(ControllerTest, OptionalBodyFieldMayBeMissingOrNull) {
    EXPECT_EQ(send(Method::Post, "/api/users", R"({"name":"Ada"})").body(), "Ada/-");
    EXPECT_EQ(send(Method::Post, "/api/users", R"({"name":"Ada","nickname":null})").body(),
              "Ada/-");
}

TEST_F(ControllerTest, RejectsABadBody) {
    EXPECT_EQ(send(Method::Post, "/api/users", "not json").status(), 400);
    EXPECT_EQ(send(Method::Post, "/api/users", "").status(), 400);
    EXPECT_EQ(send(Method::Post, "/api/users", R"({"nickname":"A"})").status(), 400);
    EXPECT_EQ(send(Method::Post, "/api/users", R"({"name":5})").status(), 400);
    EXPECT_EQ(send(Method::Post, "/api/users", "[]").status(), 400);
}

TEST_F(ControllerTest, WithStatusSendsJsonWithThatStatus) {
    const Response response = send(Method::Post, "/api/users/copies", R"({"name":"Ada"})");
    EXPECT_EQ(response.status(), 201);
    EXPECT_EQ(response.header("Content-Type"), "application/json");
    EXPECT_EQ(response.body(), R"({"name":"Ada","nickname":null})");
}

TEST_F(ControllerTest, VoidFunctionReturnsNoContent) {
    const Response response = send(Method::Delete, "/api/users/1");
    EXPECT_EQ(response.status(), 204);
    EXPECT_TRUE(response.body().empty());
}

TEST_F(ControllerTest, HttpErrorFromAControllerBecomesItsResponse) {
    EXPECT_EQ(send(Method::Delete, "/api/users/0").status(), 409);
}

TEST_F(ControllerTest, FunctionCanTakeTheRequest) {
    EXPECT_EQ(send(Method::Get, "/api/users/echo/hi").body(), "\"hi:/api/users/echo/hi\"");
}

TEST_F(ControllerTest, JsonConversionWorksBothWays) {
    const api::User user{1, "Ada", api::Address{"Baku"}, "A", {3}};
    const Json json = user;
    const auto copy = json.get<api::User>();

    EXPECT_EQ(copy.id, 1);
    EXPECT_EQ(copy.name, "Ada");
    EXPECT_EQ(copy.address.city, "Baku");
    EXPECT_EQ(copy.nickname, "A");
    EXPECT_EQ(copy.scores, std::vector<int>{3});
}
