# CppBoot

CppBoot is a native application framework for C++20, inspired by the developer experience of Spring Boot. It aims to give C++ applications one lifecycle, one configuration model and one set of modules for services, web APIs, data access, security and operations.

**Status: early Phase 1, not ready for real applications.** See [the Phase 1 roadmap](docs/roadmap/phase-1.md).

## Build on Windows

1. Install MSYS2 from <https://www.msys2.org> into `D:\msys64`.
2. Install the compiler and libraries, then build and test from PowerShell:

```powershell
.\scripts\install-deps.ps1
. .\scripts\dev-env.ps1
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

## Create a project

Install CppBoot, then use the installed CLI. The compiler, CMake and Ninja must be on PATH.

```powershell
cmake --install build\dev --prefix D:\cppboot-sdk
$env:PATH = "D:\cppboot-sdk\bin;$env:PATH"

cppboot create hello-world
cd hello-world
cppboot run
```

Press Ctrl+C to stop the application. `cppboot run` builds the project first and returns the application's exit code. Anything after `run` goes to the application, such as `cppboot run --cppboot.logging.level=debug`.

## Run the example

The example reads `cppboot.yaml` from the current folder and runs until you press Ctrl+C:

```powershell
cd examples\hello-world
..\..\build\dev\examples\hello-world\hello-world.exe
```

## Build on Linux

```bash
sudo apt-get install -y ninja-build libyaml-cpp-dev libspdlog-dev libgtest-dev
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The `asan` and `tsan` presets run the same tests under the sanitizers and work on Linux only.

## Documentation

- [Getting started](docs/tutorials/getting-started.md)
- [All documentation](docs/README.md)
- [Changelog](CHANGELOG.md)
