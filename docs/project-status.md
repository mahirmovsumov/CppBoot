# Project status

This page explains what CppBoot can do today, how it got here and what comes next. For a list of every addition, see the [changelog](../CHANGELOG.md).

## What CppBoot is

CppBoot is a framework for building C++20 applications, inspired by Spring Boot. It handles startup, settings, logging, object creation and shutdown, so that developers can focus on their own code. It runs natively, with no virtual machine.

## What works today

You can build web APIs, background services and command-line applications on Windows and Linux. A new project takes three commands:

```text
cppboot create my-app
cd my-app
cppboot run
```

To add functionality, you add a class to `src` and mark it. CppBoot creates it and passes in what it needs, with no registration code:

```cpp
class CPPBOOT_SERVICE UserService {
public:
    UserService(UserRepository& repository, const cppboot::logging::Logger& logger);
};
```

There's no ownership to manage, because CppBoot keeps each dependency alive for as long as the class using it exists. A mistake such as a missing dependency fails the build, so it can't reach a running application. The build also warns about risky patterns, and new projects have [tools for finding memory bugs](guides/memory-safety.md). C++ can't guarantee memory safety as Rust does, so code that compiles can still have these bugs.

A web API works the same way, with `cppboot create my-api --web`. Marked functions become routes, and values convert to and from JSON automatically:

```cpp
class CPPBOOT_CONTROLLER("/api/users") UserController {
public:
    explicit UserController(UserService& users);

    CPPBOOT_GET("/{id}") User get(int id) const;
};
```

To store data, add `--sqlite` or `--postgresql`. Mark a struct to read it from query results, and CppBoot applies your migrations at startup:

```cpp
struct CPPBOOT_ROW User {
    std::int64_t id{0};
    std::string name;
};

auto user = database_->queryOne<User>("SELECT * FROM users WHERE id = $1", id);
```

To protect an API, add `--security`. Once a project uses it, every route needs a valid token unless you mark it public, so a forgotten mark leaves a route locked:

```cpp
CPPBOOT_PUBLIC CPPBOOT_RATE_LIMIT("10/minute") CPPBOOT_POST("/login")
TokenReply login(const Credentials& credentials) const;
CPPBOOT_GET("/me") Profile me(const cppboot::security::Principal& who) const;
CPPBOOT_ROLE("admin") CPPBOOT_GET("/report") Report report() const;
```

To monitor it, add `--actuator`, for health checks and Prometheus metrics. All of these combine:

```text
cppboot create banking-api --web --postgresql --security --actuator
```

To serve HTTPS, give the server a certificate and a key in its settings, or put a proxy in front of it.

To deploy it, run `cppboot package`. The result runs on another machine with no SDK, and on Linux it installs as a [systemd service](guides/deployment.md) with limited access to the system.

## Progress

| Phase | Status | Result |
|---|---|---|
| 1. Core runtime and CLI | Complete | Applications start, read settings, log and shut down cleanly |
| 2. Dependency injection | Complete | Marked classes are created and connected automatically |
| 3. Web framework | Complete | Controllers, JSON, routing, middleware and an HTTP server |
| 4. Developer experience | Mostly done early | The CLI and generator were needed for Phases 1 and 2 |
| 5. Database | Complete | PostgreSQL, SQLite, connection pooling, transactions, migrations |
| 6. Security | Complete | JWT sign-in, roles, password hashing, deny by default |
| 7. Enterprise integration | After 1.0 | Redis, Kafka, RabbitMQ and similar |
| 8. Observability | Complete | Health checks and Prometheus metrics |
| 9. Production readiness | Complete | Rate limiting, HTTPS, memory safety tools, Linux services and stress tests |

## What has been built

