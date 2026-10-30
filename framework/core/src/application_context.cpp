#include "cppboot/core/application_context.hpp"

#include <utility>

namespace cppboot {

ApplicationContext::ApplicationContext(const config::Config& config, const logging::Logger& logger,
                                       std::function<void()> request_shutdown,
                                       WorkerGroup::FailureHandler on_worker_failure)
    : config_(config),
      logger_(logger),
      request_shutdown_(std::move(request_shutdown)),
      workers_(std::move(on_worker_failure)) {}

void ApplicationContext::requestShutdown() const {
    request_shutdown_();
}

}  // namespace cppboot
