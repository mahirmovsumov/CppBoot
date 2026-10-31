# 007: Platform abstraction

**Status:** Accepted

## Rule

Operating system headers and calls appear only in `framework/platform/windows` and `framework/platform/linux`. Everything else uses the interfaces in `framework/platform/include`.

## Current interfaces

| Interface | Windows | Linux |
|---|---|---|
| `environmentVariables()` | `GetEnvironmentStringsW`, converted to UTF-8 | `environ` |
| `ShutdownSignal` | Console control handler and event objects | `sigaction` for SIGINT and SIGTERM, with a semaphore |
| `runProcess()` | `CreateProcessW`, with a handler that waits for the child | `fork` and `execvp`, forwarding SIGINT and SIGTERM to the child |
| `executablePath()` | `GetModuleFileNameW` | `/proc/self/exe` |
| `residentMemoryBytes()` | `GetProcessMemoryInfo` | `/proc/self/statm` |
| `notifyServiceManager()` | Does nothing | A datagram to `NOTIFY_SOCKET`, with no dependency on libsystemd |

## Platform differences

- **Closing a Windows console:** Windows ends the process when the handler for a close, logoff or shutdown event returns. The handler therefore holds the process open until shutdown finishes, for up to five seconds.
- **Windows services:** these use a different control mechanism from console events, and aren't supported yet. Linux services use the normal signals.
- **Sanitizers:** MinGW GCC doesn't provide AddressSanitizer or ThreadSanitizer, so memory and thread checks run only on Linux, and `cppboot build --sanitize` fails on Windows.
