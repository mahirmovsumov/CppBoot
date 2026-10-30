#include "cppboot/platform/process.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <system_error>

namespace cppboot::platform {

namespace {

std::atomic<pid_t> g_child{0};

// Caught signals return to their default behavior in the child when it calls exec.
extern "C" void forwardToChild(int signal_number) {
    const int saved_errno = errno;
    if (const pid_t child = g_child.load(); child > 0) {
        kill(child, signal_number);
    }
    errno = saved_errno;
}

Error errnoError(const std::string& action, int error_number) {
    return Error{error_number == ENOENT ? ErrorCode::NotFound : ErrorCode::PlatformError,
                 action + " failed: " + std::strerror(error_number)};
}

/// Restores the previous signal handlers when the wait is over.
class ForwardedSignals {
public:
    ForwardedSignals() {
        struct sigaction action {};
        action.sa_handler = forwardToChild;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        sigaction(SIGINT, &action, &previous_interrupt_);
        sigaction(SIGTERM, &action, &previous_terminate_);
    }

    ~ForwardedSignals() {
        sigaction(SIGINT, &previous_interrupt_, nullptr);
        sigaction(SIGTERM, &previous_terminate_, nullptr);
    }

    ForwardedSignals(const ForwardedSignals&) = delete;
    ForwardedSignals& operator=(const ForwardedSignals&) = delete;

private:
    struct sigaction previous_interrupt_ {};
    struct sigaction previous_terminate_ {};
};

}  // namespace

Result<int> runProcess(const ProcessOptions& options) {
    if (options.command.empty()) {
        return Error{ErrorCode::InvalidArgument, "No command to run"};
    }

    std::vector<char*> arguments;
    arguments.reserve(options.command.size() + 1);
    for (const std::string& argument : options.command) {
        arguments.push_back(const_cast<char*>(argument.c_str()));
    }
    arguments.push_back(nullptr);

    const std::string action = "Starting " + options.command.front();

    // Closes on a successful exec, so reading from it reports whether exec failed.
    int exec_status[2];
    if (pipe2(exec_status, O_CLOEXEC) != 0) {
        return errnoError("pipe2", errno);
    }

    const ForwardedSignals forwarded;

    const pid_t child = fork();
    if (child < 0) {
        const int error_number = errno;
        close(exec_status[0]);
        close(exec_status[1]);
        return errnoError("fork", error_number);
    }

    if (child == 0) {
        close(exec_status[0]);
        const bool changed = options.working_directory.empty() ||
                             chdir(options.working_directory.c_str()) == 0;
        if (changed) {
            execvp(arguments[0], arguments.data());
        }
        const int error_number = errno;
        const ssize_t written = write(exec_status[1], &error_number, sizeof(error_number));
        static_cast<void>(written);
        _exit(127);
    }

    g_child.store(child);
    close(exec_status[1]);

    int exec_error = 0;
    ssize_t received = 0;
    do {
        received = read(exec_status[0], &exec_error, sizeof(exec_error));
    } while (received == -1 && errno == EINTR);
    close(exec_status[0]);

    int status = 0;
    pid_t waited = 0;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);
    const int wait_error = errno;
    g_child.store(0);

    if (received > 0) {
        return errnoError(action, exec_error);
    }
    if (waited == -1) {
        return errnoError("waitpid", wait_error);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return WEXITSTATUS(status);
}

Result<std::filesystem::path> executablePath() {
    std::error_code error;
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return Error{ErrorCode::PlatformError,
                     "Cannot read /proc/self/exe: " + error.message()};
    }
    return path;
}

std::string executableFileName(std::string_view name) {
    return std::string{name};
}

}  // namespace cppboot::platform
