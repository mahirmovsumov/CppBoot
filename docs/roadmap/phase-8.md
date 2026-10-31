# Phase 8: Observability

**Status:** Complete. CI passes on Linux with GCC, Clang and the sanitizers, and on Windows with GCC.

## Done

- `/actuator/health` with automatic database checks, and `HealthIndicator` for your own
- `/actuator/health/live` for orchestrator liveness checks
- `/actuator/metrics` in Prometheus format, for requests, latency, memory and the database pool
- `/actuator/info`
- `Metrics` with counters, gauges and histograms, available to any component
- Metrics protected by a token, and locked by default in projects that use security
- `cppboot create --actuator`, and every `cppboot create` option can now be combined

## Done when

`cppboot create banking-api --web --postgresql --security --actuator` creates a project that builds and runs, and reports its health correctly as its database stops and restarts. This has to pass on Linux and Windows.

The end-to-end test builds and starts a project with every option on every CI run, using SQLite. The PostgreSQL run, including stopping its database, was checked by hand on Windows.

## Left for later

- Distributed tracing and OpenTelemetry
- Automatic request logs
- A separate port for actuator routes

## Next

Phase 9 is production readiness: packaging, running as a Linux service, and stress testing.
