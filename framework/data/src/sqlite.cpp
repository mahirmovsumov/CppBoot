#include <sqlite3.h>

#include <charconv>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <variant>

#include "cppboot/data/driver.hpp"

namespace cppboot::data {

namespace {

constexpr int kBusyTimeoutMilliseconds = 5000;

class SqliteConnection : public Connection {
public:
    explicit SqliteConnection(const std::string& path) {
        const int status = sqlite3_open_v2(path.c_str(), &database_,
                                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (status != SQLITE_OK) {
            const std::string message =
                database_ != nullptr ? sqlite3_errmsg(database_) : sqlite3_errstr(status);
            sqlite3_close(database_);
            database_ = nullptr;
            throw DatabaseError(DatabaseError::Kind::Connection,
                                "Cannot open the SQLite database: " + message);
        }

        sqlite3_busy_timeout(database_, kBusyTimeoutMilliseconds);
        // SQLite ignores foreign keys unless asked, and WAL lets reads continue during a write.
        executeScript("PRAGMA foreign_keys = ON");
        if (path != ":memory:") {
            executeScript("PRAGMA journal_mode = WAL");
        }
    }

    ~SqliteConnection() override { sqlite3_close(database_); }

    SqliteConnection(const SqliteConnection&) = delete;
    SqliteConnection& operator=(const SqliteConnection&) = delete;

    QueryResult execute(std::string_view sql, const std::vector<Param>& params) override {
        sqlite3_stmt* raw_statement = nullptr;
        const char* tail = nullptr;
        if (sqlite3_prepare_v2(database_, sql.data(), static_cast<int>(sql.size()), &raw_statement,
                               &tail) != SQLITE_OK) {
            throwLastError();
        }
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement{raw_statement,
                                                                                   sqlite3_finalize};

        const std::string_view rest = sql.substr(static_cast<std::size_t>(tail - sql.data()));
        if (rest.find_first_not_of(" \t\r\n;") != std::string_view::npos) {
            throw DatabaseError(DatabaseError::Kind::Query,
                                "A query can only contain one statement");
        }
        if (!statement) {
            return {};
        }

        bind(statement.get(), params);

        QueryResult result;
        const int column_count = sqlite3_column_count(statement.get());
        for (int i = 0; i < column_count; ++i) {
            result.columns.emplace_back(sqlite3_column_name(statement.get(), i));
        }

        while (true) {
            const int status = sqlite3_step(statement.get());
            if (status == SQLITE_DONE) {
                break;
            }
            if (status != SQLITE_ROW) {
                throwLastError();
            }

            auto& row = result.rows.emplace_back();
            row.reserve(static_cast<std::size_t>(column_count));
            for (int i = 0; i < column_count; ++i) {
                if (sqlite3_column_type(statement.get(), i) == SQLITE_NULL) {
                    row.emplace_back(std::nullopt);
                } else {
                    const auto* text = reinterpret_cast<const char*>(
                        sqlite3_column_text(statement.get(), i));
                    row.emplace_back(std::string{
                        text, static_cast<std::size_t>(sqlite3_column_bytes(statement.get(), i))});
                }
            }
        }

        if (sqlite3_stmt_readonly(statement.get()) == 0) {
            result.affected = sqlite3_changes(database_);
        }
        return result;
    }

    void executeScript(const std::string& sql) override {
        if (sqlite3_exec(database_, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
            throwLastError();
        }
    }

    bool isHealthy() const noexcept override {
        // Autocommit is off during a transaction, so this finds one that was left open.
        return database_ != nullptr && sqlite3_get_autocommit(database_) != 0;
    }

private:
    void bind(sqlite3_stmt* statement, const std::vector<Param>& params) {
        // SQLite numbers placeholders in the order they first appear, whatever their names, so
        // $2 before $1 needs each name read to find its value.
        const int count = sqlite3_bind_parameter_count(statement);
        for (int i = 1; i <= count; ++i) {
            const char* const name = sqlite3_bind_parameter_name(statement, i);
            const std::string_view text = name == nullptr ? std::string_view{} : name;

            std::size_t number = 0;
            const char* const end = text.data() + text.size();
            if (text.size() < 2 || text.front() != '$' ||
                std::from_chars(text.data() + 1, end, number).ec != std::errc{} || number == 0) {
                throw DatabaseError(DatabaseError::Kind::Query,
                                    "Placeholders must look like $1, $2 and so on");
            }
            if (number > params.size()) {
                throw DatabaseError(DatabaseError::Kind::Query,
                                    "The query uses " + std::string{text} + ", but has only " +
                                        std::to_string(params.size()) + " parameters");
            }

            std::visit(
                [&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, std::monostate>) {
                        sqlite3_bind_null(statement, i);
                    } else if constexpr (std::is_same_v<T, bool>) {
                        sqlite3_bind_int(statement, i, value ? 1 : 0);
                    } else if constexpr (std::is_same_v<T, std::int64_t>) {
                        sqlite3_bind_int64(statement, i, value);
                    } else if constexpr (std::is_same_v<T, double>) {
                        sqlite3_bind_double(statement, i, value);
                    } else {
                        sqlite3_bind_text(statement, i, value.data(),
                                          static_cast<int>(value.size()), SQLITE_TRANSIENT);
                    }
                },
                params[number - 1]);
        }
    }

    [[noreturn]] void throwLastError() const {
        const int code = sqlite3_extended_errcode(database_);
        const std::string message = sqlite3_errmsg(database_);

        DatabaseError::Kind kind = DatabaseError::Kind::Query;
        if (code == SQLITE_CONSTRAINT_UNIQUE || code == SQLITE_CONSTRAINT_PRIMARYKEY) {
            kind = DatabaseError::Kind::UniqueViolation;
        } else if ((code & 0xFF) == SQLITE_CONSTRAINT) {
            kind = DatabaseError::Kind::ConstraintViolation;
        } else if ((code & 0xFF) == SQLITE_BUSY || (code & 0xFF) == SQLITE_LOCKED) {
            kind = DatabaseError::Kind::Timeout;
        }
        throw DatabaseError(kind, "SQLite: " + message);
    }

    sqlite3* database_{nullptr};
};

}  // namespace

std::unique_ptr<Connection> openSqlite(const std::string& path) {
    return std::make_unique<SqliteConnection>(path);
}

}  // namespace cppboot::data
