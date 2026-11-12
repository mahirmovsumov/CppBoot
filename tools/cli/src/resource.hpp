#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot::cli {

enum class FieldType { Text, Int, Long, Double, Bool };

/// A field from the command line, such as price:double or note:string?.
struct ResourceField {
    std::string name;
    FieldType type{FieldType::Text};
    /// Written with a ? after the type, so it can be missing.
    bool optional{false};
};

/// Reads fields written as name:type, with a ? after the type for one that can be missing. The
/// types are string, int, long, double and bool. Names are lower-case, and can't be id, which
/// every resource has, or a word that C++ or SQL reserves.
Result<std::vector<ResourceField>> parseResourceFields(const std::vector<std::string>& specs);

/// Writes everything a resource needs, which works as soon as it's built: a struct for what's
/// stored and returned, one for what clients send with checks on it, a repository, a controller
/// with GET, POST, PUT and DELETE under /api/<plural>, a migration that creates its table, and a
/// test that runs the repository against a real database. Returns the files it wrote. Fails without
/// writing anything if the project has no web server or database, a file or type already exists,
/// or a field is invalid.
Result<std::vector<std::filesystem::path>> addResource(const std::filesystem::path& project,
                                                       std::string_view name,
                                                       const std::vector<std::string>& field_specs);

}  // namespace cppboot::cli
