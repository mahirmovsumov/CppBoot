#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cppboot/base/result.hpp"

namespace cppboot {

/// Owns background threads and stops them cooperatively through a std::stop_token.
class WorkerGroup {
public:
    using Work = std::function<void(std::stop_token)>;
    using FailureHandler = std::function<void(std::string_view worker, std::string_view reason)>;

    /// The handler runs on the failing worker's thread when its work throws an exception.
    explicit WorkerGroup(FailureHandler on_failure);
    ~WorkerGroup();

    WorkerGroup(const WorkerGroup&) = delete;
    WorkerGroup& operator=(const WorkerGroup&) = delete;

    /// Starts a worker. Fails once stopAll() has been called.
    Result<void> spawn(std::string name, Work work);

    /// Requests every worker to stop and waits for them. Must not be called from a worker.
    void stopAll() noexcept;

    std::size_t size() const;

private:
    FailureHandler on_failure_;
    mutable std::mutex mutex_;
    std::vector<std::jthread> workers_;
    bool stopped_{false};
};

}  // namespace cppboot
