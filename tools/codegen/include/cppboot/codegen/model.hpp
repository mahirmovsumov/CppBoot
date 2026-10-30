#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::codegen {

/// A component with this interface is added to a group rather than bound to the interface, since
/// an application has many modules.
inline constexpr std::string_view kModuleInterface = "cppboot::Module";

/// Types that cppboot::runApplication() provides, so components can depend on them without any
/// component declaring them.
inline constexpr std::array<std::string_view, 2> kFrameworkTypes = {
    "cppboot::config::Config",
    "cppboot::logging::Logger",
};

/// A class marked for registration. Type names are fully qualified.
struct Component {
    std::string type;
    /// Where the class is declared, as "file:line", for error messages.
    std::string location;
    /// The abstract base classes that other components can depend on instead of this type.
    std::vector<std::string> interfaces;
    /// The component types its constructor needs, in parameter order.
    std::vector<std::string> dependencies;
};

struct JsonField {
    std::string name;
    /// A std::optional field, which may be missing from JSON.
    bool is_optional{false};
};

/// A struct marked for JSON conversion.
struct JsonType {
    std::string type;
    std::string location;
    std::vector<JsonField> fields;
};

enum class ParameterSource { Path, Query, Body, Request };

struct RouteParameter {
    ParameterSource source;
    std::string name;
    /// The value type to convert to, without const or a reference.
    std::string type;
};

enum class ReturnKind { Nothing, Response, Value };

struct Route {
    /// An enumerator of cppboot::web::Method, such as "Get".
    std::string method;
    /// The full path, including the controller's base path.
    std::string path;
    std::string function;
    std::string location;
    std::vector<RouteParameter> parameters;
    ReturnKind return_kind{ReturnKind::Value};
};

struct Controller {
    std::string type;
    std::vector<Route> routes;
};

/// Everything marked in one or more headers.
struct Declarations {
    std::vector<Component> components;
    std::vector<JsonType> json_types;
    std::vector<Controller> controllers;

    void append(Declarations other);
};

/// Fails if a dependency has no provider, a type has two providers, dependencies form a cycle, or
/// two routes have the same method and path.
Result<void> validateDeclarations(const Declarations& declarations);

/// Returns a header defining JSON conversions and cppboot::generated::registerComponents(), which
/// registers each component with a cppboot::di::Container. If there are controllers, it also
/// registers a router with their routes, and a module that runs the server.
std::string renderRegistration(const Declarations& declarations,
                               const std::vector<std::string>& headers);

}  // namespace cppboot::codegen
