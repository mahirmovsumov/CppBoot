# Benchmarks

These results measure CppBoot on one CPU core with a 2 GB memory limit, in Release mode. They show the framework's own cost, so a real application will be slower, because it does more work per request.

## Setup

| | Linux (GitHub Actions) | Windows (development laptop) |
|---|---|---|
| CPU | AMD EPYC 9V74 | Intel i7-8750H |
| Server | Pinned to 1 logical core | Pinned to 1 logical core |
| Load generator | The core's other hyperthread | 10 other logical cores |
| Memory limit | 2 GB, enforced by a cgroup, including kernel socket memory | None |
| Threads | 1 network, 1 worker | 1 network, 1 worker |

The Linux load generator shares a physical core with the server, so that machine's results are lower than a dedicated core would give. An earlier run on a different cloud CPU, an AMD EPYC 9V45, reached about 61,000 requests per second for the same test, before the accept fix.

Every request is over loopback, with no real network or TLS, and responses are small. Each connection sends a request, waits for the response and repeats, as fast as it can.

## Throughput on one core

64 connections:

| Route | Linux requests/second | Linux p50 | Linux p99 | Windows requests/second |
|---|---|---|---|---|
| Plain text | 37,000 | 1.7 ms | 3.5 ms | 12,000 to 14,000 |
| JSON | 36,300 | 1.8 ms | 3.4 ms | 12,700 |
| Route with parameters | 36,700 | 1.7 ms | 3.5 ms | 12,500 |
| SQLite read by primary key | 32,100 | 2.0 ms | 3.7 ms | 10,000 |

Routing and JSON add little, because a request's cost is mostly system calls and switching between threads.

## Latency as connections grow

These connections all send constantly, so the server is at full load. Throughput stays steady, and each connection gets a smaller share, so latency grows with their number. A server at normal load has much lower latency.

| Active connections | Requests/second | p50 | p99 | Errors |
|---|---|---|---|---|
| 1 | 15,600 | 0.06 ms | 0.1 ms | 0 |
| 16 | 34,300 | 0.45 ms | 1.0 ms | 0 |
| 256 | 35,700 | 7.2 ms | 10 ms | 0 |
| 1,000 | 32,600 | 30 ms | 38 ms | 0 |
| 4,096 | 26,900 | 160 ms | 188 ms | 0 |
| 10,000 | 27,800 | 370 ms | 1.1 s | 0 |
| 50,000 | 32,300 | 700 ms | 23 s | 703 |

## Open connections with 2 GB

Each size used a fresh server. The connections opened and waited, then all sent one request at the same moment, which is the worst case for memory.

| Open connections | Server memory while idle | With kernel sockets | Peak during the burst | Result |
|---|---|---|---|---|
| 1,000 | 16 MB | 10 MB (see note) | 14 MB | All worked |
| 10,000 | 53 MB | 80 MB | 115 MB | All worked |
| 50,000 | 216 MB | 394 MB | 563 MB | All worked |
| 100,000 | 421 MB | 786 MB | 1,104 MB | All worked |
| 150,000 | 625 MB | 1,178 MB | 1,651 MB | All worked |
| 200,000 | 830 MB | 1,570 MB | 2,047 MB | 46,176 requests failed, and the server died |

Note: the two memory columns are measured differently, so the first can be higher at small sizes.

Each open connection costs about 4.2 KB in the server, and about 8 KB in total with the kernel's socket memory. At 200,000 connections, peak memory reached the 2 GB limit just as the server died. That points to the limit as the cause, though the kill counter wasn't captured, because the cgroup disappeared with the server.

A real 2 GB server also needs memory for its operating system and your application's own data, so plan for less than these results show.

## What this doesn't show

- **Comparison:** nothing here measures another framework, so these results don't show whether CppBoot is faster than any of them.
- **Real networks and TLS:** both add cost, and neither is measured.
- **PostgreSQL:** the database test uses SQLite on the same machine. A networked database is much slower, and usually becomes the limit before the framework does.
- **Your CPU:** results changed by up to 4 times between the machines tested. Measure on your own hardware.

## Before running in production

- **`cppboot.server.max_connections`:** the default is 1,024, and connections beyond it are closed. Raise it if you need more.
- **Open file limit:** Linux often limits a process to 1,024 open files, and each connection needs one. Raise it with `ulimit -n` or `LimitNOFILE` in systemd.
- **`cppboot.server.worker_threads`:** the default is one per core. If handlers wait on a database or another service, more threads let other requests continue meanwhile.

## Run them yourself

On Linux, with at least 2 CPU cores and `sudo`:

```bash
cmake -S . -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DCPPBOOT_BUILD_BENCHMARKS=ON \
    -DCPPBOOT_BUILD_TESTS=OFF -DCPPBOOT_BUILD_EXAMPLES=OFF -DCPPBOOT_BUILD_CODEGEN=OFF
cmake --build build/bench --target cppboot-bench-server cppboot-load
bash scripts/benchmark-linux.sh build/bench
```

You can also start the Benchmark workflow by hand on GitHub.

## What benchmarking found

The first run showed new connections waiting for seconds under load, up to 52 seconds at 10,000 connections, and being refused on Windows. The server took one connection per turn of its event loop, so it now keeps 64 accepts waiting. At 1,024 connections on Windows, the slowest request went from 2.7 s to 0.17 s, and refused connections from 414 to 0.
