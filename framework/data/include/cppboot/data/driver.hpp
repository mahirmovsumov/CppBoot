#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cppboot/base/exceptions.hpp"

namespace cppboot::data {

/// A database failure. Queries throw this, so that a failure inside a transaction rolls it back.
/// Its message can contain SQL details, so it shouldn't be sent to clients.
class DatabaseError : public Exception {
public:
    enum class Kind {
        /// The connection failed or was lost.
        Connection,
        /// No connection became free in time, or the database was busy.
        Timeout,
        /// A unique constraint or primary key rejected a value.
        UniqueViolation,
        /// Another constraint rejected a value, such as a foreign key.
        ConstraintViolation,
        /// The database rejected the statement.
        Query,
        /// A result couldn't be converted to the requested type.
        Mapping,
    };

    DatabaseError(Kind kind, const std::string& message)
        : Exception(Error{ErrorCode::IoError, message}), kind_(kind) {}

    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

/// A value bound to a placeholder such as $1. std::monostate is SQL NULL.
using Param = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

/// What a statement returned. Every value is text, or empty for SQL NULL.
struct QueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<std::optional<std::string>>> rows;
    std::int64_t affected{0};
};

/// One database connection, used by one thread at a time. Implement this to add a database.
class Connection {
public:
    virtual ~Connection() = default;

    /// Runs one statement. Placeholders are $1, $2 and so on, for every driver.
    virtual QueryResult execute(std::string_view sql, const std::vector<Param>& params) = 0;

    /// Runs any number of statements with no parameters, such as a migration file.
    virtual void executeScript(const std::string& sql) = 0;

    /// Returns false if this connection is broken, or was left inside a transaction. The pool
    /// closes it rather than give it to someone else.
    virtual bool isHealthy() const noexcept = 0;

    /// Stops other processes from running migrations until unlockMigrations(). Does nothing by
    /// default, for databases that lock while a transaction writes.
    virtual void lockMigrations() {}
    virtual void unlockMigrations() {}
};

using ConnectionFactory = std::function<std::unique_ptr<Connection>()>;

/// Returns a factory for a URL such as "postgresql://user:password@host/name" or "sqlite:app.db".
/// An unsupported URL throws DatabaseError. Messages never include the URL, which may hold a
/// password.
ConnectionFactory connectionFactoryFor(std::string_view url);

std::unique_ptr<Connection> openPostgresql(const std::string& url);
std::unique_ptr<Connection> openSqlite(const std::string& path);

}  // namespace cppboot::data
