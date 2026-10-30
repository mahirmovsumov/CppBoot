# Database

`cppboot::data` connects your application to PostgreSQL or SQLite. It shares connections through a pool, runs queries safely, maps rows to your structs, and applies schema changes at startup.

To start a project that uses a database, run one of these. The [database tutorial](../tutorials/database.md) walks through one.

```powershell
cppboot create my-api --web --sqlite
cppboot create my-api --web --postgresql
```

## Set up the connection

Add the connection to `cppboot.yaml`:

```yaml
cppboot:
  database:
    url: sqlite:app.db
    migrations: db/migrations
```

| Key | Default | Purpose |
|---|---|---|
| `cppboot.database.url` | Required | See the table below |
| `cppboot.database.migrations` | None | A folder of migrations to apply at startup, relative to the working directory |
| `cppboot.database.pool_size` | `10` | The most connections open at once |
| `cppboot.database.acquire_timeout_seconds` | `5` | How long to wait for a free connection before failing |

| Database | URL |
|---|---|
| PostgreSQL | `postgresql://user:password@host:5432/name` |
| SQLite file | `sqlite:app.db` |
| SQLite in memory | `sqlite::memory:`, which always uses one connection |

**Keep real passwords out of `cppboot.yaml`.** Set the URL in the environment instead, and leave a local default in the file:

```powershell
$env:CPPBOOT_DATABASE_URL = "postgresql://app:secret@db.example.com/shop"
```

CppBoot hides this setting in its configuration log, and leaves the URL out of its own error messages. A PostgreSQL connection error can still name the host.

## Use the database

A marked class gets the database through its constructor, with no setup of its own. Any project that marks a class with `CPPBOOT_ROW`, or depends on `Database`, gets the database registered automatically:

```cpp
struct CPPBOOT_ROW User {
    std::int64_t id{0};
    std::string name;
    std::optional<std::string> nickname;
};

class CPPBOOT_SERVICE UserRepository {
public:
    explicit UserRepository(std::shared_ptr<cppboot::data::Database> database)
        : database_(std::move(database)) {}

    std::optional<User> find(std::int64_t id) {
        return database_->queryOne<User>("SELECT * FROM users WHERE id = $1", id);
    }

private:
    std::shared_ptr<cppboot::data::Database> database_;
};
```

Link to `cppboot::data`. A project made with `--sqlite` or `--postgresql` already does.

## Run queries

| Function | Returns |
|---|---|
| `queryAs<T>(sql, values...)` | A `std::vector<T>` of every row |
| `queryOne<T>(sql, values...)` | A `std::optional<T>` of the first row |
| `queryValue<T>(sql, values...)` | A `std::optional<T>` of the first column of the first row |
| `execute(sql, values...)` | The number of rows changed |
| `query(sql, values...)` | `Rows`, for reading columns yourself |

```cpp
auto count = database_->queryValue<std::int64_t>("SELECT COUNT(*) FROM users");
auto changed = database_->execute("UPDATE users SET name = $1 WHERE id = $2", name, id);

for (const auto row : database_->query("SELECT id, name FROM users")) {
    auto name = row.get<std::string>("name");
}
```

To get the ID of a new row, use `RETURNING`, which both databases support:

```cpp
auto user = database_->queryOne<User>(
    "INSERT INTO users (name) VALUES ($1) RETURNING *", name);
```

### Always use placeholders

Values go in as `$1`, `$2` and so on, for both databases. They travel separately from the SQL, so they can't change what the statement does. **Never build SQL by joining text with values**, which is what makes SQL injection possible:

```cpp
// Safe, whatever the name contains.
database_->query("SELECT * FROM users WHERE name = $1", name);

// Never do this.
database_->query("SELECT * FROM users WHERE name = '" + name + "'");
```

A value can be text, a number, a `bool`, `nullptr` or a `std::optional` of those. An empty `std::optional` is SQL `NULL`.

## Map rows to structs

`CPPBOOT_ROW` generates reading a struct from a result:

