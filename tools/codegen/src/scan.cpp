#include "cppboot/codegen/scan.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <exception>
#include <utility>

namespace cppboot::codegen {

namespace {

using nlohmann::json;

json fieldsToJson(const std::vector<StructField>& fields) {
    json result = json::array();
    for (const StructField& field : fields) {
        result.push_back({{"name", field.name}, {"is_optional", field.is_optional}});
    }
    return result;
}

json structsToJson(const std::vector<MappedStruct>& structs) {
    json result = json::array();
    for (const MappedStruct& mapped : structs) {
        result.push_back(
            {{"type", mapped.type}, {"location", mapped.location}, {"fields", fieldsToJson(mapped.fields)}});
    }
    return result;
}

json routeToJson(const Route& route) {
    json parameters = json::array();
    for (const RouteParameter& parameter : route.parameters) {
        parameters.push_back({{"source", static_cast<int>(parameter.source)},
                              {"name", parameter.name},
                              {"type", parameter.type}});
    }
    json result = {{"method", route.method},
                   {"path", route.path},
                   {"function", route.function},
                   {"location", route.location},
                   {"parameters", std::move(parameters)},
                   {"return_kind", static_cast<int>(route.return_kind)},
                   {"is_public", route.is_public},
                   {"roles", route.roles}};
    if (route.rate_limit) {
        result["rate_limit"] = {{"requests", route.rate_limit->requests},
                                {"window_seconds", route.rate_limit->window_seconds}};
    }
    return result;
}

std::vector<StructField> fieldsFromJson(const json& value) {
    std::vector<StructField> fields;
    for (const json& field : value) {
        fields.push_back(StructField{field.at("name"), field.at("is_optional")});
    }
    return fields;
}

std::vector<MappedStruct> structsFromJson(const json& value) {
    std::vector<MappedStruct> structs;
    for (const json& mapped : value) {
        structs.push_back(
            MappedStruct{mapped.at("type"), mapped.at("location"), fieldsFromJson(mapped.at("fields"))});
    }
    return structs;
}

Route routeFromJson(const json& value) {
    Route route;
    route.method = value.at("method");
    route.path = value.at("path");
    route.function = value.at("function");
    route.location = value.at("location");
    for (const json& parameter : value.at("parameters")) {
        route.parameters.push_back(RouteParameter{
            static_cast<ParameterSource>(parameter.at("source").get<int>()), parameter.at("name"),
            parameter.at("type")});
    }
    route.return_kind = static_cast<ReturnKind>(value.at("return_kind").get<int>());
    route.is_public = value.at("is_public");
    route.roles = value.at("roles").get<std::vector<std::string>>();
    if (value.contains("rate_limit")) {
        route.rate_limit = RouteRateLimit{value.at("rate_limit").at("requests"),
                                          value.at("rate_limit").at("window_seconds")};
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

    json components = json::array();
    for (const Component& component : declarations.components) {
        components.push_back({{"type", component.type},
                              {"location", component.location},
                              {"interfaces", component.interfaces},
                              {"dependencies", component.dependencies}});
    }

    json controllers = json::array();
    for (const Controller& controller : declarations.controllers) {
        json routes = json::array();
        for (const Route& route : controller.routes) {
            routes.push_back(routeToJson(route));
        }
        controllers.push_back({{"type", controller.type}, {"routes", std::move(routes)}});
    }

    json middleware = json::array();
    for (const Middleware& entry : declarations.middleware) {
        middleware.push_back({{"type", entry.type}, {"location", entry.location}, {"order", entry.order}});
    }

    const json document = {{"header", scanned.header},
                           {"components", std::move(components)},
                           {"json_types", structsToJson(declarations.json_types)},
                           {"row_types", structsToJson(declarations.row_types)},
                           {"controllers", std::move(controllers)},
                           {"middleware", std::move(middleware)},
                           {"warnings", declarations.warnings}};
    return document.dump(2);
}

Result<ScannedHeader> scannedHeaderFromJson(std::string_view text) {
    try {
        const json document = json::parse(text);
        ScannedHeader scanned;
        scanned.header = document.at("header");

        Declarations& declarations = scanned.declarations;
        for (const json& component : document.at("components")) {
            declarations.components.push_back(Component{
                component.at("type"), component.at("location"),
                component.at("interfaces").get<std::vector<std::string>>(),
                component.at("dependencies").get<std::vector<std::string>>()});
        }
        declarations.json_types = structsFromJson(document.at("json_types"));
        declarations.row_types = structsFromJson(document.at("row_types"));
        for (const json& controller : document.at("controllers")) {
            Controller read{controller.at("type"), {}};
            for (const json& route : controller.at("routes")) {
                read.routes.push_back(routeFromJson(route));
            }
            declarations.controllers.push_back(std::move(read));
        }
        for (const json& entry : document.at("middleware")) {
            declarations.middleware.push_back(
                Middleware{entry.at("type"), entry.at("location"), entry.at("order")});
        }
        declarations.warnings = document.at("warnings").get<std::vector<std::string>>();
        return scanned;
    } catch (const std::exception& exception) {
        // These are only written by this tool, so this means a damaged or outdated file.
        return Error{ErrorCode::ParseError, std::string{"A saved scan can't be read, so delete the "
                                                        "build folder and build again: "} +
                                                exception.what()};
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
