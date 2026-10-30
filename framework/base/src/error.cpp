#include "cppboot/base/error.hpp"

#include <utility>

namespace cppboot {

std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Unknown:
            return "Unknown";
        case ErrorCode::InvalidArgument:
            return "InvalidArgument";
        case ErrorCode::InvalidState:
            return "InvalidState";
        case ErrorCode::NotFound:
            return "NotFound";
        case ErrorCode::ParseError:
            return "ParseError";
        case ErrorCode::IoError:
            return "IoError";
        case ErrorCode::DependencyError:
            return "DependencyError";
        case ErrorCode::StartupError:
            return "StartupError";
        case ErrorCode::PlatformError:
            return "PlatformError";
    }
    return "Unknown";
}

Error::Error(ErrorCode code, std::string message, std::source_location location)
    : code_(code), message_(std::move(message)), location_(location) {}

std::string Error::describe() const {
    std::string_view file = location_.file_name();
    if (const auto slash = file.find_last_of("/\\"); slash != std::string_view::npos) {
        file.remove_prefix(slash + 1);
    }

    std::string text;
    text += '[';
    text += toString(code_);
    text += "] ";
    text += message_;
    text += " (";
    text += file;
    text += ':';
    text += std::to_string(location_.line());
    text += ')';
    return text;
}

}  // namespace cppboot
