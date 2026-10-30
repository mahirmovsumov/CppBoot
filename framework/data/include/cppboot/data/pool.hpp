#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include "cppboot/data/driver.hpp"

namespace cppboot::data {

struct PoolOptions {
    /// The most connections open at once. They open only when needed.
    std::size_t max_size{10};
    /// How long acquire() waits for a free connection before it throws.
    std::chrono::milliseconds acquire_timeout{std::chrono::seconds{5}};
};

/// Shares a limited number of connections between threads. A connection that's broken, or was left
/// inside a transaction, is closed rather than reused. It must outlive every Lease.
class ConnectionPool {
public:
    ConnectionPool(ConnectionFactory factory, PoolOptions options);

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    /// A connection on loan, which goes back to the pool when this is destroyed.
    class Lease {
    public:
        Lease(ConnectionPool& pool, std::unique_ptr<Connection> connection)
            : pool_(&pool), connection_(std::move(connection)) {}
        Lease(Lease&& other) noexcept = default;
        Lease& operator=(Lease&&) = delete;
        ~Lease();

        Connection& operator*() const noexcept { return *connection_; }
        Connection* operator->() const noexcept { return connection_.get(); }

        /// Closes the connection on return, rather than reusing it.
        void discard() noexcept { discarded_ = true; }

    private:
        ConnectionPool* pool_;
        std::unique_ptr<Connection> connection_;
        bool discarded_{false};
    };

    /// Throws DatabaseError if no connection becomes free in time, or one can't be opened.
    Lease acquire();

    /// The number of connections open, including those on loan.
    std::size_t openCount() const;
    std::size_t idleCount() const;

private:
    void release(std::unique_ptr<Connection> connection, bool discarded) noexcept;

    ConnectionFactory factory_;
    PoolOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::vector<std::unique_ptr<Connection>> idle_;
    std::size_t open_{0};
};

}  // namespace cppboot::data
