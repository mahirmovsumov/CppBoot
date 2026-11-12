#include <gtest/gtest.h>

#include <cppboot/base/json.hpp>
#include <cppboot/codegen/model.hpp>
#include <string>
#include <vector>

using namespace cppboot::codegen;
using cppboot::ErrorCode;
using cppboot::Json;

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

TEST(CheckTest, MessagesSayWhatAValueHasToBe) {
    EXPECT_EQ(checkMessage({CheckKind::NotEmpty, ValueShape::Text, "", ""}), "must not be empty");
    EXPECT_EQ(checkMessage({CheckKind::Length, ValueShape::Text, "1", "50"}),
              "must have from 1 to 50 characters");
    EXPECT_EQ(checkMessage({CheckKind::Length, ValueShape::Items, "0", "3"}),
              "must have from 0 to 3 items");
    EXPECT_EQ(checkMessage({CheckKind::Length, ValueShape::Text, "3", "3"}),
              "must have exactly 3 characters");
    EXPECT_EQ(checkMessage({CheckKind::Length, ValueShape::Items, "1", "1"}),
              "must have exactly 1 item");
    EXPECT_EQ(checkMessage({CheckKind::Length, ValueShape::Text, "0", "1"}),
              "must have from 0 to 1 character");
    EXPECT_EQ(checkMessage({CheckKind::Min, ValueShape::Number, "0", ""}), "must be at least 0");
    EXPECT_EQ(checkMessage({CheckKind::Max, ValueShape::Number, "", "-0.5"}), "must be at most -0.5");
    EXPECT_EQ(checkMessage({CheckKind::Range, ValueShape::Number, "1", "100"}),
              "must be from 1 to 100");
}

TEST(CheckTest, ExpressionsCallTheValidationFunctions) {
    EXPECT_EQ(checkExpression({CheckKind::NotEmpty, ValueShape::Text, "", ""}, "value.name"),
              "cppboot::validation::notEmpty(value.name)");
    EXPECT_EQ(checkExpression({CheckKind::Length, ValueShape::Items, "1", "3"}, "v"),
              "cppboot::validation::lengthInRange(v, 1, 3)");
    EXPECT_EQ(checkExpression({CheckKind::Min, ValueShape::Number, "-1", ""}, "v"),
              "cppboot::validation::atLeast(v, -1)");
    EXPECT_EQ(checkExpression({CheckKind::Max, ValueShape::Number, "", "9"}, "v"),
              "cppboot::validation::atMost(v, 9)");
    EXPECT_EQ(checkExpression({CheckKind::Range, ValueShape::Number, "0", "0.5"}, "v"),
              "cppboot::validation::inRange(v, 0, 0.5)");
}

TEST(RenderRegistrationTest, WritesChecksIntoJsonConversions) {
    Declarations declarations;
    declarations.json_types = {{"Order",
                                "a.hpp:1",
                                {{"quantity", false, {{CheckKind::Range, ValueShape::Number, "1", "9"}}},
                                 {"note", true, {}}}}};
    const std::string text = renderRegistration(declarations, {});

    EXPECT_NE(text.find("#include <cppboot/base/validation.hpp>"), std::string::npos);
    EXPECT_NE(text.find("if (!cppboot::validation::inRange(value.quantity, 1, 9)) {\n"
                        "            throw cppboot::JsonError(\"field 'quantity': must be from 1 "
                        "to 9\");"),
              std::string::npos)
        << text;
}

TEST(RenderRegistrationTest, LeavesValidationOutWhenNothingIsChecked) {
    Declarations declarations;
    declarations.json_types = {{"Order", "a.hpp:1", {{"quantity", false, {}}}}};
    const std::string text = renderRegistration(declarations, {});
    EXPECT_EQ(text.find("validation"), std::string::npos);
    EXPECT_EQ(text.find("config/binding.hpp"), std::string::npos);
}

