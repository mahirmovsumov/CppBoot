# 009: Code generation

**Status:** Prototype. The approach is proven for services, but the tool isn't installed or available through the `cppboot` CLI yet.

## Question

C++20 can't list a class's constructor parameters, so CppBoot needs a source code generator for dependency injection. The prototype tests whether a generator can read marked classes reliably and produce code that compiles and works.

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
- Every constructor parameter is a `std::shared_ptr` to a component, by value or by reference.

A component can be requested by its own type or by any abstract base class it publicly inherits from directly.

The generator also rejects missing dependencies, two providers of the same type and dependency cycles, so these become build errors rather than runtime failures.

## Results

| Finding | Detail |
|---|---|
| Accuracy | It reads namespaces, type aliases, `const` and references correctly, because it uses Clang's own parser rather than text matching |
| Speed | About 0.8 seconds for one header that includes `<memory>`, on Windows. A project with many headers will need caching or parallel runs |
| Generated code | It compiles with GCC and connects real objects in the tests, with no framework code beyond a small test registry |
| Built-in headers | libclang can't always find its built-in headers when loaded as a library, so the build passes `-resource-dir` explicitly |

## Known limits

- **Path in the binary:** the `-resource-dir` path is fixed at build time, which won't work for an installed tool on another machine. Packaging has to solve this.
- **Absolute includes:** generated headers include their inputs by absolute path, so they can't be moved between machines. That's acceptable because they are build output.
- **Interfaces:** only direct abstract base classes count, and each interface can have only one provider. Choosing between providers needs a qualifier feature.
- **Services only:** controllers and routes aren't implemented.

## Decision

Build dependency injection on this approach. The container's API must not require generated code, so that components can also be registered by hand and Phase 2 can be tested on its own.
