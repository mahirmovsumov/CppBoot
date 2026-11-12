#include <algorithm>
#include <cctype>
#include <charconv>
#include <initializer_list>
#include <map>
#include <set>
#include <system_error>

#include "cppboot/base/json.hpp"
#include "cppboot/codegen/model.hpp"

namespace cppboot::codegen {

namespace {

/// app::User becomes app.User, since OpenAPI names can't contain colons.
std::string schemaName(std::string_view type) {
    std::string name;
    for (std::size_t i = 0; i < type.size(); ++i) {
        if (type[i] == ':') {
            if (i + 1 < type.size() && type[i + 1] == ':') {
                ++i;
            }
            name += '.';
        } else {
            name += type[i];
        }
    }
    return name;
}

Json object(std::initializer_list<std::pair<std::string_view, Json>> members) {
    Json result = Json::object();
    for (const auto& [key, value] : members) {
        result[key] = value;
    }
    return result;
}

/// A check's limit, as the number it was written as.
Json limit(const std::string& text) {
    std::int64_t whole = 0;
    const char* const end = text.data() + text.size();
    if (const auto [parsed, error] = std::from_chars(text.data(), end, whole);
        error == std::errc{} && parsed == end) {
        return Json::fromInteger(whole);
    }
    double fraction = 0;
    std::from_chars(text.data(), end, fraction);
    return Json::fromDouble(fraction);
}

class DocumentBuilder {
public:
    DocumentBuilder(const Declarations& declarations, const RenderOptions& options)
        : declarations_(declarations), options_(options) {
        for (const MappedStruct& json_type : declarations.json_types) {
            json_types_.insert(json_type.type);
        }
    }

    Json build();

private:
    Json schema(std::string_view text) const;
    Json schemaWithChecks(const std::string& text, const std::vector<Check>& checks) const;
    Json operation(const Controller& controller, const Route& route, bool guarded);