- Fields match columns by name, and extra columns are ignored.
- Fields can be text, a number, a `bool` or a `std::optional` of those.
- A column that can be `NULL` needs a `std::optional` field. A `NULL` in any other field throws an exception.
- The struct needs a default constructor and public fields, and can't have base classes.
- One struct can have both marks: `struct CPPBOOT_JSON CPPBOOT_ROW User { ... };`

## Transactions

`transaction` runs several statements as one unit. It commits if your function returns, and rolls back if it throws:

```cpp
database_->transaction([&](cppboot::data::Transaction& transaction) {
    transaction.execute("UPDATE accounts SET balance = balance - $1 WHERE id = $2", amount, from);
    transaction.execute("UPDATE accounts SET balance = balance + $1 WHERE id = $2", amount, to);
});
```

Whatever the function returns, `transaction` returns too. A `Transaction` has the same query functions as `Database`. Use the one you're given inside the function, because a call on `Database` there uses a different connection, outside the transaction.

Transactions can't be nested.

## Errors

Query functions throw `cppboot::data::DatabaseError`, which rolls back any transaction it passes through. Its `kind()` tells you what happened:

| Kind | Meaning |
|---|---|
| `Connection` | The connection failed or was lost |
| `Timeout` | No connection became free in time, or the database was busy |
| `UniqueViolation` | A unique constraint or primary key rejected a value |
| `ConstraintViolation` | Another constraint rejected a value |
| `Query` | The database rejected the statement |
| `Mapping` | A result couldn't be converted to the requested type |

In a controller, an error you don't catch becomes a generic 500, and its details go to the log, so that SQL details can't reach clients. To give a better response, catch the kinds you expect:

```cpp
try {
    return {201, users_->add(details)};
} catch (const cppboot::data::DatabaseError& error) {
    if (error.kind() == cppboot::data::DatabaseError::Kind::UniqueViolation) {
        throw cppboot::web::HttpError(409, "That name is taken");
    }
    throw;
}
```

## Migrations

Migrations are SQL files that change your schema, kept with your code and applied at startup. Name them with a number, an underscore and a description:

```text
db/migrations/
├── 001_create_users.sql
└── 002_add_nickname.sql
```

Each file can hold any number of statements. CppBoot applies new files in number order, each in its own transaction, and records them in a `cppboot_migrations` table.

Startup fails, with a clear message, if any of these is true:

- A migration fails. It rolls back, and those before it stay applied.
- A file that was applied has changed or gone. Never edit an applied migration. Add a new one.
- A new file is numbered below one already applied.
- Two files have the same number, or a file name doesn't follow the pattern.

Several instances can start at once on PostgreSQL, because a lock lets only one apply migrations at a time.

Don't put `BEGIN` or `COMMIT` in a migration, because CppBoot manages the transaction. For the same reason, statements that can't run in a transaction, such as PostgreSQL's `CREATE INDEX CONCURRENTLY`, aren't supported.

## Startup and threads

`DatabaseModule` starts before your other modules and the web server. It checks the connection, so that a bad URL or a stopped server fails at startup rather than on the first request, and then applies migrations.

`Database` is safe to use from any thread. Each statement borrows a connection only while it runs, and a transaction holds one until it finishes. Keep transactions short, because each holds a connection that other requests may be waiting for.

If a connection is lost, the statement using it fails, and the pool replaces that connection. CppBoot doesn't retry statements, because it can't know whether a failed write took effect.

## Without the generator

You can use `Database` directly, with mappers you write yourself:

```cpp
auto mappers = std::make_shared<cppboot::data::RowMappers>();
mappers->add<User>([](const cppboot::data::Row& row, User& value) {
    row.read("id", value.id);
    row.read("name", value.name);
});

auto database = std::make_shared<cppboot::data::Database>(
    cppboot::data::connectionFactoryFor("sqlite:app.db"), cppboot::data::PoolOptions{}, mappers);

auto applied = cppboot::data::runMigrations(*database, "db/migrations");
```

## Not supported yet

- A full ORM, or generating SQL from your structs. You write the SQL
- MySQL, MariaDB and SQL Server. To add a database, implement `cppboot::data::Connection`
- Date, time and binary column types, other than reading them as text
- Rolling back migrations
- Savepoints and choosing an isolation level
