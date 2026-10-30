# Phase 5: Database

**Status:** Complete on Windows. It's finished once CI passes on Linux.

## Done

- `ConnectionPool`: shared connections, limits, timeouts and health checks
- PostgreSQL and SQLite drivers behind one `Connection` interface
- `Database` and `Transaction`: parameterized queries, with commit and rollback
- `CPPBOOT_ROW`: generated mapping from query results to structs
- Migrations: versioned SQL files applied at startup, with checks for edited and missing files
- `DatabaseModule`: checks the connection and applies migrations before other modules start
- `cppboot create --web --sqlite` and `--postgresql`

## Done when

The tutorial's users API stores its data in a database, in a project made by `cppboot create`, on both SQLite and PostgreSQL. This has to pass on Linux and Windows.

The end-to-end test builds and runs the SQLite project on every CI run. The driver test suite runs against both databases on Linux. The tutorial's `curl` requests were checked by hand on Windows, against both databases.

## Left for later

- A full ORM, which isn't planned for 1.0
- MySQL, MariaDB and SQL Server
- Date, time and binary column types
- Savepoints and isolation levels
- Rolling back migrations, which is left out on purpose. See [011](../architecture/011-data-access.md)

## Next

Phase 6 is security, with basic JWT authentication for 1.0. It needs middleware in projects that use controllers, which isn't possible yet.
