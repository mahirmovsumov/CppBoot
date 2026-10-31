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

`CPPBOOT_SERVICE` compiles to nothing in a normal build. When the generator defines `CPPBOOT_CODEGEN`, it becomes `[[clang::annotate("cppboot:service")]]`, which libclang can read. This keeps applications buildable with GCC, Clang and MSVC, and only the generator needs libclang.

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

`cppboot_generate_components()` runs the tool during the build, and passes it the target's include folders. A project made by `cppboot create` calls it with every `.hpp` file in `src`, so files are found automatically. Changing a listed header runs the generator again.

## Known limits

- **libclang is still required:** the installed tool links to libclang as a shared library, so that has to be on the machine. The SDK isn't a standalone download yet.
- **Build time:** each listed header adds a libclang parse to the build, including headers with no marked classes. This will need caching or parallel runs as projects grow.
- **Changes in included files:** only listed headers trigger the generator, so a change in a header outside `src` that affects a constructor won't run it again.
- **Absolute includes:** generated headers include their inputs by absolute path, so they can't be moved between machines. That's acceptable because they are build output.
- **Interfaces:** only direct abstract base classes count, and each interface can have only one provider. Choosing between providers needs a qualifier feature.
