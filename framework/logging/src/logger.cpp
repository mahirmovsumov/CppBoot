#include "cppboot/logging/logger.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <optional>
#include <system_error>

#include "cppboot/platform/console.hpp"

namespace cppboot::logging {

namespace {

std::string_view levelName(Level level) noexcept {
    switch (level) {
        case Level::Trace:
            return "trace";
        case Level::Debug:
            return "debug";
        case Level::Info:
            return "info";
        case Level::Warn:
            return "warning";
        case Level::Error:
            return "error";
        case Level::Critical:
            return "critical";
        case Level::Off:
            break;
    }
    return "off";
}

std::string_view levelColor(Level level) noexcept {
    switch (level) {
        case Level::Trace:
            return "\x1b[37m";
        case Level::Debug:
            return "\x1b[36m";
        case Level::Info:
            return "\x1b[32m";
        case Level::Warn:
            return "\x1b[33m\x1b[1m";
        case Level::Error:
            return "\x1b[31m\x1b[1m";
        case Level::Critical:
            return "\x1b[1m\x1b[41m";
        case Level::Off:
            break;
    }
    return "";
}

constexpr std::string_view kResetColor = "\x1b[m";

/// Such as "2026-11-04 02:53:33.601", in local time.
std::string timestamp(std::chrono::system_clock::time_point now) {
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::tm local = platform::localTime(std::chrono::system_clock::to_time_t(now));
    char text[32];
    const std::size_t length = std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    return std::format("{}.{:03}", std::string_view{text, length}, milliseconds);
}

}  // namespace

struct Logger::Impl {
    std::string name;
    Level level{Level::Info};
    bool color{false};
    /// Only set when there's a log file.
    std::optional<std::ofstream> file;
    /// One line at a time, so that lines from different threads never mix.
    std::mutex mutex;
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
    auto impl = std::make_unique<Impl>();
    impl->name = options.name;
    impl->level = options.level;
    impl->color = platform::consoleSupportsColor();

    if (!options.file.empty()) {
        std::error_code error;
        if (options.file.has_parent_path()) {
            std::filesystem::create_directories(options.file.parent_path(), error);
        }
        impl->file.emplace(options.file, std::ios::binary | std::ios::app);
        if (!*impl->file) {
            return Error{ErrorCode::IoError,
                         "Cannot create logger: cannot open " + options.file.generic_string()};
        }
    }
    return Logger{std::move(impl)};
}

Logger::Logger(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Logger::Logger(Logger&& other) noexcept = default;
Logger& Logger::operator=(Logger&& other) noexcept = default;
Logger::~Logger() {
    if (impl_) {
        flush();
    }
}

void Logger::log(Level level, std::string_view message) const {
    if (!enabled(level)) {
        return;
    }
    const std::string time = timestamp(std::chrono::system_clock::now());
    const std::string_view name = levelName(level);
    const std::string plain = std::format("[{}] [{}] [{}] {}\n", time, impl_->name, name, message);

    const std::lock_guard lock{impl_->mutex};
    if (impl_->color) {
        const std::string colored = std::format("[{}] [{}] [{}{}{}] {}\n", time, impl_->name,
                                                levelColor(level), name, kResetColor, message);
        std::fwrite(colored.data(), 1, colored.size(), stdout);
    } else {
        std::fwrite(plain.data(), 1, plain.size(), stdout);
    }
    // Every line, so that the log is complete when a program stops, or when output goes to a pipe.
    std::fflush(stdout);

    if (impl_->file) {
        impl_->file->write(plain.data(), static_cast<std::streamsize>(plain.size()));
        // Writing a file on every line would be slow, so only what's serious is written at once.
        if (level >= Level::Warn) {
            impl_->file->flush();
        }
    }
}

bool Logger::enabled(Level level) const noexcept {
    return level != Level::Off && level >= impl_->level;
}

void Logger::flush() const noexcept {
    const std::lock_guard lock{impl_->mutex};
    std::fflush(stdout);
    if (impl_->file) {
        impl_->file->flush();
    }
}

}  // namespace cppboot::logging
