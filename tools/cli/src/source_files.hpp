#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "cppboot/base/result.hpp"

/// Helpers for the commands that write source files into a project.
namespace cppboot::cli {

std::string readFile(const std::filesystem::path& path);

/// Writes a file and the folders it needs.
Result<void> writeNew(const std::filesystem::path& path, const std::string& content);

std::string replaceAll(std::string text, std::string_view placeholder, std::string_view value);

/// OrderItem becomes order_item.
std::string snakeCase(std::string_view name);

/// order_item becomes ORDER_ITEM.
std::string upperCase(std::string_view text);

/// order becomes orders. Words that already end with s are left as they are.
std::string plural(const std::string& word);

/// Letters and digits, starting with a capital, such as Order.
bool isClassName(std::string_view name);

/// Fails unless the folder holds a project that cppboot create made.
Result<void> checkProject(const std::filesystem::path& project);

/// Whether the project's settings use PostgreSQL, rather than SQLite.
bool usesPostgresql(const std::filesystem::path& project);

/// Whether a migration in the folder already creates the table.
bool createsTable(const std::filesystem::path& folder, const std::string& table);

/// The path, relative to the project, of the next migration, such as
/// db/migrations/002_create_orders.sql. Fails if a migration already creates the table.
Result<std::filesystem::path> newTableMigration(const std::filesystem::path& project,
                                                const std::string& table);

}  // namespace cppboot::cli
