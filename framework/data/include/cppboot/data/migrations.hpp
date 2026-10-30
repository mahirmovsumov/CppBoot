#pragma once

#include <cstddef>
#include <filesystem>

#include "cppboot/base/result.hpp"
#include "cppboot/data/database.hpp"

namespace cppboot::data {

/// Applies new migrations from a folder of files named like 001_create_users.sql, in number order,
/// each in its own transaction, and returns how many were applied.
///
/// Fails without applying anything further if a folder or file name is invalid, a file that was
/// applied has changed or gone, or a new file is numbered below one already applied. If a
/// migration fails, it rolls back, and those before it stay applied.
Result<std::size_t> runMigrations(Database& database, const std::filesystem::path& directory);

}  // namespace cppboot::data
