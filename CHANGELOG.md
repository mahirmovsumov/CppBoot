# Changelog

This file records what has been added to CppBoot. Versions follow [semantic versioning](https://semver.org). Until 1.0, any release may change the API.

## Unreleased: 0.1.0

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
