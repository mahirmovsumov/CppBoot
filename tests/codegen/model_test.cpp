#include <gtest/gtest.h>

#include <cppboot/codegen/model.hpp>
#include <string>
#include <vector>

using namespace cppboot::codegen;
using cppboot::ErrorCode;

namespace {

auto validateComponents(std::vector<Component> components) {
    Declarations declarations;
    declarations.components = std::move(components);
    return validateDeclarations(declarations);
}

Route makeRoute(std::string method, std::string path) {
    Route route;
    route.method = std::move(method);
    route.path = std::move(path);
    route.function = "handle";
    route.location = "a.hpp:1";
    return route;
}

}  // namespace

TEST(ValidateDeclarationsTest, AcceptsResolvedDependencies) {
    EXPECT_TRUE(validateComponents({
        {"Service", "a.hpp:1", {}, {"Repository"}},
        {"SqlRepository", "a.hpp:2", {"Repository"}, {}},
    }));
}

TEST(ValidateDeclarationsTest, RejectsMissingDependency) {
    const auto result = validateComponents({{"Service", "a.hpp:1", {}, {"Absent"}}});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::DependencyError);
    EXPECT_NE(result.error().message().find("Service (a.hpp:1) needs Absent"), std::string::npos);
}

TEST(ValidateDeclarationsTest, RejectsTwoProvidersOfOneInterface) {
    const auto result = validateComponents({
        {"SqlRepository", "a.hpp:1", {"Repository"}, {}},
        {"MemoryRepository", "a.hpp:2", {"Repository"}, {}},
    });
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("provided by both"), std::string::npos);
}

TEST(ValidateDeclarationsTest, RejectsTheSameTypeTwice) {
    EXPECT_FALSE(validateComponents({{"A", "a.hpp:1", {}, {}}, {"A", "b.hpp:1", {}, {}}}));
}

TEST(ValidateDeclarationsTest, ReportsTheCyclePath) {
    const auto result = validateComponents({
        {"Start", "a.hpp:1", {}, {"A"}},
        {"A", "a.hpp:2", {}, {"B"}},
        {"B", "a.hpp:3", {}, {"A"}},
    });
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("A -> B -> A"), std::string::npos);
    EXPECT_EQ(result.error().message().find("Start"), std::string::npos);
}

TEST(ValidateDeclarationsTest, RejectsADependencyOnItself) {
    EXPECT_FALSE(validateComponents({{"A", "a.hpp:1", {}, {"A"}}}));
}

TEST(ValidateDeclarationsTest, AllowsSharedDependencies) {
    EXPECT_TRUE(validateComponents({
        {"Top", "a.hpp:1", {}, {"Left", "Right"}},
        {"Left", "a.hpp:2", {}, {"Base"}},
        {"Right", "a.hpp:3", {}, {"Base"}},
        {"Base", "a.hpp:4", {}, {}},
    }));
}

TEST(ValidateDeclarationsTest, FrameworkTypesNeedNoProvider) {
    EXPECT_TRUE(validateComponents({
        {"Service", "a.hpp:1", {}, {"cppboot::config::Config", "cppboot::logging::Logger"}},
    }));
}

TEST(ValidateDeclarationsTest, AllowsManyModules) {
    EXPECT_TRUE(validateComponents({
        {"First", "a.hpp:1", {"cppboot::Module"}, {}},
        {"Second", "a.hpp:2", {"cppboot::Module"}, {}},
    }));
}

TEST(ValidateDeclarationsTest, ModulesCannotBeInjectedByTheirInterface) {
    EXPECT_FALSE(validateComponents({
        {"First", "a.hpp:1", {"cppboot::Module"}, {}},
        {"Needs", "a.hpp:2", {}, {"cppboot::Module"}},
    }));
}

TEST(ValidateDeclarationsTest, RejectsRoutesThatMatchTheSameRequests) {
    Declarations declarations;
    declarations.controllers.push_back(
        Controller{"A", {makeRoute("Get", "/users/{id}"), makeRoute("Post", "/users/{id}")}});
    EXPECT_TRUE(validateDeclarations(declarations));

    declarations.controllers.push_back(Controller{"B", {makeRoute("Get", "/users/{name}")}});
    const auto result = validateDeclarations(declarations);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("both handle Get /users/{name}"), std::string::npos);
}

