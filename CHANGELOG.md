# Changelog

This file records what has been added to CppBoot. Versions follow [semantic versioning](https://semver.org). Until 1.0, any release may change the API.

## Unreleased: 0.1.0

### Application runtime

- `cppboot::Application`, the framework entry point, which owns the lifecycle, configuration, logging and modules.
- A lifecycle state machine with a `Failed` state, so every failure path still ends in cleanup.
- Exit codes for success (0), failure (1) and invalid configuration (78).
- Graceful shutdown on Ctrl+C on Windows and on SIGINT or SIGTERM on Linux.

### Modules and workers

- `cppboot::Module`, with startup in dependency order and shutdown in reverse order.
- Startup is rejected on duplicate names, missing dependencies and dependency cycles.
- `WorkerGroup`, which manages background threads with cooperative cancellation. A worker that throws shuts the application down with a failure exit code.

### Configuration

- YAML files, profiles, environment variables and command-line arguments, in a fixed order of precedence.
- A selected profile must exist, so a typo can't silently run the wrong settings.
- Typed getters that report invalid values, and hidden values for sensitive keys in configuration dumps.

### Logging

- Thread-safe console and file logging, with no global state.

### Command-line tool

- `cppboot create`, `build`, `run` and `info`.
- CMake install rules, so projects can use `find_package(CppBoot)`.

### Project setup

- CMake presets for debug, release and the sanitizers.
- CI on Linux with GCC, Clang, AddressSanitizer, UBSan and ThreadSanitizer, and on Windows with GCC.
- Unit tests, plus an end-to-end test that installs the framework, then creates, builds and runs a project.
- Architecture documents, a coding standard and user guides.