TEST(RenderRegistrationTest, FillsSettingsStructsBeforeAnythingElse) {
    Declarations declarations;
    declarations.config_types = {
        {"app::Shop",
         "a.hpp:1",
         "shop",
         {{"name", true, {{CheckKind::NotEmpty, ValueShape::Text, "", ""}}},
          {"retry.attempts", false, {{CheckKind::Range, ValueShape::Number, "1", "10"}}}}},
        {"app::Mail", "a.hpp:9", "shop.mail", {{"host", false, {}}}},
    };
    declarations.components = {{"app::Greeter", "a.hpp:20", {}, {"app::Shop"}}};
    ASSERT_TRUE(validateDeclarations(declarations));
    const std::string text = renderRegistration(declarations, {});

    EXPECT_NE(text.find("#include <cppboot/config/binding.hpp>"), std::string::npos);
    EXPECT_NE(text.find("cppboot::config::SettingsBinder binder{*config, \"shop\", \"app::Shop\"};"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("binder.bind(\"name\", settings->name);\n        binder.require(\"name\");\n"
                        "        binder.check(\"name\", cppboot::validation::notEmpty(settings->name), "
                        "\"must not be empty\");"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("binder.bind(\"retry.attempts\", settings->retry.attempts);"),
              std::string::npos);
    // The struct with the longer prefix reads those settings, so the first one leaves them alone.
    EXPECT_NE(text.find("binder.ignoreGroup(\"mail\");"), std::string::npos);
    EXPECT_EQ(text.find("binder.ignoreGroup(\"shop"), std::string::npos);
    EXPECT_NE(text.find("container.registerInstance(std::move(settings));"), std::string::npos);

    EXPECT_LT(text.find("SettingsBinder binder"), text.find("registerSingleton<app::Greeter>"));
}

TEST(RenderRegistrationTest, ChecksParametersBeforeCallingTheRoute) {
    Declarations declarations;
    declarations.components = {{"Api", "a.hpp:1", {}, {}}};
    Route route;
    route.method = "Get";
    route.path = "/items/{code}";
    route.function = "list";
    route.parameters = {
        {ParameterSource::Path, "code", "std::string", {{CheckKind::Length, ValueShape::Text, "3", "3"}}},
        {ParameterSource::Query, "size", "int", {{CheckKind::Max, ValueShape::Number, "", "50"}}},
        {ParameterSource::Query, "plain", "int", {}},
    };
    declarations.controllers = {{"Api", {route}}};
    const std::string text = renderRegistration(declarations, {});

    EXPECT_NE(text.find("const auto parameter_0 = cppboot::web::pathParam<std::string>(request, "
                        "\"code\");"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("throw cppboot::web::HttpError(400, \"Path parameter 'code' must have "
                        "exactly 3 characters\");"),
              std::string::npos);
    EXPECT_NE(text.find("throw cppboot::web::HttpError(400, \"Query parameter 'size' must be at "
                        "most 50\");"),
              std::string::npos);
    EXPECT_NE(text.find("controller->list(parameter_0, parameter_1, "
                        "cppboot::web::queryParam<int>(request, \"plain\"))"),
              std::string::npos)
        << text;
}

TEST(ValidateDeclarationsTest, RejectsTwoStructsReadingTheSameSettings) {
    Declarations declarations;
    declarations.config_types = {{"A", "a.hpp:1", "mail", {{"host", false, {}}}},
                                 {"B", "b.hpp:2", "mail", {{"port", false, {}}}}};
    const auto result = validateDeclarations(declarations);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find(
                  "A (a.hpp:1) and B (b.hpp:2) both read the settings under \"mail\""),
              std::string::npos)
        << result.error().message();
}

TEST(ValidateDeclarationsTest, SettingsStructsCountAsProvidedButOthersStillDoNot) {
    Declarations declarations;
    declarations.config_types = {{"Mail", "a.hpp:1", "mail", {{"host", false, {}}}}};
    declarations.components = {{"Mailer", "a.hpp:5", {}, {"Mail"}}};
    EXPECT_TRUE(validateDeclarations(declarations));

    declarations.components = {{"Mailer", "a.hpp:5", {}, {"Mail", "Missing"}}};
    const auto result = validateDeclarations(declarations);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message().find("needs Missing"), std::string::npos);
}

namespace {

Declarations bookDeclarations() {
    Declarations declarations;
    declarations.row_types = {{"Book",
                               "a.hpp:1",
                               {{"id", false, {}, ValueKind::Number},
                                {"title", false, {}, ValueKind::Text},
                                {"unit_price", false, {}, ValueKind::Number},
                                {"in_print", false, {}, ValueKind::Bool},
                                {"series", true, {}, ValueKind::Text}}}};
    declarations.repositories = {{"Books", "a.hpp:9", "books", "Book", {}}};
    return declarations;
}

RepositoryFunction function(std::string name, RepositoryReturn returns,
                            std::vector<RepositoryParameter> parameters = {},
                            std::string element = "Book") {
    RepositoryFunction result;
    result.name = std::move(name);
    result.location = "a.hpp:12";
    result.returns = returns;
    result.element_type = returns == RepositoryReturn::List || returns == RepositoryReturn::Optional
                              ? std::move(element)
                              : "";
    result.parameters = std::move(parameters);
    return result;
}

RepositoryParameter text(std::string name) {
    return {std::move(name), "const std::string &", ValueKind::Text};
}
RepositoryParameter number(std::string name) {
    return {std::move(name), "double", ValueKind::Number};
}

/// The SQL for a function of Books, which must be valid.
std::string sqlFor(const RepositoryFunction& candidate) {
    const Declarations declarations = bookDeclarations();
    const auto query = repositoryQuery(declarations.repositories[0], candidate, declarations);
    EXPECT_TRUE(query) << (query ? "" : query.error().message());
    return query ? query.value().sql : "";
}

/// The error for a function of Books, which must be rejected.
std::string errorFor(const RepositoryFunction& candidate) {
    const Declarations declarations = bookDeclarations();
    const auto query = repositoryQuery(declarations.repositories[0], candidate, declarations);
    EXPECT_FALSE(query) << candidate.name << " was accepted as " << (query ? query.value().sql : "");
    return query ? "" : query.error().message();
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST(RepositoryQueryTest, ReadsFieldsOfSeveralWords) {
    EXPECT_EQ(sqlFor(function("findByUnitPriceLessThanOrderByUnitPriceDesc", RepositoryReturn::List,
                              {number("most")})),
              "SELECT id, title, unit_price, in_print, series FROM books WHERE unit_price < $1 "
              "ORDER BY unit_price DESC");
    EXPECT_EQ(sqlFor(function("countByInPrintTrue", RepositoryReturn::Integer)),
              "SELECT COUNT(*) FROM books WHERE in_print = TRUE");
}

TEST(RepositoryQueryTest, NamesItsMistakes) {
    EXPECT_TRUE(contains(errorFor(function("lookUp", RepositoryReturn::List)),
                         "a.hpp:12: Books::lookUp can't be written from its name"));
    EXPECT_TRUE(contains(errorFor(function("findByAuthor", RepositoryReturn::List, {text("a")})),
                         "refers to Author, which isn't a field of Book. Its fields are id, title, "
                         "unit_price, in_print, series"));
    EXPECT_TRUE(contains(errorFor(function("findByTitleRoughly", RepositoryReturn::List, {text("t")})),
                         "has 'Roughly' after title"));
    EXPECT_TRUE(contains(errorFor(function("findByTitleIn", RepositoryReturn::List, {text("t")})),
                         "uses In, which isn't supported yet"));
    EXPECT_TRUE(contains(errorFor(function("findByTitle", RepositoryReturn::List)),
                         "needs a parameter for title, but it has too few"));
    EXPECT_TRUE(contains(errorFor(function("findByTitle", RepositoryReturn::List,
                                           {text("a"), text("b")})),
                         "has 2 parameters, but its name uses 1"));
    EXPECT_TRUE(contains(errorFor(function("findByTitle", RepositoryReturn::List, {number("t")})),
                         "compares title, which is text, with parameter 't', which is a number"));
    EXPECT_TRUE(contains(errorFor(function("findByUnitPriceContaining", RepositoryReturn::List,
                                           {number("p")})),
                         "matches text in unit_price, which isn't text"));
    EXPECT_TRUE(contains(errorFor(function("findByTitleTrue", RepositoryReturn::List)),
                         "isn't a bool"));
    EXPECT_TRUE(contains(errorFor(function("findByUnitPriceIgnoreCase", RepositoryReturn::List,
                                           {number("p")})),
                         "ignores case for unit_price"));
    EXPECT_TRUE(contains(errorFor(function("findByTitleAnd", RepositoryReturn::List, {text("t")})),
                         "ends with And"));
    EXPECT_TRUE(contains(errorFor(function("findByTitleOrderBy", RepositoryReturn::List, {text("t")})),
                         "ends with OrderBy"));
    EXPECT_TRUE(contains(errorFor(function("findByOrderByWeight", RepositoryReturn::List)),
                         "orders by Weight"));
}

TEST(RepositoryQueryTest, TheReturnTypeHasToFit) {
    EXPECT_TRUE(contains(errorFor(function("findByTitle", RepositoryReturn::Integer, {text("t")})),
                         "has to return std::vector<Book> or std::optional<Book>"));
    EXPECT_TRUE(contains(errorFor(function("findByTitle", RepositoryReturn::List, {text("t")}, "Other")),
                         "returns Other, but Books holds Book"));
    EXPECT_TRUE(contains(errorFor(function("findFirstByTitle", RepositoryReturn::List, {text("t")})),
                         "finds one row, so it has to return std::optional<Book>"));
    EXPECT_TRUE(contains(errorFor(function("findTop3ByTitle", RepositoryReturn::Optional, {text("t")})),
                         "has to return std::vector<Book>"));
    EXPECT_TRUE(contains(errorFor(function("findTop0ByTitle", RepositoryReturn::List, {text("t")})),
                         "has to be a whole number from 1"));
    EXPECT_TRUE(contains(errorFor(function("countByTitle", RepositoryReturn::Bool, {text("t")})),
                         "has to return std::int64_t"));
    EXPECT_TRUE(contains(errorFor(function("existsByTitle", RepositoryReturn::Integer, {text("t")})),
                         "has to return bool"));
    EXPECT_TRUE(contains(errorFor(function("deleteByTitle", RepositoryReturn::List, {text("t")})),
                         "deletes rows"));
    EXPECT_TRUE(contains(errorFor(function("countByTitleOrderByTitle", RepositoryReturn::Integer,
                                           {text("t")})),
                         "has OrderBy, which only changes anything when finding rows"));
}

TEST(RepositoryQueryTest, NeverDeletesEverythingFromAName) {
    EXPECT_TRUE(contains(errorFor(function("deleteBy", RepositoryReturn::Integer)),
                         "would delete every row"));
    EXPECT_TRUE(contains(errorFor(function("deleteAll", RepositoryReturn::Void)),
                         "can't be written from its name"));
}

TEST(RepositoryQueryTest, APageComesLastAndOnlyWhenListing) {
    const RepositoryParameter page{"page", "cppboot::data::Page", ValueKind::Page};
    const Declarations declarations = bookDeclarations();
    const auto query = repositoryQuery(
        declarations.repositories[0],
        function("findByInPrintTrueOrderByTitle", RepositoryReturn::List, {page}), declarations);
    ASSERT_TRUE(query) << query.error().message();
    EXPECT_TRUE(query.value().sql.ends_with("ORDER BY title ASC LIMIT $1 OFFSET $2"));
    EXPECT_EQ(query.value().arguments, (std::vector<std::string>{"page.limit", "page.offset"}));

    EXPECT_TRUE(contains(errorFor(function("findByTitle", RepositoryReturn::List, {page, text("t")})),
                         "a cppboot::data::Page that isn't its last parameter"));
    EXPECT_TRUE(contains(errorFor(function("findTop2ByTitle", RepositoryReturn::List, {text("t"), page})),
                         "only works when returning a std::vector with no First or Top"));
    EXPECT_TRUE(contains(errorFor(function("countByInPrintTrue", RepositoryReturn::Integer, {page})),
                         "only works when finding rows"));
}

TEST(RepositoryQueryTest, OwnSqlHasToMatchItsParametersAndTypes) {
    Declarations declarations = bookDeclarations();
    const auto check = [&](RepositoryFunction candidate, std::string query) {
        candidate.query = std::move(query);
        return repositoryQuery(declarations.repositories[0], candidate, declarations);
    };

    auto ok = check(function("cheap", RepositoryReturn::List, {number("most")}),
                    "SELECT * FROM books WHERE unit_price < $1");
    ASSERT_TRUE(ok) << ok.error().message();
    EXPECT_EQ(ok.value().run, QueryRun::Rows);

    // A $ inside quotes isn't a placeholder.
    ok = check(function("dollars", RepositoryReturn::Integer), "SELECT COUNT(*) FROM books WHERE title = '$9'");
    ASSERT_TRUE(ok) << ok.error().message();
    EXPECT_EQ(ok.value().run, QueryRun::Value);

    ok = check(function("raise", RepositoryReturn::Integer, {number("f")}),
               "UPDATE books SET unit_price = unit_price * $1");
    ASSERT_TRUE(ok);
    EXPECT_EQ(ok.value().run, QueryRun::Change);

    auto bad = check(function("cheap", RepositoryReturn::List, {number("a"), number("b")}),
                     "SELECT * FROM books WHERE unit_price < $1");
    ASSERT_FALSE(bad);
    EXPECT_TRUE(contains(bad.error().message(), "has 2 parameters, but its SQL goes up to $1"));

    bad = check(function("names", RepositoryReturn::List, {}, "std::basic_string<char>"),
                "SELECT title FROM books");
    ASSERT_FALSE(bad);
    EXPECT_TRUE(contains(bad.error().message(), "which isn't marked CPPBOOT_ROW"));

    bad = check(function("touched", RepositoryReturn::Bool), "DELETE FROM books");
    ASSERT_FALSE(bad);
    EXPECT_TRUE(contains(bad.error().message(), "returns bool, which only works for a SELECT"));
}

TEST(ValidateDeclarationsTest, ChecksEveryRepositoryFunctionAndProvidesTheInterface) {
    Declarations declarations = bookDeclarations();
    declarations.repositories[0].functions = {
        function("findByTitle", RepositoryReturn::List, {text("t")})};
    declarations.components = {{"Shelf", "b.hpp:1", {}, {"Books"}}};
    const auto valid = validateDeclarations(declarations);
    EXPECT_TRUE(valid) << valid.error().message();

    declarations.repositories[0].functions.push_back(function("findByWeight", RepositoryReturn::List,
                                                              {number("w")}));
    const auto invalid = validateDeclarations(declarations);
    ASSERT_FALSE(invalid);
    EXPECT_TRUE(contains(invalid.error().message(), "Books::findByWeight refers to Weight"));

    declarations.repositories[0].row_type = "Missing";
    declarations.repositories[0].functions.pop_back();
    const auto no_row = validateDeclarations(declarations);
    ASSERT_FALSE(no_row);
    EXPECT_TRUE(contains(no_row.error().message(), "holds Missing, which isn't marked CPPBOOT_ROW"));
}

TEST(OpenApiTest, DescribesSecurityChecksAndLimits) {
    Declarations declarations;
    declarations.json_types = {
        {"shop::Order",
         "a.hpp:1",
         {{"quantity", false, {{CheckKind::Range, ValueShape::Number, "1", "100"}}, ValueKind::Number,
           "integer/int32"},
          {"note", true, {{CheckKind::Length, ValueShape::Text, "0", "200"}}, ValueKind::Text, "string"},
          {"items", false, {{CheckKind::NotEmpty, ValueShape::Items, "", ""}}, ValueKind::Other,
           "array:ref:shop::Item"},
          {"gift", true, {}, ValueKind::Other, "ref:shop::Order"},
          {"extra", false, {}, ValueKind::Other, "map:number/double"},
          {"opaque", false, {}, ValueKind::Other, "ref:shop::NotJson"}}},
    };

    Route list;
    list.method = "Get";
    list.path = "/orders";
    list.function = "list";
    list.parameters = {{ParameterSource::Query,
                        "size",
                        "std::optional<int>",
                        {{CheckKind::Max, ValueShape::Number, "", "50"}},
                        "integer/int32"}};
    list.response_schema = "array:ref:shop::Order";
    list.roles = {"admin", "staff"};
    list.rate_limit = RouteRateLimit{5, 60};

    Route health;
    health.method = "Get";
    health.path = "/health";
    health.function = "health";
    health.is_public = true;
    health.return_kind = ReturnKind::Response;

    Route create;
    create.method = "Post";
    create.path = "/orders";
    create.function = "create";
    create.parameters = {{ParameterSource::Body, "order", "shop::Order", {}, "ref:shop::Order"},
                         {ParameterSource::Principal, "who", "cppboot::security::Principal", {}, ""}};
    create.response_schema = "ref:shop::Order";
    create.status_varies = true;

    declarations.controllers = {{"shop::OrderController", {list, health, create}}};

    RenderOptions options;
    options.title = "shop";
    options.version = "1.2.3";
    const Json document = Json::parse(renderOpenApi(declarations, options));

    EXPECT_EQ(document.at("info").at("version"), "1.2.3");
    EXPECT_EQ(document.at("components").at("securitySchemes").at("bearerAuth").at("scheme"), "bearer");

    const Json& get = document.at("paths").at("/orders").at("get");
    EXPECT_EQ(get.at("operationId"), "shop.OrderController.list");
    EXPECT_EQ(get.at("security").at(0).at("bearerAuth"), Json::array());
    for (const char* status : {"200", "400", "401", "403", "429"}) {
        EXPECT_TRUE(get.at("responses").contains(status)) << status;
    }
    EXPECT_NE(get.at("description").asString().find("admin, staff"), std::string::npos);
    const Json& size = get.at("parameters").at(0);
    EXPECT_EQ(size.at("required"), false);
    EXPECT_EQ(size.at("schema").at("maximum"), 50);

    // A public route has no security and no 401, even in a secured project.
    const Json& open = document.at("paths").at("/health").at("get");
    EXPECT_FALSE(open.contains("security"));
    EXPECT_FALSE(open.at("responses").contains("401"));
    EXPECT_TRUE(open.at("responses").contains("default"));

    const Json& post = document.at("paths").at("/orders").at("post");
    EXPECT_EQ(post.at("requestBody").at("content").at("application/json").at("schema").at("$ref"),
              "#/components/schemas/shop.Order");
    EXPECT_TRUE(post.at("responses").contains("2XX"));
    EXPECT_FALSE(post.at("responses").contains("403"));
    // A Principal comes from the token, not from the request, so it isn't a parameter.
    EXPECT_FALSE(post.contains("parameters"));

    const Json& order = document.at("components").at("schemas").at("shop.Order");
    EXPECT_EQ(order.at("required"),
              (Json{std::vector<std::string>{"quantity", "items", "extra", "opaque"}}));
    const Json& properties = order.at("properties");
    EXPECT_EQ(properties.at("quantity").at("minimum"), 1);
    EXPECT_EQ(properties.at("quantity").at("maximum"), 100);
    EXPECT_EQ(properties.at("note").at("maxLength"), 200);
    EXPECT_EQ(properties.at("note").at("nullable"), true);
    EXPECT_EQ(properties.at("items").at("minItems"), 1);
    // shop::Item isn't converted to JSON, so it's left open rather than a broken $ref.
    EXPECT_EQ(properties.at("items").at("items"), Json::object());
    EXPECT_EQ(properties.at("gift").at("allOf").at(0).at("$ref"), "#/components/schemas/shop.Order");
    EXPECT_EQ(properties.at("gift").at("nullable"), true);
    EXPECT_EQ(properties.at("extra").at("additionalProperties").at("format"), "double");
    EXPECT_EQ(properties.at("opaque"), Json::object());
}

TEST(OpenApiTest, AProjectWithoutSecurityHasNoSchemes) {
    Declarations declarations;
    Route route;
    route.method = "Delete";
    route.path = "/items/{id}";
    route.function = "remove";
    route.return_kind = ReturnKind::Nothing;
    route.parameters = {{ParameterSource::Path, "id", "long long", {}, "integer/int64"}};
    declarations.controllers = {{"Items", {route}}};

    const Json document = Json::parse(renderOpenApi(declarations, {}));
    EXPECT_FALSE(document.at("components").contains("securitySchemes"));
    const Json& remove = document.at("paths").at("/items/{id}").at("delete");
    EXPECT_FALSE(remove.contains("security"));
    EXPECT_TRUE(remove.at("responses").contains("204"));
    EXPECT_EQ(remove.at("parameters").at(0).at("schema").at("format"), "int64");
    EXPECT_EQ(remove.at("parameters").at(0).at("required"), true);
}
