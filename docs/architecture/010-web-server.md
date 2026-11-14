# 010: Web server

**Status:** Accepted.

## Threads

The server has a set of network threads and a pool of worker threads.

- **Network threads:** run all connection handling, as Asio coroutines. Each has its own `io_context` and list of connections. A connection stays on one thread for life, and only that thread touches it, so connection state needs no locks.
- **Worker pool:** runs `Router::handle`. The session coroutine hands each request over and resumes on its own network thread when the response is ready.

Developers can write ordinary blocking handlers without stalling other connections.

The first network thread owns the acceptor, and hands each new connection to the next thread in turn. So a thread is chosen before knowing how busy it will be, and a few expensive connections can land together. Separate listening sockets with `SO_REUSEPORT` wouldn't help, because Windows doesn't have that behavior.

The default is one network thread for every two cores, from 1 to 4. It started at one thread, because that seemed enough to move bytes, and because a benchmark should justify more. TLS provided that: on 4 logical cores, 4 threads served more than twice as many HTTPS requests as one, and a flood of handshakes hurt other connections about 9 times less. See the [benchmarks](../benchmarks.md#network-threads). The limit of 4 is as far as this has been measured, and `io_threads` can go higher.

TLS handshakes stay on network threads, rather than getting their own executor. A stream can't easily move between executors after a handshake, and more threads gave most of the benefit without that complexity.

Each connection handles one request at a time, so the number of queued requests is limited by `max_connections`. That bounds memory under load.

A request is copied once, from the connection's memory into a `Request` that owns its data, on the network thread. That's what makes it safe to hand to a worker, and it's kept cheap: the headers go into one block, and the body is moved. See [005](005-memory-management.md#making-it-faster-without-giving-any-of-this-up) for what the request path allocates, and the test that keeps it from growing.

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
3. After `shutdown_timeout`, ask handlers still running to stop, log how many there are, and cancel network operations on all connections.
4. Wait for the worker pool.

A running handler can't be safely interrupted in C++, so step 4 waits for it, and the shutdown timeout doesn't limit handlers that ignore the request to stop. `Request::stopToken()` and `Request::shouldStop()` make stopping cooperative. A timeout that ended the process instead wouldn't be safe for a library to choose, because a handler may be halfway through a change. A service manager can make that choice, and the Linux service does, after 45 seconds.

Handlers get `shutdown_timeout` to finish before being asked, so that a normal request doesn't see a stop request just because shutdown started.

## Lifecycle

A server is `Created`, `Running` or `Stopped`. A failed `start()` undoes what it did and stays `Created`, so it can be called again, such as once a port is free. A stopped server can't start again, because its thread pools have ended, so that needs a new `Server`. A mutex means `start()` and `stop()` can't overlap.

## Routing

Routes are matched with a linear scan, which is simple and fast enough for the number of routes an API has. Benchmarks can justify a tree later. A fixed segment beats a parameter, comparing from the left, so the result doesn't depend on registration order.

## Platform notes

- `SO_REUSEADDR` is set on Linux so that a restarted server can reuse its port. It isn't set on Windows, where it would let another process take over a port in use.
- Asio requires Windows 10 APIs (`_WIN32_WINNT=0x0A00`) and links to `ws2_32` and `mswsock`.
- With HTTPS added, a Debug build with Clang on Windows fails to link the web tests, with "relocation truncated to fit". The cause hasn't been found. A Release build with Clang on Windows works, as does Debug with GCC. CI's Clang job runs on Linux.

## ThreadSanitizer

ThreadSanitizer can't track `atomic_thread_fence`, which Asio uses, and GCC reports this as a warning. The `tsan` preset defines `BOOST_ASIO_DISABLE_FENCED_BLOCK` to remove those fences from that build only. This keeps the check strict, because code relying on a removed fence is reported as a race rather than hidden.
