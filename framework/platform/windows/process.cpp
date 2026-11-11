#include "cppboot/platform/process.hpp"

#include <optional>
#include <string>
#include <utility>

#include "text.hpp"

namespace cppboot::platform {

namespace {

// Follows the rules of CommandLineToArgvW, where backslashes are special only before a quote.
void appendQuoted(std::wstring& command_line, std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        command_line += argument;
        return;
    }

    command_line += L'"';
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            command_line.append(backslashes * 2 + 1, L'\\');
            command_line += c;
            backslashes = 0;
        } else {
            command_line.append(backslashes, L'\\');
            command_line += c;
            backslashes = 0;
        }
    }
    command_line.append(backslashes * 2, L'\\');
    command_line += L'"';
}

Error lastError(const std::string& action) {
    const DWORD code = GetLastError();
    const ErrorCode error_code = (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                                     ? ErrorCode::NotFound
                                     : ErrorCode::PlatformError;
    return Error{error_code, action + " failed with Windows error " + std::to_string(code)};
}

// A real handler rather than SetConsoleCtrlHandler(nullptr, TRUE), because child processes
// inherit that setting and would then ignore Ctrl+C too.
BOOL WINAPI keepWaitingForChild(DWORD) {
    return TRUE;
}

/// The standard handles for a child. Without shared input, it reads from NUL, so that neither it nor
/// the programs it starts touch a pipe that a thread here is reading, which would make them wait.
class ChildInput {
public:
    explicit ChildInput(bool share_input) {
        if (!share_input) {
            SECURITY_ATTRIBUTES inheritable{};
            inheritable.nLength = sizeof(inheritable);
            inheritable.bInheritHandle = TRUE;
            empty_ = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &inheritable, OPEN_EXISTING, 0, nullptr);
        }
    }
    ~ChildInput() {
        if (empty_ != INVALID_HANDLE_VALUE) {
            CloseHandle(empty_);
        }
    }
    ChildInput(const ChildInput&) = delete;
    ChildInput& operator=(const ChildInput&) = delete;

    void applyTo(STARTUPINFOW& startup) const {
        if (empty_ == INVALID_HANDLE_VALUE) {
            return;
        }
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = empty_;
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }

private:
    HANDLE empty_{INVALID_HANDLE_VALUE};
};

}  // namespace

Result<int> runProcess(const ProcessOptions& options) {
    if (options.command.empty()) {
        return Error{ErrorCode::InvalidArgument, "No command to run"};
    }

    std::wstring command_line;
    for (const std::string& argument : options.command) {
        if (!command_line.empty()) {
            command_line += L' ';
        }
        appendQuoted(command_line, detail::toWide(argument));
    }

    const std::wstring directory = options.working_directory.wstring();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    const ChildInput input{options.share_input};
    input.applyTo(startup);
    PROCESS_INFORMATION process{};

    SetConsoleCtrlHandler(keepWaitingForChild, TRUE);
    const BOOL created =
        CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, 0, nullptr,
                       directory.empty() ? nullptr : directory.c_str(), &startup, &process);
    if (created == FALSE) {
        auto error = lastError("Starting " + options.command.front());
        SetConsoleCtrlHandler(keepWaitingForChild, FALSE);
        return error;
    }

    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);

    DWORD exit_code = 1;
    const BOOL read = GetExitCodeProcess(process.hProcess, &exit_code);
    auto read_error = read == FALSE ? std::optional<Error>{lastError("GetExitCodeProcess")}
                                    : std::nullopt;
    CloseHandle(process.hProcess);
    SetConsoleCtrlHandler(keepWaitingForChild, FALSE);

    if (read_error) {
        return *std::move(read_error);
    }
    return static_cast<int>(exit_code);
}

