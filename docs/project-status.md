# Project status

This page explains what CppBoot can do today, how it got here and what comes next. For a list of every addition, see the [changelog](../CHANGELOG.md).

## What CppBoot is

CppBoot is a framework for building C++20 applications, inspired by Spring Boot. It handles startup, settings, logging, object creation and shutdown, so that developers can focus on their own code. It runs natively, with no virtual machine.

## What works today

You can build background services and command-line applications on Windows and Linux. A new project takes three commands:

```text
cppboot create my-app
cd my-app
cppboot run
```

To add functionality, you add a class to `src` and mark it. CppBoot creates it and passes in what it needs, with no registration code:

```cpp
class CPPBOOT_SERVICE UserService {
public:
    UserService(std::shared_ptr<UserRepository> repository,
                std::shared_ptr<const cppboot::logging::Logger> logger);
};
```

A mistake such as a missing dependency fails the build, so it can't reach a running application.

You can also serve HTTP requests by defining routes in code, with middleware, error handling and limits on request size and time. Marked controllers and automatic JSON conversion are still to come, and there's no HTTPS or database support yet.

## Progress

| Phase | Status | Result |
|---|---|---|
| 1. Core runtime and CLI | Complete | Applications start, read settings, log and shut down cleanly |
| 2. Dependency injection | Complete | Marked classes are created and connected automatically |
| 3. Web framework | In progress | The server and routing work. JSON and controllers are next |
| 4. Developer experience | Mostly done early | The CLI and generator were needed for Phases 1 and 2 |
| 5. Database | Not started | PostgreSQL, SQLite, connection pooling, transactions |
| 6. Security | Not started | Basic JWT authentication for 1.0 |
| 7. Enterprise integration | After 1.0 | Redis, Kafka, RabbitMQ and similar |
| 8. Observability | Not started | Health and metrics endpoints for 1.0 |
| 9. Production readiness | Not started | Stress testing, packaging and service integration |

## What has been built

| Part | What it does | Guide |
|---|---|---|
| Application runtime | Runs the startup and shutdown sequence, with clean-up on every failure | [Modules](guides/modules.md) |
| Modules and workers | Split an application into parts, and run background threads safely | [Modules](guides/modules.md) |
| Configuration | Reads YAML, profiles, environment variables and command-line arguments | [Configuration](guides/configuration.md) |
| Logging | Writes to the console and to a file from any thread | [Configuration](guides/configuration.md) |
| Dependency injection | Creates components and manages their lifetimes | [Dependency injection](guides/dependency-injection.md) |
| Code generator | Reads marked classes and generates their registration | [Code generation](architecture/009-code-generation.md) |
| `cppboot` CLI | Creates, builds and runs projects | [CLI](guides/cli.md) |
| Web server | Serves HTTP with routes, middleware and error handling | [Web server](guides/web.md) |

## How it's tested

Every push runs on Linux with GCC and Clang, on Linux with memory and thread checkers (AddressSanitizer, UBSan and ThreadSanitizer), and on Windows with GCC.

The tests include an end-to-end test that installs the framework, creates a new project with the CLI, adds a marked class, then builds and runs it. A real Ctrl+C on Windows can't be sent automatically in CI, so that's checked by hand.

## Main decisions

- **C++20 only:** no later language features are required.
- **Windows and Linux from the start:** development happens on Windows, and CI verifies Linux.
- **A code generator, not runtime reflection:** C++20 can't inspect classes at runtime, so a tool reads the source during the build. Applications keep native performance, and wiring mistakes become build errors.
- **Modules stay independent:** the core doesn't depend on dependency injection or web code, so a small service only pays for what it uses.
- **A smaller 1.0:** with one developer, 1.0 covers the core, dependency injection, web, PostgreSQL, basic JWT authentication, actuator and the CLI. Full ORM, Kafka, RabbitMQ and OAuth2 come after 1.0.

The [system architecture](architecture/001-system-architecture.md) explains these in more detail.

## Known limits

- The SDK isn't a standalone download, because it needs the compiler, libraries and libclang installed.
- Marked classes are always singletons. Transient and scoped components need manual registration.
- Every header in `src` adds time to the build, because the generator parses each one.
