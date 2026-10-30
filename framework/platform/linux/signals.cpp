#include <semaphore.h>
#include <signal.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include "cppboot/platform/signals.hpp"

namespace cppboot::platform {

namespace {

// Signal handlers receive no user data pointer, so this state has to be process-wide.
// sem_post is one of the few synchronization calls that is async-signal-safe.
sem_t g_requested;
std::atomic<bool> g_installed{false};

extern "C" void handleSignal(int) {
    const int saved_errno = errno;
    sem_post(&g_requested);
    errno = saved_errno;
}

Error lastError(const char* action) {
    return Error{ErrorCode::PlatformError, std::string{action} + " failed: " + std::strerror(errno)};
}

}  // namespace

struct ShutdownSignal::Impl {
    struct sigaction previous_interrupt {};
    struct sigaction previous_terminate {};

    ~Impl() {
        sigaction(SIGINT, &previous_interrupt, nullptr);
        sigaction(SIGTERM, &previous_terminate, nullptr);
        sem_destroy(&g_requested);
        g_installed.store(false);
    }
};

Result<ShutdownSignal> ShutdownSignal::install() {
    if (g_installed.exchange(true)) {
        return Error{ErrorCode::InvalidState, "A shutdown signal is already installed"};
    }

    if (sem_init(&g_requested, 0, 0) != 0) {
        g_installed.store(false);
        return lastError("sem_init");
    }

    auto impl = std::make_unique<Impl>();

    struct sigaction action {};
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;

    if (sigaction(SIGINT, &action, &impl->previous_interrupt) != 0) {
        auto error = lastError("sigaction(SIGINT)");
        impl->previous_interrupt.sa_handler = SIG_DFL;
        impl->previous_terminate.sa_handler = SIG_DFL;
        return error;
    }
    if (sigaction(SIGTERM, &action, &impl->previous_terminate) != 0) {
        auto error = lastError("sigaction(SIGTERM)");
        impl->previous_terminate.sa_handler = SIG_DFL;
        return error;
    }

    return ShutdownSignal{std::move(impl)};
}

ShutdownSignal::ShutdownSignal(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ShutdownSignal::ShutdownSignal(ShutdownSignal&& other) noexcept = default;
ShutdownSignal& ShutdownSignal::operator=(ShutdownSignal&& other) noexcept = default;
ShutdownSignal::~ShutdownSignal() = default;

void ShutdownSignal::wait() const {
    while (sem_wait(&g_requested) == -1 && errno == EINTR) {
    }
    // Put the count back so later calls to wait() and requested() still see the request.
    sem_post(&g_requested);
}

void ShutdownSignal::request() const noexcept {
    sem_post(&g_requested);
}

bool ShutdownSignal::requested() const noexcept {
    int count = 0;
    sem_getvalue(&g_requested, &count);
    return count > 0;
}

}  // namespace cppboot::platform
