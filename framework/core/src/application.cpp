#include "cppboot/core/application.hpp"

#include <exception>
#include <iostream>
#include <string_view>

#include "cppboot/base/exceptions.hpp"
#include "cppboot/base/version.hpp"
#include "cppboot/platform/environment.hpp"

namespace cppboot {

Application::Application(int argc, char* argv[], ApplicationOptions options)
    : options_(std::move(options)) {
    for (int i = 1; i < argc; ++i) {
        arguments_.emplace_back(argv[i]);
    }
}

Application::~Application() = default;

Application& Application::addModule(std::unique_ptr<Module> instance) {
    modules_.add(std::move(instance));
    return *this;
}

int Application::run() noexcept {
    int exit_code = kExitFailure;
    try {
        exit_code = execute();
    } catch (const std::exception& exception) {
        exit_code = fail(Error{ErrorCode::Unknown, exception.what()}, kExitFailure);
    } catch (...) {
        exit_code = fail(Error{ErrorCode::Unknown, "Unknown exception"}, kExitFailure);
    }

    shutdown();

    if (exit_code == kExitSuccess && worker_failed_.load()) {
        exit_code = kExitFailure;
    }
    static_cast<void>(lifecycle_.transitionTo(LifecycleState::Terminated));
    return exit_code;
}

void Application::requestShutdown() {
    const std::lock_guard lock{shutdown_mutex_};
    shutdown_requested_ = true;
    if (signal_) {
        signal_->request();
    }
}

int Application::execute() {
    if (auto started = lifecycle_.transitionTo(LifecycleState::Configuring); !started) {
        report(started.error());
        return kExitFailure;
    }

    auto installed = platform::ShutdownSignal::install();
    if (!installed) {
        return fail(installed.error(), kExitFailure);
    }
    {
        const std::lock_guard lock{shutdown_mutex_};
        signal_.emplace(std::move(installed).value());
        if (shutdown_requested_) {
            signal_->request();
        }
    }

    config::ConfigSources sources;
    sources.defaults = options_.default_settings;
    sources.directory = options_.config_directory;
    sources.environment =
        options_.environment ? *options_.environment : platform::environmentVariables();
    sources.arguments = arguments_;

    auto loaded = config::Config::load(sources);
    if (!loaded) {
        return fail(loaded.error(), kExitConfigurationError);
    }
    config_.emplace(std::move(loaded).value());

    const auto level = logging::parseLevel(config_->getString("cppboot.logging.level", "info"));
    if (!level) {
        return fail(level.error(), kExitConfigurationError);
    }

    logging::LoggerOptions logger_options;
    logger_options.name = config_->getString("cppboot.application.name", "cppboot");
    logger_options.level = level.value();
    logger_options.file = config_->getString("cppboot.logging.file");

    auto logger = logging::Logger::create(logger_options);
    if (!logger) {
        return fail(logger.error(), kExitConfigurationError);
    }
    logger_.emplace(std::move(logger).value());

    const auto shutdown_after_start = config_->getBool("cppboot.runtime.shutdown_after_start", false);
    if (!shutdown_after_start) {
        return fail(shutdown_after_start.error(), kExitConfigurationError);
    }

    logger_->info("Starting CppBoot {} (profile: {})", kVersionString,
                  config_->profile().empty() ? std::string_view{"none"}
                                             : std::string_view{config_->profile()});
    logger_->debug("Configuration:\n{}", config_->describe());

    advance(LifecycleState::RegisteringModules);
    if (auto resolved = modules_.resolveOrder(); !resolved) {
        return fail(resolved.error(), kExitFailure);
    }

    advance(LifecycleState::CreatingContext);
    context_ = std::make_unique<ApplicationContext>(
        *config_, *logger_, [this] { requestShutdown(); },
        [this](std::string_view worker, std::string_view reason) {
            worker_failed_.store(true);
            logger_->critical("Worker {} failed: {}", worker, reason);
            requestShutdown();
        });

    advance(LifecycleState::Starting);
    if (auto started = modules_.startAll(*context_); !started) {
        return fail(started.error(), kExitFailure);
    }

    advance(LifecycleState::Running);
    logger_->info("Application is running");
    if (shutdown_after_start.value()) {
        requestShutdown();
    }
    signal_->wait();

    advance(LifecycleState::Stopping);
    logger_->info("Shutting down");
    return kExitSuccess;
}

int Application::fail(const Error& error, int exit_code) {
    report(error);
    static_cast<void>(lifecycle_.transitionTo(LifecycleState::Failed));
    return exit_code;
}

void Application::advance(LifecycleState next) {
    if (auto moved = lifecycle_.transitionTo(next); !moved) {
        throw Exception(moved.error());
    }
}

void Application::report(const Error& error) const {
    if (logger_) {
        logger_->critical("{}", error.describe());
    } else {
        std::cerr << "CppBoot failed: " << error.describe() << '\n';
    }
}

void Application::shutdown() noexcept {
    modules_.stopAll();
    if (context_) {
        context_->workers().stopAll();
    }

    {
        const std::lock_guard lock{shutdown_mutex_};
        signal_.reset();
    }

    if (logger_) {
        logger_->flush();
    }
}

}  // namespace cppboot