TEST(RenderRegistrationTest, AddsModulesToTheirGroup) {
    Declarations declarations;
    declarations.components = {{"ns::Worker", "a.hpp:1", {"cppboot::Module"}, {}}};
    const std::string text = renderRegistration(declarations, {});

    EXPECT_NE(text.find("container.bindAll<cppboot::Module, ns::Worker>();"), std::string::npos);
    EXPECT_EQ(text.find("container.bind<cppboot::Module"), std::string::npos);
}

TEST(RenderRegistrationTest, WritesIncludesInterfacesAndArguments) {
    Declarations declarations;
    declarations.components = {
        {"ns::Service", "a.hpp:1", {"ns::IService"}, {"ns::A", "ns::B"}},
        {"ns::Leaf", "a.hpp:2", {}, {}},
    };
    const std::string text = renderRegistration(declarations, {"D:/app/a.hpp"});

    EXPECT_NE(text.find("#include \"D:/app/a.hpp\""), std::string::npos);
    EXPECT_NE(text.find("container.registerSingleton<ns::Service>("
                        "cppboot::di::Deps<ns::A, ns::B>{});"),
              std::string::npos);
    EXPECT_NE(text.find("container.bind<ns::IService, ns::Service>();"), std::string::npos);
    EXPECT_NE(text.find("container.registerSingleton<ns::Leaf>(cppboot::di::Deps<>{});"),
              std::string::npos);
}

TEST(RenderRegistrationTest, LeavesOutWebHeadersWhenNotNeeded) {
    Declarations declarations;
    declarations.components = {{"Service", "a.hpp:1", {}, {}}};
    const std::string text = renderRegistration(declarations, {});

    EXPECT_EQ(text.find("cppboot/web"), std::string::npos);
    EXPECT_EQ(text.find("Router"), std::string::npos);
}

TEST(RenderRegistrationTest, WritesJsonConversions) {
    Declarations declarations;
    declarations.json_types = {
        {"ns::User", "a.hpp:1", {{"id", false}, {"nickname", true}}},
    };
    const std::string text = renderRegistration(declarations, {});

    EXPECT_NE(text.find("cppboot/web/binding.hpp"), std::string::npos);
    EXPECT_NE(text.find("struct nlohmann::adl_serializer<ns::User>"), std::string::npos);
    EXPECT_NE(text.find("json[\"id\"] = value.id;"), std::string::npos);
    EXPECT_NE(text.find("json.at(\"id\").get_to(value.id);"), std::string::npos);
    EXPECT_NE(text.find("if (json.contains(\"nickname\"))"), std::string::npos);
    EXPECT_EQ(text.find("Router"), std::string::npos);
}

TEST(RenderRegistrationTest, WritesRoutesForEachReturnKindAndSource) {
    Route value = makeRoute("Get", "/users/{id}");
    value.function = "get";
    value.parameters = {{ParameterSource::Path, "id", "int"},
                        {ParameterSource::Query, "page", "std::optional<int>"},
                        {ParameterSource::Request, "request", "cppboot::web::Request"}};

    Route response = makeRoute("Post", "/users");
    response.function = "create";
    response.return_kind = ReturnKind::Response;
    response.parameters = {{ParameterSource::Body, "body", "ns::NewUser"}};

    Route nothing = makeRoute("Delete", "/users/{id}");
    nothing.function = "remove";
    nothing.return_kind = ReturnKind::Nothing;

    Declarations declarations;
    declarations.components = {{"ns::Users", "a.hpp:1", {}, {}}};
    declarations.controllers = {{"ns::Users", {value, response, nothing}}};
    const std::string text = renderRegistration(declarations, {});

    EXPECT_NE(text.find("resolver.resolve<ns::Users>()"), std::string::npos);
    EXPECT_NE(text.find("cppboot::web::Method::Get, \"/users/{id}\""), std::string::npos);
    EXPECT_NE(text.find("toJsonResponse(controller->get(cppboot::web::pathParam<int>(request, "
                        "\"id\"), cppboot::web::queryParam<std::optional<int>>(request, "
                        "\"page\"), request))"),
              std::string::npos);
    EXPECT_NE(text.find("return controller->create(cppboot::web::bodyAs<ns::NewUser>(request));"),
              std::string::npos);
    EXPECT_NE(text.find("controller->remove();"), std::string::npos);
    EXPECT_NE(text.find("cppboot::web::Response{204}"), std::string::npos);
    EXPECT_NE(text.find("container.bindAll<cppboot::Module, cppboot::web::ServerModule>();"),
              std::string::npos);
}
