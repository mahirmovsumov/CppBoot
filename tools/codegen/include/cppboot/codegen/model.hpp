#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::codegen {

/// A component with one of these interfaces is added to a group rather than bound to the interface,
/// since an application has many of them.
inline constexpr std::string_view kModuleInterface = "cppboot::Module";
inline constexpr std::string_view kHealthIndicatorInterface = "cppboot::actuator::HealthIndicator";

/// Generated code registers this when the project asks for the actuator.
inline constexpr std::string_view kMetricsType = "cppboot::actuator::Metrics";

/// Types that cppboot::runApplication() provides, so components can depend on them without any
/// component declaring them.
inline constexpr std::array<std::string_view, 5> kFrameworkTypes = {
    "cppboot::config::Config",
    "cppboot::logging::Logger",
    "cppboot::data::Database",
    "cppboot::security::TokenService",
    kMetricsType,
};

/// Generated code protects every route when something depends on this, or a route uses security.
inline constexpr std::string_view kTokenServiceType = "cppboot::security::TokenService";

/// Generated code registers the database when something depends on this, or a struct is mapped.
inline constexpr std::string_view kDatabaseType = "cppboot::data::Database";

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

struct StructField {
    std::string name;
    /// A std::optional field, which may be missing from JSON.
    bool is_optional{false};
};

/// A struct marked with CPPBOOT_JSON or CPPBOOT_ROW.
struct MappedStruct {
    std::string type;
    std::string location;
    std::vector<StructField> fields;
};

enum class ParameterSource { Path, Query, Body, Request, Principal };

struct RouteParameter {
    ParameterSource source;
    std::string name;
    /// The value type to convert to, without const or a reference.
    std::string type;
};

enum class ReturnKind { Nothing, Response, Value };

/// A route marked with CPPBOOT_RATE_LIMIT.
struct RouteRateLimit {
    std::uint32_t requests{0};
    std::uint32_t window_seconds{0};
};

struct Route {
    /// An enumerator of cppboot::web::Method, such as "Get".
    std::string method;
    /// The full path, including the controller's base path.
    std::string path;
    std::string function;
    std::string location;
    std::vector<RouteParameter> parameters;
    ReturnKind return_kind{ReturnKind::Value};
    /// Marked CPPBOOT_PUBLIC. Once a project uses security, every other route needs a token.
    bool is_public{false};
    /// With any of these, a user may call this route. Empty means any signed-in user.
    std::vector<std::string> roles;
    std::optional<RouteRateLimit> rate_limit;
};

struct Controller {
    std::string type;
    std::vector<Route> routes;
};

struct Middleware {
    std::string type;
    std::string location;
    /// Lower numbers run first.
    int order{0};
};

/// Everything marked in one or more headers.
struct Declarations {
    std::vector<Component> components;
    std::vector<MappedStruct> json_types;
    std::vector<MappedStruct> row_types;
    std::vector<Controller> controllers;
    std::vector<Middleware> middleware;

    void append(Declarations other);
};

struct RenderOptions {
    /// Adds the /actuator routes, and starts a web server even with no controllers.
    bool actuator{false};
};

/// Fails if a dependency has no provider, a type has two providers, dependencies form a cycle, two
/// routes have the same method and path, there's middleware with nothing for it to run around, or
/// something uses the actuator in a project that hasn't asked for it.
Result<void> validateDeclarations(const Declarations& declarations, RenderOptions options = {});

/// Returns a header defining JSON conversions and cppboot::generated::registerComponents(), which
/// registers each component with a cppboot::di::Container. If there are controllers, it also
/// registers a router with their routes, and a module that runs the server. If anything uses the
/// database, it registers that first, so that it's ready before other modules start.
std::string renderRegistration(const Declarations& declarations,
                               const std::vector<std::string>& headers, RenderOptions options = {});

}  // namespace cppboot::codegen
