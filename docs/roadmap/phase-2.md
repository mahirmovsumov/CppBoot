# Phase 2: Dependency injection

**Status:** Complete. CI passes on Linux with GCC, Clang and the sanitizers, and on Windows with GCC.

## Done

- `cppboot::di::Container` with singleton, transient, scoped and external lifetimes
- Registration with declared dependencies, factories, instances, interface bindings and groups
- `validate()` for missing dependencies, cycles and singletons that depend on scoped components
- Cycle detection at runtime, and thread-safe resolution
- `cppboot-codegen` installed with the SDK, with its own copy of Clang's built-in headers
- `cppboot_generate_components()`, which runs the generator during a project's build
- `cppboot::runApplication()`, which connects the container to the application
- A `cppboot create` template that uses all of this, with a marked service and module

## Done when

A project created by `cppboot create` can add a class marked with `CPPBOOT_SERVICE` and have it created and injected, with no registration code written by hand. The end-to-end test checks exactly this, and that a missing dependency fails the build.

## Left for later

See "Not included yet" in [004](../architecture/004-dependency-injection.md) and "Known limits" in [009](../architecture/009-code-generation.md).

## Next

Phase 3 is the web framework, including controllers and routes in the generator.
