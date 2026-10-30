# CppBoot

CppBoot is a native application framework for C++20, inspired by the developer experience of Spring Boot. It aims to give C++ applications one lifecycle, one configuration model and one set of modules for services, web APIs, data access, security and operations.

**Status: early Phase 1, not ready for real applications.** The runtime foundation builds and passes its tests on Windows. Linux hasn't been built yet. See [the Phase 1 roadmap](docs/roadmap/phase-1.md).

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

- [System architecture](docs/architecture/001-system-architecture.md)
- [Core runtime](docs/architecture/002-core-runtime.md)
- [Module system](docs/architecture/003-module-system.md)
- [Platform abstraction](docs/architecture/007-platform-abstraction.md)
- [C++20 coding standard](docs/standards/cpp20-coding-standard.md)
- [Phase 1 roadmap](docs/roadmap/phase-1.md)
