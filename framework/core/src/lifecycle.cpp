#include "cppboot/core/lifecycle.hpp"

#include <string>

namespace cppboot {

std::string_view toString(LifecycleState state) noexcept {
    switch (state) {
        case LifecycleState::Initialized:
            return "Initialized";
        case LifecycleState::Configuring:
            return "Configuring";
        case LifecycleState::RegisteringModules:
            return "RegisteringModules";
        case LifecycleState::CreatingContext:
            return "CreatingContext";
        case LifecycleState::Starting:
            return "Starting";
        case LifecycleState::Running:
            return "Running";
        case LifecycleState::Stopping:
            return "Stopping";
        case LifecycleState::Failed:
            return "Failed";
        case LifecycleState::Terminated:
            return "Terminated";
    }
    return "Unknown";
}

bool Lifecycle::isAllowed(LifecycleState from, LifecycleState to) noexcept {
    switch (from) {
        case LifecycleState::Initialized:
            return to == LifecycleState::Configuring || to == LifecycleState::Failed;
        case LifecycleState::Configuring:
            return to == LifecycleState::RegisteringModules || to == LifecycleState::Failed;
        case LifecycleState::RegisteringModules:
            return to == LifecycleState::CreatingContext || to == LifecycleState::Failed;
        case LifecycleState::CreatingContext:
            return to == LifecycleState::Starting || to == LifecycleState::Failed;
        case LifecycleState::Starting:
            return to == LifecycleState::Running || to == LifecycleState::Failed;
        case LifecycleState::Running:
            return to == LifecycleState::Stopping || to == LifecycleState::Failed;
        case LifecycleState::Stopping:
            return to == LifecycleState::Terminated || to == LifecycleState::Failed;
        case LifecycleState::Failed:
            return to == LifecycleState::Terminated;
        case LifecycleState::Terminated:
            return false;
    }
    return false;
}

Result<void> Lifecycle::transitionTo(LifecycleState next) {
    LifecycleState current = state_.load();
    while (isAllowed(current, next)) {
        if (state_.compare_exchange_weak(current, next)) {
            return {};
        }
    }

    return Error{ErrorCode::InvalidState, "Invalid lifecycle transition from " +
                                              std::string{toString(current)} + " to " +
                                              std::string{toString(next)}};
}

}  // namespace cppboot
