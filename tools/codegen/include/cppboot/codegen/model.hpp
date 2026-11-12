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

enum class CheckKind { NotEmpty, Length, Min, Max, Range };

/// What a checked value is, which decides how its checks read in messages.
enum class ValueShape { Number, Text, Items };

/// A check from CPPBOOT_NOT_EMPTY, CPPBOOT_LENGTH, CPPBOOT_MIN, CPPBOOT_MAX or CPPBOOT_RANGE.
struct Check {
    CheckKind kind{CheckKind::NotEmpty};
    ValueShape shape{ValueShape::Text};
    /// The limits as written in the source, such as "1" and "100". Unused ones are empty.
    std::string min;
    std::string max;
};

/// A C++ expression that's true when a value passes a check.
std::string checkExpression(const Check& check, std::string_view value);

/// What a value has to be to pass a check, such as "must be from 1 to 100".
std::string checkMessage(const Check& check);

/// What kind of value a field or parameter holds, as far as SQL is concerned.
enum class ValueKind { Number, Text, Bool, Page, Other };

struct StructField {
    std::string name;
    /// A std::optional field, which may be missing from JSON.
    bool is_optional{false};
    std::vector<Check> checks{};
    /// What it holds, without the std::optional.
    ValueKind kind{ValueKind::Other};
    /// Its JSON type, for OpenAPI. See Schema.
    std::string schema{};
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
    /// For path and query parameters.
    std::vector<Check> checks{};
    /// Its JSON type, for OpenAPI. See Schema.
    std::string schema{};
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
    /// The JSON type it returns, for OpenAPI, when it returns a value. See Schema.
    std::string response_schema{};
    /// It returns a WithStatus, so its status is only known when it runs.
    bool status_varies{false};
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

struct ConfigField {
    /// Where the field is from its struct, which is also its key under the prefix. A field of a
    /// nested struct has a path such as "retry.attempts".
    std::string path;
    /// Marked CPPBOOT_REQUIRED, so startup stops if its setting isn't set.
    bool required{false};
    std::vector<Check> checks{};
};

/// A struct marked with CPPBOOT_CONFIG, which is filled from settings and is also a component.
struct ConfigStruct {
    std::string type;
    std::string location;
    /// The settings it reads start with this and a dot.
    std::string prefix;
    std::vector<ConfigField> fields;
};

struct RepositoryParameter {
    std::string name;
    /// As the override has to spell it, such as "const std::basic_string<char> &".
    std::string type;
    ValueKind kind{ValueKind::Other};
};

enum class RepositoryReturn { Void, List, Optional, Integer, Bool, Other };

/// A pure virtual function of a repository, which the generator writes.
struct RepositoryFunction {
    std::string name;
    std::string location;
    /// As the override has to spell it.
    std::string return_type;
    RepositoryReturn returns{RepositoryReturn::Other};
    /// For a std::vector or std::optional, the type inside it.
    std::string element_type;
    ValueKind element_kind{ValueKind::Other};
    std::vector<RepositoryParameter> parameters;
    bool is_const{false};
    /// From CPPBOOT_QUERY. Empty means the query comes from the function's name.
    std::string query;
};

/// An interface marked with CPPBOOT_REPOSITORY, which derives from cppboot::data::Repository<T>.
struct RepositoryInterface {
    std::string type;
    std::string location;
    std::string table;
    /// The CPPBOOT_ROW struct that its rows are.
    std::string row_type;
    std::vector<RepositoryFunction> functions;
};

/// Everything marked in one or more headers.
struct Declarations {
    std::vector<Component> components;
    std::vector<MappedStruct> json_types;
    std::vector<MappedStruct> row_types;
    std::vector<ConfigStruct> config_types;
    std::vector<RepositoryInterface> repositories;
    std::vector<Controller> controllers;
    std::vector<Middleware> middleware;
    /// Risky patterns that don't stop the build, as "file:line: warning: message".
    std::vector<std::string> warnings;

    void append(Declarations other);
};

struct RenderOptions {
    /// Adds the /actuator routes, and starts a web server even with no controllers.
    bool actuator{false};
    /// For the OpenAPI document.
    std::string title{"API"};
    std::string version{"0.0.0"};
};

/// The OpenAPI 3.0 document that describes the routes, from the same declarations as their code.
/// JSON types are written in the model as text, so that they can nest and be saved in scans:
/// integer/int32, integer/int64, number/double, number/float, string, boolean, array:<type>,
/// map:<type>, ref:<qualified C++ type>, or any. A std::optional is written as the type inside.
std::string renderOpenApi(const Declarations& declarations, const RenderOptions& options);

/// Whether routes need a token unless they're marked public, which is so once anything in the
/// project uses security.
bool routesNeedTokens(const Declarations& declarations);

/// How a repository function runs its SQL, which decides the code around it.
enum class QueryRun {
    /// Every row, as a std::vector.
    Rows,
    /// At most one row, as a std::optional. Finding two is an error.
    OneRow,
    /// The first row, if any.
    FirstRow,
    /// The first column of the first row, such as a COUNT, as a number or bool.
    Value,
    /// Whether there are any rows.
    Exists,
    /// A statement that changes rows, returning how many, or nothing.
    Change,
};

struct RepositoryQuery {
    std::string sql;
    /// The C++ expressions for $1, $2 and so on, in order.
    std::vector<std::string> arguments;
    QueryRun run{QueryRun::Rows};
};

/// Works out a repository function's SQL, from CPPBOOT_QUERY or from its name, the way Spring Data
/// does: findByCategoryAndPriceLessThanOrderByNameAsc, countByActiveTrue, existsByEmail,
/// deleteByCreatedBefore, findFirstByOrderByPriceDesc, findTop5By... The row struct gives the
/// columns. Fails with a message that names the function if the name or its types don't fit.
Result<RepositoryQuery> repositoryQuery(const RepositoryInterface& repository,
                                        const RepositoryFunction& function,
                                        const Declarations& declarations);

/// Fails if a dependency has no provider, a type has two providers, dependencies form a cycle, two
/// routes have the same method and path, two structs read the same settings, there's middleware
/// with nothing for it to run around, or something uses the actuator in a project that hasn't asked
/// for it.
Result<void> validateDeclarations(const Declarations& declarations, RenderOptions options = {});

/// Returns a header defining JSON conversions and cppboot::generated::registerComponents(), which
/// registers each component with a cppboot::di::Container. If there are controllers, it also
/// registers a router with their routes, and a module that runs the server. If anything uses the
/// database, it registers that first, so that it's ready before other modules start.
std::string renderRegistration(const Declarations& declarations,
                               const std::vector<std::string>& headers, RenderOptions options = {});

}  // namespace cppboot::codegen
