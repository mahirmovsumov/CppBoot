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
