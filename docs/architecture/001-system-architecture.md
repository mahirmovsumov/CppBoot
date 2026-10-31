# 001: System architecture

**Status:** Accepted

## Goal

CppBoot is an application framework, not a collection of libraries. Every module follows one lifecycle and one configuration model. The core must work on its own for background services and command-line tools, with no web server included.

## Decisions

| Area | Decision |
|---|---|
| Language | C++20 minimum, with no C++23 features |
| Runtime | Native, with no VM and no required runtime reflection |
| Development platform | Windows, using GCC from MSYS2 UCRT64 |
| Supported platforms | Windows and Linux, both built from the start |
| Compilers | GCC and Clang now, MSVC later |
| Build | CMake 3.25+ with Ninja |
| Developer API | Macros, a code generator and templates |

Linux code can't be run on the development machine, so it is verified only by CI. Linux support isn't considered working until CI has run it.

## Dependency layers

Each layer may depend only on the layers below it. Modules in the same layer may not depend on each other unless a line below says so.

```text
Application code
      |
Feature modules      web, data, security, actuator
      |
cppboot-core         Application, lifecycle, modules, workers
      |
cppboot-config  cppboot-logging  cppboot-platform
      |
cppboot-base         Result, Error, Exception, version
```

`cppboot-di` depends only on `cppboot-base`, and the core doesn't depend on it. `cppboot-boot` sits above both and connects them. See [004](004-dependency-injection.md). `cppboot-security` and `cppboot-actuator` also use `cppboot-web`, because they work with requests and routes.

This replaces the original diagram, which had configuration and logging depending on core while `Application` in core needed both to start. The fix is a small base library at the bottom, with `Application` above the services it starts.

## Rules

1. Core never depends on web, data or security modules.
2. Optional features stay optional.
3. Public headers don't expose third-party types. yaml-cpp, spdlog, Boost, OpenSSL, libpq and SQLite are private dependencies. The one exception is `cppboot::web::Json`, which is `nlohmann::ordered_json`, because applications work with JSON directly and a wrapper would add a large API for no benefit.
4. Module dependency cycles are rejected at startup.
5. Modules start as folders with a few CMake targets. A module gets its own target only when something needs to use it separately.

## Developer API

C++20 can't list a class's constructor parameters or fields, so injection, routing, JSON mapping and repositories all depend on a source code generator. Annotation syntax such as `@RestController` isn't valid C++, so CppBoot uses one macro form:

```cpp
class CPPBOOT_CONTROLLER("/api/users") UserController {
public:
    CPPBOOT_GET("/{id}") User get(int id);
};
```

The macros compile to nothing in a normal build. When the generator parses the code, they become `[[clang::annotate(...)]]` attributes that it reads through libclang's stable C API.

Registration is generated code, which the CMake function `cppboot_generate_components()` compiles directly into the application. Self-registering static objects aren't used, for two reasons: linkers can drop them from static libraries, and they would break the rule against global mutable state.

The generator was the largest technical risk, so it got a prototype right after Phase 1 rather than waiting for Phase 4. See [009](009-code-generation.md) for the results.

## Execution model

Handlers must not run on the network event loop, because one blocking call would stall every request sharing that thread. They run on a worker pool. Handlers are ordinary functions, and there's no coroutine API for them yet.

## Error handling

- `Result<T>` is for expected failures such as I/O, parsing and validation.
- `Exception` is for bugs and for failures that make continuing impossible.
- A module may throw from `start()`. The framework catches it and fails startup cleanly.

## Scope of 1.0

CppBoot is built by one developer working with an AI assistant, so 1.0 is limited to:

- Core runtime
- Dependency injection
- Web framework, with HTTPS and rate limits
- PostgreSQL and SQLite access
- Basic JWT authentication
- Actuator
- CLI, with packaging and Linux services

Full ORM, Kafka, RabbitMQ, service discovery, OAuth2 and MSVC support come after 1.0. [What 1.0 promises](../stability.md) covers which of these stay stable.
