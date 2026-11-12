#include "cppboot/codegen/model.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <utility>

namespace cppboot::codegen {

namespace {

using Providers = std::map<std::string, const Component*>;

bool isFrameworkType(std::string_view type) {
    return std::find(kFrameworkTypes.begin(), kFrameworkTypes.end(), type) != kFrameworkTypes.end();
}

bool isGroupInterface(std::string_view type) {
    return type == kModuleInterface || type == kHealthIndicatorInterface;
}

Result<void> addProvider(Providers& providers, const std::string& type, const Component& component) {
    const auto [entry, inserted] = providers.emplace(type, &component);
    if (!inserted) {
        return Error{ErrorCode::DependencyError,
                     type + " is provided by both " + entry->second->type + " (" +
                         entry->second->location + ") and " + component.type + " (" +
                         component.location + ")"};
    }
    return {};
}

enum class VisitState { InProgress, Done };

Result<void> checkForCycle(const Component& component, const Providers& providers,
                           std::map<const Component*, VisitState>& states,
                           std::vector<const Component*>& path) {
    if (const auto state = states.find(&component); state != states.end()) {
        if (state->second == VisitState::Done) {
            return {};
        }

        std::string cycle;
        const auto start = std::find(path.begin(), path.end(), &component);
        for (auto step = start; step != path.end(); ++step) {
            cycle += (*step)->type + " -> ";
        }
        cycle += component.type;
        return Error{ErrorCode::DependencyError, "Circular dependency: " + cycle};
    }

    states.emplace(&component, VisitState::InProgress);
    path.push_back(&component);
    for (const std::string& dependency : component.dependencies) {
        // The framework and settings structs provide the rest, and they depend on no component.
        const auto provider = providers.find(dependency);
        if (provider == providers.end()) {
            continue;
        }
        if (auto checked = checkForCycle(*provider->second, providers, states, path); !checked) {
            return checked;
        }
    }
    path.pop_back();
    states[&component] = VisitState::Done;
    return {};
}

/// Makes /users/{id} and /users/{name} compare equal, since they match the same requests.
std::string routeShape(std::string_view path) {
    std::string shape;
    bool in_parameter = false;
    for (const char c : path) {
        if (c == '{') {
            in_parameter = true;
            shape += "{}";
        } else if (c == '}') {
            in_parameter = false;
        } else if (!in_parameter) {
            shape += c;
        }
    }
    return shape;
}

std::string quoted(std::string_view text) {
    std::string result = "\"";
    for (const char c : text) {
        if (c == '\n') {
            result += "\\n";
        } else if (c == '\r') {
            result += "\\r";
        } else if (c == '\t') {
            result += "\\t";
        } else {
            if (c == '"' || c == '\\') {
                result += '\\';
            }
            result += c;
        }
    }
    return result + '"';
}

/// Text as a raw string literal, or an ordinary one if the text happens to contain the delimiter.
std::string rawLiteral(const std::string& text) {
    constexpr std::string_view kDelimiter = "cppboot_text";
    if (text.find(")" + std::string{kDelimiter} + "\"") != std::string::npos) {
        return quoted(text);
    }
    return "R\"" + std::string{kDelimiter} + "(" + text + ")" + std::string{kDelimiter} + "\"";
}

bool hasChecks(const Declarations& declarations) {
    const auto checked_field = [](const StructField& field) { return !field.checks.empty(); };
    for (const MappedStruct& json_type : declarations.json_types) {
        if (std::any_of(json_type.fields.begin(), json_type.fields.end(), checked_field)) {
            return true;
        }
    }
    for (const ConfigStruct& settings : declarations.config_types) {
        if (std::any_of(settings.fields.begin(), settings.fields.end(),
                        [](const ConfigField& field) { return !field.checks.empty(); })) {
            return true;
        }
    }
    for (const Controller& controller : declarations.controllers) {
        for (const Route& route : controller.routes) {
            if (std::any_of(route.parameters.begin(), route.parameters.end(),
                            [](const RouteParameter& parameter) {
                                return !parameter.checks.empty();
                            })) {
                return true;
            }
        }
    }
    return false;
}

bool isConfigType(const Declarations& declarations, std::string_view type) {
    return std::any_of(declarations.config_types.begin(), declarations.config_types.end(),
                       [&](const ConfigStruct& settings) { return settings.type == type; });
}

/// Fills each settings struct when components are registered, rather than when something first
/// asks for it, so that a wrong setting stops startup even if nothing uses the struct yet.
void renderConfigType(const ConfigStruct& settings, const std::vector<ConfigStruct>& all,
                      std::string& text) {
    text += "    {\n";
    text += "        static_assert(std::is_default_constructible_v<" + settings.type + ">, " +
            quoted(settings.type + " must be default constructible to be read from settings") +
            ");\n";
    text += "        const auto config = container.resolve<cppboot::config::Config>();\n";
    text += "        auto settings = std::make_shared<" + settings.type + ">();\n";
    text += "        cppboot::config::SettingsBinder binder{*config, " + quoted(settings.prefix) +
            ", " + quoted(settings.type) + "};\n";
    for (const ConfigField& field : settings.fields) {
        const std::string key = quoted(field.path);
        const std::string value = "settings->" + field.path;
        text += "        binder.bind(" + key + ", " + value + ");\n";
        if (field.required) {
            text += "        binder.require(" + key + ");\n";
        }
        for (const Check& check : field.checks) {
            text += "        binder.check(" + key + ", " + checkExpression(check, value) + ", " +
                    quoted(checkMessage(check)) + ");\n";
        }
    }
    // Settings under a longer prefix belong to the struct that reads them.
    const std::string group = settings.prefix + ".";
    for (const ConfigStruct& other : all) {
        if (other.prefix.starts_with(group)) {
            text += "        binder.ignoreGroup(" + quoted(other.prefix.substr(group.size())) + ");\n";
        }
    }
    text += "        binder.finish();\n";
    text += "        container.registerInstance(std::move(settings));\n";
    text += "    }\n";
}

/// app::ProductRepository becomes app_ProductRepositoryImplementation, a name for its generated
/// class. The suffix keeps it apart from the interface, even one with no namespace.
std::string implementationName(const RepositoryInterface& repository) {
    std::string name;
    for (const char c : repository.type) {
        name += c == ':' ? '_' : c;
    }
    std::string collapsed;
    for (const char c : name) {
        if (!(c == '_' && !collapsed.empty() && collapsed.back() == '_')) {
            collapsed += c;
        }
    }
    return collapsed + "Implementation";
}

/// The code that runs a query and returns what the function returns.
std::string renderQueryCall(const RepositoryFunction& function, const RepositoryQuery& query) {
    std::string arguments = quoted(query.sql);
    for (const std::string& argument : query.arguments) {
        arguments += ", " + argument;
    }

    switch (query.run) {
        case QueryRun::Rows:
            return "        return database_.queryAs<" + function.element_type + ">(" + arguments +
                   ");\n";
        case QueryRun::FirstRow:
            return "        return database_.queryOne<" + function.element_type + ">(" + arguments +
                   ");\n";
        case QueryRun::OneRow:
            return "        auto rows = database_.queryAs<" + function.element_type + ">(" +
                   arguments + ");\n"
                   "        if (rows.size() > 1) {\n"
                   "            throw cppboot::data::DatabaseError(cppboot::data::DatabaseError::Kind::Query, " +
                   quoted(function.name + " found more than one row, so it can't return one. Return "
                                          "a std::vector, or start its name with findFirstBy") +
                   ");\n        }\n"
                   "        if (rows.empty()) {\n            return std::nullopt;\n        }\n"
                   "        return std::move(rows.front());\n";
        case QueryRun::Value:
            if (function.returns == RepositoryReturn::Optional) {
                return "        return database_.queryValue<" + function.element_type + ">(" +
                       arguments + ");\n";
            }
            return "        return database_.queryValue<" +
                   std::string{function.returns == RepositoryReturn::Bool ? "bool" : "std::int64_t"} +
                   ">(" + arguments + ").value_or(" +
                   (function.returns == RepositoryReturn::Bool ? "false" : "0") + ");\n";
        case QueryRun::Exists:
            return "        return database_.queryValue<std::int64_t>(" + arguments +
                   ").has_value();\n";
        case QueryRun::Change:
            if (function.returns == RepositoryReturn::Void) {
                return "        database_.execute(" + arguments + ");\n";
            }
            return "        return database_.execute(" + arguments + ");\n";
    }
    return "";
}

/// A class that implements a repository's functions with the queries their names describe.
void renderRepository(const RepositoryInterface& repository, const Declarations& declarations,
                      std::string& text) {
    const std::string name = implementationName(repository);
    // Qualified from the global namespace, so that the name can't mean anything in this one.
    text += "class " + name + " final : public ::" + repository.type + " {\npublic:\n";
    text += "    explicit " + name +
            "(cppboot::data::Database& database) : database_(database) {}\n";
    for (const RepositoryFunction& function : repository.functions) {
        // Checked by validateDeclarations, so this always has a value here.
        const auto query = repositoryQuery(repository, function, declarations);
        if (!query) {
            continue;
        }
        text += "\n    " + function.return_type + " " + function.name + "(";
        for (std::size_t i = 0; i < function.parameters.size(); ++i) {
            text += i == 0 ? "" : ", ";
            text += function.parameters[i].type + " " + function.parameters[i].name;
        }
        text += std::string{")"} + (function.is_const ? " const" : "") + " override {\n";
        text += renderQueryCall(function, query.value());
        text += "    }\n";
    }
    text += "\nprivate:\n    cppboot::data::Database& database_;\n};\n\n";
}

bool usesDatabase(const Declarations& declarations) {
    if (!declarations.row_types.empty() || !declarations.repositories.empty()) {
        return true;
    }
    return std::any_of(declarations.components.begin(), declarations.components.end(),
                       [](const Component& component) {
                           return std::find(component.dependencies.begin(),
                                            component.dependencies.end(),
                                            kDatabaseType) != component.dependencies.end();
                       });
}

/// Fails safe: one mark anywhere protects every route that isn't marked public.
bool usesSecurity(const Declarations& declarations) {
    const bool has_dependency = std::any_of(
        declarations.components.begin(), declarations.components.end(),
        [](const Component& component) {
            return std::find(component.dependencies.begin(), component.dependencies.end(),
                             kTokenServiceType) != component.dependencies.end();
        });
    if (has_dependency) {
        return true;
    }

    for (const Controller& controller : declarations.controllers) {
        for (const Route& route : controller.routes) {
            const bool takes_principal =
                std::any_of(route.parameters.begin(), route.parameters.end(),
                            [](const RouteParameter& parameter) {
                                return parameter.source == ParameterSource::Principal;
                            });
            if (route.is_public || !route.roles.empty() || takes_principal) {
                return true;
            }
        }
    }
    return false;
}

bool usesRateLimits(const Declarations& declarations) {
    return std::any_of(declarations.controllers.begin(), declarations.controllers.end(),
                       [](const Controller& controller) {
                           return std::any_of(controller.routes.begin(), controller.routes.end(),
                                              [](const Route& route) {
                                                  return route.rate_limit.has_value();
                                              });
                       });
}

void renderDatabase(const std::vector<MappedStruct>& row_types, std::string& text) {
    text += "    auto row_mappers = std::make_shared<cppboot::data::RowMappers>();\n";
    for (const MappedStruct& row_type : row_types) {
        text += "    row_mappers->add<" + row_type.type +
                ">([](const cppboot::data::Row& row, " + row_type.type + "& value) {\n";
        for (const StructField& field : row_type.fields) {
            text += "        row.read(" + quoted(field.name) + ", value." + field.name + ");\n";
        }
        text += "    });\n";
    }
    text += "    container.registerSingleton<cppboot::data::Database>("
            "[row_mappers](cppboot::di::Resolver& resolver) {\n";
    text += "        return cppboot::data::Database::fromSettings("
            "*resolver.resolve<cppboot::config::Config>(), row_mappers);\n    });\n";
    text += "    container.registerSingleton<cppboot::data::DatabaseModule>("
            "cppboot::di::Deps<cppboot::data::Database, cppboot::config::Config>{});\n";
    text += "    container.bindAll<cppboot::Module, cppboot::data::DatabaseModule>();\n";
}

void renderJsonType(const MappedStruct& json_type, std::string& text) {
    text += "template <>\nstruct cppboot::JsonConvert<" + json_type.type + "> {\n";
    text += "    static_assert(std::is_default_constructible_v<" + json_type.type + ">, " +
            quoted(json_type.type + " must be default constructible for JSON conversion") + ");\n\n";

    // Templates, so that a field whose type is converted further down the file still works: they're
    // only compiled where they're used.
    text += "    template <typename JsonType>\n";
    text += "    static void toJson(JsonType& json, const " + json_type.type + "& value) {\n";
    text += "        json = JsonType::object();\n";
    for (const StructField& field : json_type.fields) {
        text += "        json[" + quoted(field.name) + "] = JsonType{value." + field.name + "};\n";
    }
    text += "    }\n\n";

    // The helpers name the field in any error, so a client learns which one was wrong.
    text += "    template <typename JsonType>\n";
    text += "    static void fromJson(const JsonType& json, " + json_type.type + "& value) {\n";
    text += "        if (!json.isObject()) {\n";
    text += "            throw cppboot::JsonError(\"expected an object\");\n        }\n";
    for (const StructField& field : json_type.fields) {
        text += std::string{"        cppboot::detail::"} +
                (field.is_optional ? "readOptionalJsonField" : "readJsonField") + "(json, " +
                quoted(field.name) + ", value." + field.name + ");\n";
        for (const Check& check : field.checks) {
            text += "        if (!" + checkExpression(check, "value." + field.name) + ") {\n";
            text += "            throw cppboot::JsonError(" +
                    quoted("field '" + field.name + "': " + checkMessage(check)) + ");\n";
            text += "        }\n";
        }
    }
    text += "    }\n};\n\n";
}

std::string renderArgument(const RouteParameter& parameter) {
    switch (parameter.source) {
        case ParameterSource::Path:
            return "cppboot::web::pathParam<" + parameter.type + ">(request, " +
                   quoted(parameter.name) + ")";
        case ParameterSource::Query:
            return "cppboot::web::queryParam<" + parameter.type + ">(request, " +
                   quoted(parameter.name) + ")";
        case ParameterSource::Body:
            return "cppboot::web::bodyAs<" + parameter.type + ">(request)";
        case ParameterSource::Request:
            return "request";
        case ParameterSource::Principal:
            return "*principal";
    }
    return "request";
}

void renderGuard(const Route& route, std::string& text) {
    text += "                const auto principal = cppboot::security::authenticate(*tokens, request);\n";
    text += "                if (!principal) {\n";
    text += "                    return cppboot::security::unauthorizedResponse();\n";
    text += "                }\n";
    if (!route.roles.empty()) {
        std::string roles;
        for (const std::string& role : route.roles) {
            roles += roles.empty() ? "" : ", ";
            roles += quoted(role);
        }
        text += "                if (!principal->hasAnyRole({" + roles + "})) {\n";
        text += "                    return cppboot::security::forbiddenResponse();\n";
        text += "                }\n";
    }
}

void renderRoute(const Route& route, bool uses_security, std::string& text) {
    const bool guarded = uses_security && !route.is_public;

    std::string captures = "controller";
    if (guarded) {
        captures += ", tokens";
    }
    if (route.rate_limit) {
        captures += ", limiter = std::make_shared<cppboot::web::RateLimiter>("
                    "cppboot::web::RateLimit{" +
                    std::to_string(route.rate_limit->requests) + ", std::chrono::seconds{" +
                    std::to_string(route.rate_limit->window_seconds) + "}}, rate_limit_options)";
    }

    text += "            router->add(cppboot::web::Method::" + route.method + ", " +
            quoted(route.path) + ", [" + captures +
            "]([[maybe_unused]] const cppboot::web::Request& request) -> "
            "cppboot::web::Response {\n";

    // Comes first, so that it also limits attempts to guess a token.
    if (route.rate_limit) {
        text += "                if (auto rejected = limiter->check(request)) {\n";
        text += "                    return *std::move(rejected);\n";
        text += "                }\n";
    }

    if (guarded) {
        renderGuard(route, text);
    }

    // A checked parameter is read into a variable first, so that it can be checked before the call.
    std::string call = "controller->" + route.function + "(";
    for (std::size_t i = 0; i < route.parameters.size(); ++i) {
        const RouteParameter& parameter = route.parameters[i];
        call += i == 0 ? "" : ", ";
        if (parameter.checks.empty()) {
            call += renderArgument(parameter);
            continue;
        }

        const std::string variable = "parameter_" + std::to_string(i);
        const std::string name =
            std::string{parameter.source == ParameterSource::Path ? "Path" : "Query"} +
            " parameter '" + parameter.name + "' ";
        text += "                const auto " + variable + " = " + renderArgument(parameter) + ";\n";
        for (const Check& check : parameter.checks) {
            text += "                if (!" + checkExpression(check, variable) + ") {\n";
            text += "                    throw cppboot::web::HttpError(400, " +
                    quoted(name + checkMessage(check)) + ");\n";
            text += "                }\n";
        }
        call += variable;
    }
    call += ")";

    switch (route.return_kind) {
        case ReturnKind::Nothing:
            text += "                " + call + ";\n";
            text += "                return cppboot::web::Response{204};\n";
            break;
        case ReturnKind::Response:
            text += "                return " + call + ";\n";
            break;
        case ReturnKind::Value:
            text += "                return cppboot::web::toJsonResponse(" + call + ");\n";
            break;
    }
    text += "            });\n";
}

void renderMiddleware(std::vector<Middleware> middleware, std::string& text) {
    std::stable_sort(middleware.begin(), middleware.end(),
                     [](const Middleware& left, const Middleware& right) {
                         return left.order < right.order;
                     });

    for (const Middleware& entry : middleware) {
        text += "        {\n";
        text += "            auto middleware = resolver.resolve<" + entry.type + ">();\n";
        text += "            router->use([middleware](const cppboot::web::Request& request, "
                "const cppboot::web::Next& next) {\n";
        text += "                return middleware->handle(request, next);\n";
        text += "            });\n";
        text += "        }\n";
    }
}

void renderActuator(bool uses_database, bool uses_security, std::string& text) {
    text += "        auto actuator = resolver.resolve<cppboot::actuator::Actuator>();\n";
    if (uses_database) {
        text += "        auto database = resolver.resolve<cppboot::data::Database>();\n";
        text += "        actuator->addCheck(\"database\", [database] { "
                "database->query(\"SELECT 1\"); });\n";
        text += "        actuator->metrics().gaugeFunction(\"database_connections_open\", "
                "\"Database connections open\", [database] { "
                "return static_cast<double>(database->pool().openCount()); });\n";
        text += "        actuator->metrics().gaugeFunction(\"database_connections_idle\", "
                "\"Database connections not in use\", [database] { "
                "return static_cast<double>(database->pool().idleCount()); });\n";
    }
    text += "        for (auto& indicator : container.resolveAll<"
            "cppboot::actuator::HealthIndicator>()) {\n";
    text += "            actuator->addIndicator(std::move(indicator));\n";
    text += "        }\n";
    // First, so that it measures the other middleware too.
    text += "        cppboot::actuator::mount(*router, actuator, ";
    text += uses_security ? "true" : "false";
    text += ");\n";
}

void renderRouter(const Declarations& declarations, bool uses_database, bool uses_security,
                  bool actuator, std::string& text) {
    text += "    container.registerSingleton<cppboot::web::Router>([";
    text += actuator || !declarations.controllers.empty() ? "&container" : "";
    text += "](cppboot::di::Resolver& resolver) {\n";
    text += "        auto router = std::make_shared<cppboot::web::Router>();\n";
    if (uses_security) {
        // Resolved here even if every route is public, so that a missing secret stops startup.
        text += "        auto tokens = resolver.resolve<cppboot::security::TokenService>();\n";
    }
    if (usesRateLimits(declarations)) {
        text += "        const auto rate_limit_options = "
                "cppboot::web::RateLimiterOptions::fromSettings("
                "*resolver.resolve<cppboot::config::Config>(), "
                "*resolver.resolve<cppboot::logging::Logger>());\n";
    }
    if (actuator) {
        renderActuator(uses_database, uses_security, text);
    }
    renderMiddleware(declarations.middleware, text);
    if (!declarations.controllers.empty()) {
        // Off unless asked for, because it lists every route to whoever can reach the server. A
        // container with no settings, as in some tests, leaves it off.
        text += "        if (container.contains(typeid(cppboot::config::Config)) &&\n"
                "            resolver.resolve<cppboot::config::Config>()->getBool("
                "\"cppboot.openapi.enabled\", false).value()) {\n";
        text += "            router->get(\"/openapi.json\", [](const cppboot::web::Request&) {\n";
        text += "                return cppboot::web::Response::json(std::string{kOpenApi});\n";
        text += "            });\n        }\n";
    }
    for (const Controller& controller : declarations.controllers) {
        text += "        {\n";
        text += "            auto controller = resolver.resolve<" + controller.type + ">();\n";
        for (const Route& route : controller.routes) {
            renderRoute(route, uses_security, text);
        }
        text += "        }\n";
    }
    text += "        return router;\n    });\n";
    text += "    container.registerSingleton<cppboot::web::ServerModule>("
            "cppboot::di::Deps<cppboot::web::Router>{});\n";
    text += "    container.bindAll<cppboot::Module, cppboot::web::ServerModule>();\n";
}

}  // namespace

bool routesNeedTokens(const Declarations& declarations) {
    return usesSecurity(declarations);
}

std::string checkExpression(const Check& check, std::string_view value) {
    const std::string argument{value};
    switch (check.kind) {
        case CheckKind::NotEmpty:
            return "cppboot::validation::notEmpty(" + argument + ")";
        case CheckKind::Length:
            return "cppboot::validation::lengthInRange(" + argument + ", " + check.min + ", " +
                   check.max + ")";
        case CheckKind::Min:
            return "cppboot::validation::atLeast(" + argument + ", " + check.min + ")";
        case CheckKind::Max:
            return "cppboot::validation::atMost(" + argument + ", " + check.max + ")";
        case CheckKind::Range:
            return "cppboot::validation::inRange(" + argument + ", " + check.min + ", " +
                   check.max + ")";
    }
    return "true";
}

std::string checkMessage(const Check& check) {
    switch (check.kind) {
        case CheckKind::NotEmpty:
            return "must not be empty";
        case CheckKind::Length: {
            const bool text = check.shape == ValueShape::Text;
            const std::string unit = check.max == "1" ? (text ? " character" : " item")
                                                      : (text ? " characters" : " items");
            if (check.min == check.max) {
                return "must have exactly " + check.max + unit;
            }
            return "must have from " + check.min + " to " + check.max + unit;
        }
        case CheckKind::Min:
            return "must be at least " + check.min;
        case CheckKind::Max:
            return "must be at most " + check.max;
        case CheckKind::Range:
            return "must be from " + check.min + " to " + check.max;
    }
    return "is invalid";
}

void Declarations::append(Declarations other) {
    components.insert(components.end(), std::make_move_iterator(other.components.begin()),
                      std::make_move_iterator(other.components.end()));
    json_types.insert(json_types.end(), std::make_move_iterator(other.json_types.begin()),
                      std::make_move_iterator(other.json_types.end()));
    row_types.insert(row_types.end(), std::make_move_iterator(other.row_types.begin()),
                     std::make_move_iterator(other.row_types.end()));
    config_types.insert(config_types.end(), std::make_move_iterator(other.config_types.begin()),
                        std::make_move_iterator(other.config_types.end()));
    repositories.insert(repositories.end(), std::make_move_iterator(other.repositories.begin()),
                        std::make_move_iterator(other.repositories.end()));
    controllers.insert(controllers.end(), std::make_move_iterator(other.controllers.begin()),
                       std::make_move_iterator(other.controllers.end()));
    middleware.insert(middleware.end(), std::make_move_iterator(other.middleware.begin()),
                      std::make_move_iterator(other.middleware.end()));
    warnings.insert(warnings.end(), std::make_move_iterator(other.warnings.begin()),
                    std::make_move_iterator(other.warnings.end()));
}

Result<void> validateDeclarations(const Declarations& declarations, RenderOptions options) {
    const std::vector<Component>& components = declarations.components;

    if (!options.actuator) {
        for (const Component& component : components) {
            const bool uses_actuator =
                std::find(component.dependencies.begin(), component.dependencies.end(),
                          kMetricsType) != component.dependencies.end() ||
                std::find(component.interfaces.begin(), component.interfaces.end(),
                          kHealthIndicatorInterface) != component.interfaces.end();
            if (uses_actuator) {
                return Error{ErrorCode::DependencyError,
                             component.type + " (" + component.location +
                                 ") uses the actuator, so pass ACTUATOR to "
                                 "cppboot_generate_components"};
            }
        }
    }

    Providers providers;
    for (const Component& component : components) {
        if (auto added = addProvider(providers, component.type, component); !added) {
            return added;
        }
        for (const std::string& interface_type : component.interfaces) {
            if (isGroupInterface(interface_type)) {
                continue;
            }
            if (auto added = addProvider(providers, interface_type, component); !added) {
                return added;
            }
        }
    }

    // A repository's interface is provided by the class the generator writes for it.
    std::vector<Component> repository_components;
    for (const RepositoryInterface& repository : declarations.repositories) {
        repository_components.push_back(
            Component{repository.type, repository.location, {}, {std::string{kDatabaseType}}});
        for (const RepositoryFunction& function : repository.functions) {
            if (auto query = repositoryQuery(repository, function, declarations); !query) {
                return query.error();
            }
        }
    }
    for (const Component& repository : repository_components) {
        if (auto added = addProvider(providers, repository.type, repository); !added) {
            return added;
        }
    }

    std::map<std::string, const ConfigStruct*> prefixes;
    for (const ConfigStruct& settings : declarations.config_types) {
        const auto [entry, inserted] = prefixes.emplace(settings.prefix, &settings);
        if (!inserted) {
            return Error{ErrorCode::DependencyError,
                         entry->second->type + " (" + entry->second->location + ") and " +
                             settings.type + " (" + settings.location +
                             ") both read the settings under \"" + settings.prefix +
                             "\". Use one struct, or give them different prefixes"};
        }
    }

    for (const Component& component : components) {
        for (const std::string& dependency : component.dependencies) {
            if (!providers.contains(dependency) && !isFrameworkType(dependency) &&
                !isConfigType(declarations, dependency)) {
                return Error{ErrorCode::DependencyError,
                             component.type + " (" + component.location + ") needs " + dependency +
                                 ", but no component provides it"};
            }
        }
    }

    std::map<const Component*, VisitState> states;
    std::vector<const Component*> path;
    for (const Component& component : components) {
        if (auto checked = checkForCycle(component, providers, states, path); !checked) {
            return checked;
        }
    }

    if (!declarations.middleware.empty() && declarations.controllers.empty() && !options.actuator) {
        const Middleware& middleware = declarations.middleware.front();
        return Error{ErrorCode::DependencyError,
                     middleware.type + " (" + middleware.location +
                         ") is middleware, but there are no controllers for it to run around"};
    }

    std::map<std::string, const Route*> routes;
    for (const Controller& controller : declarations.controllers) {
        for (const Route& route : controller.routes) {
            const std::string key = route.method + " " + routeShape(route.path);
            const auto [entry, inserted] = routes.emplace(key, &route);
            if (!inserted) {
                return Error{ErrorCode::DependencyError,
                             "Routes at " + entry->second->location + " and " + route.location +
                                 " both handle " + route.method + " " + route.path};
            }
        }
    }
    return {};
}

std::string renderRegistration(const Declarations& declarations,
                               const std::vector<std::string>& headers, RenderOptions options) {
    const bool has_router = !declarations.controllers.empty() || options.actuator;
    const bool uses_database = usesDatabase(declarations);
    const bool uses_security = usesSecurity(declarations);

    std::string text =
        "// Generated by cppboot-codegen. Do not edit.\n"
        "#pragma once\n\n"
        "#include <cppboot/di/container.hpp>\n";
    if (uses_database) {
        text +=
            "#include <cppboot/data/database.hpp>\n#include <cppboot/data/database_module.hpp>\n";
    }
    if (uses_security) {
        text += "#include <cppboot/security/guard.hpp>\n";
    }
    if (options.actuator) {
        text += "#include <cppboot/actuator/actuator.hpp>\n";
    }
    if (has_router || !declarations.json_types.empty()) {
        text += "#include <cppboot/web/binding.hpp>\n";
    }
    if (!declarations.config_types.empty()) {
        text += "#include <cppboot/config/binding.hpp>\n";
    }
    if (hasChecks(declarations)) {
        text += "#include <cppboot/base/validation.hpp>\n";
    }
    if (usesRateLimits(declarations)) {
        text += "#include <chrono>\n#include <cppboot/web/rate_limit.hpp>\n";
    }
    if (has_router) {
        text += "#include <cppboot/web/router.hpp>\n#include <cppboot/web/server_module.hpp>\n";
    }
    if (!declarations.controllers.empty()) {
        text += "#include <cppboot/config/config.hpp>\n#include <string>\n#include <string_view>\n"
                "#include <typeinfo>\n";
    }
    if (!declarations.repositories.empty()) {
        text += "#include <cppboot/data/repository.hpp>\n#include <optional>\n#include <string>\n"
                "#include <vector>\n";
    }
    text += "#include <memory>\n#include <type_traits>\n#include <utility>\n\n";

    for (const std::string& header : headers) {
        text += "#include \"" + header + "\"\n";
    }
    text += "\n";

    for (const MappedStruct& json_type : declarations.json_types) {
        renderJsonType(json_type, text);
    }

    if (!declarations.repositories.empty()) {
        text += "namespace cppboot::generated::repositories {\n\n";
        for (const RepositoryInterface& repository : declarations.repositories) {
            renderRepository(repository, declarations, text);
        }
        text += "}  // namespace cppboot::generated::repositories\n\n";
    }

    text += "namespace cppboot::generated {\n\n";
    if (!declarations.controllers.empty()) {
        text += "/// The OpenAPI document for the routes, which /openapi.json serves when "
                "cppboot.openapi.enabled is\n/// true. The build also writes it to openapi.json "
                "next to this file.\n";
        text += "inline constexpr std::string_view kOpenApi = " +
                rawLiteral(renderOpenApi(declarations, options)) + ";\n\n";
    }
    text += "inline void registerComponents([[maybe_unused]] cppboot::di::Container& container) {\n";

    for (const ConfigStruct& settings : declarations.config_types) {
        renderConfigType(settings, declarations.config_types, text);
    }

    // Modules with no ordering constraint start in registration order, so this comes first.
    if (uses_database) {
        renderDatabase(declarations.row_types, text);
    }

    if (uses_security) {
        text += "    container.registerSingleton<cppboot::security::TokenService>("
                "[](cppboot::di::Resolver& resolver) {\n";
        text += "        return cppboot::security::TokenService::fromSettings("
                "*resolver.resolve<cppboot::config::Config>());\n    });\n";
    }

    if (options.actuator) {
        text += "    container.registerSingleton<cppboot::actuator::Metrics>();\n";
        text += "    container.registerSingleton<cppboot::actuator::Actuator>("
                "cppboot::di::Deps<cppboot::actuator::Metrics, cppboot::logging::Logger, "
                "cppboot::config::Config>{});\n";
    }

    for (const Component& component : declarations.components) {
        text += "    container.registerSingleton<" + component.type + ">(cppboot::di::Deps<";
        for (std::size_t i = 0; i < component.dependencies.size(); ++i) {
            text += i == 0 ? "" : ", ";
            text += component.dependencies[i];
        }
        text += ">{});\n";

        for (const std::string& interface_type : component.interfaces) {
            text += isGroupInterface(interface_type) ? "    container.bindAll<"
                                                     : "    container.bind<";
            text += interface_type + ", " + component.type + ">();\n";
        }
    }

    for (const RepositoryInterface& repository : declarations.repositories) {
        const std::string implementation =
            "cppboot::generated::repositories::" + implementationName(repository);
        text += "    container.registerSingleton<" + implementation +
                ">(cppboot::di::Deps<cppboot::data::Database>{});\n";
        text += "    container.bind<" + repository.type + ", " + implementation + ">();\n";
    }

    if (has_router) {
        renderRouter(declarations, uses_database, uses_security, options.actuator, text);
    }

    text += "}\n\n}  // namespace cppboot::generated\n";
    return text;
}

}  // namespace cppboot::codegen
