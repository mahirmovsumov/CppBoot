#include "cppboot/data/migrations.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace cppboot::data {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kCreateTable =
    "CREATE TABLE IF NOT EXISTS cppboot_migrations ("
    "version BIGINT PRIMARY KEY, "
    "name TEXT NOT NULL, "
    "checksum TEXT NOT NULL, "
    "applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)";

struct MigrationFile {
    std::string name;
    std::string sql;
    std::string checksum;
};

struct AppliedMigration {
    std::string name;
    std::string checksum;
};

/// FNV-1a, which detects a changed file. Line endings are ignored, because Git can change them
/// between machines.
std::string checksumOf(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        if (c == '\r') {
            continue;
        }
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return std::to_string(hash);
}

Result<std::map<std::int64_t, MigrationFile>> readFiles(const fs::path& directory) {
    std::error_code error;
    if (!fs::is_directory(directory, error)) {
        return Error{ErrorCode::NotFound,
                     "The migrations folder doesn't exist: " + directory.generic_string()};
    }

    std::map<std::int64_t, MigrationFile> files;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory}) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const std::string name = entry.path().filename().string();
        std::int64_t version = 0;
        const auto [number_end, number_error] =
            std::from_chars(name.data(), name.data() + name.size(), version);
        if (number_error != std::errc{} || version <= 0 || *number_end != '_' ||
            entry.path().extension() != ".sql") {
            return Error{ErrorCode::InvalidArgument,
                         "Migration files must be named like 001_create_users.sql, but found " +
                             name};
        }

        std::ifstream input{entry.path(), std::ios::binary};
        std::string sql{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        if (!input) {
            return Error{ErrorCode::IoError, "Cannot read migration " + name};
        }

        const std::string checksum = checksumOf(sql);
        const auto [existing, inserted] =
            files.try_emplace(version, MigrationFile{name, std::move(sql), checksum});
        if (!inserted) {
            return Error{ErrorCode::InvalidArgument, "Migrations " + existing->second.name +
                                                         " and " + name + " have the same number"};
        }
    }
    return files;
}

Result<void> checkHistory(const std::map<std::int64_t, MigrationFile>& files,
                          const std::map<std::int64_t, AppliedMigration>& applied) {
    for (const auto& [version, migration] : applied) {
        const auto file = files.find(version);
        if (file == files.end()) {
            return Error{ErrorCode::InvalidState,
                         "Migration " + migration.name +
                             " was applied to this database, but its file is missing"};
        }
        if (file->second.checksum != migration.checksum) {
            return Error{ErrorCode::InvalidState,
                         "Migration " + migration.name +
                             " has changed since it was applied. Add a new migration instead"};
        }
    }

    if (!applied.empty()) {
        const auto newest_applied = applied.rbegin()->first;
        for (const auto& [version, file] : files) {
            if (version < newest_applied && !applied.contains(version)) {
                return Error{ErrorCode::InvalidState,
                             "Migration " + file.name + " is numbered below one already applied. "
                                                        "Give it a higher number"};
            }
        }
    }
    return {};
}

/// Releases the migration lock on every path out, including exceptions.
class MigrationLock {
public:
    explicit MigrationLock(Connection& connection) : connection_(connection) {
        connection_.lockMigrations();
    }
    ~MigrationLock() {
        try {
            connection_.unlockMigrations();
        } catch (...) {
            // PostgreSQL releases the lock when the connection closes, and a failure here means
            // the connection is already broken, so the pool won't reuse it.
        }
    }

    MigrationLock(const MigrationLock&) = delete;
    MigrationLock& operator=(const MigrationLock&) = delete;

private:
    Connection& connection_;
};

void applyMigration(Connection& connection, std::int64_t version, const MigrationFile& file) {
    connection.executeScript("BEGIN");
    try {
        connection.executeScript(file.sql);
        connection.execute(
            "INSERT INTO cppboot_migrations (version, name, checksum) VALUES ($1, $2, $3)",
            {version, file.name, file.checksum});
        connection.executeScript("COMMIT");
    } catch (...) {
        try {
            connection.executeScript("ROLLBACK");
        } catch (...) {
            // The connection's state is unknown, and the pool checks it on return.
        }
        throw;
    }
}

}  // namespace

Result<std::size_t> runMigrations(Database& database, const fs::path& directory) {
    auto files = readFiles(directory);
    if (!files) {
        return files.error();
    }

    try {
        ConnectionPool::Lease lease = database.pool().acquire();
        Connection& connection = *lease;
        const MigrationLock lock{connection};

        connection.execute(kCreateTable, {});

        std::map<std::int64_t, AppliedMigration> applied;
        const Rows rows{connection.execute(
            "SELECT version, name, checksum FROM cppboot_migrations ORDER BY version", {})};
        for (const Row row : rows) {
            applied.try_emplace(row.get<std::int64_t>("version"),
                                AppliedMigration{row.get<std::string>("name"),
                                                 row.get<std::string>("checksum")});
        }

        if (auto valid = checkHistory(files.value(), applied); !valid) {
            return valid.error();
        }

        std::size_t count = 0;
        for (const auto& [version, file] : files.value()) {
            if (applied.contains(version)) {
                continue;
            }
            try {
                applyMigration(connection, version, file);
            } catch (const DatabaseError& error) {
                return Error{ErrorCode::IoError,
                             "Migration " + file.name + " failed: " + error.error().message()};
            }
            ++count;
        }
        return count;
    } catch (const DatabaseError& error) {
        return error.error();
    }
}

}  // namespace cppboot::data
