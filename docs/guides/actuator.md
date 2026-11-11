# Health and metrics

The actuator adds routes for checking whether your application is healthy, and seeing what it's doing.

To add it to a new project, use `--actuator`. It works with any other options, and a project with no web API gets a server just for these routes:

```powershell
cppboot create my-api --web --actuator
cppboot create my-service --actuator
```

For an existing project, add `ACTUATOR` to its CMake setup:

```cmake
cppboot_generate_components(my-api ACTUATOR HEADERS ${headers})
```

## Routes

| Route | Purpose | Who can call it |
|---|---|---|
| `/actuator/health` | Is the application able to do its work? | Anyone |
| `/actuator/health/live` | Is the process running? | Anyone |
| `/actuator/metrics` | Metrics in Prometheus format | See [Who can see metrics](#who-can-see-metrics) |
| `/actuator/info` | Name, version and uptime | Same as metrics |

## Health

```json
{"status":"UP","checks":{"database":"UP"}}
```

If any check fails, the status is `DOWN` and the response code is 503, which tells a load balancer to stop sending traffic here. The application recovers by itself once the check passes again.

Responses show only `UP` or `DOWN`, because this route is public. The reason for a failure goes to the log.

**Use the two routes for different things.** If you run on an orchestrator such as Kubernetes:

- Use `/actuator/health` for its readiness check, which decides whether to send traffic.
- Use `/actuator/health/live` for its liveness check, which decides whether to restart the process.

`/actuator/health/live` ignores health checks on purpose. Restarting the application can't fix a database that's down, and restarting every instance at once would make things worse.

### Add your own check

The database is checked automatically if the project has one. To add a check, mark a class that derives from `HealthIndicator`, and throw to report a problem:

```cpp
class CPPBOOT_SERVICE QueueHealth : public cppboot::actuator::HealthIndicator {
public:
    std::string name() const override { return "queue"; }

    void check() override {
        if (!queue_->isConnected()) {
            throw std::runtime_error("The queue is unreachable");
        }
    }
};
```

Keep checks quick, because load balancers call them often. A check that can wait on something needs its own time limit. The database check can take as long as `cppboot.database.acquire_timeout_seconds` if all connections are busy.

## Metrics

Metrics are in the text format that [Prometheus](https://prometheus.io) reads.

| Metric | Type | Meaning |
|---|---|---|
| `http_requests_total` | Counter | Requests handled, by `method`, `route` and `status` |
| `http_request_duration_seconds` | Histogram | How long requests took, by `method` and `route` |
| `http_requests_in_progress` | Gauge | Requests being handled now |
| `process_uptime_seconds` | Gauge | Time since the application started |
| `process_resident_memory_bytes` | Gauge | Physical memory in use |
| `database_connections_open` | Gauge | Database connections open, if there's a database |
| `database_connections_idle` | Gauge | Database connections not in use, if there's a database |

Recording these costs about 4% of throughput on one core in our [benchmark](../benchmarks.md), with responses that do almost nothing. The share is smaller when handlers do real work.

### Add your own metrics

Take `Metrics` in a constructor, and hold on to the metrics you create:

```cpp
class CPPBOOT_CONTROLLER("/orders") OrderController {
public:
    explicit OrderController(cppboot::actuator::Metrics& metrics)
        : created_(metrics.counter("orders_created_total", "Orders created")) {}

    CPPBOOT_POST("/") void create() { created_.increment(); }

private:
    cppboot::actuator::Counter& created_;
};
```

| Kind | Function | Use it for |
|---|---|---|
| Counter | `metrics.counter(name, help, labels)` | Values that only go up, such as orders created |
| Gauge | `metrics.gauge(name, help, labels)` | Values that go up and down, such as queue size |
| Gauge from a function | `metrics.gaugeFunction(name, help, function)` | A value to read when metrics are collected |
| Histogram | `metrics.histogram(name, help, labels, bounds)` | The spread of values, such as durations |

Metrics are safe to use from any thread. A reference stays valid for as long as the application runs.

**Never use client-chosen values as labels**, such as a path or a user name. Each new combination uses memory until the application stops, so a client could use up all of it. This is why `http_requests_total` records the route pattern `/users/{id}` instead of the path `/users/7`, and puts every unmatched request under `route="unmatched"`.

## Who can see metrics

Metrics and info reveal details about your application, so they're protected:

| Project | `cppboot.actuator.metrics_token` | Result |
|---|---|---|
| Uses security | Not set | **Locked for everyone** |
| Uses security | Set | Needs that token |
| No security | Set | Needs that token |
| No security | Not set | Only from this machine |

"This machine" means a client at `127.0.0.1` or `::1`, so you can look at them while developing. A request that came through a proxy, with an `X-Forwarded-For` or `Forwarded` header, doesn't count, because a proxy on the same machine connects from a local address on behalf of clients anywhere. To collect metrics from another machine, set a token.

Set the token in the environment. It's separate from user sign-in, because a metrics collector isn't a user, so a user's JWT doesn't work here:

```powershell
$env:CPPBOOT_ACTUATOR_METRICS_TOKEN = "a-long-random-value"
```

Then give it to Prometheus:

```yaml
scrape_configs:
  - job_name: my-api
    metrics_path: /actuator/metrics
    authorization:
      credentials: a-long-random-value
    static_configs:
      - targets: ["localhost:8080"]
```

## Not included yet

- Distributed tracing and OpenTelemetry
- Automatic request logs. To add them, write [middleware](security.md#middleware)
- A separate port for actuator routes
- Changing these routes' paths
