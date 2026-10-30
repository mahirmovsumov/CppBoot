#pragma once

#include <atomic>
#include <concepts>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cppboot/base/error.hpp"
#include "cppboot/config/config.hpp"
#include "cppboot/core/application_context.hpp"
#include "cppboot/core/lifecycle.hpp"
#include "cppboot/core/module.hpp"
#include "cppboot/logging/logger.hpp"
#include "cppboot/platform/signals.hpp"

namespace cppboot {

inline constexpr int kExitSuccess = 0;
inline constexpr int kExitFailure = 1;
inline constexpr int kExitConfigurationError = 78;

struct ApplicationOptions {
    std::filesystem::path config_directory{"."};
    std::map<std::string, std::string> default_settings{};
    /// Replaces the process environment when set, which keeps tests independent of the machine.
    std::optional<std::map<std::string, std::string>> environment{};
};

/// The framework entry point. It owns the lifecycle, configuration, logging and modules.
class Application {
public:
    Application(int argc, char* argv[], ApplicationOptions options = {});
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    Application& addModule(std::unique_ptr<Module> instance);

    template <std::derived_from<Module> M, typename... Args>
    Application& emplaceModule(Args&&... args) {
        return addModule(std::make_unique<M>(std::forward<Args>(args)...));
    }

    /// Runs until shutdown is requested and returns the process exit code. Can run only once.
    int run() noexcept;

    /// Asks the application to shut down. Safe to call from any thread, even before run().
    void requestShutdown();

    LifecycleState state() const noexcept { return lifecycle_.state(); }

private:
    int execute();
    int fail(const Error& error, int exit_code);
    void advance(LifecycleState next);
    void report(const Error& error) const;
    void shutdown() noexcept;

    std::vector<std::string> arguments_;
    ApplicationOptions options_;
    Lifecycle lifecycle_;

    std::mutex shutdown_mutex_;
    bool shutdown_requested_{false};
    std::optional<platform::ShutdownSignal> signal_;
    std::atomic<bool> worker_failed_{false};

    // Destroyed in reverse order, so modules can use the context and logger in their destructors.
    std::optional<config::Config> config_;
    std::optional<logging::Logger> logger_;
    std::unique_ptr<ApplicationContext> context_;
    ModuleManager modules_;
};

}  // namespace cppboot
