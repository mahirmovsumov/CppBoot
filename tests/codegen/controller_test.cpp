#include <gtest/gtest.h>

#include <cppboot/config/config.hpp>
#include <cppboot/di/container.hpp>
#include <cppboot/web/binding.hpp>
#include <cppboot/web/router.hpp>
#include <filesystem>
#include <map>
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

TEST_F(ControllerTest, BadBodyMessageNamesTheProblemWithoutLibraryDetails) {
    const Response response = send(Method::Post, "/api/users", R"({"nickname":"A"})");
    EXPECT_NE(response.body().find("Invalid request body: missing field 'name'"), std::string::npos)
        << response.body();

    // A field of the wrong type is named, so the client knows which one to fix.
    const Response wrong_type = send(Method::Post, "/api/users", R"({"name":5})");
    EXPECT_NE(wrong_type.body().find("field 'name': expected a string, but found a number"),
              std::string::npos)
        << wrong_type.body();
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

namespace {

/// A router whose container has these settings, as an application's would.
std::shared_ptr<Router> routerWithSettings(Container& container,
                                           std::map<std::string, std::string> settings) {
    cppboot::config::ConfigSources sources;
    sources.defaults = std::move(settings);
    sources.directory = std::filesystem::temp_directory_path() / "cppboot-openapi-test-none";
    container.registerInstance(
        std::make_shared<cppboot::config::Config>(cppboot::config::Config::load(sources).value()));
    cppboot::generated::registerComponents(container);
    return container.resolve<Router>();
}

}  // namespace

TEST(OpenApiRouteTest, IsOffUnlessTurnedOn) {
    Container container;
    const auto router = routerWithSettings(container, {});
    EXPECT_EQ(router->handle(Request::create(Method::Get, "/openapi.json").value()).status(), 404);
}

TEST(OpenApiRouteTest, ServesTheDocumentWhenTurnedOn) {
    Container container;
    const auto router = routerWithSettings(container, {{"cppboot.openapi.enabled", "true"}});
    const Response response = router->handle(Request::create(Method::Get, "/openapi.json").value());
    ASSERT_EQ(response.status(), 200);
    EXPECT_EQ(response.header("Content-Type"), "application/json");

    const Json document = Json::parse(response.body());
    EXPECT_EQ(document.at("openapi"), "3.0.3");
    EXPECT_EQ(document.at("info").at("title"), "cppboot-generated-controller-tests");
    EXPECT_EQ(Json::parse(std::string{cppboot::generated::kOpenApi}), document);

    const Json& get = document.at("paths").at("/api/users/{id}").at("get");
    EXPECT_EQ(get.at("parameters").at(0).at("in"), "path");
    EXPECT_EQ(get.at("responses").at("200").at("content").at("application/json").at("schema").at("$ref"),
              "#/components/schemas/api.User");
    // A WithStatus return has a status that's only known when it runs.
    EXPECT_TRUE(document.at("paths").at("/api/users/copies").at("post").at("responses").contains("2XX"));
    EXPECT_TRUE(document.at("paths").at("/api/users/{id}").at("delete").at("responses").contains("204"));

    const Json& user = document.at("components").at("schemas").at("api.User");
    const std::vector<std::string> required{"id", "name", "address", "scores"};
    EXPECT_EQ(user.at("required"), Json{required});
    EXPECT_EQ(user.at("properties").at("nickname").at("nullable"), true);
    EXPECT_EQ(user.at("properties").at("scores").at("items").at("type"), "integer");
}

TEST(OpenApiRouteTest, AWrongSettingStopsStartup) {
    Container container;
    EXPECT_THROW(routerWithSettings(container, {{"cppboot.openapi.enabled", "maybe"}}),
                 cppboot::Exception);
}
