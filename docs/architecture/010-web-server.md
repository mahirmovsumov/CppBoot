# 010: Web server

**Status:** Accepted.

## Threads

The server has one network thread and a pool of worker threads.

- **Network thread:** runs all connection handling, as Asio coroutines. Connection state is only touched here, so it needs no locks.
- **Worker pool:** runs `Router::handle`. The session coroutine hands each request over and resumes on the network thread when the response is ready.

Developers can write ordinary blocking handlers without stalling other connections. One network thread is enough for now, because it only moves bytes. Benchmarks can justify more later.

Each connection handles one request at a time, so the number of queued requests is limited by `max_connections`. That bounds memory under load.

The server keeps 64 accepts waiting at once. A single one takes one connection per turn of the event loop, which is only a few hundred per second under load, so new clients would queue for seconds. See the [benchmarks](../benchmarks.md).

## Public headers

Boost.Asio and Boost.Beast appear only in the server's source files. Public headers use CppBoot's own `Request`, `Response` and `Router`. This limits Boost's compile-time cost to one file, and allows replacing it without changing applications.

## Limits and failures

| Situation | Response |
|---|---|
| Body over `max_body_bytes` | 413, then close |
| Malformed HTTP or invalid percent-encoding | 400 |
| Unsupported method | 400 |
| Client too slow | Close when `request_timeout` expires |
| More than `max_connections` | Close at once |
| Waited longer than `queue_timeout` for a worker | 503 with `Retry-After`, and the handler doesn't run |
| Handler throws `HttpError` | That status, with its message |
| Handler throws anything else | 500 with a generic message, with the details logged |

Percent-encoded null characters are rejected, because they can fool later code that passes values to C APIs. Line breaks in response headers throw an exception, which stops header injection.

The default host is `127.0.0.1`, so a service is only exposed to a network if someone chooses that.

## Overload

A stress test found that clients could send a slow request and leave at once, and the worker pool would still run it. Once work arrived faster than it finished, the queue only grew. Open connections reached `max_connections`, and well-behaved clients waited over 30 seconds, although the server kept running and used no extra memory.

So each request records when it was queued, and a worker that picks up one older than `queue_timeout` answers 503 without running it. Workers take requests in order, so once a backlog is too old, the rest of it clears in microseconds per request, and new requests get served. Time is a better limit than a queue length, because the right length depends entirely on how slow the handlers are.

Rejected requests skip middleware, because the point is to do no work for them, so metrics don't count them. The server logs them instead.

That wasn't enough on its own. A later run kept the queue at its 5-second limit, because requests from clients that had gone arrived as fast as the workers could run them. The server served only about 7 requests per second, with very few errors. So a worker that picks up a request that waited at least 10 ms also asks the socket whether the client has gone, and skips the handler if it has. The check costs a system call, so a server that's keeping up shouldn't pay for it, and a queue built this way soon passes 10 ms anyway. Compared in alternating rounds with the version before it, on one busy core, the median stayed at about 12,700 requests per second, so its cost was too small to measure. This is a non-blocking peek in `platform::peerHasClosed`, which is safe from a worker because the network thread leaves a connection alone while its request is running.

It has limits. A handler that has already started can't be stopped. A client that half-closes its connection after sending a request looks like one that has gone, so under load it gets an empty 503 rather than a response, which is close to how nginx treats it. With TLS, a client that sends `close_notify` looks like one with data to read, so only the TCP close that follows is seen. A client that stays connected still counts as real traffic, so rate limits and capacity are the only answer to it.

## Shutdown

1. Stop accepting connections, and close idle ones.
2. Let each running request finish and send its response, with `Connection: close`.
3. After `shutdown_timeout`, cancel network operations on all connections.
4. Wait for the worker pool.

A running handler can't be safely interrupted in C++, so step 4 waits for it, and the shutdown timeout doesn't limit handlers.

## Routing

Routes are matched with a linear scan, which is simple and fast enough for the number of routes an API has. Benchmarks can justify a tree later. A fixed segment beats a parameter, comparing from the left, so the result doesn't depend on registration order.

## Platform notes

- `SO_REUSEADDR` is set on Linux so that a restarted server can reuse its port. It isn't set on Windows, where it would let another process take over a port in use.
- Asio requires Windows 10 APIs (`_WIN32_WINNT=0x0A00`) and links to `ws2_32` and `mswsock`.
- With HTTPS added, a Debug build with Clang on Windows fails to link the web tests, with "relocation truncated to fit". The cause hasn't been found. A Release build with Clang on Windows works, as does Debug with GCC. CI's Clang job runs on Linux.

## ThreadSanitizer

ThreadSanitizer can't track `atomic_thread_fence`, which Asio uses, and GCC reports this as a warning. The `tsan` preset defines `BOOST_ASIO_DISABLE_FENCED_BLOCK` to remove those fences from that build only. This keeps the check strict, because code relying on a removed fence is reported as a race rather than hidden.
