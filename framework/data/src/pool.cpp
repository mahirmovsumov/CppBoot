#include "cppboot/data/pool.hpp"

#include <exception>
#include <utility>

namespace cppboot::data {

ConnectionPool::ConnectionPool(ConnectionFactory factory, PoolOptions options)
    : factory_(std::move(factory)), options_(options) {
    if (options_.max_size == 0) {
        throw DatabaseError(DatabaseError::Kind::Connection, "The pool size must be at least 1");
    }
}

ConnectionPool::Lease::~Lease() {
    if (connection_) {
        pool_->release(std::move(connection_), discarded_);
    }
}

ConnectionPool::Lease ConnectionPool::acquire() {
    const auto deadline = std::chrono::steady_clock::now() + options_.acquire_timeout;

    std::unique_lock lock{mutex_};
    while (true) {
        while (!idle_.empty()) {
            std::unique_ptr<Connection> connection = std::move(idle_.back());
            idle_.pop_back();
            if (connection->isHealthy()) {
                return Lease{*this, std::move(connection)};
            }
            --open_;
        }

        if (open_ < options_.max_size) {
            break;
        }

        if (available_.wait_until(lock, deadline) == std::cv_status::timeout && idle_.empty() &&
            open_ >= options_.max_size) {
            throw DatabaseError(DatabaseError::Kind::Timeout,
                                "No database connection became free in time. All " +
                                    std::to_string(options_.max_size) + " are in use");
        }
    }

    // Reserves a place, then opens outside the lock, because connecting can take a long time.
    ++open_;
    lock.unlock();
    try {
        return Lease{*this, factory_()};
    } catch (...) {
        lock.lock();
        --open_;
        lock.unlock();
        available_.notify_one();
        throw;
    }
}

void ConnectionPool::release(std::unique_ptr<Connection> connection, bool discarded) noexcept {
    {
        const std::lock_guard lock{mutex_};
        if (!discarded && connection->isHealthy()) {
            idle_.push_back(std::move(connection));
        } else {
            --open_;
        }
    }
    available_.notify_one();
}

std::size_t ConnectionPool::openCount() const {
    const std::lock_guard lock{mutex_};
    return open_;
}

std::size_t ConnectionPool::idleCount() const {
    const std::lock_guard lock{mutex_};
    return idle_.size();
}

}  // namespace cppboot::data