| Part | What it does | Guide |
|---|---|---|
| Application runtime | Runs the startup and shutdown sequence, with clean-up on every failure | [Modules](guides/modules.md) |
| Modules and workers | Split an application into parts, and run background threads safely | [Modules](guides/modules.md) |
| Configuration | Reads YAML, profiles, environment variables and command-line arguments | [Configuration](guides/configuration.md) |
| Logging | Writes to the console and to a file from any thread | [Configuration](guides/configuration.md) |
| Dependency injection | Creates components and manages their lifetimes | [Dependency injection](guides/dependency-injection.md), [Ownership and memory safety](guides/memory-safety.md) |
| Code generator | Reads marked classes and generates their registration | [Code generation](architecture/009-code-generation.md) |
| `cppboot` CLI | Creates, builds, runs and packages projects | [CLI](guides/cli.md), [Deployment](guides/deployment.md) |
| Web framework | Serves HTTP and HTTPS with controllers, JSON, routes, middleware and error handling | [Web server](guides/web.md) |
| Database | Connects to PostgreSQL and SQLite, with pooling, transactions and migrations | [Database](guides/database.md) |
| Security | Signs users in with tokens, protects routes by default, and limits request rates | [Security](guides/security.md) |
| Actuator | Reports health and metrics | [Health and metrics](guides/actuator.md) |

## How it's tested

Every push runs on Linux with GCC and Clang, on Linux with memory and thread checkers (AddressSanitizer, UBSan and ThreadSanitizer), and on Windows with GCC. The database tests run against SQLite everywhere, and against a real PostgreSQL server on Linux.

The tests include an end-to-end test that installs the framework, creates a new project with the CLI, adds a marked class, then builds, runs and packages it. Another CI job installs a real application as a systemd service, crashes, upgrades and removes it, and runs its package in a clean container. It also builds the code printed in the tutorials and guides. A short stress run under the memory checker runs on every push, and long ones on request. A real Ctrl+C on Windows can't be sent automatically in CI, so that's checked by hand.

## Performance

On one Linux cloud core, CppBoot served about 37,000 small requests per second. With a 2 GB limit, 150,000 open connections stayed open and working, or 25,000 with HTTPS. The [benchmarks](benchmarks.md) give full results and their limits, and no other framework has been measured for comparison.

Over 30 minutes in each of 5 builds, [stress tests](benchmarks.md#stress-tests) checked about 20 million results, with none unexpected, and memory stayed flat. Earlier runs found two ways that overload could stop the server from serving, which are fixed.

## Before 1.0

All the planned phases for 1.0 are complete, but 1.0 hasn't been released. The [release checklist](release-checklist.md) lists what's left, and these need the owner's decision: a license, an independent security review, whether to make the repository public, and how people get the SDK.

## Main decisions

- **C++20 only:** no later language features are required.
- **Windows and Linux from the start:** development happens on Windows, and CI verifies Linux.
- **A code generator, not runtime reflection:** C++20 can't inspect classes at runtime, so a tool reads the source during the build. Applications keep native performance, and wiring mistakes become build errors.
- **Modules stay independent:** the core doesn't depend on dependency injection or web code, so a small service only pays for what it uses.
- **A smaller 1.0:** with one developer, 1.0 covers the core, dependency injection, web, PostgreSQL, basic JWT authentication, actuator and the CLI. Full ORM, Kafka, RabbitMQ and OAuth2 come after 1.0.

The [system architecture](architecture/001-system-architecture.md) explains these in more detail.

## Known limits

- The SDK isn't a standalone download, because it needs the compiler, libraries and libclang installed. Packages of your application don't need any of this.
- A package includes its own copies of libraries such as OpenSSL, so a security fix in one means building a new package.
- A Windows package can't install as a service.
- Marked classes are always singletons. Transient and scoped components need manual registration.
- Every header in `src` adds time to the build, because the generator parses each one.
- Tokens can't be revoked, so they stay valid until they expire.
- An open HTTPS connection uses 76 to 95 KB, against 4 to 6 KB for plain HTTP.
- A handler that doesn't check `request.shouldStop()` can't be stopped, so it can hold up shutdown. A blocking call such as a database query needs its own timeout.
- A renewed certificate needs a restart, and there's no HTTP/2.
- Rate limits only apply to routes you mark, count per address and per process, and reset when the application restarts.
- A struct used with a query but not marked with `CPPBOOT_ROW` fails when it runs, not when it builds.
