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

Every request is over loopback, with no real network, and responses are small. Only the [HTTPS](#https) section uses TLS. Each connection sends a request, waits for the response and repeats, as fast as it can.

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
- **Real networks:** these add cost, and aren't measured. The [HTTPS](#https) results are also over loopback.
- **PostgreSQL:** the database test uses SQLite on the same machine. A networked database is much slower, and usually becomes the limit before the framework does.
- **Your CPU:** results changed by up to 4 times between the machines tested. Measure on your own hardware.

## The cost of metrics

Recording [metrics](guides/actuator.md) on every request took about 4% of throughput, measured on the Windows laptop with a route that does almost nothing, over 3 rounds:

| Round | Without metrics | With metrics |
|---|---|---|
| 1 | 12,945 requests/second | 12,337 requests/second |
| 2 | 12,992 requests/second | 12,595 requests/second |
| 3 | 13,292 requests/second | 12,627 requests/second |

## HTTPS

These compare HTTP and HTTPS on the same machines as above, with 1 network thread and 1 worker thread, over loopback. Each number is requests per second, and kept-open results are from 3 rounds on Linux, and 2 or 3 on Windows.

| Test | Linux HTTP | Linux HTTPS | Windows HTTP | Windows HTTPS |
|---|---|---|---|---|
| Small response, connection kept open, 64 connections | 46,100 to 47,300 | 34,000 to 37,200 | 12,700 to 14,600 | 11,000 to 11,700 |
| 64 KB response, connection kept open, 64 connections | 4,700 to 4,800 | 5,400 to 5,500 | 5,100 to 6,900 | 2,100 to 2,200 |
| New connection for every request, ECDSA certificate | 15,036 | 1,357 | 5,986 | 952 |
| New connection for every request, RSA 2048 certificate | 15,036 | 1,150 | 5,986 | 586 |

Some cautions about these numbers:

- **The 64 KB result on Linux is unexplained.** HTTPS came out slightly faster than HTTP there, and much slower on Windows. Don't conclude that HTTPS is faster. On Linux, the load generator shares a physical core with the server, and its own work can affect the result.
- **The handshake numbers may be limited by the client.** The load generator does its half of every handshake too. On Linux, it does that on the other half of the server's core, so a dedicated server could do better.
- **The small-response HTTP number is higher than the table at the top of this page,** at 46,000 against 37,000, on the same CPU model. These are different runs, with `TCP_NODELAY` added in between. Whether that caused the difference hasn't been tested.

### Open connections with 2 GB

On Linux, with the same real 2 GB limit as above. Connections opened at 700 per second and waited, then all sent one request at the same moment:

| Open HTTPS connections | Server memory while idle | With kernel sockets | Peak during the burst | Result |
|---|---|---|---|---|
| 1,000 | 90 MB | 80 MB | 83 MB | All worked |
| 5,000 | 386 MB | 392 MB | 405 MB | All worked |
| 10,000 | 756 MB | 782 MB | 809 MB | All worked |
| 20,000 | 1,463 MB | 1,528 MB | 1,581 MB | All worked |
| 25,000 | 1,865 MB | 1,950 MB | 2,013 MB | All worked, very close to the limit |
| 30,000 | | | 2,043 MB | The server died, and no requests worked |

Each open HTTPS connection costs about 76 KB in the server on Linux, and about 95 KB on Windows, against about 4 to 6 KB for HTTP. **That lowers the limit from 150,000 connections with HTTP to about 25,000 with HTTPS.** A real server also needs memory for its operating system and your application, so plan for less. The server's peak was at the limit when it died, though the kill counter wasn't captured.

Set `cppboot.server.max_connections` below what your memory can hold. The default of 1,024 is safe, but with HTTPS, its limit is what stands between a connection flood and running out of memory.

### During a flood of handshakes

One open connection kept working, with no errors, while 16 clients kept opening new HTTPS connections:

| One open connection, on Linux | No flood | During the flood |
|---|---|---|
| Requests/second | 14,415 | 2,142 |
| p50 | 64 µs | 235 µs |
| p99 | 137 µs | 4,002 µs |

On Windows during the flood, its p50 was 6 ms and its p99 18 ms.

### What this means for planning

- **Keep connections open.** On both machines, a request on a new HTTPS connection cost more than 10 times what one on an open connection did.
- **Prefer ECDSA certificates.** They handled more new connections than RSA: about 18% more on Linux, and about 60% more on Windows.
- **Plan memory around connections.** 10,000 open HTTPS connections need about 800 MB to 1 GB.

## The effect of rate limits

A flood of 600 sign-in attempts over 32 connections, against a project created by `cppboot create --web --security`, using 2 worker threads on the Windows laptop. Meanwhile, another signed-in user made 30 requests:

| | Flood results | That user's median request | Their slowest request |
|---|---|---|---|
| Limit off | 600 passwords checked | 1,395 ms | 1,515 ms |
| `10/minute` limit | 9 passwords checked, 591 rejected | 2 ms | 25 ms |

This was one run. The other user's own sign-in used one of the 10 attempts, because both came from one address. A limit only helps when the flood comes from few addresses.

The check itself [costs under 1% of a request for IPv4, and about 2% for IPv6](architecture/014-rate-limiting.md#cost), on routes that have a limit.

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
