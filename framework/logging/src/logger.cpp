#include "cppboot/logging/logger.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <vector>

namespace cppboot::logging {

namespace {

spdlog::level::level_enum toSpdlogLevel(Level level) noexcept {
    switch (level) {
        case Level::Trace:
            return spdlog::level::trace;
        case Level::Debug:
            return spdlog::level::debug;
        case Level::Info:
            return spdlog::level::info;
        case Level::Warn:
            return spdlog::level::warn;
        case Level::Error:
            return spdlog::level::err;
        case Level::Critical:
            return spdlog::level::critical;
        case Level::Off:
            return spdlog::level::off;
    }
    return spdlog::level::info;
}

}  // namespace

struct Logger::Impl {
    std::shared_ptr<spdlog::logger> logger;
};

Result<Level> parseLevel(std::string_view text) {
    if (text == "trace") return Level::Trace;
    if (text == "debug") return Level::Debug;
    if (text == "info") return Level::Info;
    if (text == "warn") return Level::Warn;
    if (text == "error") return Level::Error;
    if (text == "critical") return Level::Critical;
    if (text == "off") return Level::Off;
    return Error{ErrorCode::InvalidArgument, "Unknown log level: " + std::string{text}};
}

Result<Logger> Logger::create(const LoggerOptions& options) {
    try {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        if (!options.file.empty()) {
            sinks.push_back(
                std::make_shared<spdlog::sinks::basic_file_sink_mt>(options.file.string(), false));
        }

        // Built directly so that it stays out of spdlog's global registry.
        auto logger = std::make_shared<spdlog::logger>(options.name, sinks.begin(), sinks.end());
        logger->set_level(toSpdlogLevel(options.level));
        logger->flush_on(spdlog::level::warn);

        return Logger{std::make_unique<Impl>(Impl{std::move(logger)})};
    } catch (const spdlog::spdlog_ex& exception) {
        return Error{ErrorCode::IoError, std::string{"Cannot create logger: "} + exception.what()};
    }
}

Logger::Logger(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Logger::Logger(Logger&& other) noexcept = default;
Logger& Logger::operator=(Logger&& other) noexcept = default;
Logger::~Logger() = default;

void Logger::log(Level level, std::string_view message) const {
    impl_->logger->log(toSpdlogLevel(level), "{}", message);
}

bool Logger::enabled(Level level) const noexcept {
    return impl_->logger->should_log(toSpdlogLevel(level));
}

void Logger::flush() const noexcept {
    impl_->logger->flush();
}

}  // namespace cppboot::logging
