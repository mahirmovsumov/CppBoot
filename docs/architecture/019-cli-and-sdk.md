# 019: The command-line tool and the SDK

**Status:** Accepted.

This record explains how CppBoot reaches a developer's machine and how its command-line tool is built. The [installation guide](../guides/installation.md) and the [command-line guide](../guides/cli.md) explain how to use them.

## The SDK

An SDK is a folder, made by `cmake --install` of a release build:

```text
bin/                  cppboot, cppboot-codegen
include/cppboot/      public headers, one folder per library, and cppboot.hpp for all of them
lib/                  libcppboot-*.a, one static library per framework library
lib/cmake/CppBoot/    CppBootConfig.cmake, the targets, CppBootCodegen.cmake
lib/cppboot/          Clang's own headers for the generator, and build-info.txt
share/doc/cppboot/    licenses
```

### Decisions

- **Static libraries.** An application is one executable, with no CppBoot libraries to find at runtime or keep at the same version as the application. Link-time cost is small, since an application links only the libraries it uses.
- **Built with the compiler that uses it.** C++ has no stable binary interface between compilers or standard libraries, so a Windows SDK is built with MSYS2's UCRT64 GCC and a Linux SDK with Ubuntu 24.04's GCC, and `build-info.txt` records which. `CppBootPreferToolchain.cmake` makes CMake look for libraries such as OpenSSL and libpq in the compiler's own folder first, because a copy built for another compiler, such as one that PostgreSQL's Windows installer adds, won't link. `cppboot doctor` warns about a compiler of another version.
- **The generator carries Clang's headers.** libclang needs the headers of its own version to parse standard library code, so they ship in `lib/cppboot`, and the generator doesn't depend on which Clang the machine has.
- **A project finds it with `find_package(CppBoot)`**, so a project is an ordinary CMake project that also builds without the `cppboot` tool, in an IDE or in CI.

### `cppboot_generate_components`

`CppBootCodegen.cmake` gives projects one function:

```cmake
cppboot_generate_components(my-app)
```

It adds a scan step for each header in `src`, with the target's own include folders, definitions and options, and a merge step that writes `cppboot_components.generated.hpp` and `openapi.json` into the build folder ([009](009-code-generation.md#build-steps)). The title and version in the OpenAPI document come from the target's name and the project's version.

## The installers

| Platform | Installer | Installs |
|---|---|---|
| Windows | `scripts/install.ps1` | MSYS2, then GCC, CMake, Ninja, Boost, OpenSSL, libpq, SQLite, GDB and GoogleTest with `pacman`, then the SDK |
| Linux | `scripts/install.sh` | The same packages with `apt`, then the SDK, or `--from-source` to build the SDK on that machine |

Each one:

1. Downloads the SDK for its platform from the GitHub release, with `SHA256SUMS`.
2. **Checks the download against its checksum, and installs nothing if it doesn't match.** The release workflow writes `SHA256SUMS` from the files it built.
3. Puts `bin` on `PATH`.
4. Runs `cppboot doctor`, so the installation ends with a check, not with a first build that fails.

On Windows, MSYS2's `bin` folder isn't added to `PATH`, because its programs, such as its own `python` or `find`, would take the place of others. The installer records it in `CPPBOOT_TOOLCHAIN` instead, and `cppboot` puts it first on `PATH` only for the programs it starts.

The `install-check` workflow downloads the latest release and installs it on fresh machines, which covers what the release workflow can't: that the release can be found, downloaded and checked.

## The `cppboot` tool

### Structure

`tools/cli` is a small library, `cppboot-cli-lib`, and a `main.cpp` that calls it. Each command lives in its own file:

| File | Commands, or what it holds |
|---|---|
| `cli.cpp` | Reading the command line and dispatching, `help`, `info` |
| `project.cpp`, `project_template.cpp`, `prompts.cpp` | `create`, the files of a new project, and the questions `create` asks with no name |
| `generators.cpp`, `source_files.cpp` | `add <kind>`, and the files it writes |
| `resource.cpp` | `add resource` |
| `build.cpp` | `build`, `run`, `test`, `openapi`, and the build folders |
| `dev.cpp` | `dev` |
| `package.cpp`, `package_files.cpp` | `package`, the service file and install script |
| `doctor.cpp` | `doctor` |
| `toolchain.cpp` | Finding the SDK, putting the toolchain on `PATH`, running programs |

Every function takes its input and output streams and the project folder as parameters, rather than using `std::cin`, `std::cout` or the current folder. The tests then run every command in a temporary folder and compare what it wrote, and `tests/integration/cli_end_to_end.cmake` builds and runs real projects with the installed tool.

### It drives CMake, it doesn't replace it

`cppboot build` runs `cmake` to configure and build, with Ninja. A project is a plain CMake project, so nothing the tool does is out of reach of someone who doesn't use it, and IDEs that understand CMake understand CppBoot projects. The tool only removes the steps that are the same every time:

- **One build folder per kind of build:** `build`, `build-address`, `build-thread` and `build-release`. Switching between a sanitizer build and a normal one never reconfigures a folder, which would rebuild everything.
- **The SDK's location** is passed as `CMAKE_PREFIX_PATH` on the first configure, from `CPPBOOT_HOME` or the folder the tool is in.
- **Programs run from the project folder,** so the application finds `cppboot.yaml`.

### Writing files safely

`create`, `add` and `add resource` check everything before they write anything: the name, the project's kind, existing files and existing types, and for a repository, the next migration number and existing tables. **They never replace a file.** A command that stops halfway would leave a project that doesn't build, and one that replaces a file would lose someone's work.

What they write is ordinary code that the developer owns from then on. Nothing is generated again over it, so changing it is always safe. Code that is generated again on every build, by `cppboot-codegen`, goes into the build folder, where nobody edits it.

### `cppboot dev`

`dev` builds and runs the application, and does it again when a watched file changes: `src`, `db`, `CMakeLists.txt` and `cppboot*.yaml`.

- **It polls**, every 250 ms, comparing each file's modification time and size. Each platform's file notification API has its own limits and failure modes, while polling a project's few hundred files costs almost nothing and works the same everywhere, including on network drives.
- **It waits for files to settle** after a change, because editors often save in more than one step.
- **It stops the application before building**, because Windows can't replace a running executable. The application gets a normal shutdown request, the same as Ctrl+C, and up to 10 seconds, so it shuts down as it would in production.
- **Programs it starts get no input**: their standard input is the null device. A thread reads `r` and `q` from the terminal, and on Windows, a child process that shares a pipe that another thread is reading can stall when it starts.

### `cppboot package`

`package` builds a release into `build-release` and puts in `dist/<name>/` what it needs on another machine:

- the executable, stripped;
- `cppboot.yaml`, every `cppboot-*.yaml` and `db/`;
- the shared libraries it uses that aren't part of the operating system, found from the executable. On Linux, the executable finds them through an `RPATH` of `$ORIGIN/../lib`;
- on Linux, a systemd service and a script that installs it as its own user, with the hardening options of [Deployment](../guides/deployment.md).

A file left in `dist` from an earlier package is removed first, so a package never contains anything from before.

### `cppboot doctor`

`doctor` checks each thing a build needs, and says how to fix each one that fails: the SDK, CMake, Ninja, the generator, the compiler and libraries, GoogleTest and a debugger. `--full` also creates, builds and runs a sample project, which is the only check that proves everything works together. The installers run it last.

## Exit codes

`cppboot` returns 0 on success, 1 when a command fails, and 2 when the command or its arguments aren't recognized. `build` returns CMake's code when the build fails, and `run` returns the application's, so scripts can tell what failed.
