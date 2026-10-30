# Phase 1: Core runtime

**Status:** Complete. CI passes on Linux with GCC, Clang and the sanitizers, and on Windows with GCC.

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

## Not tested automatically

CI covers everything above except a real Ctrl+C on Windows, which was checked by hand for both `hello-world` and `cppboot run`.

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
