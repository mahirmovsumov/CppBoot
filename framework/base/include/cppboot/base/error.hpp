#pragma once

#include <source_location>
#include <string>
#include <string_view>

namespace cppboot {

enum class ErrorCode {
    Unknown,
    InvalidArgument,
    InvalidState,
    NotFound,
    ParseError,
    IoError,
    DependencyError,
    StartupError,
    PlatformError,
};

std::string_view toString(ErrorCode code) noexcept;

/// Describes a failure and the source location that reported it.
class Error {
public:
    Error(ErrorCode code, std::string message,
          std::source_location location = std::source_location::current());

    ErrorCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }
    const std::source_location& location() const noexcept { return location_; }

    /// Returns a single-line description suitable for logs.
    std::string describe() const;

private:
    ErrorCode code_;
    std::string message_;
    std::source_location location_;
};

}  // namespace cppboot
