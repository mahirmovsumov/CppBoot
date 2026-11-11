# 006: Concurrency model

**Status:** Accepted.

This record covers which threads CppBoot runs, what each one may touch, and how they stop. The web server's own threads are described in more detail in [010: Web server](010-web-server.md).

## Threads

| Thread | Started by | Runs |
|---|---|---|
| Main | The operating system | The lifecycle: configuration, creating components, starting and stopping modules. Then it waits for a shutdown request |
| Signal handler | The operating system, on Ctrl+C, SIGINT or SIGTERM | Only marks that shutdown was requested |
| Module workers | `context.workers().spawn()` | Background work that a module starts |
| Network threads | The web server, `io_threads` of them | Reading and writing connections, and TLS, as Asio coroutines |
| Handler workers | The web server, `worker_threads` of them | Middleware and route handlers |

There are no other threads. The logger writes on the thread that logs, and the database runs each statement on the thread that calls it.

## Startup is single-threaded

Everything that's set up once happens on the main thread before any other thread starts:

1. Configuration is loaded, and doesn't change after that.
2. The container creates every component that a module needs, which for a web project includes the router, every controller, every middleware and what they depend on.
3. Modules start in dependency order. The server module starts its threads last.

So components are fully built before any request can reach them, and the router doesn't change once requests run. Its documentation says so, because it has no lock.

## Components are shared

A component is created once and used by every thread that runs a handler, at the same time. It has to be safe for that:

- **No state, or state that doesn't change after the constructor**, needs nothing. Most components are like this.
- **State that changes** needs a lock or an atomic in the component itself.

The generator warns about a field that can change in a component with no lock, which catches the common mistake but isn't proof of safety ([016](016-ownership.md#field-checks)).

Per-request components were considered and left out, because they add work to every request and handlers rarely need them. Request data stays in the `Request`, which only one thread handles at a time.

## What's safe from any thread

| Object | How |
|---|---|
| `Config` | Doesn't change after loading |
| `Logger` | spdlog's thread-safe sinks |
| `Container` | A recursive mutex around registration and resolution |
| `Database` | Each statement borrows its own connection from the pool |
| `Metrics` and `Actuator` | Atomics for values, and a mutex for looking up and adding metrics |
| `RateLimiter` | A mutex around its table |
| `TokenService` | Doesn't change after startup |
| `requestShutdown()` | Sets an event |
| `Router::handle()` | Reads only, once set up |

A database `Connection` is used by one thread at a time, which the pool guarantees.

`std::shared_mutex` isn't used. Its read lock can fail under contention with MinGW, so plain mutexes are used even where most access is reading.

## Blocking

Handlers may block, such as on a database query or a call to another service, because they run on handler workers, not network threads. Network threads never wait for a handler: the connection's coroutine suspends until its response is ready, and other connections carry on.

A blocked handler still holds a worker. There's one worker for each processor core by default, so slow handlers limit throughput. When requests wait longer than `queue_timeout` for a worker, they get 503 without running, so a backlog can't grow without limit ([010](010-web-server.md#overload)).

Asynchronous handlers, written as coroutines, would free workers while waiting. They aren't part of the public API, because every database and HTTP client a handler uses would also need to be asynchronous.

## Stopping

Nothing in CppBoot ends a thread from outside. Stopping is cooperative everywhere, through `std::stop_token`:

- **Module workers** get a stop token, and `stopAll()` requests a stop and joins every one. A worker that throws asks the application to shut down, and the exit code reports the failure.
- **Handlers** get `Request::stopToken()` and `Request::deadline()`. A handler that ignores them runs to the end, and shutdown waits for it ([010](010-web-server.md#shutdown)).

Shutdown runs in this order:

1. A signal, or `requestShutdown()`, wakes the main thread.
2. Modules stop in the reverse of their start order. The server stops accepting, lets running requests finish, then asks the rest to stop and waits for its threads.
3. Module workers are asked to stop and joined.
4. Components are destroyed, in the reverse of their creation order.

So when a component's destructor runs, no thread can still be using it.

## Cancellation in the CLI

`cppboot dev` runs the application as a child process, and stops it the same way Ctrl+C would: Ctrl+Break to a separate process group on Windows, and SIGTERM on Linux. The application gets its normal shutdown, and is ended only if it hasn't finished after 10 seconds.

## Checks

- A ThreadSanitizer build of the framework and its tests runs in CI, on Linux. The stress workflow, started by hand, also runs the server under it with four network threads, over HTTP and HTTPS. Asio's fences are turned off in that build so that it can see them ([010](010-web-server.md#threadsanitizer)).
- `cppboot build --sanitize=thread` checks an application.
- The generator warns about unprotected shared state in components.
