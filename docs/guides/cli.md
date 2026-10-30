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
    └── main.cpp
```

### `cppboot build`

Builds the project in the current folder into `build/`. The first build configures the project as a debug build.

### `cppboot run [arguments]`

Builds the project, then runs it from the project folder so that it finds `cppboot.yaml`. Everything after `run` goes to the application, and you can put `--` first if you prefer:

```powershell
cppboot run --cppboot.logging.level=debug
cppboot run -- --cppboot.logging.level=debug
```

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
