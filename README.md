# CppBoot

CppBoot is a native application framework for C++20, inspired by the developer experience of Spring Boot. It gives C++ applications one lifecycle, one configuration model and a set of modules for web APIs, data access, security and operations.

**Status: feature complete for 1.0, but not released.** It has had long stress tests, but no independent security review, and its API can still change. See [the project status](docs/project-status.md).

```cpp
class CPPBOOT_CONTROLLER("/api/users") UserController {
public:
    explicit UserController(UserRepository& users) : users_(users) {}

    CPPBOOT_GET("/{id}") User get(std::int64_t id) const { return users_.find(id); }

private:
    UserRepository& users_;
};
```

That's a whole route. CppBoot creates the class and passes in its dependencies, reads `id` from the path, and sends the result as JSON, with no registration code. A mistake such as a missing dependency fails the build.

## What's included

- **Web:** HTTP and HTTPS, controllers, JSON, middleware and rate limits
- **Data:** PostgreSQL and SQLite, connection pooling, transactions and migrations
- **Security:** token sign-in, roles, password hashing, and routes that are protected by default
- **Operations:** health checks, Prometheus metrics, and packages that install as a Linux service
- **Tools:** a CLI, build-time checks, and [checkers that find memory bugs](docs/guides/memory-safety.md)

```text
cppboot create banking-api --web --postgresql --security --actuator
```

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

## Build on Linux

```bash
sudo apt-get install -y ninja-build libyaml-cpp-dev libspdlog-dev libgtest-dev libclang-dev libboost-dev nlohmann-json3-dev libpq-dev libsqlite3-dev libssl-dev
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The `asan` and `tsan` presets run the same tests under the sanitizers, and work on Linux only.

## Create a project

Install CppBoot, then use the installed CLI. The compiler, CMake and Ninja must be on PATH.

```powershell
cmake --preset release
cmake --build --preset release
cmake --install build\release --prefix D:\cppboot-sdk
$env:PATH = "D:\cppboot-sdk\bin;$env:PATH"

cppboot create hello-world
cd hello-world
cppboot run
```

Press Ctrl+C to stop the application. To deploy it, run `cppboot package`.

## Documentation

- [Getting started](docs/tutorials/getting-started.md)
- [All documentation](docs/README.md)
- [Project status](docs/project-status.md)
- [What 1.0 promises](docs/stability.md)
- [Security policy](SECURITY.md)
- [Changelog](CHANGELOG.md)

## License

CppBoot has no license yet, so it can't be used or copied without the owner's permission.
