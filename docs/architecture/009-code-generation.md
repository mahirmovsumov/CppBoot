# 009: Code generation

**Status:** Accepted.

## Context

C++20 can't list a class's constructor parameters, so CppBoot needs a source code generator for dependency injection. A prototype came first, to test whether a generator could read marked classes reliably and produce code that compiles and works. It could, so the generator now ships with the SDK.

## How it works

```text
Header with CPPBOOT_SERVICE classes
  -> libclang parses the header with CPPBOOT_CODEGEN defined
  -> the parser reads each marked class's interfaces and constructor
  -> the validator checks dependencies across all headers
  -> the renderer writes a header containing registerComponents()
  -> the build compiles that header into the application
```

`CPPBOOT_SERVICE` compiles to nothing in a normal build. When the generator defines `CPPBOOT_CODEGEN`, it becomes `[[clang::annotate("cppboot:service")]]`, which libclang can read. So applications don't need any compiler extension, and only the generator needs libclang. CI builds with GCC and Clang, and MSVC hasn't been tried.

## Rules for components

A marked class must meet all of these rules, and the generator reports an error with the file and line if it doesn't:

- It isn't abstract and isn't a template.
- It isn't in an anonymous namespace, because generated code couldn't name it.
- It has one public constructor, not counting copy and move constructors, or no declared constructor at all.
- Every constructor parameter is a reference to a component, or a `std::shared_ptr` to one.
- No field points at a request, a response or a `Principal`, unless it's marked `CPPBOOT_UNCHECKED`. [016](016-ownership.md) covers this and the generator's warnings.

A component can be requested by its own type or by any abstract base class it publicly inherits from directly. The exception is `cppboot::Module`. An application has many modules, so they join a group that `runApplication` reads with `resolveAll`, and can't be injected through that interface.

The generator also rejects missing dependencies, two providers of the same type and dependency cycles, so these become build errors rather than runtime failures.

## JSON and controllers

`CPPBOOT_JSON` generates a specialization of `nlohmann::adl_serializer` for each marked struct, which also handles nested structs. These are defined in the generated header, so user headers can't convert marked structs themselves. `WithStatus<T>` exists for this reason: a controller can choose a status while generated code does the conversion.

`CPPBOOT_CONTROLLER` makes a class a component and generates its routes. All route code is in the generated header, in a factory that registers `Router`, and `ServerModule` is registered with it. An application with no controllers doesn't need `cppboot-web`, because the generated header only includes web headers when it uses them.

The parser decides where each function parameter comes from. Its type decides this rather than extra macros, so controllers read like ordinary functions. The generator rejects a route that can't work, such as a path parameter with no function parameter, or two routes that match the same requests.

## Results

| Finding | Detail |
|---|---|
| Accuracy | It reads namespaces, type aliases, `const` and references correctly, because it uses Clang's own parser rather than text matching |
| Speed | About 0.8 seconds for one header that includes `<memory>`, on Windows. A project with many headers will need caching or parallel runs |
| Generated code | It compiles with GCC and Clang and connects real objects in the tests, using only the public API of `cppboot::di::Container` |
| Built-in headers | libclang can't always find its built-in headers when loaded as a library, so the tool passes `-resource-dir` explicitly |

## Packaging

The SDK installs a copy of Clang's built-in headers, about 15 MB, in `lib/cppboot/clang`. The installed tool finds them relative to its own location, and they're always the same version as the libclang it was built with. When running from a build folder, it uses the path found at build time.

`cppboot_generate_components()` runs the tool during the build. A project made by `cppboot create` calls it with every `.hpp` file in `src`, so files are found automatically.

## Build steps

Each header gets its own step, `cppboot-codegen --scan`, which saves what it found as JSON in the build folder. A last step, `--merge`, reads every saved scan, checks them together and writes the generated header. So:

- **Only changed headers are read again.** A step reruns when its header changes, or any file that header includes, which the tool lists in a dependency file from libclang. Ninja and Make read this. With other CMake generators, only the header itself counts.
- **Headers are read in parallel,** because the steps are independent.
- **Checks still see everything.** Missing dependencies, cycles and duplicate routes involve several headers, so the merge step checks all of them every time. That's fast, because it doesn't parse anything.
- **Warnings print once,** when a header is read, rather than on every build.

The dependency file has every path resolved through symbolic links. libclang can name a file as `/../lib/gcc/x86_64-linux-gnu/14/../../../../include/c++/14/string` on Ubuntu, where `/lib` links to `/usr/lib`. Ninja removes `..` without following links, which names a file that doesn't exist, so every header was read again on every build. Only Linux CI showed this.

There's no separate cache to manage, because the saved scans are ordinary build outputs. A damaged one gives an error that says to rebuild.

### Reading headers as the compiler does

The scan step gets the target's include folders, definitions and options, including those from the libraries it links to, plus `CMAKE_CXX_FLAGS` for the build type. So a class inside `#ifdef FEATURE` is found exactly when the compiler sees it.

These come from CMake's generator expressions rather than `compile_commands.json`. That file lists source files, not headers, so it would need a guess at which entry applies. Not every CMake generator writes it, and the expressions give the same information directly.

A target's options are written for GCC or Clang, and libclang rejects some of them, so the tool keeps only those that change what a header declares: `-I`, `-D`, `-U`, `-isystem`, `-iquote`, `-idirafter`, `-include`, `-imacros`, `-isysroot` and `-std=`. Warnings and optimization options are left out. Options set some other way, such as a compiler wrapper or a toolchain file's hidden flags, aren't seen.

## Build time

Measured on Windows with an i7-8750H, in a project of 50 headers with one service each:

| | Before separate steps | After |
|---|---|---|
| First build | 85 to 93 s | 40 to 51 s |
| Rebuild after one independent header changed | 80 s, all 50 read again | 15 s, 1 read again |
| Rebuild after one header in a chain of 50 changed | 84 s, all read again | 28 to 30 s, the 26 that include it read again |

Most of what remains is compiling `main.cpp`, which includes the generated header and so every listed header.

## Known limits

- **libclang is still required:** the installed tool links to libclang as a shared library, so that has to be on the machine. The SDK isn't a standalone download yet.
- **Build time:** each listed header still needs a libclang parse the first time, including headers with no marked classes.
- **Absolute includes:** generated headers include their inputs by absolute path, so they can't be moved between machines. That's acceptable because they are build output.
- **Interfaces:** only direct abstract base classes count, and each interface can have only one provider. Choosing between providers needs a qualifier feature.
