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

## Not included

Distributed tracing, OpenTelemetry, automatic request logs and a separate management port are left for later.
