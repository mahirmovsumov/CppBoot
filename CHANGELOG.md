# Changelog

This file records what has been added to CppBoot. Versions follow [semantic versioning](https://semver.org). Until 1.0, any release may change the API.

## Unreleased

### Command-line tool

- `cppboot create` with no name asks for the name and each option.
- `cppboot add <kind> <Name>`, which adds a service, controller, repository, middleware, module or health check to `src`. A repository also gets a migration. It never replaces a file, and checks that the project has what the kind needs.
- New projects have a `tests/` folder, with a test that checks every component can get what its constructor asks for.
- `cppboot test`, which builds and runs the tests, with `--sanitize` as for `build` and `run`.
- `cppboot dev`, which builds and runs the project, then stops it, builds it and starts it again whenever you save a change. Type `r` to restart, or `q` to quit.
- `ChildProcess` in the platform library, which starts a process without waiting for it, and stops it as Ctrl+C would.
- New projects have Visual Studio Code settings in `.vscode/`: tasks for build, dev, run and test, with compiler errors in the Problems panel, debugging with F5, and code completion from `build/compile_commands.json`. See [Editors](docs/guides/editors.md).
- `cppboot doctor` warns if GoogleTest or gdb is missing, and the Linux installer installs gdb.
- After creating a project with `--security`, `cppboot create` shows the environment variables to set before running it.
- **Fix:** on Windows, `cppboot dev` could wait minutes to start a build when its input was a pipe, because programs started while another thread reads a pipe they share have to wait for that read. The programs it starts now get empty input, through `ProcessOptions::share_input`.
- The CLI's code is split into one file per command.

### Settings structs and checks

- `CPPBOOT_CONFIG("prefix")` fills a struct from the settings under a prefix when the application starts, like Spring's `@ConfigurationProperties`. Fields can be bool, numbers, text, `std::chrono` durations such as `30s`, and `std::optional` or `std::vector` of those, and nested structs. The struct is a component that others take as `const T&`. See [settings as a struct](docs/guides/configuration.md#settings-as-a-struct).
- A missing `CPPBOOT_REQUIRED` setting, a value of the wrong type, a failed check, or a setting under the prefix that no field reads stops startup with exit code 78, listing every problem at once and suggesting the closest name for a misspelled key.
- Checks, like Bean Validation: `CPPBOOT_NOT_EMPTY`, `CPPBOOT_LENGTH`, `CPPBOOT_MIN`, `CPPBOOT_MAX` and `CPPBOOT_RANGE` on fields of `CPPBOOT_JSON` and `CPPBOOT_CONFIG` structs, and on path and query parameters. A failed check on a request is a 400 that names the field. A check that can't apply to its field fails the build. See [checking what clients send](docs/guides/web.md#checking-what-clients-send).
- Repositories written from their names, like Spring Data. Declare an interface with `CPPBOOT_REPOSITORY("table")` that derives from `cppboot::data::Repository<Row>`, with pure virtual functions such as `findByAuthorAndPriceLessThanOrderByTitleDesc`, `findFirstBy`, `findTop5By`, `countBy`, `existsBy` and `deleteBy`, and the generator writes and registers a class that implements them. Conditions include comparisons, `Between`, `Like`, `StartingWith`, `Containing`, `IsNull`, `True` and `IgnoreCase`, and `cppboot::data::Page` returns one page. `CPPBOOT_QUERY("...")` gives a function its own SQL. A name that refers to a missing field, a parameter of the wrong type or a return type that doesn't fit fails the build. See [repositories written from their names](docs/guides/database.md#repositories-written-from-their-names).
- An OpenAPI 3.0 document, which the build writes from the same declarations as the routes: paths, parameters, request bodies, response types, `CPPBOOT_JSON` schemas with checks as `minimum`, `maxLength` and so on, and bearer token security with `401` and `403`. `cppboot openapi` writes it to `openapi.json`, and `cppboot.openapi.enabled: true` serves it at `/openapi.json`. CI checks the documents with openapi-spec-validator. See [OpenAPI](docs/guides/web.md#openapi).
- New projects have a version, `0.1.0`, in `project()`, which the OpenAPI document uses.
- `cppboot add config <Name>` writes a settings struct.
- `cppboot add resource <Name> name:string price:double note:string?` writes a whole REST resource: a struct for JSON and rows, an input struct with checks, a repository, a controller with paged `GET`, `GET` by ID, `POST`, `PUT` and `DELETE`, a migration for the project's database, and tests that run the repository against a real database. See [the command-line guide](docs/guides/cli.md#cppboot-add-resource-name-fieldtype).
- `ErrorCode::InvalidConfiguration`, which `runApplication` turns into exit code 78 when it's thrown while components are registered.
- The test that new projects come with now reads environment variables too, so a required setting given that way doesn't fail it.

### Performance

- A request makes far fewer allocations. A plain `GET` with a browser's headers takes 5, where it took 30, and one with path parameters and a query string takes 9, where it took 38. The work between parsing a request and writing its response takes less than half the time: about 560 ns, where it took 1,320 ns, for the same `GET` through two middleware.
  - A request's headers are kept in one block of memory, not two strings for each.
  - Header fields are parsed into one piece of memory for each request, not one for each field.
  - A response's body is moved into what's written, not copied.
  - Finding a route and calling middleware don't allocate. Before, each route with as many segments as the path cost one, and each middleware cost one.
  - Query and path parameters are kept in a list, not a map.
- `Request::headerView` reads a header without copying it. Checking a token uses it.
- **Change:** `Request::headers()` returns a copy of the headers, as a `Headers`, where it returned a reference. `Request::create` takes a `RequestHeaders`, which a `Headers` converts to, so existing calls work.
- The logger writes several threads' lines with one system call when they arrive together, where it made one for each line. Every line is still written before the call that logged it returns, or before the call of a thread that was already waiting does.
- A test counts the allocations for each kind of request and fails if one goes over its budget, so the request path can't get slower without anyone noticing. See [making it faster without giving any of this up](docs/architecture/005-memory-management.md#making-it-faster-without-giving-any-of-this-up).

### Fewer dependencies

- Settings files no longer use yaml-cpp. CppBoot's own reader handles the YAML that settings use, and names the line of anything else. See [what a settings file can contain](docs/guides/configuration.md#what-a-settings-file-can-contain).
- **Change:** a key written twice in the same map is now an error. Before, the last one silently won.
- JSON no longer uses nlohmann/json. **Change:** `cppboot::web::Json` is now `cppboot::Json`, CppBoot's own type, with a smaller API: `parse`, `tryParse`, `dump`, `at`, `find`, `[]`, `push`, `get<T>()`, and `as...` and `is...` functions. See [working with JSON directly](docs/guides/web.md#working-with-json-directly). `CPPBOOT_JSON` structs work as before.
- **Change:** request bodies are read strictly. A repeated key, invalid UTF-8, nesting deeper than 128 and numbers that don't fit their field are rejected with a 400. Errors name the field, such as `field 'name': expected a string, but found a number`.
- A fuzz job in CI feeds random input to the settings and JSON readers for two minutes each on every push.
- Logging no longer uses spdlog and {fmt}. CppBoot's own logger writes the same lines, with the level in color on a terminal, and turns color off for `NO_COLOR`, pipes and files. Nothing changes for applications.

### Health and metrics

- **Change:** with no `cppboot.actuator.metrics_token`, `/actuator/metrics` and `/actuator/info` in a project without security now answer only clients on the same machine, and not requests passed on by a proxy. Before, they were open to anyone who could reach the server. Set a token to collect metrics from another machine.

### Testing

- CI's stress test now also runs under ThreadSanitizer, and both stress runs use four network threads.

### Documentation

- Architecture records 005, 006 and 008, which were planned from the start but never written: memory management, the concurrency model, and the security architecture as a whole.
- [How CppBoot works](docs/architecture/README.md), a guide to the architecture as a whole: the libraries and how they depend on each other, what happens at build time, at startup, during a request and at shutdown, with diagrams, and where each decision is recorded.
- Architecture records 017, 018 and 019: configuration, JSON, and the command-line tool and SDK.
- Record 001 has the current dependency layers.

## 0.1.0, pre-release, 2026-11-04

The first release, marked as a pre-release because the API can still change, and CppBoot hasn't had an independent security review.

### Installation

- Installers for Windows and Ubuntu 24.04, which set up the compiler and libraries, download the SDK, check it against its checksums, and run `cppboot doctor`. On Windows, they install MSYS2 if needed.
- `cppboot doctor`, which checks the installation, the tools and the libraries together, and says how to fix each problem. `--full` also builds and runs a sample project.
- Downloadable SDKs, built and tested on clean machines by a Release workflow. The Windows SDK carries the libraries its own programs need, so it works before anything is on PATH.
- `CPPBOOT_TOOLCHAIN`, a folder that `cppboot` puts first on PATH for the programs it starts.
- **Fix:** with MinGW, projects now find PostgreSQL, SQLite and OpenSSL in the compiler's own folder first. Before this, a Windows machine with PostgreSQL installed could find its copy, built for Microsoft's compiler, and fail to link.

### License

- CppBoot is now licensed under the Apache License 2.0, with `NOTICE`, and `THIRD_PARTY.md` listing the projects it uses.
- The SDK installs these in `share/doc/cppboot`, and installs LLVM's license with Clang's headers where it can find it.

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
- Components can take the application's `Config` and `Logger` in their constructors, with no registration.

### Code generator

- `cppboot-codegen`, which reads classes marked with `CPPBOOT_SERVICE` and generates code that registers them with the container.
- Build-time errors for missing dependencies, duplicate providers and dependency cycles.
- Installed with the SDK, along with `cppboot_generate_components()` for running it during a build.
- Projects made by `cppboot create` now use it: add a marked class in `src`, and it's created and injected with no registration code.

### Web server

- `cppboot::web::Server`, an HTTP/1.1 server where handlers run on a worker pool, so a slow handler can't block other requests.
- `Router`, with path and query parameters, 404 and 405 responses, and `HEAD` support.
- Middleware that runs around every request, and can stop it or change its response.
- `HttpError` for sending errors to clients. Other exceptions become a generic 500, so that internal details can't reach them.
- Limits on body size, connections and time, plus protection against header injection and encoded null characters.
- Graceful shutdown, which lets running requests finish.
- `ServerModule`, which runs the server within an application using settings under `cppboot.server`.
- `CPPBOOT_JSON`, which generates conversion between structs and JSON, including nested structs, containers and optional fields.
- `CPPBOOT_CONTROLLER` with `CPPBOOT_GET`, `CPPBOOT_POST`, `CPPBOOT_PUT`, `CPPBOOT_PATCH` and `CPPBOOT_DELETE`, which turn marked functions into routes. Parameters come from the path, query string or JSON body, and return values become JSON.
- `WithStatus<T>`, for sending a JSON value with a status other than 200.
- `cppboot create --web`, which creates a project with a sample controller.

### Database

- `cppboot::data::Database`, with PostgreSQL and SQLite drivers behind one interface.
- A connection pool with limits, timeouts and health checks.
- Parameterized queries using `$1`, `$2` and so on for both databases, which prevents SQL injection.
- `transaction()`, which commits when a function returns and rolls back when it throws.
- `CPPBOOT_ROW`, which generates reading a struct from a query result.
- Migrations: numbered SQL files applied at startup, each in a transaction, with checks for edited, missing and out-of-order files.
- `DatabaseError`, with a kind for each failure, such as a unique violation.
- Components can take `Database` in their constructors, and it's set up automatically from settings under `cppboot.database`.
- `cppboot create --web --sqlite` and `--postgresql`.
- `cppboot.database.url` is hidden in configuration logs, and left out of CppBoot's error messages.

### Security

- `TokenService`, which issues and verifies JSON Web Tokens signed with HS256.
- `CPPBOOT_PUBLIC` and `CPPBOOT_ROLE`. Once a project uses security, every other route needs a valid token, so a forgotten mark leaves a route locked.
- `Principal` parameters on route functions, which say who sent a request.
- `hashPassword` and `verifyPassword`, using salted scrypt.
- `CPPBOOT_MIDDLEWARE`, for middleware in projects that use controllers.
- The application won't start with a missing or short signing secret, and security marks that would do nothing fail the build.
- `cppboot create --web --security`.

### Health and metrics

- `/actuator/health`, with automatic database checks, and `HealthIndicator` for your own.
- `/actuator/health/live`, for orchestrator liveness checks.
- `/actuator/metrics` in Prometheus format, and `/actuator/info`, protected by a token.
- `Metrics`, with counters, gauges and histograms that any component can use.
- `Request::route()`, which gives the matched route pattern.
- `cppboot create --actuator`, and `cppboot_generate_components(... ACTUATOR ...)`.
- All `cppboot create` options can now be combined.

### Rate limits

- `CPPBOOT_RATE_LIMIT("10/minute")` on a route or controller, which sends a 429 with `Retry-After` to a client over its limit, before the route's function or token check runs. A limit that can't be read fails the build.
- `RateLimiter` and `rateLimit()`, for hand-written routes and for middleware.
- Memory is bounded, with settings under `cppboot.rate_limit`.
- `cppboot create --security` now limits its sign-in route.
- `Request::clientAddress()`, with `cppboot.server.trusted_proxies` for servers behind a proxy. `X-Forwarded-For` is ignored unless it comes from a listed proxy.

### HTTPS

- `cppboot.server.tls.certificate_file` and `private_key_file`, which make the server use HTTPS. Any mistake in them stops startup, so it can't fall back to plain HTTP.
- TLS 1.2 and 1.3 only, with `cppboot.server.tls.min_version` to require 1.3. TLS 1.2 is limited to ciphers with forward secrecy and authenticated encryption, and compression and renegotiation are refused.
- A warning when a server listens on a public address with plain HTTP and no proxy.
- `cppboot-web` now needs OpenSSL, including in projects that don't use HTTPS.
- Connections now set `TCP_NODELAY`.
- The load generator can test HTTPS and new connections, and reports the first error it sees.

### Ownership and memory safety

- Components can take their dependencies as plain references: `OrderService(InventoryService& inventory)`. `std::shared_ptr` still works, and both can be mixed.
- An instance now keeps its dependencies alive until it's destroyed, so a reference is safe for any lifetime.
- Dependencies are now always created from left to right, on every compiler.
- A component with a field pointing at a `Request`, `Response` or `Principal` fails the build.
- New warnings from the generator: state that threads can change with no lock, views stored in a component, copies of a dependency, and routes that return a view.
- `CPPBOOT_UNCHECKED("reason")`, which accepts a risky field.
- `cppboot build --sanitize=address` and `--sanitize=thread`, and the same for `run`, on Linux.
- New projects turn on `_GLIBCXX_ASSERTIONS`, `-Wall` and `-Wextra`, and take plain references in their samples.
- `CPPBOOT_LIFETIMEBOUND`, on request accessors and `Result::value()`, which lets Clang warn about dangling references.

### Deployment

- `cppboot package`, which builds a release with its shared libraries, settings and migrations, and an archive of them all.
- On Linux, a package includes a systemd service, and scripts that install, upgrade and remove it. The service runs under its own account with limited access to the system.
- Applications tell systemd when they're ready and when they're stopping.

### Faster, more accurate code generation

- The generator now reads headers with the target's definitions and options, as well as its include folders. Before this, a class inside `#ifdef` was found or missed differently from how the compiler saw it.
- Each header is read in its own build step, and again only when it, or a file it includes, changes. In a project of 50 headers, a rebuild after changing one went from 80 seconds to 15.
- A change in a file that a header includes, such as one outside `src`, now runs the generator again. Before this, it was missed.

### Server threads and lifecycle

- `cppboot.server.io_threads`: connections now share several network threads, by default one for every two cores, from 1 to 4. On 4 logical cores, this more than doubled throughput, and a flood of TLS handshakes hurt other connections about 9 times less.
- `Request::shouldStop()`, `stopToken()` and `deadline()`, with `cppboot.server.handler_deadline_seconds`, so that a long-running handler can stop when asked. After the shutdown timeout, the server asks handlers to stop, and logs how many are still running.
- **Fix:** `Server::start()` can now be called again after it fails, such as once a port is free. Before this, it reported that the server had already started.

### Stress tests and overload

- `cppboot-stress-client`, `scripts/stress-linux.sh` and a Stress workflow, which check responses, memory, open files and threads over long runs.
- A request that waits longer than `cppboot.server.queue_timeout_seconds` for a worker now gets a 503, and its handler doesn't run. Before this, an overloaded server could keep well-behaved clients waiting for over 30 seconds.
- A request that waited for a worker no longer runs if its client has gone by then. Before this, clients could keep every worker busy by sending slow requests and leaving at once.

### Preparing for 1.0

- [What 1.0 promises](docs/stability.md), a [release checklist](docs/release-checklist.md) and a [security policy](SECURITY.md).
- CI builds the code in the tutorials and guides, checks links in the documentation, and checks that each public header compiles by itself.

### Fixes

- Settings with underscores in their names can now be set from the environment, even when they aren't in YAML. For example, `CPPBOOT_SERVER_MAX_CONNECTIONS` had no effect before, unless the key was also in `cppboot.yaml`.

- Routes with no parameters no longer generate an unused variable, which broke builds using `-Werror`.
- New connections no longer wait for seconds, or get refused, when the server is under load.

### Benchmarks

- A benchmark server, a load generator, a script for one core with a 2 GB limit, and a workflow to run it on GitHub. See the [results](docs/benchmarks.md).

### Project setup

- CMake presets for debug, release and the sanitizers.
- CI on Linux with GCC, Clang, AddressSanitizer, UBSan and ThreadSanitizer, and on Windows with GCC.
- Unit tests, plus an end-to-end test that installs the framework, then creates, builds and runs a project.
- Architecture documents, a coding standard and user guides.
