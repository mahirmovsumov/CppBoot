#include "cppboot/codegen/scan.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>

#include "cppboot/base/json.hpp"

namespace cppboot::codegen {

namespace {

Json makeObject(std::initializer_list<std::pair<std::string_view, Json>> members) {
    Json object = Json::object();
    for (const auto& [key, value] : members) {
        object[key] = value;
    }
    return object;
}

Json checksToJson(const std::vector<Check>& checks) {
    Json result = Json::array();
    for (const Check& check : checks) {
        result.push(makeObject({{"kind", static_cast<int>(check.kind)},
                                {"shape", static_cast<int>(check.shape)},
                                {"min", check.min},
                                {"max", check.max}}));
    }
    return result;
}

std::vector<Check> checksFromJson(const Json& value) {
    std::vector<Check> checks;
    for (const Json& check : value.items()) {
        checks.push_back(Check{static_cast<CheckKind>(check.at("kind").get<int>()),
                               static_cast<ValueShape>(check.at("shape").get<int>()),
                               check.at("min").asString(), check.at("max").asString()});
    }
    return checks;
}

Json fieldsToJson(const std::vector<StructField>& fields) {
    Json result = Json::array();
    for (const StructField& field : fields) {
        result.push(makeObject({{"name", field.name},
                                {"is_optional", field.is_optional},
                                {"checks", checksToJson(field.checks)},
                                {"kind", static_cast<int>(field.kind)},
                                {"schema", field.schema}}));
    }
    return result;
}

Json repositoriesToJson(const std::vector<RepositoryInterface>& repositories) {
    Json result = Json::array();
    for (const RepositoryInterface& repository : repositories) {
        Json functions = Json::array();
        for (const RepositoryFunction& function : repository.functions) {
            Json parameters = Json::array();
            for (const RepositoryParameter& parameter : function.parameters) {
                parameters.push(makeObject({{"name", parameter.name},
                                            {"type", parameter.type},
                                            {"kind", static_cast<int>(parameter.kind)}}));
            }
            functions.push(makeObject({{"name", function.name},
                                       {"location", function.location},
                                       {"return_type", function.return_type},
                                       {"returns", static_cast<int>(function.returns)},
                                       {"element_type", function.element_type},
                                       {"element_kind", static_cast<int>(function.element_kind)},
                                       {"parameters", std::move(parameters)},
                                       {"is_const", function.is_const},
                                       {"query", function.query}}));
        }
        result.push(makeObject({{"type", repository.type},
                                {"location", repository.location},
                                {"table", repository.table},
                                {"row_type", repository.row_type},
                                {"functions", std::move(functions)}}));
    }
    return result;
}

std::vector<RepositoryInterface> repositoriesFromJson(const Json& value) {
    std::vector<RepositoryInterface> repositories;
    for (const Json& repository : value.items()) {
        RepositoryInterface read{repository.at("type").asString(),
                                 repository.at("location").asString(),
                                 repository.at("table").asString(),
                                 repository.at("row_type").asString(),
                                 {}};
        for (const Json& function : repository.at("functions").items()) {
            RepositoryFunction entry;
            entry.name = function.at("name").asString();
            entry.location = function.at("location").asString();
            entry.return_type = function.at("return_type").asString();
            entry.returns = static_cast<RepositoryReturn>(function.at("returns").get<int>());
            entry.element_type = function.at("element_type").asString();
            entry.element_kind = static_cast<ValueKind>(function.at("element_kind").get<int>());
            entry.is_const = function.at("is_const").asBool();
            entry.query = function.at("query").asString();
            for (const Json& parameter : function.at("parameters").items()) {
                entry.parameters.push_back(RepositoryParameter{
                    parameter.at("name").asString(), parameter.at("type").asString(),
                    static_cast<ValueKind>(parameter.at("kind").get<int>())});
            }
            read.functions.push_back(std::move(entry));
        }
        repositories.push_back(std::move(read));
    }
    return repositories;
}

Json configTypesToJson(const std::vector<ConfigStruct>& config_types) {
    Json result = Json::array();
    for (const ConfigStruct& settings : config_types) {
        Json fields = Json::array();
        for (const ConfigField& field : settings.fields) {
            fields.push(makeObject({{"path", field.path},
                                    {"required", field.required},
                                    {"checks", checksToJson(field.checks)}}));
        }
        result.push(makeObject({{"type", settings.type},
                                {"location", settings.location},
                                {"prefix", settings.prefix},
                                {"fields", std::move(fields)}}));
    }
    return result;
}

std::vector<ConfigStruct> configTypesFromJson(const Json& value) {
    std::vector<ConfigStruct> config_types;
    for (const Json& settings : value.items()) {
        ConfigStruct read{settings.at("type").asString(), settings.at("location").asString(),
                          settings.at("prefix").asString(), {}};
        for (const Json& field : settings.at("fields").items()) {
            read.fields.push_back(ConfigField{field.at("path").asString(),
                                              field.at("required").asBool(),
                                              checksFromJson(field.at("checks"))});
        }
        config_types.push_back(std::move(read));
    }
    return config_types;
}

Json structsToJson(const std::vector<MappedStruct>& structs) {
    Json result = Json::array();
    for (const MappedStruct& mapped : structs) {
        result.push(makeObject({{"type", mapped.type},
                                {"location", mapped.location},
                                {"fields", fieldsToJson(mapped.fields)}}));
    }
    return result;
}

Json routeToJson(const Route& route) {
    Json parameters = Json::array();
    for (const RouteParameter& parameter : route.parameters) {
        parameters.push(makeObject({{"source", static_cast<int>(parameter.source)},
                                    {"name", parameter.name},
                                    {"type", parameter.type},
                                    {"checks", checksToJson(parameter.checks)},
                                    {"schema", parameter.schema}}));
    }
    Json result = makeObject({{"method", route.method},
                              {"path", route.path},
                              {"function", route.function},
                              {"location", route.location},
                              {"parameters", std::move(parameters)},
                              {"return_kind", static_cast<int>(route.return_kind)},
                              {"is_public", route.is_public},
                              {"roles", route.roles},
                              {"response_schema", route.response_schema},
                              {"status_varies", route.status_varies}});
    if (route.rate_limit) {
        result["rate_limit"] = makeObject({{"requests", route.rate_limit->requests},
                                           {"window_seconds", route.rate_limit->window_seconds}});
    }
    return result;
}

std::vector<StructField> fieldsFromJson(const Json& value) {
    std::vector<StructField> fields;
    for (const Json& field : value.items()) {
        fields.push_back(StructField{field.at("name").asString(), field.at("is_optional").asBool(),
                                     checksFromJson(field.at("checks")),
                                     static_cast<ValueKind>(field.at("kind").get<int>()),
                                     field.at("schema").asString()});
    }
    return fields;
}

std::vector<MappedStruct> structsFromJson(const Json& value) {
    std::vector<MappedStruct> structs;
    for (const Json& mapped : value.items()) {
        structs.push_back(MappedStruct{mapped.at("type").asString(), mapped.at("location").asString(),
                                       fieldsFromJson(mapped.at("fields"))});
    }
    return structs;
}

Route routeFromJson(const Json& value) {
    Route route;
    route.method = value.at("method").asString();
    route.path = value.at("path").asString();
    route.function = value.at("function").asString();
    route.location = value.at("location").asString();
    for (const Json& parameter : value.at("parameters").items()) {
        route.parameters.push_back(RouteParameter{
            static_cast<ParameterSource>(parameter.at("source").get<int>()),
            parameter.at("name").asString(), parameter.at("type").asString(),
            checksFromJson(parameter.at("checks")), parameter.at("schema").asString()});
    }
    route.return_kind = static_cast<ReturnKind>(value.at("return_kind").get<int>());
    route.is_public = value.at("is_public").asBool();
    route.roles = value.at("roles").get<std::vector<std::string>>();
    route.response_schema = value.at("response_schema").asString();
    route.status_varies = value.at("status_varies").asBool();
    if (const Json* const limit = value.find("rate_limit")) {
        route.rate_limit = RouteRateLimit{limit->at("requests").get<std::uint32_t>(),
                                          limit->at("window_seconds").get<std::uint32_t>()};
    }
    return route;
}

/// Arguments whose value is the next argument, unless it's joined on.
constexpr std::array<std::string_view, 9> kWithValue = {
    "-I", "-D", "-U", "-isystem", "-iquote", "-idirafter", "-include", "-imacros", "-isysroot",
};

std::string escapeForDepfile(const std::filesystem::path& path) {
    std::string escaped;
    for (const char c : path.generic_string()) {
        if (c == ' ' || c == '#' || c == '\\') {
            escaped += '\\';
        } else if (c == '$') {
            escaped += '$';
        }
        escaped += c;
    }
    return escaped;
}

}  // namespace

std::string toJson(const ScannedHeader& scanned) {
    const Declarations& declarations = scanned.declarations;

    Json components = Json::array();
    for (const Component& component : declarations.components) {
        components.push(makeObject({{"type", component.type},
                                    {"location", component.location},
                                    {"interfaces", component.interfaces},
                                    {"dependencies", component.dependencies}}));
    }

    Json controllers = Json::array();
    for (const Controller& controller : declarations.controllers) {
        Json routes = Json::array();
        for (const Route& route : controller.routes) {
            routes.push(routeToJson(route));
        }
        controllers.push(makeObject({{"type", controller.type}, {"routes", std::move(routes)}}));
    }

    Json middleware = Json::array();
    for (const Middleware& entry : declarations.middleware) {
        middleware.push(
            makeObject({{"type", entry.type}, {"location", entry.location}, {"order", entry.order}}));
    }

    const Json document = makeObject({{"header", scanned.header},
                                      {"components", std::move(components)},
                                      {"json_types", structsToJson(declarations.json_types)},
                                      {"row_types", structsToJson(declarations.row_types)},
                                      {"config_types", configTypesToJson(declarations.config_types)},
                                      {"repositories", repositoriesToJson(declarations.repositories)},
                                      {"controllers", std::move(controllers)},
                                      {"middleware", std::move(middleware)},
                                      {"warnings", declarations.warnings}});
    return document.dump();
}

Result<ScannedHeader> scannedHeaderFromJson(std::string_view text) {
    try {
        const Json document = Json::parse(text);
        ScannedHeader scanned;
        scanned.header = document.at("header").asString();

        Declarations& declarations = scanned.declarations;
        for (const Json& component : document.at("components").items()) {
            declarations.components.push_back(Component{
                component.at("type").asString(), component.at("location").asString(),
                component.at("interfaces").get<std::vector<std::string>>(),
                component.at("dependencies").get<std::vector<std::string>>()});
        }
        declarations.json_types = structsFromJson(document.at("json_types"));
        declarations.row_types = structsFromJson(document.at("row_types"));
        declarations.config_types = configTypesFromJson(document.at("config_types"));
        declarations.repositories = repositoriesFromJson(document.at("repositories"));
        for (const Json& controller : document.at("controllers").items()) {
            Controller read{controller.at("type").asString(), {}};
            for (const Json& route : controller.at("routes").items()) {
                read.routes.push_back(routeFromJson(route));
            }
            declarations.controllers.push_back(std::move(read));
        }
        for (const Json& entry : document.at("middleware").items()) {
            declarations.middleware.push_back(Middleware{entry.at("type").asString(),
                                                         entry.at("location").asString(),
                                                         entry.at("order").get<int>()});
        }
        declarations.warnings = document.at("warnings").get<std::vector<std::string>>();
        return scanned;
    } catch (const JsonError& error) {
        // These are only written by this tool, so this means a damaged or outdated file.
        return Error{ErrorCode::ParseError, std::string{"A saved scan can't be read, so delete the "
                                                        "build folder and build again: "} +
                                                error.what()};
    }
}

std::vector<std::string> usableCompilerArguments(const std::vector<std::string>& arguments) {
    std::vector<std::string> usable;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];

        bool kept = false;
        for (const std::string_view option : kWithValue) {
            if (argument == option) {
                if (i + 1 < arguments.size()) {
                    usable.push_back(argument);
                    usable.push_back(arguments[++i]);
                }
                kept = true;
                break;
            }
            if (argument.starts_with(option)) {
                usable.push_back(argument);
                kept = true;
                break;
            }
        }
        if (!kept && argument.starts_with("-std=")) {
            usable.push_back(argument);
        }
    }
    return usable;
}

std::string makeDepfile(const std::filesystem::path& target,
                        const std::vector<std::filesystem::path>& dependencies) {
    std::string text = escapeForDepfile(target) + ":";
    for (const std::filesystem::path& dependency : dependencies) {
        text += " \\\n  " + escapeForDepfile(dependency);
    }
    return text + "\n";
}

}  // namespace cppboot::codegen
