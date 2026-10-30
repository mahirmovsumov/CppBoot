#pragma once

#include <functional>

#include "cppboot/config/config.hpp"
#include "cppboot/core/worker_group.hpp"
#include "cppboot/logging/logger.hpp"

namespace cppboot {

/// The services an application shares with its modules. It outlives every module.
class ApplicationContext {
public:
    ApplicationContext(const config::Config& config, const logging::Logger& logger,
                       std::function<void()> request_shutdown,
                       WorkerGroup::FailureHandler on_worker_failure);

    const config::Config& config() const noexcept { return config_; }
    const logging::Logger& logger() const noexcept { return logger_; }
    WorkerGroup& workers() noexcept { return workers_; }

    /// Asks the application to shut down. Safe to call from any thread.
    void requestShutdown() const;

private:
    const config::Config& config_;
    const logging::Logger& logger_;
    std::function<void()> request_shutdown_;
    WorkerGroup workers_;
};

}  // namespace cppboot