Result<ProcessOutput> runProcessCapturingOutput(const ProcessOptions& options) {
    if (options.command.empty()) {
        return Error{ErrorCode::InvalidArgument, "No command to run"};
    }

    std::wstring command_line;
    for (const std::string& argument : options.command) {
        if (!command_line.empty()) {
            command_line += L' ';
        }
        appendQuoted(command_line, detail::toWide(argument));
    }
    const std::wstring directory = options.working_directory.wstring();

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (CreatePipe(&read_end, &write_end, &inheritable, 0) == FALSE) {
        return lastError("CreatePipe");
    }
    // Only the child gets the end it writes to.
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    PROCESS_INFORMATION process{};

    const BOOL created =
        CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, directory.empty() ? nullptr : directory.c_str(), &startup, &process);
    // Closing this end here means reading ends when the child, the only other holder, exits.
    CloseHandle(write_end);
    if (created == FALSE) {
        auto error = lastError("Starting " + options.command.front());
        CloseHandle(read_end);
        return error;
    }
    CloseHandle(process.hThread);

    ProcessOutput result;
    char buffer[4096];
    DWORD received = 0;
    while (ReadFile(read_end, buffer, sizeof(buffer), &received, nullptr) != FALSE && received > 0) {
        result.output.append(buffer, received);
    }
    CloseHandle(read_end);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    const BOOL read = GetExitCodeProcess(process.hProcess, &exit_code);
    auto read_error = read == FALSE ? std::optional<Error>{lastError("GetExitCodeProcess")}
                                    : std::nullopt;
    CloseHandle(process.hProcess);
    if (read_error) {
        return *std::move(read_error);
    }
    result.exit_code = static_cast<int>(exit_code);
    return result;
}

struct ChildProcess::Impl {
    HANDLE process{nullptr};
    DWORD id{0};
    std::optional<int> exit_code;

    ~Impl() {
        if (process != nullptr) {
            CloseHandle(process);
        }
    }
};

Result<ChildProcess> ChildProcess::start(const ProcessOptions& options) {
    if (options.command.empty()) {
        return Error{ErrorCode::InvalidArgument, "No command to run"};
    }

    std::wstring command_line;
    for (const std::string& argument : options.command) {
        if (!command_line.empty()) {
            command_line += L' ';
        }
        appendQuoted(command_line, detail::toWide(argument));
    }
    const std::wstring directory = options.working_directory.wstring();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    const ChildInput input{options.share_input};
    input.applyTo(startup);
    PROCESS_INFORMATION process{};
    // Its own process group, so Ctrl+C reaches only this process, which then decides when to stop
    // the child. Ctrl+Break still reaches it, and that's what stop() sends.
    const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                                        CREATE_NEW_PROCESS_GROUP, nullptr,
                                        directory.empty() ? nullptr : directory.c_str(), &startup,
                                        &process);
    if (created == FALSE) {
        return lastError("Starting " + options.command.front());
    }
    CloseHandle(process.hThread);

    auto impl = std::make_unique<Impl>();
    impl->process = process.hProcess;
    impl->id = process.dwProcessId;
    return ChildProcess{std::move(impl)};
}

ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ChildProcess::ChildProcess(ChildProcess&& other) noexcept = default;
ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            stop(std::chrono::seconds{10});
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

ChildProcess::~ChildProcess() {
    if (impl_) {
        stop(std::chrono::seconds{10});
    }
}

std::optional<int> ChildProcess::exitCode() {
    if (!impl_->exit_code && WaitForSingleObject(impl_->process, 0) == WAIT_OBJECT_0) {
        DWORD code = 1;
        GetExitCodeProcess(impl_->process, &code);
        impl_->exit_code = static_cast<int>(code);
    }
    return impl_->exit_code;
}

int ChildProcess::stop(std::chrono::milliseconds timeout) {
    if (const auto code = exitCode()) {
        return *code;
    }
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, impl_->id);
    if (WaitForSingleObject(impl_->process, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
        TerminateProcess(impl_->process, 1);
        WaitForSingleObject(impl_->process, INFINITE);
    }
    return exitCode().value_or(1);
}

Result<std::filesystem::path> executablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD size =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return lastError("GetModuleFileNameW");
        }
        if (size < buffer.size()) {
            buffer.resize(size);
            return std::filesystem::path{buffer};
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::string executableFileName(std::string_view name) {
    return std::string{name} + ".exe";
}

}  // namespace cppboot::platform
