# Store data in a database

In this tutorial, you create a web API that stores its data in a database, and then change the database's schema. It takes about 10 minutes. Follow [Getting started](getting-started.md) first, so that CppBoot is installed and on PATH.

SQLite needs no server, so this tutorial starts with it. [Switch to PostgreSQL](#switch-to-postgresql) comes at the end.

## Create the project

```powershell
cppboot create users-db --web --sqlite
cd users-db
cppboot run
```

The log shows the migration being applied, then the server starting:

```text
[users-db] [info] Applied 1 new database migrations
[users-db] [info] Database is ready
[users-db] [info] Server listening on port 8080
```

In a second window, add a user and read it back:

```powershell
'{"name":"Ada"}' | Out-File -Encoding ascii body.json
curl.exe --data-binary "@body.json" localhost:8080/api/users
curl.exe localhost:8080/api/users/1
```

```json
{"id":1,"name":"Ada"}
```

Press Ctrl+C in the first window, then run `cppboot run` again. The user is still there, because it's in the `users-db.db` file. The log now says `Applied 0 new database migrations`, because that migration has already run.

## See how it works

The project has three parts:

- `db/migrations/001_create_users.sql` creates the table.
- `cppboot.yaml` names the database and the migrations folder.
- `src/users.hpp` has the code.

In `src/users.hpp`, `CPPBOOT_ROW` lets `User` be read from query results, and `UserRepository` receives the database through its constructor:

```cpp
struct CPPBOOT_JSON CPPBOOT_ROW User {
    std::int64_t id{0};
    std::string name;
};
```

```cpp
User add(const NewUser& details) {
    return *database_->queryOne<User>("INSERT INTO users (name) VALUES ($1) RETURNING id, name",
                                      details.name);
}
```

The value goes in as `$1`, never as part of the SQL text, which is what prevents SQL injection.

## Change the schema

Stop the server, then create `db/migrations/002_add_nickname.sql`:

```sql
ALTER TABLE users ADD COLUMN nickname TEXT;
```

In `src/users.hpp`, add the field to `User`, and select it. `nickname` can be `NULL`, so it's a `std::optional`:

```cpp
struct CPPBOOT_JSON CPPBOOT_ROW User {
    std::int64_t id{0};
    std::string name;
    std::optional<std::string> nickname;
};
```

Then replace the column list `id, name` with `id, name, nickname` in the three queries in `UserRepository`.

Run `cppboot run`. The log says `Applied 1 new database migrations`, and your existing user now has a nickname of `null`:

```powershell
curl.exe localhost:8080/api/users/1
```

```json
{"id":1,"name":"Ada","nickname":null}
```

Never edit a migration that has already run. CppBoot notices the change and refuses to start, because the database no longer matches the file. Add a new migration instead.

## Handle a duplicate

To see error handling, make names unique. Stop the server, and create `db/migrations/003_unique_names.sql`:

```sql
CREATE UNIQUE INDEX users_name ON users (name);
```

In `UserController`, replace `create` with this:

```cpp
CPPBOOT_POST("/") cppboot::web::WithStatus<User> create(const NewUser& details) {
    try {
        return {201, users_->add(details)};
    } catch (const cppboot::data::DatabaseError& error) {
        if (error.kind() == cppboot::data::DatabaseError::Kind::UniqueViolation) {
            throw cppboot::web::HttpError(409, "That name is taken");
        }
        throw;
    }
}
```

Run the server, and post the same body again:

```powershell
curl.exe --data-binary "@body.json" localhost:8080/api/users
```

```json
{"error":"That name is taken","status":409}
```

Without that `catch`, the client would get a generic 500 and the details would go to the log.

## Switch to PostgreSQL

A new project made with `--postgresql` is the same apart from two things: `cppboot.yaml` has a PostgreSQL URL, and the migration creates its ID with `BIGSERIAL`. You write queries the same way, with the same placeholders.

```powershell
cppboot create users-pg --web --postgresql
```

Create the database on your server, then run the project with your own connection details:

```powershell
createdb -h localhost -U postgres users-pg
$env:CPPBOOT_DATABASE_URL = "postgresql://postgres:yourpassword@localhost:5432/users-pg"
cd users-pg
cppboot run
```

The environment variable overrides the URL in `cppboot.yaml`, which is how to keep real passwords out of your code.

## Add a resource with one command

What this tutorial built by hand, the CLI can write for you. In the `users-db` folder:

```powershell
cppboot add resource Product title:string price:double stock:int note:string?
cppboot test
cppboot run
```

This adds `src/product.hpp`, with a repository and a controller for `/api/products`, a migration for the `products` table, and tests that run the repository against a database. `cppboot test` runs them, and `cppboot run` creates the table. Then try it:

```powershell
curl.exe -s -X POST localhost:8080/api/products --data-binary '{\"title\":\"Tea\",\"price\":2.5,\"stock\":10}'
curl.exe -s "localhost:8080/api/products?limit=10"
```

See [`cppboot add resource`](../guides/cli.md#cppboot-add-resource-name-fieldtype) for the field types and everything it writes.

## Next steps

The [database guide](../guides/database.md) covers transactions, every query function, and all the rules for migrations.
