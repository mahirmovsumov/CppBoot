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
    EXPECT_NE(text.find("struct cppboot::JsonConvert<ns::User>"), std::string::npos);
    EXPECT_NE(text.find("json[\"id\"] = JsonType{value.id};"), std::string::npos);
    EXPECT_NE(text.find("cppboot::detail::readJsonField(json, \"id\", value.id);"), std::string::npos);
    EXPECT_NE(text.find("cppboot::detail::readOptionalJsonField(json, \"nickname\", value.nickname);"),
              std::string::npos);
    EXPECT_EQ(text.find("Router"), std::string::npos);
}

TEST(ValidateDeclarationsTest, RejectsMiddlewareWithNoControllers) {
    Declarations declarations;
    declarations.middleware = {{"Log", "a.hpp:1", 1}};
    const auto result = validateDeclarations(declarations);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("no controllers"), std::string::npos);
}

TEST(ValidateDeclarationsTest, UsingTheActuatorNeedsTheOption) {
    Declarations metrics_user;
    metrics_user.components = {{"Orders", "a.hpp:1", {}, {"cppboot::actuator::Metrics"}}};
    const auto result = validateDeclarations(metrics_user);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("ACTUATOR"), std::string::npos);
    EXPECT_TRUE(validateDeclarations(metrics_user, RenderOptions{true}));

    Declarations indicator;
    indicator.components = {{"Disk", "a.hpp:1", {"cppboot::actuator::HealthIndicator"}, {}}};
    EXPECT_FALSE(validateDeclarations(indicator));
    EXPECT_TRUE(validateDeclarations(indicator, RenderOptions{true}));
}

TEST(ValidateDeclarationsTest, AllowsManyHealthIndicators) {
    Declarations declarations;
    declarations.components = {
        {"Disk", "a.hpp:1", {"cppboot::actuator::HealthIndicator"}, {}},
        {"Queue", "a.hpp:2", {"cppboot::actuator::HealthIndicator"}, {}},
    };
    EXPECT_TRUE(validateDeclarations(declarations, RenderOptions{true}));
}

TEST(RenderRegistrationTest, LeavesOutTheActuatorUnlessAskedFor) {
    Declarations declarations;
    declarations.components = {{"C", "a.hpp:1", {}, {}}};
    declarations.controllers = {{"C", {makeRoute("Get", "/")}}};
    EXPECT_EQ(renderRegistration(declarations, {}).find("actuator"), std::string::npos);
}

TEST(RenderRegistrationTest, ActuatorStartsAServerEvenWithNoControllers) {
    const std::string text = renderRegistration(Declarations{}, {}, RenderOptions{true});

    EXPECT_NE(text.find("cppboot/actuator/actuator.hpp"), std::string::npos);
    EXPECT_NE(text.find("cppboot::actuator::mount(*router, actuator, false)"), std::string::npos);
    EXPECT_NE(text.find("cppboot::web::ServerModule"), std::string::npos);
}

TEST(RenderRegistrationTest, ActuatorLocksMetricsInASecuredProject) {
    Route route = makeRoute("Get", "/");
    route.is_public = true;

    Declarations declarations;
    declarations.components = {{"C", "a.hpp:1", {}, {}}};
    declarations.controllers = {{"C", {route}}};

    EXPECT_NE(renderRegistration(declarations, {}, RenderOptions{true})
                  .find("cppboot::actuator::mount(*router, actuator, true)"),
              std::string::npos);
}

TEST(RenderRegistrationTest, ActuatorChecksTheDatabaseWhenThereIsOne) {
    Declarations declarations;
    declarations.row_types = {{"User", "a.hpp:1", {{"id", false}}}};
    const std::string text = renderRegistration(declarations, {}, RenderOptions{true});

    EXPECT_NE(text.find("actuator->addCheck(\"database\""), std::string::npos);
    EXPECT_NE(text.find("database_connections_open"), std::string::npos);
}

TEST(RenderRegistrationTest, HealthIndicatorsJoinTheirGroup) {
    Declarations declarations;
    declarations.components = {{"Disk", "a.hpp:1", {"cppboot::actuator::HealthIndicator"}, {}}};
    const std::string text = renderRegistration(declarations, {}, RenderOptions{true});

    EXPECT_NE(text.find("bindAll<cppboot::actuator::HealthIndicator, Disk>"), std::string::npos);
    EXPECT_NE(text.find("resolveAll<cppboot::actuator::HealthIndicator>"), std::string::npos);
}

TEST(RenderRegistrationTest, LeavesOutSecurityWhenNotUsed) {
    Declarations declarations;
    declarations.components = {{"C", "a.hpp:1", {}, {}}};
    declarations.controllers = {{"C", {makeRoute("Get", "/")}}};
    const std::string text = renderRegistration(declarations, {});

    EXPECT_EQ(text.find("cppboot/security"), std::string::npos);
    EXPECT_EQ(text.find("authenticate"), std::string::npos);
}

