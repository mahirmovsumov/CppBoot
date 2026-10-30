#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cppboot/config/config.hpp"
#include "cppboot/data/driver.hpp"
#include "cppboot/data/pool.hpp"
#include "cppboot/data/row.hpp"

namespace cppboot::data {

/// Knows how to fill each struct from a row. CPPBOOT_ROW registers these automatically. Add every
/// mapper before queries start, because this isn't safe to change while they're running.
class RowMappers {
public:
    template <std::default_initializable T>
    void add(std::function<void(const Row&, T&)> mapper) {
        mappers_.insert_or_assign(typeid(T), [mapper = std::move(mapper)](const Row& row, void* out) {
            mapper(row, *static_cast<T*>(out));
        });
    }

    /// Throws DatabaseError if T has no mapper.
    template <std::default_initializable T>
    T map(const Row& row) const {
        T value{};
        find(typeid(T))(row, &value);
        return value;
    }

private:
    using ErasedMapper = std::function<void(const Row&, void*)>;

    const ErasedMapper& find(std::type_index type) const;

    std::unordered_map<std::type_index, ErasedMapper> mappers_;
};

namespace detail {

template <typename T>
Param toParam(const T& value) {
    if constexpr (std::is_same_v<T, Param>) {
        return value;
    } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
        return std::monostate{};
    } else if constexpr (std::is_same_v<T, bool>) {
        return value;
    } else if constexpr (std::is_integral_v<T>) {
        return static_cast<std::int64_t>(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        return static_cast<double>(value);
    } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        return std::string{std::string_view{value}};
    } else if constexpr (IsOptional<T>::value) {
        if (!value) {
            return std::monostate{};
        }
        return toParam(*value);
    } else {
        static_assert(!sizeof(T),
                      "Query parameters can be text, a number, a bool, nullptr or a std::optional");
    }
}

}  // namespace detail

/// Runs statements. Values go separately from the SQL as $1, $2 and so on, so they can't change
/// what the statement does. That's what prevents SQL injection, so never build SQL from input.
class QueryRunner {
public:
    virtual ~QueryRunner() = default;

    /// Runs a statement and returns its rows.
    template <typename... Args>
    Rows query(std::string_view sql, const Args&... args) {
        return run(sql, std::vector<Param>{detail::toParam(args)...});
    }

    /// Runs a statement and returns the number of rows it changed.
    template <typename... Args>
    std::int64_t execute(std::string_view sql, const Args&... args) {
        return query(sql, args...).affected();
    }

    /// Returns every row as a struct marked with CPPBOOT_ROW.
    template <typename T, typename... Args>
    std::vector<T> queryAs(std::string_view sql, const Args&... args) {
        const Rows rows = query(sql, args...);
        std::vector<T> values;
        values.reserve(rows.size());
        for (const Row row : rows) {
            values.push_back(requireMappers().map<T>(row));
        }
        return values;
    }

    /// Returns the first row as a struct marked with CPPBOOT_ROW, or nothing if there are no rows.
    template <typename T, typename... Args>
    std::optional<T> queryOne(std::string_view sql, const Args&... args) {
        const Rows rows = query(sql, args...);
        if (rows.empty()) {
            return std::nullopt;
        }
        return requireMappers().map<T>(rows[0]);
    }

    /// Returns the first column of the first row, or nothing if there are no rows.
    template <typename T, typename... Args>
    std::optional<T> queryValue(std::string_view sql, const Args&... args) {
        const Rows rows = query(sql, args...);
        if (rows.empty()) {
            return std::nullopt;
        }
        return rows[0].template get<T>(std::size_t{0});
    }

protected:
    virtual Rows run(std::string_view sql, std::vector<Param> params) = 0;
    virtual const RowMappers& requireMappers() const = 0;
};

/// Runs statements as one unit. It's only valid inside the Database::transaction() call that
/// created it, on that thread.
class Transaction : public QueryRunner {
public:
    ~Transaction() override;

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

protected:
    Rows run(std::string_view sql, std::vector<Param> params) override;
    const RowMappers& requireMappers() const override;

private:
    friend class Database;

    Transaction(ConnectionPool::Lease& lease, const RowMappers* mappers);
    void commit();

    ConnectionPool::Lease& lease_;
    const RowMappers* mappers_;
    bool active_{true};
};

/// The main way to use a database. All functions are safe to call from any thread, and each
/// statement borrows a connection from a pool for as long as it runs.
class Database : public QueryRunner {
public:
    /// Opens no connection yet. Connections open when first needed.
    Database(ConnectionFactory factory, PoolOptions options,
             std::shared_ptr<const RowMappers> mappers = {});

    /// Creates a database from settings under cppboot.database. A missing URL or an invalid value
    /// throws cppboot::Exception.
    static std::shared_ptr<Database> fromSettings(const config::Config& config,
                                                  std::shared_ptr<const RowMappers> mappers = {});

    /// Calls a function with a Transaction, and returns what it returns. The transaction commits
    /// if the function returns, and rolls back if it throws.
    template <std::invocable<Transaction&> Function>
    auto transaction(Function&& function) {
        ConnectionPool::Lease lease = pool_.acquire();
        Transaction transaction{lease, mappers_.get()};

        using Return = std::invoke_result_t<Function, Transaction&>;
        if constexpr (std::is_void_v<Return>) {
            std::forward<Function>(function)(transaction);
            transaction.commit();
        } else {
            Return result = std::forward<Function>(function)(transaction);
            transaction.commit();
            return result;
        }
    }

    ConnectionPool& pool() noexcept { return pool_; }

protected:
    Rows run(std::string_view sql, std::vector<Param> params) override;
    const RowMappers& requireMappers() const override;

private:
    ConnectionPool pool_;
    std::shared_ptr<const RowMappers> mappers_;
};

}  // namespace cppboot::data
