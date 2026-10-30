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
- Application settings with any key name can be overridden if they're already set, such as `myapp.timeout` with `CPPBOOT_MYAPP_TIMEOUT` or `--myapp.timeout=5`.
- A selected profile must exist, so a typo can't silently run the wrong settings.
- Typed getters that report invalid values, and hidden values for sensitive keys in configuration dumps.

### Logging

- Thread-safe console and file logging, with no global state.

### Command-line tool

- `cppboot create`, `build`, `run` and `info`.
- CMake install rules, so projects can use `find_package(CppBoot)`.

### Dependency injection

- `cppboot::di::Container`, with singleton, transient, scoped and external lifetimes.
- Registration by declared dependencies, factories, existing instances and interface bindings.
- `validate()`, which finds missing dependencies, cycles and singletons that depend on scoped components, without creating anything.
- Groups for interfaces with many implementations, with `bindAll` and `resolveAll`.
- Clear errors instead of a stack overflow when resolving a cycle, and thread-safe resolution.
- `cppboot::runApplication()`, which registers components, validates them and starts every component that's a module.

### Code generator

- `cppboot-codegen`, which reads classes marked with `CPPBOOT_SERVICE` and generates code that registers them with the container.
- Build-time errors for missing dependencies, duplicate providers and dependency cycles.
- Installed with the SDK, along with `cppboot_generate_components()` for running it during a build.
- Projects made by `cppboot create` now use it: add a marked class in `src`, and it's created and injected with no registration code.

### Project setup

- CMake presets for debug, release and the sanitizers.
- CI on Linux with GCC, Clang, AddressSanitizer, UBSan and ThreadSanitizer, and on Windows with GCC.
- Unit tests, plus an end-to-end test that installs the framework, then creates, builds and runs a project.
- Architecture documents, a coding standard and user guides.
