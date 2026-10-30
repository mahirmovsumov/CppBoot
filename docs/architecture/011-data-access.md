# 011: Data access

**Status:** Accepted.

## Layers

```text
Application repositories
        |
Database, Transaction     Queries, row mapping
        |
ConnectionPool            Sharing, limits, health
        |
Connection                PostgreSQL (libpq) or SQLite
```

`cppboot-data` depends on the core for `DatabaseModule`, and the core doesn't depend on it. Database libraries are private to `cppboot-data`, and public headers use CppBoot's own types.

## Decisions

### libpq rather than libpqxx

The original plan named libpqxx. `libpq` is PostgreSQL's own C library, with a stable API across versions and platforms, and it removes a dependency. The driver is one file behind the `Connection` interface, so a C++ wrapper would save little.

### Exceptions for query failures

Elsewhere, CppBoot uses `Result` for expected failures. Queries throw `DatabaseError` instead, because a query inside `transaction()` must abort the function and trigger a rollback, however deep the call. A forgotten `Result` check there would commit partial work. In a controller, an uncaught error becomes a generic 500, with its details logged and not sent to the client.

`runMigrations` returns a `Result`, because it runs once at startup and its caller reports the failure.

### One placeholder syntax

Both drivers accept `$1`, `$2` and so on, so queries read the same on both. SQLite numbers parameters by first appearance, whatever their names, so its driver reads each placeholder's name to find the right value. This lets placeholders repeat or appear out of order, as they can in PostgreSQL.

### Text results

Both drivers return every value as text, and `Row` converts on read. This keeps the `Connection` interface small, and makes adding a driver simple. The cost is parsing numbers on read, which is small next to a database round trip.

### Row mapping at runtime

`CPPBOOT_ROW` registers a mapper with `RowMappers` when the application starts, and `queryAs<T>` looks it up by type. A compile-time approach, like the one for JSON, isn't possible here: repositories call `queryAs<T>` inside user headers, before the generated code is visible, and a template specialization has to be declared before its first use. A map lookup per query is small next to the query itself.

A struct with no mapper fails at runtime, on its first query, and not at build time. This is the main cost of this design.

### Pool behavior

- Connections open only when needed, up to `max_size`.
- `acquire()` waits up to `acquire_timeout`, and then throws. Waiting forever would hide overload, and hold up request threads.
- Connecting happens outside the pool's lock, because it can take seconds.
- A connection is checked when borrowed and when returned. One that's broken, or still inside a transaction, is closed rather than reused, so a bug in one request can't leak into another.

Statements aren't retried after a lost connection, because a failed write may still have taken effect.

### Migrations

Each file runs in its own transaction, together with the record of having run, so a file can't be half applied or applied twice. PostgreSQL's transactional DDL makes this reliable, and SQLite's does too.

A checksum detects edits to applied files. It's FNV-1a rather than a cryptographic hash, because it guards against mistakes, not attackers. It ignores carriage returns, so that Git's line ending settings don't look like edits.

Instances that start together on PostgreSQL take an advisory lock, held for the whole run on one connection. SQLite doesn't need one, because a write transaction locks the file.

Rollback scripts are left out on purpose. They're rarely tested and often can't undo a data change, so the fix for a bad migration is a new one.

### Startup order

Generated code registers `DatabaseModule` before all other components. Modules with no ordering constraint start in registration order, so migrations finish before any other module starts, including the web server.

## Secrets

`cppboot.database.url` can hold a password, so `Config::describe()` hides it, and CppBoot's error messages never include it. Messages from libpq can include the host name.

## Testing

The same test suite runs against both drivers, so they can't drift apart. PostgreSQL tests run when `CPPBOOT_TEST_POSTGRESQL_URL` is set, which CI does on Linux, using the official PostgreSQL image.

The `RejectsTwoStatementsInOneQuery` test crashes a server process when run against MSYS2's PostgreSQL 18 build on Windows, which terminates with exception `0xC0000028` on that statement. The client only sends the statement, so this is a problem in that server build. Skip that test when running against it locally.
