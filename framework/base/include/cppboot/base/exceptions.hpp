#pragma once

#include <stdexcept>
#include <utility>

#include "cppboot/base/error.hpp"

namespace cppboot {

/// Base class for exceptions thrown by the framework.
class Exception : public std::runtime_error {
public:
    explicit Exception(Error error) : std::runtime_error(error.describe()), error_(std::move(error)) {}

    const Error& error() const noexcept { return error_; }

private:
    Error error_;
};

}  // namespace cppboot
