#pragma once

#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "cppboot/base/result.hpp"

namespace cppboot::logging {

enum class Level { Trace, Debug, Info, Warn, Error, Critical, Off };

Result<Level> parseLevel(std::string_view text);

struct LoggerOptions {
    std::string name{"cppboot"};
    Level level{Level::Info};
    /// Log file to write in addition to the console. Leave empty for console output only.
    std::filesystem::path file{};
};

/// A thread-safe logger owned by the application. No logging state is global.
class Logger {
public:
    static Result<Logger> create(const LoggerOptions& options);

    Logger(Logger&& other) noexcept;
    Logger& operator=(Logger&& other) noexcept;
    ~Logger();

    void log(Level level, std::string_view message) const;
    bool enabled(Level level) const noexcept;
    void flush() const noexcept;

    template <typename... Args>
    void debug(std::format_string<Args...> format, Args&&... args) const {
        write(Level::Debug, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(std::format_string<Args...> format, Args&&... args) const {
        write(Level::Info, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(std::format_string<Args...> format, Args&&... args) const {
        write(Level::Warn, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(std::format_string<Args...> format, Args&&... args) const {
        write(Level::Error, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void critical(std::format_string<Args...> format, Args&&... args) const {
        write(Level::Critical, format, std::forward<Args>(args)...);
    }

private:
    struct Impl;

    explicit Logger(std::unique_ptr<Impl> impl);

    template <typename... Args>
    void write(Level level, std::format_string<Args...> format, Args&&... args) const {
        if (enabled(level)) {
            log(level, std::format(format, std::forward<Args>(args)...));
        }
    }

    std::unique_ptr<Impl> impl_;
};

}  // namespace cppboot::logging
