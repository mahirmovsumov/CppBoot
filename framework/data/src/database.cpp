#include "cppboot/data/database.hpp"

#include <chrono>
#include <cstdint>
#include <limits>

namespace cppboot::data {

namespace {

constexpr std::string_view kSqlitePrefix = "sqlite:";
constexpr std::string_view kInMemory = ":memory:";

const RowMappers& requireMappersFrom(const RowMappers* mappers) {
    if (mappers == nullptr) {
        throw DatabaseError(DatabaseError::Kind::Mapping,
                            "This database has no row mappers. Mark the struct with CPPBOOT_ROW, "
                            "or pass mappers when creating the database");
    }
    return *mappers;
}

std::int64_t readPositive(const config::Config& config, std::string_view key, std::int64_t fallback,
                          std::int64_t maximum) {
    const auto value = config.getInt(key, fallback);
    if (!value) {
        throw Exception(value.error());
    }
    if (value.value() < 1 || value.value() > maximum) {
        throw Exception(Error{ErrorCode::InvalidArgument,
                              "Setting " + std::string{key} + " must be between 1 and " +
                                  std::to_string(maximum)});
    }
    return value.value();
}

}  // namespace

const RowMappers::ErasedMapper& RowMappers::find(std::type_index type) const {
    const auto entry = mappers_.find(type);
    if (entry == mappers_.end()) {
        throw DatabaseError(DatabaseError::Kind::Mapping,
                            std::string{"No row mapper for type "} + type.name() +
                                ". Mark the struct with CPPBOOT_ROW");
    }
    return entry->second;
}

ConnectionFactory connectionFactoryFor(std::string_view url) {
    if (url.starts_with(kSqlitePrefix)) {
        return [path = std::string{url.substr(kSqlitePrefix.size())}] { return openSqlite(path); };
    }
    if (url.starts_with("postgresql://") || url.starts_with("postgres://")) {
        return [url = std::string{url}] { return openPostgresql(url); };
    }
    throw DatabaseError(DatabaseError::Kind::Connection,
                        "Unsupported database URL. It must start with postgresql:// or sqlite:");
}

Transaction::Transaction(ConnectionPool::Lease& lease, const RowMappers* mappers)
    : lease_(lease), mappers_(mappers) {
    lease_->executeScript("BEGIN");
}

Transaction::~Transaction() {
    if (!active_) {
        return;
    }
    try {
        lease_->executeScript("ROLLBACK");
    } catch (...) {
        // The connection can't be trusted now, so make sure nobody else gets it.
        lease_.discard();
    }
}

void Transaction::commit() {
    lease_->executeScript("COMMIT");
    active_ = false;
}

Rows Transaction::run(std::string_view sql, std::vector<Param> params) {
    return Rows{lease_->execute(sql, params)};
}

const RowMappers& Transaction::requireMappers() const {
    return requireMappersFrom(mappers_);
}

Database::Database(ConnectionFactory factory, PoolOptions options,
                   std::shared_ptr<const RowMappers> mappers)
    : pool_(std::move(factory), options), mappers_(std::move(mappers)) {}

std::shared_ptr<Database> Database::fromSettings(const config::Config& config,
                                                 std::shared_ptr<const RowMappers> mappers) {
    const auto url = config.find("cppboot.database.url");
    if (!url || url->empty()) {
        throw Exception(Error{ErrorCode::InvalidArgument,
                              "Set cppboot.database.url, such as sqlite:app.db or "
                              "postgresql://user:password@host/name"});
    }

    PoolOptions options;
    options.max_size = static_cast<std::size_t>(
        readPositive(config, "cppboot.database.pool_size", 10, 1000));
    options.acquire_timeout = std::chrono::seconds{
        readPositive(config, "cppboot.database.acquire_timeout_seconds", 5, 3600)};

    // Each connection to an in-memory SQLite database gets its own separate, empty database.
    if (std::string_view{*url}.substr(kSqlitePrefix.size()) == kInMemory &&
        url->starts_with(kSqlitePrefix)) {
        options.max_size = 1;
    }

    return std::make_shared<Database>(connectionFactoryFor(*url), options, std::move(mappers));
}

Rows Database::run(std::string_view sql, std::vector<Param> params) {
    ConnectionPool::Lease lease = pool_.acquire();
    return Rows{lease->execute(sql, params)};
}

const RowMappers& Database::requireMappers() const {
    return requireMappersFrom(mappers_.get());
}

}  // namespace cppboot::data
