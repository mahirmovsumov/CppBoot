# 005: Memory management

**Status:** Accepted.

This record covers how CppBoot owns memory and keeps its use bounded. How components keep their dependencies alive is in [016: Ownership and lifetimes](016-ownership.md), and the tools that find memory bugs are in the [memory safety guide](../guides/memory-safety.md).

## Ownership

Every resource has one owner that releases it in its destructor. There's no garbage collector, no reference-counted base class and no custom allocator.

| What | Owned by |
|---|---|
| Framework objects such as `Config`, `Logger` and the module list | `Application`, through `std::unique_ptr` |
| Components | The DI container, through `std::shared_ptr` |
| Background threads | `WorkerGroup`, through `std::jthread` |
| Connections and their buffers | The network thread that serves them |
| Database connections | `ConnectionPool`, lent out as a `Lease` that returns the connection when destroyed |
| C library handles, such as `PGconn`, `sqlite3` and `PGresult` | A class or `std::unique_ptr` whose destructor calls the library's release function |

The framework has one `new`, in the container, and its result goes straight into a `std::shared_ptr` with the deleter described in 016. Everything else uses `std::make_unique`, `std::make_shared` or a container.

### `shared_ptr` only where ownership is shared

Components use `std::shared_ptr`, because the container and every dependent component hold them, and they can't know who finishes last. Elsewhere, single ownership with `std::unique_ptr` or a plain member is the rule. Developers don't see either: a component takes `T&` and the container keeps `T` alive (016).

### Destruction order

`runApplication()` declares the container after the `Application`, so components are destroyed while the logger and configuration still exist. The container destroys singletons in the reverse of their creation order, and modules stop in the reverse of their start order (003). So nothing is destroyed while something that uses it still runs.

## Values crossing threads

`Request` and `Response` are plain values with their own `std::string` and `std::vector` members. A request moves from its network thread to a worker, and its response moves back, so neither thread ever holds a reference into the other's memory.

- The request body is moved out of Beast's parser into `Request`, not copied.
- Headers are copied once into `Request`, because Beast's storage belongs to the parser, which is reused for the next request on the connection.
- The response body is copied once into Beast's response in `toBeastResponse`. Moving it would need `Response` to give up its body, which its public API doesn't allow today.

Accessors that return references into a request or response are marked `CPPBOOT_LIFETIMEBOUND`, so Clang warns about a reference to a temporary (016).

## Bounded memory

A server must not let a client choose how much memory it uses. Each place that grows with input has a limit:

| What grows | Limit |
|---|---|
| A request body | `max_body_bytes`, 1 MB by default, answered with 413 |
| Request headers | Beast's parser limit of 8 KB, which isn't a setting yet |
| Connections | `max_connections`, 1,024 by default. Each one has one request at a time, so this also bounds queued requests ([010](010-web-server.md)) |
| Rate limit entries | `max_clients` per limited route, 100,000 by default, about 13 MB, with the least recently seen dropped first ([014](014-rate-limiting.md)) |
| Metric labels | HTTP metrics use the matched route pattern, never the path, so clients can't create new labels ([013](013-observability.md)) |
| Database connections | The pool's size, with a 5-second wait for a free one before it fails ([011](011-data-access.md)) |
| Password hashes | Limits on a stored hash's scrypt parameters, so a bad one can't use all of memory ([012](012-security.md)) |

### Idle connections

With TLS, `SSL_MODE_RELEASE_BUFFERS` frees OpenSSL's buffers on idle connections, and there's no server-side session cache ([015](015-https.md)). Even so, an HTTPS connection uses 76 to 95 KB, against about 14 KB for plain HTTP, and where the rest goes hasn't been measured. That makes it the main limit on connections for a small server.

## Making it faster without giving any of this up

An allocation costs time, and under load it's also a lock or a cache miss, so the request path makes as few as it can. A plain `GET` takes 5, where it took 30, and the work between parsing a request and writing its response takes less than half the time it did. Each change kept the rules above, and the next one has to as well.

**Copying less isn't the same as owning less.** Wherever a copy is removed, it has to stay clear who owns the data and how long it lives:

| Where | Rule |
|---|---|
| What a handler sees | A request that owns all of its data |
| Inside the framework | Views into data may be used while its owner is certainly alive, and never stored |
| Between threads | The data moves, or is copied. Nothing is shared that can change |
| When a request ends | Everything it used is released, and nothing of it reaches the next request |

What that allows, and what it doesn't:

- **A request's headers are one block of memory**, not two strings for each header. `RequestHeaders` holds them as lengths followed by text, with no pointers into itself, so a copied or moved request is complete. `Request::headerView` returns a view into that block, which lives as long as the request, as `body()` and `path()` do.
- **Header fields are parsed into memory that the connection lends** for one request, in one allocation, rather than one for each field. Only that connection's network thread uses it, the handler's request is copied out of it before it crosses to a worker, and it's freed when the response has been written. So a connection waiting for its next request holds none, and [idle connections](#idle-connections) cost what they did.
- **A response's body is moved** into what's written, not copied.
- **Requests and responses aren't pooled or reused.** An object that served one client and then serves another is how data leaks between them, and it would break the rule that a request is a value of its own. Making them cheap to create is safer than making them last.
- **A handler never gets a view into the connection's buffer.** The connection can close while a handler runs, and saving one copy of a few hundred bytes isn't worth a pointer that can dangle.
- **No allocator is passed through the types** that handlers use. `std::pmr` would change `std::string` to another type everywhere in the public interface, for what's left to save after the changes above.

A change to the request path answers five questions before it goes in:

1. Does it make fewer allocations, or take less time? Measured, not expected.
2. Does every piece of data still have one clear owner?
3. Can one request still never see another's data?
4. Does the public interface change?
5. Does a connection that's waiting use more memory?

`tests/perf/allocation_test.cpp` answers the first on every run of the tests. It counts the allocations for each of three kinds of request, through the server and through the router alone, and fails when one takes more than its budget. A count is the same on every run, which a timing isn't.

## What isn't done

- **Allocations per request haven't been measured.** A request builds several strings and maps: the path, its segments, query and route parameters, headers, and two metric keys. That's fine for the throughput measured so far ([benchmarks](../benchmarks.md)), and nothing will change without a measurement that shows it matters.
- **No arenas or pools for request data.** They'd make lifetimes harder to reason about, and the cost they'd save hasn't been shown.
- **The header limit can't be changed**, and the response body is copied once. Both are small changes once there's a reason.

## Checks

- Every CI run includes an AddressSanitizer and UndefinedBehaviorSanitizer build of the whole framework and its tests, on Linux.
- Every CI run includes a 90-second HTTPS stress test of the server built with the same sanitizers, with four network threads, and well-behaved clients alongside slow, abandoned and oversized requests. The stress workflow, started by hand, does longer runs.
- New projects build with `_GLIBCXX_ASSERTIONS`, and `cppboot build --sanitize=address` checks an application (016).

C++ can't guarantee memory safety, so these find bugs rather than proving there are none.