    const Declarations& declarations_;
    const RenderOptions& options_;
    std::set<std::string> json_types_;
    std::set<std::string> operation_ids_;
};

Json DocumentBuilder::schema(std::string_view text) const {
    if (text.starts_with("array:")) {
        return object({{"type", "array"}, {"items", schema(text.substr(6))}});
    }
    if (text.starts_with("map:")) {
        return object({{"type", "object"}, {"additionalProperties", schema(text.substr(4))}});
    }
    if (text.starts_with("ref:")) {
        const std::string type{text.substr(4)};
        // A type that isn't converted to JSON can't be described, so it's left open.
        if (!json_types_.contains(type)) {
            return Json::object();
        }
        return object({{"$ref", "#/components/schemas/" + schemaName(type)}});
    }
    const auto slash = text.find('/');
    const std::string_view type = text.substr(0, slash);
    if (type == "integer" || type == "number" || type == "string" || type == "boolean") {
        Json result = object({{"type", type}});
        if (slash != std::string_view::npos) {
            result["format"] = text.substr(slash + 1);
        }
        return result;
    }
    return Json::object();
}

Json DocumentBuilder::schemaWithChecks(const std::string& text, const std::vector<Check>& checks) const {
    Json result = schema(text);
    for (const Check& check : checks) {
        const bool items = check.shape == ValueShape::Items;
        switch (check.kind) {
            case CheckKind::NotEmpty:
                result[items ? "minItems" : "minLength"] = 1;
                break;
            case CheckKind::Length:
                result[items ? "minItems" : "minLength"] = limit(check.min);
                result[items ? "maxItems" : "maxLength"] = limit(check.max);
                break;
            case CheckKind::Min:
                result["minimum"] = limit(check.min);
                break;
            case CheckKind::Max:
                result["maximum"] = limit(check.max);
                break;
            case CheckKind::Range:
                result["minimum"] = limit(check.min);
                result["maximum"] = limit(check.max);
                break;
        }
    }
    return result;
}

Json errorResponse(std::string_view description) {
    return object({{"description", description},
                   {"content", object({{"application/json",
                                        object({{"schema", object({{"$ref",
                                                                    "#/components/schemas/Error"}})}})}})}});
}

Json DocumentBuilder::operation(const Controller& controller, const Route& route, bool guarded) {
    const std::string tag = schemaName(controller.type);
    std::string id = tag + "." + route.function;
    for (int suffix = 2; operation_ids_.contains(id); ++suffix) {
        id = tag + "." + route.function + std::to_string(suffix);
    }
    operation_ids_.insert(id);

    Json result = object({{"operationId", id}, {"tags", std::vector<std::string>{tag}}});

    Json parameters = Json::array();
    bool reads_request = false;
    for (const RouteParameter& parameter : route.parameters) {
        if (parameter.source == ParameterSource::Path || parameter.source == ParameterSource::Query) {
            const bool path = parameter.source == ParameterSource::Path;
            parameters.push(object({{"name", parameter.name},
                                    {"in", path ? "path" : "query"},
                                    {"required", path || !parameter.type.starts_with("std::optional<")},
                                    {"schema", schemaWithChecks(parameter.schema, parameter.checks)}}));
            reads_request = true;
        } else if (parameter.source == ParameterSource::Body) {
            result["requestBody"] =
                object({{"required", true},
                        {"content", object({{"application/json",
                                             object({{"schema", schema(parameter.schema)}})}})}});
            reads_request = true;
        }
    }
    if (!parameters.empty()) {
        result["parameters"] = std::move(parameters);
    }

    Json responses = Json::object();
    switch (route.return_kind) {
        case ReturnKind::Nothing:
            responses["204"] = object({{"description", "Done, with nothing to return"}});
            break;
        case ReturnKind::Response:
            responses["default"] = object({{"description", "A response that the function builds"}});
            break;
        case ReturnKind::Value:
            responses[route.status_varies ? "2XX" : "200"] = object(
                {{"description", "The result"},
                 {"content", object({{"application/json",
                                      object({{"schema", schema(route.response_schema)}})}})}});
            break;
    }
    if (reads_request) {
        responses["400"] = errorResponse("The request doesn't fit what the route reads");
    }
    if (guarded) {
        responses["401"] = errorResponse("No valid token");
        if (!route.roles.empty()) {
            responses["403"] = errorResponse("The token's user has none of the roles");
        }
    }
    if (route.rate_limit) {
        responses["429"] = errorResponse("Too many requests from this client");
    }
    result["responses"] = std::move(responses);

    if (guarded) {
        result["security"] = std::vector<Json>{object({{"bearerAuth", Json::array()}})};
        if (!route.roles.empty()) {
            std::string roles;
            for (const std::string& role : route.roles) {
                roles += (roles.empty() ? "" : ", ") + role;
            }
            result["description"] = "Needs a token for a user with one of these roles: " + roles;
        }
    }
    return result;
}

Json DocumentBuilder::build() {
    const bool security = routesNeedTokens(declarations_);

    Json paths = Json::object();
    for (const Controller& controller : declarations_.controllers) {
        for (const Route& route : controller.routes) {
            std::string method = route.method;
            std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            paths[route.path][method] = operation(controller, route, security && !route.is_public);
        }
    }

    Json schemas = Json::object();
    for (const MappedStruct& json_type : declarations_.json_types) {
        Json properties = Json::object();
        std::vector<std::string> required;
        for (const StructField& field : json_type.fields) {
            Json property = schemaWithChecks(field.schema, field.checks);
            if (field.is_optional && property.contains("$ref")) {
                // OpenAPI 3.0 ignores anything next to a $ref, so a nullable one is wrapped.
                property = object({{"allOf", std::vector<Json>{property}}, {"nullable", true}});
            } else if (field.is_optional) {
                property["nullable"] = true;
            } else {
                required.push_back(field.name);
            }
            properties[field.name] = std::move(property);
        }
        Json definition = object({{"type", "object"}, {"properties", std::move(properties)}});
        if (!required.empty()) {
            definition["required"] = required;
        }
        schemas[schemaName(json_type.type)] = std::move(definition);
    }
    schemas["Error"] = object(
        {{"type", "object"},
         {"properties", object({{"error", object({{"type", "string"}})},
                                {"status", object({{"type", "integer"}, {"format", "int32"}})}})},
         {"required", std::vector<std::string>{"error", "status"}}});

    Json components = object({{"schemas", std::move(schemas)}});
    if (security) {
        components["securitySchemes"] = object(
            {{"bearerAuth",
              object({{"type", "http"}, {"scheme", "bearer"}, {"bearerFormat", "JWT"}})}});
    }

    return object({{"openapi", "3.0.3"},
                   {"info", object({{"title", options_.title}, {"version", options_.version}})},
                   {"paths", std::move(paths)},
                   {"components", std::move(components)}});
}

}  // namespace

std::string renderOpenApi(const Declarations& declarations, const RenderOptions& options) {
    return DocumentBuilder{declarations, options}.build().dump();
}

}  // namespace cppboot::codegen
