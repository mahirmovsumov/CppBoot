#pragma once

#include <memory>

#include "cppboot/base/result.hpp"

namespace cppboot::platform {

/// Turns operating system termination requests into a waitable shutdown request.
///
/// Linux handles SIGINT and SIGTERM. Windows handles console control events. Only one instance
/// can be installed at a time, and destroying it restores the previous behavior.
class ShutdownSignal {
public:
    static Result<ShutdownSignal> install();

    ShutdownSignal(ShutdownSignal&& other) noexcept;
    ShutdownSignal& operator=(ShutdownSignal&& other) noexcept;
    ~ShutdownSignal();

    /// Blocks until shutdown is requested. Returns immediately if it already was.
    void wait() const;

    /// Requests shutdown from application code. Safe to call from any thread.
    void request() const noexcept;

    bool requested() const noexcept;

private:
    struct Impl;

    explicit ShutdownSignal(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace cppboot::platform
