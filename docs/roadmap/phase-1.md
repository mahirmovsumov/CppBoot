# Phase 1: Core runtime

**Status:** In progress. The runtime works on Windows with GCC 16.2. The Linux code and the CI workflow haven't been run yet.

## Done

- CMake project and presets
- `cppboot-base`: `Result`, `Error`, `Exception`, version
- `cppboot-platform`: environment and shutdown signals for Windows
- `cppboot-config`: YAML, profiles, environment and command-line overrides
- `cppboot-logging`: console and file logging
- `cppboot-core`: `Application`, lifecycle, module manager, workers
- `examples/hello-world` and 46 passing unit tests on Windows
- `hello-world` shuts down cleanly on a real console Ctrl+C with exit code 0

## Written but not run

- `cppboot-platform` for Linux
- CI workflow for Linux and Windows

## Remaining

1. Add CMake install rules so generated projects can use `find_package(CppBoot)`.
2. Write the CLI: `cppboot create`, `build`, `run` and `info`.
3. Push to GitHub and pass CI on Linux, including the sanitizers.

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
