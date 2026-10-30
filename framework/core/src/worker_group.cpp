#include "cppboot/core/worker_group.hpp"

#include <exception>
#include <utility>

namespace cppboot {

WorkerGroup::WorkerGroup(FailureHandler on_failure) : on_failure_(std::move(on_failure)) {}

WorkerGroup::~WorkerGroup() {
    stopAll();
}

Result<void> WorkerGroup::spawn(std::string name, Work work) {
    const std::lock_guard lock{mutex_};
    if (stopped_) {
        return Error{ErrorCode::InvalidState, "Cannot start worker after shutdown: " + name};
    }

    workers_.emplace_back([this, name = std::move(name), work = std::move(work)](std::stop_token token) {
        try {
            work(std::move(token));
        } catch (const std::exception& exception) {
            on_failure_(name, exception.what());
        } catch (...) {
            on_failure_(name, "unknown exception");
        }
    });
    return {};
}

void WorkerGroup::stopAll() noexcept {
    std::vector<std::jthread> workers;
    {
        const std::lock_guard lock{mutex_};
        stopped_ = true;
        workers.swap(workers_);
    }

    for (auto& worker : workers) {
        worker.request_stop();
    }
    // Destroying the local vector joins every worker, outside the lock.
}

std::size_t WorkerGroup::size() const {
    const std::lock_guard lock{mutex_};
    return workers_.size();
}

}  // namespace cppboot
