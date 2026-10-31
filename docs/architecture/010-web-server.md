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
| Handler throws `HttpError` | That status, with its message |
| Handler throws anything else | 500 with a generic message, with the details logged |

Percent-encoded null characters are rejected, because they can fool later code that passes values to C APIs. Line breaks in response headers throw an exception, which stops header injection.

The default host is `127.0.0.1`, so a service is only exposed to a network if someone chooses that.

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
