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
        if (isFrameworkType(dependency)) {
            continue;
        }
        if (auto checked = checkForCycle(*providers.at(dependency), providers, states, path);
            !checked) {
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
        if (c == '"' || c == '\\') {
            result += '\\';
        }
        result += c;
    }
    return result + '"';
}

void renderJsonType(const JsonType& json_type, std::string& text) {
    text += "template <>\nstruct nlohmann::adl_serializer<" + json_type.type + "> {\n";
    text += "    static_assert(std::is_default_constructible_v<" + json_type.type + ">, " +
            quoted(json_type.type + " must be default constructible for JSON conversion") + ");\n\n";

    text += "    template <typename BasicJson>\n";
    text += "    static void to_json(BasicJson& json, const " + json_type.type + "& value) {\n";
    text += "        json = BasicJson::object();\n";
    for (const JsonField& field : json_type.fields) {
        text += "        json[" + quoted(field.name) + "] = value." + field.name + ";\n";
    }
    text += "    }\n\n";

    text += "    template <typename BasicJson>\n";
    text += "    static void from_json(const BasicJson& json, " + json_type.type + "& value) {\n";
    for (const JsonField& field : json_type.fields) {
        if (field.is_optional) {
            text += "        if (json.contains(" + quoted(field.name) + ")) {\n";
            text += "            json.at(" + quoted(field.name) + ").get_to(value." + field.name +
                    ");\n        }\n";
        } else {
            text += "        json.at(" + quoted(field.name) + ").get_to(value." + field.name + ");\n";
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
    }
    return "request";
}

void renderRoute(const Route& route, std::string& text) {
    text += "            router->add(cppboot::web::Method::" + route.method + ", " +
            quoted(route.path) +
            ", [controller](const cppboot::web::Request& request) -> cppboot::web::Response {\n";

    std::string call = "controller->" + route.function + "(";
    for (std::size_t i = 0; i < route.parameters.size(); ++i) {
        call += i == 0 ? "" : ", ";
        call += renderArgument(route.parameters[i]);
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

void renderRouter(const std::vector<Controller>& controllers, std::string& text) {
    text += "    container.registerSingleton<cppboot::web::Router>([](cppboot::di::Resolver& resolver) {\n";
    text += "        auto router = std::make_shared<cppboot::web::Router>();\n";
    for (const Controller& controller : controllers) {
        text += "        {\n";
        text += "            auto controller = resolver.resolve<" + controller.type + ">();\n";
        for (const Route& route : controller.routes) {
            renderRoute(route, text);
        }
        text += "        }\n";
    }
    text += "        return router;\n    });\n";
    text += "    container.registerSingleton<cppboot::web::ServerModule>("
            "cppboot::di::Deps<cppboot::web::Router>{});\n";
    text += "    container.bindAll<cppboot::Module, cppboot::web::ServerModule>();\n";
}

}  // namespace

void Declarations::append(Declarations other) {
    components.insert(components.end(), std::make_move_iterator(other.components.begin()),
                      std::make_move_iterator(other.components.end()));
    json_types.insert(json_types.end(), std::make_move_iterator(other.json_types.begin()),
                      std::make_move_iterator(other.json_types.end()));
    controllers.insert(controllers.end(), std::make_move_iterator(other.controllers.begin()),
                       std::make_move_iterator(other.controllers.end()));
}

Result<void> validateDeclarations(const Declarations& declarations) {
    const std::vector<Component>& components = declarations.components;

    Providers providers;
    for (const Component& component : components) {
        if (auto added = addProvider(providers, component.type, component); !added) {
            return added;
        }
        for (const std::string& interface_type : component.interfaces) {
            if (interface_type == kModuleInterface) {
                continue;
            }
            if (auto added = addProvider(providers, interface_type, component); !added) {
                return added;
            }
        }
    }

    for (const Component& component : components) {
        for (const std::string& dependency : component.dependencies) {
            if (!providers.contains(dependency) && !isFrameworkType(dependency)) {
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
                               const std::vector<std::string>& headers) {
    const bool has_controllers = !declarations.controllers.empty();

    std::string text =
        "// Generated by cppboot-codegen. Do not edit.\n"
        "#pragma once\n\n"
        "#include <cppboot/di/container.hpp>\n";
    if (has_controllers || !declarations.json_types.empty()) {
        text += "#include <cppboot/web/binding.hpp>\n";
    }
    if (has_controllers) {
        text += "#include <cppboot/web/router.hpp>\n#include <cppboot/web/server_module.hpp>\n";
    }
    text += "#include <memory>\n#include <type_traits>\n\n";

    for (const std::string& header : headers) {
        text += "#include \"" + header + "\"\n";
    }
    text += "\n";

    for (const JsonType& json_type : declarations.json_types) {
        renderJsonType(json_type, text);
    }

    text +=
        "namespace cppboot::generated {\n\n"
        "inline void registerComponents([[maybe_unused]] cppboot::di::Container& container) {\n";

    for (const Component& component : declarations.components) {
        text += "    container.registerSingleton<" + component.type + ">(cppboot::di::Deps<";
        for (std::size_t i = 0; i < component.dependencies.size(); ++i) {
            text += i == 0 ? "" : ", ";
            text += component.dependencies[i];
        }
        text += ">{});\n";

        for (const std::string& interface_type : component.interfaces) {
            text += interface_type == kModuleInterface ? "    container.bindAll<"
                                                       : "    container.bind<";
            text += interface_type + ", " + component.type + ">();\n";
        }
    }

    if (has_controllers) {
        renderRouter(declarations.controllers, text);
    }

    text += "}\n\n}  // namespace cppboot::generated\n";
    return text;
}

}  // namespace cppboot::codegen