TEST(RenderRegistrationTest, OneSecurityMarkProtectsEveryOtherRoute) {
    Route open = makeRoute("Post", "/login");
    open.function = "login";
    open.is_public = true;

    Route forgotten = makeRoute("Get", "/forgotten");
    forgotten.function = "forgotten";

    Declarations declarations;
    declarations.components = {{"C", "a.hpp:1", {}, {}}};
    declarations.controllers = {{"C", {open, forgotten}}};
    const std::string text = renderRegistration(declarations, {});

    const auto login_at = text.find("\"/login\"");
    const auto forgotten_at = text.find("\"/forgotten\"");
    const auto guard_at = text.find("authenticate(*tokens, request)");
    ASSERT_NE(guard_at, std::string::npos);
    EXPECT_LT(login_at, forgotten_at);
    EXPECT_GT(guard_at, forgotten_at);
    EXPECT_EQ(text.find("authenticate(*tokens, request)", guard_at + 1), std::string::npos);
}

TEST(RenderRegistrationTest, DependingOnTheTokenServiceTurnsOnSecurity) {
    Declarations declarations;
    declarations.components = {{"Login", "a.hpp:1", {}, {"cppboot::security::TokenService"}}};
    declarations.controllers = {{"Login", {makeRoute("Get", "/anything")}}};
    EXPECT_TRUE(validateDeclarations(declarations));

    const std::string text = renderRegistration(declarations, {});
    EXPECT_NE(text.find("TokenService::fromSettings"), std::string::npos);
    EXPECT_NE(text.find("authenticate(*tokens, request)"), std::string::npos);
}

TEST(RenderRegistrationTest, WritesRoleChecksAndThePrincipal) {
    Route route = makeRoute("Get", "/admin");
    route.roles = {"admin", "owner"};
    route.parameters = {{ParameterSource::Principal, "who", "cppboot::security::Principal"}};

    Declarations declarations;
    declarations.components = {{"C", "a.hpp:1", {}, {}}};
    declarations.controllers = {{"C", {route}}};
    const std::string text = renderRegistration(declarations, {});

    EXPECT_NE(text.find("principal->hasAnyRole({\"admin\", \"owner\"})"), std::string::npos);
    EXPECT_NE(text.find("forbiddenResponse()"), std::string::npos);
    EXPECT_NE(text.find("controller->handle(*principal)"), std::string::npos);
}

TEST(RenderRegistrationTest, WritesMiddlewareInOrder) {
    Declarations declarations;
    declarations.components = {{"C", "a.hpp:1", {}, {}}};
    declarations.controllers = {{"C", {makeRoute("Get", "/")}}};
    declarations.middleware = {{"Second", "a.hpp:2", 20}, {"First", "a.hpp:3", 10}};
    const std::string text = renderRegistration(declarations, {});

    const auto first = text.find("resolver.resolve<First>()");
    const auto second = text.find("resolver.resolve<Second>()");
    ASSERT_NE(first, std::string::npos);
    ASSERT_NE(second, std::string::npos);
    EXPECT_LT(first, second);
    EXPECT_NE(text.find("router->use("), std::string::npos);
}

TEST(RenderRegistrationTest, LeavesOutTheDatabaseWhenNotNeeded) {
    Declarations declarations;
    declarations.components = {{"Service", "a.hpp:1", {}, {}}};
    EXPECT_EQ(renderRegistration(declarations, {}).find("cppboot/data"), std::string::npos);
}

TEST(RenderRegistrationTest, RegistersTheDatabaseBeforeOtherModules) {
    Declarations declarations;
    declarations.components = {{"Worker", "a.hpp:1", {"cppboot::Module"}, {}}};
    declarations.row_types = {{"ns::User", "a.hpp:2", {{"id", false}, {"name", false}}}};
    const std::string text = renderRegistration(declarations, {});

    EXPECT_NE(text.find("cppboot/data/database.hpp"), std::string::npos);
    EXPECT_NE(text.find("row_mappers->add<ns::User>"), std::string::npos);
    EXPECT_NE(text.find("row.read(\"id\", value.id);"), std::string::npos);

    const auto database_module = text.find("bindAll<cppboot::Module, cppboot::data::DatabaseModule>");
    const auto worker = text.find("bindAll<cppboot::Module, Worker>");
    ASSERT_NE(database_module, std::string::npos);
    ASSERT_NE(worker, std::string::npos);
    EXPECT_LT(database_module, worker);
}

TEST(RenderRegistrationTest, DependingOnTheDatabaseRegistersIt) {
    Declarations declarations;
    declarations.components = {{"Repo", "a.hpp:1", {}, {"cppboot::data::Database"}}};
    EXPECT_TRUE(validateDeclarations(declarations));
    EXPECT_NE(renderRegistration(declarations, {}).find("Database::fromSettings"),
              std::string::npos);
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
