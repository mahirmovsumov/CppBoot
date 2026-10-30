#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <string>
#include <utility>

#include "cppboot/platform/signals.hpp"

namespace cppboot::platform {

namespace {

// Windows gives console handlers no user data pointer, so this state has to be process-wide.
// The handles live until process exit because a handler thread may still be using them.
HANDLE g_requested = nullptr;
HANDLE g_released = nullptr;
std::atomic<bool> g_installed{false};

// Windows ends the process when a close, logoff or shutdown handler returns, so the handler
// holds the process open while the application cleans up. Windows allows about five seconds.
constexpr DWORD kCloseWaitMilliseconds = 5000;

BOOL WINAPI handleConsoleEvent(DWORD event) {
    SetEvent(g_requested);

    switch (event) {
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            WaitForSingleObject(g_released, kCloseWaitMilliseconds);
            break;
        default:
            break;
    }
    return TRUE;
}

Error lastError(const char* action) {
    return Error{ErrorCode::PlatformError,
                 std::string{action} + " failed with Windows error " + std::to_string(GetLastError())};
}

}  // namespace

struct ShutdownSignal::Impl {
    ~Impl() {
        SetConsoleCtrlHandler(handleConsoleEvent, FALSE);
        SetEvent(g_released);
        g_installed.store(false);
    }
};

Result<ShutdownSignal> ShutdownSignal::install() {
    if (g_installed.exchange(true)) {
        return Error{ErrorCode::InvalidState, "A shutdown signal is already installed"};
    }

    if (g_requested == nullptr) {
        g_requested = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_released = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_requested == nullptr || g_released == nullptr) {
            g_installed.store(false);
            return lastError("CreateEventW");
        }
    }
    ResetEvent(g_requested);
    ResetEvent(g_released);

    if (SetConsoleCtrlHandler(handleConsoleEvent, TRUE) == FALSE) {
        g_installed.store(false);
        return lastError("SetConsoleCtrlHandler");
    }

    return ShutdownSignal{std::make_unique<Impl>()};
}

ShutdownSignal::ShutdownSignal(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ShutdownSignal::ShutdownSignal(ShutdownSignal&& other) noexcept = default;
ShutdownSignal& ShutdownSignal::operator=(ShutdownSignal&& other) noexcept = default;
ShutdownSignal::~ShutdownSignal() = default;

void ShutdownSignal::wait() const {
    WaitForSingleObject(g_requested, INFINITE);
}

void ShutdownSignal::request() const noexcept {
    SetEvent(g_requested);
}

bool ShutdownSignal::requested() const noexcept {
    return WaitForSingleObject(g_requested, 0) == WAIT_OBJECT_0;
}

}  // namespace cppboot::platform
