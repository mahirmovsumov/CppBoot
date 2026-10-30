#pragma once

#include <atomic>
#include <string_view>

#include "cppboot/base/result.hpp"

namespace cppboot {

enum class LifecycleState {
    Initialized,
    Configuring,
    RegisteringModules,
    CreatingContext,
    Starting,
    Running,
    Stopping,
    Failed,
    Terminated,
};

std::string_view toString(LifecycleState state) noexcept;

/// The application state machine. States advance in declaration order. Any state before Failed
/// can move to Failed, and Failed can only move to Terminated.
class Lifecycle {
public:
    LifecycleState state() const noexcept { return state_.load(); }

    Result<void> transitionTo(LifecycleState next);

    static bool isAllowed(LifecycleState from, LifecycleState to) noexcept;

private:
    std::atomic<LifecycleState> state_{LifecycleState::Initialized};
};

}  // namespace cppboot
