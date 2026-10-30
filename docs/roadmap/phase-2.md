# Phase 2: Dependency injection

**Status:** In progress.

## Done

- `cppboot::di::Container` with singleton, transient, scoped and external lifetimes
- Registration with declared dependencies, factories, instances and interface bindings
- `validate()` for missing dependencies, cycles and singletons that depend on scoped components
- Cycle detection at runtime, and thread-safe resolution
- Generated code that registers components with the real container

## Remaining

1. Install `cppboot-codegen` with the SDK, and replace the Clang header path that's fixed at build time.
2. Add a CMake function so that projects run the generator as part of their build.
3. Connect the container to `Application`, so that a project needs no manual wiring in `main()`.
4. Generate that setup from `cppboot create`, with a marked service in the template.

## Done when

A project created by `cppboot create` can add a class marked with `CPPBOOT_SERVICE` and have it created and injected, with no registration code written by hand. This has to pass on Linux and Windows.

## Next

Phase 3 is the web framework, including controllers and routes in the generator.
