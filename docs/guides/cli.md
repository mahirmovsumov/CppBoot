# Command-line tool

The `cppboot` tool creates, builds and runs projects. It needs CMake, Ninja and a C++20 compiler on PATH.

## Commands

### `cppboot create <name>`

Creates a project in a new folder. It won't touch a folder that already exists.

A name must start with a letter and can contain letters, digits, `-` and `_`. Names reserved by CMake or Windows, such as `test` and `con`, aren't allowed.

```text
<name>/
├── CMakeLists.txt
├── cppboot.yaml
├── README.md
├── .gitignore
└── src/
    ├── main.cpp
    └── greeting.hpp
```

The project builds every `.cpp` file in `src`, and generates component registration from every `.hpp` file in `src`. New files are picked up automatically.

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

### `cppboot build`

Builds the project in the current folder into `build/`. The first build configures the project as a debug build.

Add `--sanitize=address` or `--sanitize=thread` to [find memory errors or data races](memory-safety.md#find-bugs-while-running), on Linux. This builds into `build-address/` or `build-thread/`.

### `cppboot run [arguments]`

Builds the project, then runs it from the project folder so that it finds `cppboot.yaml`. Everything after `run` goes to the application, and you can put `--` first if you prefer:

```powershell
cppboot run --cppboot.logging.level=debug
cppboot run -- --cppboot.logging.level=debug
```

`--sanitize` can come first, and doesn't go to the application: `cppboot run --sanitize=address --cppboot.logging.level=debug`.

Press Ctrl+C to stop the application.

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

The tool expects to be in the `bin` folder of a CppBoot installation, and it uses that installation for new builds. To use a different one, set `CPPBOOT_HOME` to its folder. A project records the location on its first build, so delete the project's `build` folder after changing it.
