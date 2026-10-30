# Phase 1: Core runtime

**Status:** In progress. The runtime and CLI work on Windows with GCC 16.2. The Linux process code and the installed CLI haven't been run on Linux yet.

## Done

- CMake project and presets
- `cppboot-base`: `Result`, `Error`, `Exception`, version
- `cppboot-platform`: environment and shutdown signals for Windows
- `cppboot-config`: YAML, profiles, environment and command-line overrides
- `cppboot-logging`: console and file logging
- `cppboot-core`: `Application`, lifecycle, module manager, workers
- `cppboot-platform` for Linux: environment and shutdown signals, with CI on GCC, Clang and the sanitizers
- CMake install rules, so projects can use `find_package(CppBoot)`
- CLI: `cppboot create`, `build`, `run` and `info`
- An end-to-end test that installs the framework, then creates, builds and runs a project
- `examples/hello-world` and unit tests

## Remaining

1. Pass CI on Linux and Windows with the CLI and the Linux process code.

## Done when

```text
cppboot create hello-world
cd hello-world
cppboot build
cppboot run
```

The application must load its configuration, start its modules, handle Ctrl+C on Windows and SIGTERM on Linux, shut down gracefully, release all resources and exit with the correct status.

## Next

Prototype the code generator with libclang, then begin Phase 2, dependency injection.
