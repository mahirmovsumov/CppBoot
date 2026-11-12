# Command-line tool

The `cppboot` tool creates, builds and runs projects. It needs CMake, Ninja and a C++20 compiler on PATH.

## Commands

### `cppboot create [name]`

Creates a project in a new folder. It won't touch a folder that already exists.

With no name, it asks for the name and each option below:

```text
> cppboot create
Project name: shop
A web API, with a sample controller? (y/n) [n]: y
Database: none, sqlite or postgresql? [none]: sqlite
Sign-in and protected routes? (y/n) [n]:
Health and metrics routes? (y/n) [n]: y
Created shop. Next:
  cd shop
  cppboot run
```

A name must start with a letter and can contain letters, digits, `-` and `_`. Names reserved by CMake or Windows, such as `test` and `con`, aren't allowed.

```text
<name>/
├── CMakeLists.txt
├── cppboot.yaml
├── README.md
├── .gitignore
├── .vscode/          settings for Visual Studio Code, see Editors
├── src/
│   ├── main.cpp
│   └── greeting.hpp
└── tests/
    ├── components_test.cpp
    └── greeting_test.cpp
```

The project builds every `.cpp` file in `src`, and generates component registration from every `.hpp` file in `src`. New files are picked up automatically. `tests/` holds [tests](#cppboot-test-arguments) that a normal build leaves out.

Add `--web` for a web API. The project then has `src/users.hpp`, with a sample controller, in place of `src/greeting.hpp`, and server settings in `cppboot.yaml`:

```powershell
cppboot create my-api --web
```

Add `--sqlite` or `--postgresql` as well to store the API's data in a database. The project then has a repository in `src/users.hpp`, a migration in `db/migrations`, and database settings in `cppboot.yaml`. These options need `--web`:

```powershell
cppboot create my-api --web --sqlite
cppboot create my-api --web --postgresql
```

Add `--security` for sign-in and protected routes. The project then has a sample in `src/auth.hpp`, and needs a secret in the environment to start. It also needs `--web`.

Add `--actuator` for [health and metrics routes](actuator.md). It doesn't need `--web`.

Options can be combined:

```powershell
cppboot create banking-api --web --postgresql --security --actuator
```

In a project with both `--security` and a database, the sample routes in `src/users.hpp` need a token too, because every route without `CPPBOOT_PUBLIC` does.

### `cppboot add <kind> <Name>`

Adds a marked class to `src`, ready to build. The next build finds it, so there's nothing to register.

```powershell
cppboot add service Billing        # src/billing_service.hpp, class BillingService
cppboot add controller Invoice     # src/invoice_controller.hpp, routes under /api/invoices
cppboot add repository Order       # src/order_repository.hpp and db/migrations/002_create_orders.sql
cppboot add middleware Audit       # src/audit_middleware.hpp, runs around every request
cppboot add module Report          # src/report_module.hpp, starts and stops with the application
cppboot add health Queue           # src/queue_health.hpp, a check for /actuator/health
cppboot add config Mail            # src/mail_settings.hpp, filled from the settings under mail
```

The name starts with a capital and has only letters and digits. The kind is added to it unless it's already there, so `Order` and `OrderRepository` both make `OrderRepository`.

It checks the project first, and writes nothing if something's wrong:

- It never replaces a file. If `src/billing_service.hpp` exists, it stops.
- A controller or middleware needs a project made with `--web`, a repository needs a database, and a health check needs `--actuator`.
- A repository's migration gets the next number in `db/migrations`, and uses the column types of the project's database. It stops if a migration already creates that table.

### `cppboot add resource <Name> <field:type>...`

Adds everything a REST resource needs, stored in the project's database, ready to build and run:

```powershell
cppboot add resource OrderItem name:string price:double quantity:int note:string?
```

| File | What's in it |
|---|---|
| `src/order_item.hpp` | `OrderItem`, which is returned as JSON and stored as a row. `OrderItemInput`, what clients send, with checks. `OrderItemRepository`, with `list`, `find`, `add`, `replace` and `remove`. `OrderItemController`, with the routes below |
| `db/migrations/002_create_order_items.sql` | The `order_items` table, which the next run creates |
| `tests/order_item_test.cpp` | Tests of the repository against a real database, and of the checks |

| Route | What it does |
|---|---|
| `GET /api/order-items?limit=20&offset=0` | Lists them in pages, in the order they were added. `limit` is 1 to 100 |
| `GET /api/order-items/{id}` | Returns one, or 404 |
| `POST /api/order-items` | Adds one, and answers 201 with it |
| `PUT /api/order-items/{id}` | Replaces one, or 404 |
| `DELETE /api/order-items/{id}` | Removes one, and answers 204, or 404 |

Each field is `name:type`, and a `?` after the type lets it be missing, which is `NULL` in the table and `null` in JSON:

| Type | C++ | SQLite | PostgreSQL |
|---|---|---|---|
| `string` | `std::string` | `TEXT` | `TEXT` |
| `int` | `int` | `INTEGER` | `INTEGER` |
| `long` | `std::int64_t` | `BIGINT` | `BIGINT` |
| `double` | `double` | `REAL` | `DOUBLE PRECISION` |
| `bool` | `bool` | `BOOLEAN` | `BOOLEAN` |

A text field gets `CPPBOOT_LENGTH(1, 255)`, or `(0, 255)` if it can be missing. Change the checks, add routes, or change the SQL as you like, because the files are yours from then on.

It needs a project made with `--web` and a database. It writes nothing if anything is wrong: a field name that isn't lower-case, `id`, which every resource has, a word that C++ or SQL reserves such as `order`, a type it doesn't know, a file that already exists, or a type with that name already in `src`.

In a PostgreSQL project, the repository tests need `CPPBOOT_TEST_DATABASE_URL` to name a database they may change, and are skipped without it.

### `cppboot test [arguments]`

Builds the tests in `tests/` and runs them with [GoogleTest](https://google.github.io/googletest/). They're built with the application's code, except `src/main.cpp`, so they can create its classes and call them. A normal build leaves them out.

Arguments go to the tests:

```powershell
cppboot test
cppboot test --gtest_filter=UserServiceTest.*
cppboot test --sanitize=address
```

New projects come with `tests/components_test.cpp`, which checks that every component can get what its constructor asks for, without starting the application. Add a `.cpp` file to `tests/` for more tests.

GoogleTest comes with the installers. `cppboot doctor` warns if it's missing.

It returns 0 if every test passed, and 1 if any failed.

### `cppboot build`

Builds the project in the current folder into `build/`. The first build configures the project as a debug build.

Add `--sanitize=address` or `--sanitize=thread` to [find memory errors or data races](memory-safety.md#find-bugs-while-running), on Linux. This builds into `build-address/` or `build-thread/`.

### `cppboot run [arguments]`

Builds the project, then runs it from the project folder so that it finds `cppboot.yaml`. Everything after `run` goes to the application, and you can put `--` first if you prefer:

```powershell
cppboot run --cppboot.logging.level=debug
cppboot run -- --cppboot.logging.level=debug
```

`--sanitize` can come first, and doesn't go to the application: `cppboot run --sanitize=address --cppboot.logging.level=debug`. It works the same with `cppboot test`.

Press Ctrl+C to stop the application.

### `cppboot dev [arguments]`

Builds and runs the project like `cppboot run`, then watches it. When you save a change to `src/`, `db/`, `CMakeLists.txt` or a `cppboot*.yaml` file, it stops the application, builds again and starts it again. Arguments go to the application each time it starts:

```powershell
cppboot dev --cppboot.logging.level=debug
```

- If the build fails, the application stays stopped until you save a fix.
- If the application stops by itself, for example because a setting is wrong, it starts again on the next change.
- Type `r` and Enter to build and restart now, or `q` and Enter to quit.
- Press Ctrl+C to stop. The application gets the same shutdown request that Ctrl+C would give it, and up to 10 seconds to finish. If you press Ctrl+C during a build, the build stops; press it again to quit.

Tests in `tests/` aren't watched, since they don't change the application. Run `cppboot test` for them.

### `cppboot openapi`

Builds the project and writes the [OpenAPI document](web.md#openapi) of its routes to `openapi.json` in the project folder, replacing any that's there. It fails in a project with no controllers, since there's no API to describe.

### `cppboot package`

Builds a release into `build-release/`, then puts it in `dist/` with everything it needs to run on another machine. On Linux, that includes a systemd service and a script that installs it. See [Deployment](deployment.md).

### `cppboot doctor`

Checks that everything CppBoot needs is installed and works together, and says how to fix each problem. Add `--full` to also build and run a sample project. See [Installation](installation.md#check-an-installation).

### `cppboot info`

Shows the CppBoot version, the install folder and the project in the current folder.

### `cppboot help`

Shows the list of commands.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | The command failed, for example because there's no project in the current folder |
| 2 | The command or its arguments weren't recognized |

`cppboot build` returns CMake's exit code if the build fails, and `cppboot run` returns the application's exit code.

## Finding the CppBoot installation

If `CPPBOOT_TOOLCHAIN` names a folder, the tool puts it first on `PATH` for the programs it starts, such as CMake and your application. The Windows installer sets it to MSYS2's tools.

The tool expects to be in the `bin` folder of a CppBoot installation, and it uses that installation for new builds. To use a different one, set `CPPBOOT_HOME` to its folder. A project records the location on its first build, so delete the project's `build` folder after changing it.
