# 013: Observability

**Status:** Accepted.

## Opt-in, like a starter

A project asks for the actuator with `ACTUATOR` in `cppboot_generate_components`, or `--actuator` in `cppboot create`. That links `cppboot-actuator`, and tells the generator to register it. Projects that don't ask for it have no routes, no per-request cost and no extra code.

With it, the generator creates a router and `ServerModule` even with no controllers, so a background service can have health checks. Using `Metrics` or `HealthIndicator` without it fails the build, with a message saying what to add.

## Health

- **Public, with no details.** Load balancers can't send tokens, so this route is public, and that's why it reports only `UP` or `DOWN`. Failure messages can hold addresses or credentials, so they go to the log.
- **Two routes.** `/actuator/health/live` ignores checks. An orchestrator restarts a process that fails its liveness check, which can't fix a database that's down, and would restart every instance at once.
- **Checks run on request.** There's no cache or background thread. This keeps results current and the design simple, with the cost that each check must be quick.

## Metrics

### Series limits

Every label combination uses memory until the process exits. HTTP metrics therefore use `Request::route()`, the matched pattern, and put everything else under `route="unmatched"`. Using the path would let any client exhaust memory by inventing URLs.

### No `std::shared_mutex`

The first version used a read-write lock for looking up series. On MinGW, `pthread_rwlock_rdlock` returned an error under contention, which crashed about 60% of runs of a threaded test. Without assertions, a failed lock could have corrupted the map instead. `Metrics` uses `std::mutex`, which is held only for a map lookup. Measured overhead is about 4% per request with one worker. The coding standard now rules out `std::shared_mutex`.

### Cost

Each request does two lookups, each building a key string. Holding references would avoid this for fixed metrics, but HTTP metrics depend on the route and status of each request. Caching per route could reduce the cost, if benchmarks ever show it matters.

Counters and histograms themselves use relaxed atomics, since a collection doesn't need an exact moment in time.

## Access to metrics

The rule for metrics follows deny by default: in a project with security, they stay locked until a token is set, and in any project, a configured token is required. The token is separate from JWTs, because Prometheus sends a fixed credential, and a JWT would expire. It's compared in constant time.

`cppboot-actuator` doesn't depend on `cppboot-security`, so the generator passes whether the project uses security as a flag.

## Middleware order

`mount()` adds its middleware first, so it wraps all other middleware, and records requests rejected by a security check. Routes are registered before user controllers, so a controller trying to use the same path fails at startup.

## Environment variables

Testing this phase found that `CPPBOOT_ACTUATOR_METRICS_TOKEN` had no effect, because a variable's underscore was read as a dot unless the key was already set in YAML. `Config` now also checks a key's variable when reading a key that isn't set. See [002](002-core-runtime.md).

## Access log and request IDs

The server writes the access log, not middleware, and it's on by default rather than part of the actuator. Middleware never sees a request that couldn't be read, one whose body was too large, or one that waited too long for a worker, and those are the requests an operator most needs to see. The server knows about every response it writes, so it gives each one to `Server::onRequest` as a `RequestRecord`, and `ServerModule` turns that into a log line. The [web guide](../guides/web.md#access-log) shows what a line holds.

- **The record is made after the response has been written,** on the network thread that wrote it. It holds views into the connection's memory, so it costs nothing to make, and it's only made when something will be written: at a log level above `info`, `ServerModule` doesn't ask for records at all.
- **What clients choose is made safe before it's logged.** The path loses its query string, which can hold secrets. A byte that could break a line or a terminal is written as `?`, and the path is cut at 200 characters.
- **Each request gets an ID** of 13 characters, from a counter that starts at a random number and is mixed by a function that can be undone, so that two requests from one server never get the same ID, and IDs don't reveal how busy the server is at a glance. 13 characters hold all 64 bits, and fit in a `std::string` without allocating, so an ID costs no allocation. An earlier version used 16 hexadecimal digits, and cost two allocations a request.
- **An ID from a client is believed only from a trusted proxy,** as `X-Forwarded-For` is, and only if it's short and made of characters that can't break a line. Anyone else could otherwise make their requests look like another client's.

With the access log off, the request path makes as many allocations as it did before, and `tests/perf/allocation_test.cpp` checks that.

## Not included

Distributed tracing, OpenTelemetry, structured logs such as JSON, putting a request's ID into log lines automatically, and a separate management port are left for later. Putting the ID into every log line would need the logger to know which request a thread is working on, which is state shared between the two libraries that nothing else needs yet.
