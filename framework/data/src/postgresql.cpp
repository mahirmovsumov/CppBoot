#include <libpq-fe.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>

#include "cppboot/data/driver.hpp"

namespace cppboot::data {

namespace {

using ResultHandle = std::unique_ptr<PGresult, decltype(&PQclear)>;

// An arbitrary number that identifies CppBoot's migration lock to PostgreSQL.
constexpr std::string_view kMigrationLockKey = "7273617016530816";

std::string withoutTrailingSpace(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

class PostgresqlConnection : public Connection {
public:
    explicit PostgresqlConnection(const std::string& url) {
        // A connection timeout comes first so that one in the URL overrides it. Without one, a
        // server that doesn't respond could block a thread for minutes.
        const std::array<const char*, 3> keywords = {"connect_timeout", "dbname", nullptr};
        const std::array<const char*, 3> values = {"10", url.c_str(), nullptr};
        connection_ = PQconnectdbParams(keywords.data(), values.data(), 1);

        if (connection_ == nullptr) {
            throw DatabaseError(DatabaseError::Kind::Connection,
                                "Cannot connect to PostgreSQL: out of memory");
        }
        if (PQstatus(connection_) != CONNECTION_OK) {
            const std::string message = withoutTrailingSpace(PQerrorMessage(connection_));
            PQfinish(connection_);
            connection_ = nullptr;
            throw DatabaseError(DatabaseError::Kind::Connection,
                                "Cannot connect to PostgreSQL: " + message);
        }

        // Stops notices, such as those from CREATE TABLE IF NOT EXISTS, from reaching stderr.
        PQsetNoticeProcessor(connection_, [](void*, const char*) {}, nullptr);
    }

    ~PostgresqlConnection() override { PQfinish(connection_); }

    PostgresqlConnection(const PostgresqlConnection&) = delete;
    PostgresqlConnection& operator=(const PostgresqlConnection&) = delete;

    QueryResult execute(std::string_view sql, const std::vector<Param>& params) override {
        std::vector<std::optional<std::string>> texts;
        texts.reserve(params.size());
        for (const Param& param : params) {
            texts.push_back(toText(param));
        }

        std::vector<const char*> values;
        values.reserve(texts.size());
        for (const auto& text : texts) {
            values.push_back(text ? text->c_str() : nullptr);
        }

        const std::string statement{sql};
        const ResultHandle result{
            PQexecParams(connection_, statement.c_str(), static_cast<int>(values.size()), nullptr,
                         values.data(), nullptr, nullptr, 0),
            PQclear};
        check(result.get());
        return readResult(result.get());
    }

    void executeScript(const std::string& sql) override {
        const ResultHandle result{PQexec(connection_, sql.c_str()), PQclear};
        check(result.get());
    }

    bool isHealthy() const noexcept override {
        return PQstatus(connection_) == CONNECTION_OK &&
               PQtransactionStatus(connection_) == PQTRANS_IDLE;
    }

    void lockMigrations() override {
        executeScript("SELECT pg_advisory_lock(" + std::string{kMigrationLockKey} + ")");
    }

    void unlockMigrations() override {
        executeScript("SELECT pg_advisory_unlock(" + std::string{kMigrationLockKey} + ")");
    }

private:
    static std::optional<std::string> toText(const Param& param) {
        return std::visit(
            [](const auto& value) -> std::optional<std::string> {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    return std::nullopt;
                } else if constexpr (std::is_same_v<T, bool>) {
                    return value ? "true" : "false";
                } else if constexpr (std::is_same_v<T, std::string>) {
                    return value;
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    return std::to_string(value);
                } else {
                    // The shortest text that reads back as exactly the same double.
                    std::array<char, 64> buffer{};
                    const auto [end, error] =
                        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
                    static_cast<void>(error);
                    return std::string{buffer.data(), end};
                }
            },
            param);
    }

    void check(const PGresult* result) const {
        const ExecStatusType status = PQresultStatus(result);
        if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) {
            return;
        }

        const char* const state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
        const std::string_view code = state == nullptr ? std::string_view{} : state;
        const std::string message = withoutTrailingSpace(PQerrorMessage(connection_));

        DatabaseError::Kind kind = DatabaseError::Kind::Query;
        if (PQstatus(connection_) != CONNECTION_OK || code.starts_with("08") ||
            code.starts_with("57P")) {
            kind = DatabaseError::Kind::Connection;
        } else if (code == "23505") {
            kind = DatabaseError::Kind::UniqueViolation;
        } else if (code.starts_with("23")) {
            kind = DatabaseError::Kind::ConstraintViolation;
        }
        throw DatabaseError(kind, "PostgreSQL: " + message);
    }

    static QueryResult readResult(const PGresult* result) {
        QueryResult output;

        const int column_count = PQnfields(result);
        for (int i = 0; i < column_count; ++i) {
            output.columns.emplace_back(PQfname(result, i));
        }

        const int row_count = PQntuples(result);
        output.rows.reserve(static_cast<std::size_t>(row_count));
        for (int r = 0; r < row_count; ++r) {
            auto& row = output.rows.emplace_back();
            row.reserve(static_cast<std::size_t>(column_count));
            for (int c = 0; c < column_count; ++c) {
                if (PQgetisnull(result, r, c) != 0) {
                    row.emplace_back(std::nullopt);
                } else {
                    row.emplace_back(std::string{
                        PQgetvalue(result, r, c), static_cast<std::size_t>(PQgetlength(result, r, c))});
                }
            }
        }

        const std::string_view affected = PQcmdTuples(const_cast<PGresult*>(result));
        std::from_chars(affected.data(), affected.data() + affected.size(), output.affected);
        return output;
    }

    PGconn* connection_{nullptr};
};

}  // namespace

std::unique_ptr<Connection> openPostgresql(const std::string& url) {
    return std::make_unique<PostgresqlConnection>(url);
}

}  // namespace cppboot::data
